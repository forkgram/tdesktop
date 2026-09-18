/*
Author: 23rd.
*/
#pragma once

#include "ui/widgets/fields/input_field.h"

namespace Forkgram::LinkReplacements {

struct Rule {
	QString pattern;
	QString replacement;
	bool enabled = true;
};

[[nodiscard]] bool Enabled();
void SetEnabled(bool enabled);

[[nodiscard]] std::vector<Rule> Default();
[[nodiscard]] std::vector<Rule> Load();
void Save(const std::vector<Rule> &rules);
void Reset();

[[nodiscard]] bool ValidPattern(const QString &pattern);
[[nodiscard]] int EnabledCount();
[[nodiscard]] rpl::producer<> Changes();

[[nodiscard]] Ui::InputField::MimeDataHook WrappedMimeHook(
	Ui::InputField::MimeDataHook original,
	not_null<Ui::InputField*> field);

} // namespace Forkgram::LinkReplacements
