/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "pbl/services/timeline/item.h"

#include <stdbool.h>

//! Initialise the in-memory conversation cache.
//! Must be called before any other function in this module.
void android_notif_grouping_init(void);

//! Try to group an incoming Android notification into an existing conversation entry.
//!
//! Grouping is attempted when the notification has both a title and a body.  If a prior
//! notification exists with the same (title, app-name) pair the new message is appended
//! to that notification's AttributeIdHeadings / AttributeIdParagraphs lists; otherwise a
//! brand-new conversation entry is created.
//!
//! When this function returns true the grouping module has:
//!   - stored the updated (or new) conversation notification
//!   - fired notifications_handle_notification_added() for it
//! The caller MUST NOT perform any further storage or notification-event operations for
//! this item.
//!
//! @param notif  Incoming, fully-deserialised notification.  Must not be NULL.
//! @return true  if the notification was handled by this module (caller skips normal path)
//! @return false if the notification should be processed by the normal path (not a groupable msg)
bool android_notif_try_group(TimelineItem *notif);

//! Handle deletion of a notification UUID that may belong to a grouped conversation.
//!
//! Call this from notif_db_delete before the normal storage-remove path.
//!
//! - If @p id is the UUID of a grouped message (not the conv UUID itself), this function
//!   removes the entire conversation from storage, fires the removed event, cleans up both
//!   caches, and returns true.  The caller must skip its own storage/event operations.
//!
//! - If @p id is the conv UUID itself, this function only cleans up the in-memory caches
//!   (so the next message starts fresh) and returns false.  The caller proceeds normally.
//!
//! - If @p id is unrelated to any tracked conversation, returns false.
bool android_notif_grouping_handle_delete(const Uuid *id);
