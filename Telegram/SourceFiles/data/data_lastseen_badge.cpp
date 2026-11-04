/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_lastseen_badge.h"

#include "base/assertion.h"

namespace Data {
namespace {

constexpr auto kRecentLimit = 15 * 60;
constexpr auto kModerateLimit = 30 * 60;
constexpr auto kStaleLimit = 60 * 60;

} // namespace

LastSeenBadge ClassifyLastSeenBadge(
		LastseenStatus status,
		TimeId now) {
	if (status.isOnline(now)) {
		return LastSeenBadge::Online;
	} else if (status.isHidden()) {
		return LastSeenBadge::None;
	}
	const auto till = status.onlineTill();
	if (!till || till > now) {
		return LastSeenBadge::None;
	}
	const auto elapsed = int64(now) - int64(till);
	return (elapsed < kRecentLimit)
		? LastSeenBadge::Recent
		: (elapsed < kModerateLimit)
		? LastSeenBadge::Moderate
		: (elapsed < kStaleLimit)
		? LastSeenBadge::Stale
		: LastSeenBadge::None;
}

TimeId NextLastSeenBadgeChange(
		LastseenStatus status,
		TimeId now) {
	const auto offset = [&] {
		switch (ClassifyLastSeenBadge(status, now)) {
		case LastSeenBadge::Recent: return kRecentLimit;
		case LastSeenBadge::Moderate: return kModerateLimit;
		case LastSeenBadge::Stale: return kStaleLimit;
		case LastSeenBadge::None:
		case LastSeenBadge::Online: return 0;
		}
		Unexpected("LastSeenBadge value in NextLastSeenBadgeChange.");
	}();
	if (!offset) {
		return 0;
	}
	const auto next = int64(status.onlineTill()) + offset;
	return (next <= std::numeric_limits<TimeId>::max())
		? TimeId(next)
		: 0;
}

} // namespace Data
