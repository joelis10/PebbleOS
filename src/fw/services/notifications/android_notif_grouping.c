/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/notifications/android_notif_grouping.h"

#include "pbl/services/notifications/notification_storage.h"
#include "pbl/services/notifications/notifications.h"
#include "pbl/services/timeline/attribute.h"
#include "pbl/services/timeline/item.h"

#include "kernel/pbl_malloc.h"
#include "system/logging.h"
#include "system/passert.h"
#include "util/size.h"
#include "util/stringlist.h"
#include "util/uuid.h"

#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

//! Max concurrent conversations tracked in memory
#define CONV_CACHE_SIZE 16

//! Max messages retained per conversation before oldest are dropped
#define CONV_MAX_MESSAGES 10

//! Max bytes used for a single sender name (including null terminator)
#define CONV_SENDER_MAX 65

//! Max bytes used for a single message body (including null terminator)
#define CONV_BODY_MAX 201

// Total allocation sizes for each StringList
#define HEADINGS_BUF_SIZE (sizeof(StringList) + CONV_MAX_MESSAGES * CONV_SENDER_MAX + 1)
#define PARAGRAPHS_BUF_SIZE (sizeof(StringList) + CONV_MAX_MESSAGES * CONV_BODY_MAX + 1)

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef struct {
  uint32_t key;    //!< FNV-1a hash of (title + app_name)
  Uuid     notif_id; //!< UUID of the stored conversation notification
} ConvEntry;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static ConvEntry s_conv_cache[CONV_CACHE_SIZE];
static uint8_t   s_conv_count;

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

//! FNV-1a 32-bit hash of a C-string (or empty string if NULL)
static uint32_t prv_fnv1a(const char *str) {
  uint32_t hash = 0x811c9dc5u;
  if (str) {
    while (*str) {
      hash ^= (uint8_t)(*str++);
      hash *= 0x01000193u;
    }
  }
  return hash;
}

//! Build a conversation key from a title and an optional app name
static uint32_t prv_conv_key(const char *title, const char *appname) {
  uint32_t h = prv_fnv1a(title);
  h ^= prv_fnv1a(appname) * 0x9e3779b9u;
  return h;
}

//! Find an existing cache entry for the given key, or NULL
static ConvEntry *prv_find_entry(uint32_t key) {
  for (int i = 0; i < s_conv_count; i++) {
    if (s_conv_cache[i].key == key) {
      return &s_conv_cache[i];
    }
  }
  return NULL;
}

//! Insert or overwrite an entry in the ring-buffer cache
static ConvEntry *prv_upsert_entry(uint32_t key, const Uuid *notif_id) {
  // Check for update
  ConvEntry *entry = prv_find_entry(key);
  if (entry) {
    entry->notif_id = *notif_id;
    return entry;
  }

  // Insert – evict oldest if full
  if (s_conv_count < CONV_CACHE_SIZE) {
    entry = &s_conv_cache[s_conv_count++];
  } else {
    memmove(&s_conv_cache[0], &s_conv_cache[1],
            (CONV_CACHE_SIZE - 1) * sizeof(ConvEntry));
    entry = &s_conv_cache[CONV_CACHE_SIZE - 1];
  }
  entry->key      = key;
  entry->notif_id = *notif_id;
  return entry;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void android_notif_grouping_init(void) {
  memset(s_conv_cache, 0, sizeof(s_conv_cache));
  s_conv_count = 0;
}

bool android_notif_try_group(TimelineItem *notif) {
  PBL_ASSERTN(notif);

  const AttributeList *attrs = &notif->attr_list;

  const char *title   = attribute_get_string(attrs, AttributeIdTitle,   NULL);
  const char *appname = attribute_get_string(attrs, AttributeIdAppName,  NULL);
  const char *body    = attribute_get_string(attrs, AttributeIdBody,     NULL);

  // Only group notifications that look like messages (need a title and a body)
  if (!title || title[0] == '\0' || !body || body[0] == '\0') {
    return false;
  }

  // Sender: prefer explicit AttributeIdSender; fall back to title (1-to-1 chats)
  const char *sender = attribute_get_string(attrs, AttributeIdSender, NULL);
  if (!sender || sender[0] == '\0') {
    sender = title;
  }

  const uint32_t key   = prv_conv_key(title, appname);
  ConvEntry *entry     = prv_find_entry(key);

  // -------------------------------------------------------------------------
  // Allocate StringList buffers (heap – can be large for the stack)
  // -------------------------------------------------------------------------
  StringList *headings   = kernel_zalloc_check(HEADINGS_BUF_SIZE);
  StringList *paragraphs = kernel_zalloc_check(PARAGRAPHS_BUF_SIZE);
  if (!headings || !paragraphs) {
    PBL_LOG_WRN("android_notif_grouping: OOM building StringLists");
    kernel_free(headings);
    kernel_free(paragraphs);
    return false;
  }

  // -------------------------------------------------------------------------
  // Load existing messages if we already have a conversation entry
  // -------------------------------------------------------------------------
  bool updating = false;
  if (entry) {
    TimelineItem existing = {};
    notification_storage_lock();
    bool loaded = notification_storage_get(&entry->notif_id, &existing);
    notification_storage_unlock();

    if (loaded) {
      updating = true;
      StringList *ex_h = attribute_get_string_list(&existing.attr_list, AttributeIdHeadings);
      StringList *ex_p = attribute_get_string_list(&existing.attr_list, AttributeIdParagraphs);
      // Keep the last (CONV_MAX_MESSAGES - 1) existing messages to leave room for the new one
      const size_t existing_count = string_list_count(ex_h);
      const size_t start = (existing_count >= CONV_MAX_MESSAGES)
                           ? (existing_count - (CONV_MAX_MESSAGES - 1)) : 0;
      // Copy [start, existing_count) entries into our buffers
      for (size_t i = start; i < existing_count; i++) {
        const char *h = string_list_get_at(ex_h, i);
        const char *p = string_list_get_at(ex_p, i);
        if (h) string_list_add_string(headings,   HEADINGS_BUF_SIZE,   h, CONV_SENDER_MAX - 1);
        if (p) string_list_add_string(paragraphs, PARAGRAPHS_BUF_SIZE, p, CONV_BODY_MAX   - 1);
      }
      timeline_item_free_allocated_buffer(&existing);
    } else {
      // Cached entry points to a notification that no longer exists – treat as new
      PBL_LOG_DBG("android_notif_grouping: cached entry not in storage, restarting");
      updating = false;
    }
  }

  // Append the new message
  string_list_add_string(headings,   HEADINGS_BUF_SIZE,   sender, CONV_SENDER_MAX - 1);
  string_list_add_string(paragraphs, PARAGRAPHS_BUF_SIZE, body,   CONV_BODY_MAX   - 1);

  // -------------------------------------------------------------------------
  // Build the merged notification attributes
  // -------------------------------------------------------------------------
  AttributeList new_attrs = {};
  attribute_list_add_cstring(&new_attrs, AttributeIdTitle, title);
  if (appname) {
    attribute_list_add_cstring(&new_attrs, AttributeIdAppName, appname);
  }
  // Keep latest message in body so the peek / banner still shows something useful
  attribute_list_add_cstring(&new_attrs, AttributeIdBody, body);
  attribute_list_add_string_list(&new_attrs, AttributeIdHeadings,   headings);
  attribute_list_add_string_list(&new_attrs, AttributeIdParagraphs, paragraphs);

  // Preserve visual attributes from the incoming notification
  const AttributeId preserve[] = {
    AttributeIdIconTiny, AttributeIdIconSmall, AttributeIdIconLarge, AttributeIdIconPin,
    AttributeIdBgColor,  AttributeIdPrimaryColor, AttributeIdSecondaryColor,
    AttributeIdSender,   AttributeIdSubtitle,
    AttributeIdiOSAppIdentifier,
  };
  for (size_t i = 0; i < ARRAY_LENGTH(preserve); i++) {
    Attribute *a = attribute_find(attrs, preserve[i]);
    if (a) {
      attribute_list_add_attribute(&new_attrs, a);
    }
  }

  // -------------------------------------------------------------------------
  // Determine the UUID to use for storage
  // -------------------------------------------------------------------------
  const Uuid conv_uuid = updating ? entry->notif_id : notif->header.id;

  // -------------------------------------------------------------------------
  // Create the merged TimelineItem
  // -------------------------------------------------------------------------
  TimelineItem *merged = timeline_item_create_with_attributes(
      notif->header.timestamp, notif->header.duration,
      TimelineItemTypeNotification, LayoutIdNotification,
      &new_attrs, &notif->action_group);

  attribute_list_destroy_list(&new_attrs);
  kernel_free(headings);
  kernel_free(paragraphs);

  if (!merged) {
    PBL_LOG_WRN("android_notif_grouping: failed to create merged item");
    return false;
  }

  merged->header.id     = conv_uuid;
  merged->header.type   = TimelineItemTypeNotification;
  merged->header.layout = LayoutIdNotification;

  // -------------------------------------------------------------------------
  // Replace in storage (remove old entry first when updating)
  // -------------------------------------------------------------------------
  notification_storage_lock();
  if (updating) {
    notification_storage_remove(&conv_uuid);
  }
  notification_storage_store(merged);
  notification_storage_unlock();

  timeline_item_destroy(merged);

  // -------------------------------------------------------------------------
  // Update the in-memory cache and fire the notification event
  // -------------------------------------------------------------------------
  prv_upsert_entry(key, &conv_uuid);

  Uuid *event_id = kernel_malloc_check(sizeof(Uuid));
  *event_id = conv_uuid;
  notifications_handle_notification_added(event_id);

  return true;
}
