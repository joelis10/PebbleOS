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
#include "util/uuid.h"

#include <stdio.h>
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

//! Max bytes for one "Sender: body\n" line
#define CONV_LINE_MAX (CONV_SENDER_MAX + 2 + CONV_BODY_MAX)

//! Total body buffer: enough for all messages plus a null terminator
#define CONV_BODY_BUF_SIZE (CONV_MAX_MESSAGES * CONV_LINE_MAX + 1)

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

  // In group chats the title is the group name; in 1-to-1 chats sender == title.
  // Only prefix lines with "Sender: " in group chats to avoid redundancy.
  const bool is_group = (strcmp(sender, title) != 0);

  const uint32_t key = prv_conv_key(title, appname);
  ConvEntry *entry   = prv_find_entry(key);

  // -------------------------------------------------------------------------
  // Allocate the body text buffer (heap)
  // -------------------------------------------------------------------------
  char *body_buf = kernel_zalloc_check(CONV_BODY_BUF_SIZE);
  if (!body_buf) {
    PBL_LOG_WRN("android_notif_grouping: OOM");
    return false;
  }

  // -------------------------------------------------------------------------
  // Build the body string
  // Each message is one line: "body" (1-to-1) or "Sender: body" (group).
  // -------------------------------------------------------------------------
  bool updating = false;

  // If Android embedded the full current message list (MessagingStyle), use it
  // as the authoritative source so dismissed-on-phone messages don't linger.
  StringList *in_h = attribute_get_string_list(attrs, AttributeIdHeadings);
  StringList *in_p = attribute_get_string_list(attrs, AttributeIdParagraphs);
  const size_t in_count = in_h ? string_list_count(in_h) : 0;

  if (in_count > 0) {
    // Rebuild body from the phone's authoritative list.
    for (size_t i = 0; i < in_count; i++) {
      const char *h = string_list_get_at(in_h, i);
      const char *p = string_list_get_at(in_p, i);
      if (!h || !p) {
        continue;
      }
      const size_t cur_len = strlen(body_buf);
      const size_t remaining = CONV_BODY_BUF_SIZE - cur_len - 1;
      if (remaining == 0) {
        break;
      }
      const char *sep = (cur_len > 0) ? "\n" : "";
      if (strcmp(h, title) != 0) {
        snprintf(body_buf + cur_len, remaining, "%s%s: %s", sep, h, p);
      } else {
        snprintf(body_buf + cur_len, remaining, "%s%s", sep, p);
      }
    }
    // Dedup: if the resulting body matches what is already stored, suppress.
    updating = (entry != NULL);
    if (updating) {
      TimelineItem existing = {};
      notification_storage_lock();
      bool loaded = notification_storage_get(&entry->notif_id, &existing);
      notification_storage_unlock();
      if (loaded) {
        const char *stored = attribute_get_string(&existing.attr_list, AttributeIdBody, "");
        const bool is_dup = (strcmp(body_buf, stored) == 0);
        timeline_item_free_allocated_buffer(&existing);
        if (is_dup) {
          kernel_free(body_buf);
          return true;
        }
      } else {
        updating = false;
      }
    }
  } else {
    // No list from Android — accumulate from our stored history + new message.
    if (entry) {
      TimelineItem existing = {};
      notification_storage_lock();
      bool loaded = notification_storage_get(&entry->notif_id, &existing);
      notification_storage_unlock();
      if (loaded) {
        updating = true;
        const char *stored = attribute_get_string(&existing.attr_list, AttributeIdBody, NULL);
        if (stored) {
          // Copy stored body, dropping the oldest lines if at the message limit.
          const char *copy_from = stored;
          size_t line_count = 0;
          for (const char *p = stored; *p; p++) {
            if (*p == '\n') {
              line_count++;
            }
          }
          while (line_count >= CONV_MAX_MESSAGES) {
            const char *nl = strchr(copy_from, '\n');
            if (!nl) {
              break;
            }
            copy_from = nl + 1;
            line_count--;
          }
          strncpy(body_buf, copy_from, CONV_BODY_BUF_SIZE - 1);
        }
        timeline_item_free_allocated_buffer(&existing);
      } else {
        PBL_LOG_DBG("android_notif_grouping: cached entry not in storage, restarting");
      }
    }

    // Dedup: suppress if the last stored line matches the incoming message.
    {
      const char *last_line = body_buf[0] ? body_buf : NULL;
      for (const char *p = body_buf; *p; p++) {
        if (*p == '\n' && *(p + 1)) {
          last_line = p + 1;
        }
      }
      char new_line[CONV_LINE_MAX + 1];
      if (is_group) {
        snprintf(new_line, sizeof(new_line), "%s: %s", sender, body);
      } else {
        snprintf(new_line, sizeof(new_line), "%s", body);
      }
      if (last_line && strcmp(last_line, new_line) == 0) {
        kernel_free(body_buf);
        return true;
      }
      // Append the new line.
      const size_t cur_len = strlen(body_buf);
      const size_t remaining = CONV_BODY_BUF_SIZE - cur_len - 1;
      if (remaining > 0) {
        const char *sep = (cur_len > 0) ? "\n" : "";
        snprintf(body_buf + cur_len, remaining, "%s%s", sep, new_line);
      }
    }
  }

  // -------------------------------------------------------------------------
  // Build the merged notification attributes
  // -------------------------------------------------------------------------
  AttributeList new_attrs = {};
  attribute_list_add_cstring(&new_attrs, AttributeIdTitle, title);
  if (appname) {
    attribute_list_add_cstring(&new_attrs, AttributeIdAppName, appname);
  }
  attribute_list_add_cstring(&new_attrs, AttributeIdBody, body_buf);

  // Preserve visual attributes from the incoming notification.
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
  kernel_free(body_buf);

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
  // When replacing an existing entry, remove the old display entry first so the
  // notification list doesn't show both the stale and updated versions.
  if (updating) {
    notifications_handle_notification_removed(event_id);
  }
  notifications_handle_notification_added(event_id);

  return true;
}
