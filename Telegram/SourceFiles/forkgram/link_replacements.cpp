/*
Author: 23rd.
*/

#include "forkgram/link_replacements.h"

#include "chat_helpers/rich_paste_toast.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "lang/lang_keys.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/qt_object_factory.h"

#include "styles/style_chat.h"

namespace Forkgram::LinkReplacements {
namespace {

constexpr auto kEnabledKey = "fork_replace_links_on_paste";
constexpr auto kRulesKey = "fork_link_replacement_rules";
constexpr auto kSerializeVersion = 1;
constexpr auto kMaxTextLength = 10000;
constexpr auto kMaxRules = 100;
constexpr auto kToastDuration = 5 * crl::time(1000);

[[nodiscard]] rpl::event_stream<> &ChangesStream() {
	static auto result = rpl::event_stream<>();
	return result;
}

[[nodiscard]] QString Expand(
		const QRegularExpressionMatch &match,
		const QString &replacement) {
	const auto size = int(replacement.size());
	auto result = QString();
	result.reserve(size);
	for (auto i = 0; i != size;) {
		const auto ch = replacement[i];
		if ((ch == '\\') && (i + 1 != size)) {
			result.append(replacement[i + 1]);
			i += 2;
		} else if ((ch == '$')
			&& (i + 1 != size)
			&& replacement[i + 1].isDigit()) {
			const auto group = replacement[i + 1].digitValue();
			if (group <= match.lastCapturedIndex()) {
				result.append(match.captured(group));
			}
			i += 2;
		} else {
			result.append(ch);
			++i;
		}
	}
	return result;
}

[[nodiscard]] int Apply(TextWithTags &text, int from, int till) {
	if (till - from > kMaxTextLength) {
		return -1;
	}
	struct Edit {
		int from = 0;
		int till = 0;
		QString with;
	};
	auto changed = false;
	for (const auto &rule : Load()) {
		const auto regex = QRegularExpression(rule.pattern);
		if (!rule.enabled || !regex.isValid()) {
			continue;
		}
		auto edits = std::vector<Edit>();
		auto i = regex.globalMatch(text.text.mid(from, till - from));
		while (i.hasNext()) {
			const auto match = i.next();
			if (match.capturedEnd() > match.capturedStart()) {
				edits.push_back({
					int(match.capturedStart()),
					int(match.capturedEnd()),
					Expand(match, rule.replacement),
				});
			}
		}
		for (auto j = edits.crbegin(); j != edits.crend(); ++j) {
			text = ChatHelpers::TextWithTagsReplaced(
				text,
				from + j->from,
				from + j->till,
				{ j->with });
			till += int(j->with.size()) - (j->till - j->from);
			changed = true;
		}
	}
	return changed ? till : -1;
}

void ShowUndoToast(
		not_null<Ui::InputField*> field,
		TextWithTags replaced,
		Fn<void()> undo) {
	const auto weakField = base::make_weak(field);
	const auto intact = [=] {
		const auto strong = weakField.get();
		return strong && (strong->getTextWithTags() == replaced);
	};
	const auto button = tr::lng_fork_link_replaced_undo(tr::now);
	const auto st = std::make_shared<style::Toast>(st::historyPremiumToast);
	st->padding.setRight(
		st::historyPremiumViewSet.style.font->width(button)
		- st::historyPremiumViewSet.width);
	const auto weak = Ui::Toast::Show(field->window(), Ui::Toast::Config{
		.text = tr::lng_fork_link_replaced(tr::now, tr::marked),
		.st = st.get(),
		.acceptinput = true,
		.duration = kToastDuration,
	});
	const auto strong = weak.get();
	if (!strong) {
		return;
	}
	const auto widget = strong->widget();
	widget->lifetime().add([st] {});
	field->changes() | rpl::filter([=] {
		return !intact();
	}) | rpl::on_next([=] {
		if (const auto strong = weak.get()) {
			strong->hideAnimated();
		}
	}, widget->lifetime());
	const auto activate = Ui::CreateChild<Ui::RoundButton>(
		widget.get(),
		rpl::single(button),
		st::historyPremiumViewSet);
	activate->show();
	activate->setClickedCallback([=] {
		if (const auto strong = weak.get()) {
			strong->hideAnimated();
		}
		if (intact()) {
			undo();
		}
	});
	rpl::combine(
		widget->sizeValue(),
		activate->sizeValue()
	) | rpl::on_next([=](QSize outer, QSize inner) {
		activate->moveToRight(
			0,
			(outer.height() - inner.height()) / 2,
			outer.width());
	}, widget->lifetime());
}

void ScheduleReplace(not_null<Ui::InputField*> field) {
	const auto was = field->getTextWithTags();
	const auto cursor = field->textCursor();
	const auto from = std::min(cursor.position(), cursor.anchor());
	crl::on_main(field, [=] {
		const auto pastedTill = int(field->textCursor().position());
		const auto pasted = field->getTextWithTags();
		if ((pastedTill <= from) || (pasted == was)) {
			return;
		}
		auto updated = pasted;
		const auto till = Apply(updated, from, pastedTill);
		if (till < 0) {
			return;
		}
		field->setTextWithTags(updated);
		field->setCursorPosition(till);
		ShowUndoToast(field, field->getTextWithTags(), [=] {
			field->setTextWithTags(pasted);
			field->setCursorPosition(pastedTill);
		});
	});
}

} // namespace

bool Enabled() {
	return Core::App().settings().readPref<bool>(kEnabledKey, true);
}

void SetEnabled(bool enabled) {
	Core::App().settings().writePref<bool>(kEnabledKey, enabled);
	Core::App().saveSettingsDelayed();
	ChangesStream().fire({});
}

std::vector<Rule> Default() {
	return {
		{
			u"https?://(?:www\\.)?(?:twitter|x)\\.com/"_q,
			u"https://fixupx.com/"_q,
		},
		{
			u"https?://(?:www\\.)?instagram\\.com/"_q,
			u"https://kkinstagram.com/"_q,
		},
		{
			u"https?://(?:www\\.|vm\\.|vt\\.)?tiktok\\.com/"_q,
			u"https://vxtiktok.com/"_q,
		},
		{
			u"https?://(?:www\\.|old\\.)?reddit\\.com/"_q,
			u"https://rxddit.com/"_q,
		},
		{
			u"https?://(?:www\\.)?bsky\\.app/"_q,
			u"https://fxbsky.app/"_q,
		},
		{
			u"https?://(?:www\\.)?pixiv\\.net/(?:en/)?artworks/"_q,
			u"https://phixiv.net/artworks/"_q,
		},
	};
}

std::vector<Rule> Load() {
	const auto stored = Core::App().settings().readPref<QByteArray>(kRulesKey);
	if (stored.isEmpty()) {
		return Default();
	}
	auto stream = QDataStream(stored);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = qint32();
	auto count = qint32();
	stream >> version >> count;
	if ((stream.status() != QDataStream::Ok)
		|| (version != kSerializeVersion)
		|| (count < 0)
		|| (count > kMaxRules)) {
		return Default();
	}
	auto result = std::vector<Rule>();
	result.reserve(count);
	for (auto i = 0; i != count; ++i) {
		auto pattern = QString();
		auto replacement = QString();
		auto enabled = qint32();
		stream >> pattern >> replacement >> enabled;
		if (stream.status() != QDataStream::Ok) {
			return Default();
		} else if (!pattern.isEmpty()) {
			result.push_back({ pattern, replacement, (enabled == 1) });
		}
	}
	return result;
}

void Save(const std::vector<Rule> &rules) {
	const auto count = int(std::min(rules.size(), size_t(kMaxRules)));
	auto data = QByteArray();
	{
		auto stream = QDataStream(&data, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		stream << qint32(kSerializeVersion) << qint32(count);
		for (auto i = 0; i != count; ++i) {
			const auto &rule = rules[i];
			stream
				<< rule.pattern
				<< rule.replacement
				<< qint32(rule.enabled ? 1 : 0);
		}
	}
	Core::App().settings().writePref<QByteArray>(kRulesKey, data);
	Core::App().saveSettingsDelayed();
	ChangesStream().fire({});
}

void Reset() {
	Core::App().settings().clearPref(kRulesKey);
	Core::App().saveSettingsDelayed();
	ChangesStream().fire({});
}

bool ValidPattern(const QString &pattern) {
	return !pattern.isEmpty() && QRegularExpression(pattern).isValid();
}

int EnabledCount() {
	const auto rules = Load();
	return int(ranges::count_if(rules, [](const Rule &rule) {
		return rule.enabled && ValidPattern(rule.pattern);
	}));
}

rpl::producer<> Changes() {
	return ChangesStream().events();
}

Ui::InputField::MimeDataHook WrappedMimeHook(
		Ui::InputField::MimeDataHook original,
		not_null<Ui::InputField*> field) {
	return [=, original = std::move(original)](
			not_null<const QMimeData*> data,
			Ui::InputField::MimeAction action) {
		if (original && original(data, action)) {
			return true;
		} else if ((action == Ui::InputField::MimeAction::Insert)
			&& Enabled()
			&& data->hasText()) {
			ScheduleReplace(field);
		}
		return false;
	};
}

} // namespace Forkgram::LinkReplacements
