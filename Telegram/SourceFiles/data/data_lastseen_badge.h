/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <limits>

#include "data/data_lastseen_status.h"

namespace Data {

enum class LastSeenBadge : uint8 {
	None,
	Online,
	Recent,
	Moderate,
	Stale,
};

[[nodiscard]] LastSeenBadge ClassifyLastSeenBadge(
	LastseenStatus status,
	TimeId now);
[[nodiscard]] TimeId NextLastSeenBadgeChange(
	LastseenStatus status,
	TimeId now);

} // namespace Data
