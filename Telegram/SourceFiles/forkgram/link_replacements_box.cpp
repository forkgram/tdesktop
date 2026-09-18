/*
Author: 23rd.
*/

#include "forkgram/link_replacements_box.h"

#include "forkgram/link_replacements.h"
#include "lang/lang_keys.h"
#include "settings/settings_common.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/vertical_list.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace Forkgram {
namespace {

using Rule = LinkReplacements::Rule;

void EditRuleBox(
		not_null<Ui::GenericBox*> box,
		Rule rule,
		Fn<void(Rule)> save,
		Fn<void()> remove) {
	box->setTitle(remove
		? tr::lng_fork_link_replacements_edit()
		: tr::lng_fork_link_replacements_add());

	const auto pattern = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_fork_link_replacements_find(),
			rule.pattern),
		st::markdownLinkFieldPadding);
	const auto replacement = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_fork_link_replacements_replace(),
			rule.replacement),
		st::markdownLinkFieldPadding);

	const auto submit = [=] {
		const auto now = pattern->getLastText().trimmed();
		if (!LinkReplacements::ValidPattern(now)) {
			pattern->showError();
			return;
		}
		save({ now, replacement->getLastText(), rule.enabled });
		box->closeBox();
	};
	pattern->submits() | rpl::on_next([=] {
		replacement->setFocus();
	}, pattern->lifetime());
	replacement->submits() | rpl::on_next([=] {
		submit();
	}, replacement->lifetime());

	box->setFocusCallback([=] {
		pattern->setFocusFast();
	});

	box->addButton(tr::lng_settings_save(), submit);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
	if (remove) {
		box->addLeftButton(tr::lng_box_delete(), [=] {
			remove();
			box->closeBox();
		});
	}
}

} // namespace

void LinkReplacementsBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(tr::lng_fork_link_replacements());
	box->setWidth(st::boxWideWidth);

	struct State {
		std::vector<Rule> rules;
		Ui::VerticalLayout *wrap = nullptr;
		Fn<void()> refresh;
	};
	const auto state = box->lifetime().make_state<State>();
	state->rules = LinkReplacements::Load();

	const auto container = box->verticalLayout();
	const auto enabled = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			tr::lng_fork_link_replacements(),
			st::settingsButtonNoIcon)
	)->toggleOn(rpl::single(LinkReplacements::Enabled()));
	enabled->toggledChanges(
	) | rpl::on_next([=](bool value) {
		LinkReplacements::SetEnabled(value);
	}, enabled->lifetime());

	Ui::AddSkip(container);
	Ui::AddDividerText(container, tr::lng_fork_link_replacements_about());
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		tr::lng_fork_link_replacements_rules());

	state->wrap = container->add(object_ptr<Ui::VerticalLayout>(container));

	const auto add = Settings::AddButtonWithIcon(
		container,
		tr::lng_fork_link_replacements_add(),
		st::settingsButtonActive,
		{
			&st::settingsIconAdd,
			Settings::IconType::Round,
			&st::windowBgActive,
		});

	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		tr::lng_fork_link_replacements_rules_about());

	const auto save = [=] {
		LinkReplacements::Save(state->rules);
		state->refresh();
	};
	const auto addRow = [=](not_null<Ui::VerticalLayout*> inner, int i) {
		const auto &rule = state->rules[i];
		const auto valid = LinkReplacements::ValidPattern(rule.pattern);
		const auto &titlePadding = st::settingsForkRuleTitlePadding;
		const auto &aboutPadding = st::settingsForkRuleAboutPadding;
		const auto row = Ui::CreateChild<Ui::SettingsButton>(
			inner.get(),
			rpl::single(QString()),
			st::settingsButtonNoIcon);
		const auto title = inner->add(
			object_ptr<Ui::FlatLabel>(
				inner,
				rule.pattern,
				st::settingsExperimentalTitle),
			titlePadding);
		const auto about = inner->add(
			object_ptr<Ui::FlatLabel>(
				inner,
				(valid
					? (u"→ "_q + rule.replacement)
					: tr::lng_fork_link_replacements_invalid(tr::now)),
				st::settingsExperimentalAbout),
			aboutPadding);
		if (!valid) {
			about->setTextColorOverride(st::attentionButtonFg->c);
		}
		title->setAttribute(Qt::WA_TransparentForMouseEvents);
		about->setAttribute(Qt::WA_TransparentForMouseEvents);
		rpl::combine(
			inner->widthValue(),
			title->heightValue(),
			about->heightValue()
		) | rpl::on_next([=](int width, int titleHeight, int aboutHeight) {
			row->resize(width, titlePadding.top()
				+ titleHeight
				+ titlePadding.bottom()
				+ aboutPadding.top()
				+ aboutHeight
				+ aboutPadding.bottom());
		}, row->lifetime());
		title->topValue(
		) | rpl::on_next([=](int top) {
			row->moveToLeft(0, top - titlePadding.top());
		}, row->lifetime());
		row->show();
		row->setClickedCallback([=] {
			box->uiShow()->showBox(Box(
				EditRuleBox,
				state->rules[i],
				crl::guard(box, [=](Rule updated) {
					state->rules[i] = updated;
					save();
				}),
				crl::guard(box, [=] {
					state->rules.erase(begin(state->rules) + i);
					save();
				})));
		});
		const auto [toggle, checkView] = Settings::AddSeparatedToggle(
			row,
			st::settingsButtonNoIcon,
			rule.enabled);
		toggle->clicks() | rpl::on_next([=] {
			const auto checked = !checkView->checked();
			checkView->setChecked(checked, anim::type::normal);
			state->rules[i].enabled = checked;
			LinkReplacements::Save(state->rules);
		}, toggle->lifetime());
	};
	state->refresh = [=] {
		state->wrap->clear();
		const auto inner = state->wrap->add(
			object_ptr<Ui::VerticalLayout>(state->wrap));
		for (auto i = 0; i != int(state->rules.size()); ++i) {
			addRow(inner, i);
		}
	};
	state->refresh();

	add->setClickedCallback([=] {
		box->uiShow()->showBox(Box(
			EditRuleBox,
			Rule(),
			crl::guard(box, [=](Rule created) {
				state->rules.push_back(created);
				save();
			}),
			Fn<void()>()));
	});

	box->addButton(tr::lng_close(), [=] {
		box->closeBox();
	});
	box->addLeftButton(tr::lng_fork_link_replacements_reset(), [=] {
		LinkReplacements::Reset();
		state->rules = LinkReplacements::Load();
		state->refresh();
	});
}

} // namespace Forkgram
