/*
Author: Forkgram Contributors.
*/

#pragma once

#include "core/core_settings.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "history/history_item.h"

namespace Forkgram {

// Check if a message is from a blocked user.
// Used to hide messages from blocked users in group chats.
// Does NOT hide messages in the blocked user's own chat dialog.
[[nodiscard]] inline bool isFromBlockedUser(
		not_null<HistoryItem*> item) {
	// Check if the feature is enabled.
	if (!Core::App().settings().fork().hideFromBlockedUsers()) {
		return false;
	}

	// Don't hide our own messages.
	if (item->out()) {
		return false;
	}

	// Don't hide messages in the blocked user's own chat.
	// Otherwise the dialog would be empty.
	if (item->from() == item->history()->peer) {
		return false;
	}

	// Check if the sender is blocked.
	const auto sender = item->from();
	if (sender->isUser() && sender->asUser()->isBlocked()) {
		return true;
	}

	// For forwarded messages, check the original sender.
	if (const auto original = item->originalSender()) {
		if (original->isUser() && original->asUser()->isBlocked()) {
			return true;
		}
	}

	return false;
}

} // namespace Forkgram
