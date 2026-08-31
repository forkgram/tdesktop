/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_lastseen_badge.h"

#include <iostream>
#include <limits>

namespace {

using Data::LastSeenBadge;
using Data::LastseenStatus;

constexpr auto kNow = TimeId(2'000'000'000);

[[nodiscard]] bool Check(
		LastseenStatus status,
		LastSeenBadge expected,
		TimeId next = 0) {
	if (Data::ClassifyLastSeenBadge(status, kNow) != expected) {
		return false;
	}
	return Data::NextLastSeenBadgeChange(status, kNow) == next;
}

[[nodiscard]] bool CheckRescheduledDeadline() {
	const auto original = LastseenStatus::OnlineTill(kNow);
	const auto changed = LastseenStatus::OnlineTill(kNow - 5 * 60);
	if (Data::ClassifyLastSeenBadge(original, kNow)
		!= LastSeenBadge::Recent
		|| Data::ClassifyLastSeenBadge(changed, kNow)
		!= LastSeenBadge::Recent) {
		return false;
	}
	const auto originalTimeout = Data::NextLastSeenBadgeChange(
		original,
		kNow) - kNow;
	const auto changedTimeout = Data::NextLastSeenBadgeChange(
		changed,
		kNow) - kNow;
	return (changedTimeout < originalTimeout)
		&& (changedTimeout == 10 * 60);
}

} // namespace

int main() {
	const auto result = Check(
		LastseenStatus::OnlineTill(kNow + 1),
		LastSeenBadge::Online)
		&& Check(
			LastseenStatus::OnlineTill(
				std::numeric_limits<TimeId>::max()),
			LastSeenBadge::Online)
		&& Check(
			LastseenStatus::OnlineTill(kNow + 1, true),
			LastSeenBadge::Online)
		&& Check(
			LastseenStatus::OnlineTill(kNow),
			LastSeenBadge::Recent,
			kNow + 15 * 60)
		&& Check(
			LastseenStatus::OnlineTill(kNow - (15 * 60 - 1)),
			LastSeenBadge::Recent,
			kNow + 1)
		&& Check(
			LastseenStatus::OnlineTill(kNow - 15 * 60),
			LastSeenBadge::Moderate,
			kNow + 15 * 60)
		&& Check(
			LastseenStatus::OnlineTill(kNow - (15 * 60 + 1)),
			LastSeenBadge::Moderate,
			kNow + 15 * 60 - 1)
		&& Check(
			LastseenStatus::OnlineTill(kNow - (30 * 60 - 1)),
			LastSeenBadge::Moderate,
			kNow + 1)
		&& Check(
			LastseenStatus::OnlineTill(kNow - 30 * 60),
			LastSeenBadge::Stale,
			kNow + 30 * 60)
		&& Check(
			LastseenStatus::OnlineTill(kNow - (30 * 60 + 1)),
			LastSeenBadge::Stale,
			kNow + 30 * 60 - 1)
		&& Check(
			LastseenStatus::OnlineTill(kNow - (60 * 60 - 1)),
			LastSeenBadge::Stale,
			kNow + 1)
		&& Check(
			LastseenStatus::OnlineTill(kNow - 60 * 60),
			LastSeenBadge::None)
		&& Check(
			LastseenStatus::OnlineTill(kNow - (60 * 60 + 1)),
			LastSeenBadge::None)
		&& Check(
			LastseenStatus::OnlineTill(kNow - 24 * 60 * 60),
			LastSeenBadge::None)
		&& Check(
			LastseenStatus::OnlineTill(kNow - 1, true),
			LastSeenBadge::None)
		&& Check(LastseenStatus::Recently(), LastSeenBadge::None)
		&& Check(LastseenStatus::WithinWeek(), LastSeenBadge::None)
		&& Check(LastseenStatus::WithinMonth(), LastSeenBadge::None)
		&& Check(LastseenStatus::LongAgo(), LastSeenBadge::None)
		&& Check(LastseenStatus::OnlineTill(1), LastSeenBadge::None)
		&& Check(LastseenStatus(), LastSeenBadge::None)
		&& CheckRescheduledDeadline();
	if (!result) {
		std::cerr << "Last-seen badge classification test failed.\n";
		return 1;
	}
	std::cout << "Last-seen badge classification tests passed.\n";
	return 0;
}
