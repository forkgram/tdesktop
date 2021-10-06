/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flags.h"

namespace Data {
class Session;
} // namespace Data

enum class ReplyMarkupFlag : uint32 {
	None                  = (1U << 0),
	ForceReply            = (1U << 1),
	HasSwitchInlineButton = (1U << 2),
	Inline                = (1U << 3),
	Resize                = (1U << 4),
	SingleUse             = (1U << 5),
	Selective             = (1U << 6),
	IsNull                = (1U << 7),
};
inline constexpr bool is_flag_type(ReplyMarkupFlag) { return true; }
using ReplyMarkupFlags = base::flags<ReplyMarkupFlag>;

struct HistoryMessageMarkupButton {
	enum class Type {
		Default,
		Url,
		Callback,
		CallbackWithPassword,
		RequestPhone,
		RequestLocation,
		RequestPoll,
		SwitchInline,
		SwitchInlineSame,
		Game,
		Buy,
		Auth,
	};

	HistoryMessageMarkupButton(
		Type type,
		const QString &text,
		const QByteArray &data = QByteArray(),
		const QString &forwardText = QString(),
		int32 buttonId = 0);

	static HistoryMessageMarkupButton *Get(
		not_null<Data::Session*> owner,
		FullMsgId itemId,
		int row,
		int column);

	Type type;
	QString text, forwardText;
	QByteArray data;
	int32 buttonId = 0;
	mutable mtpRequestId requestId = 0;

};

struct HistoryMessageMarkupData {
	HistoryMessageMarkupData() = default;
	explicit HistoryMessageMarkupData(const MTPReplyMarkup *data);

	void fillForwardedData(const HistoryMessageMarkupData &original);

	[[nodiscard]] bool isNull() const;
	[[nodiscard]] bool isTrivial() const;

	using Button = HistoryMessageMarkupButton;
	std::vector<std::vector<Button>> rows;
	ReplyMarkupFlags flags = ReplyMarkupFlag::IsNull;
	QString placeholder;

private:
	void fillRows(const QVector<MTPKeyboardButtonRow> &v);

};
