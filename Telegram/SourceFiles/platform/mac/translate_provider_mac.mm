/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/mac/translate_provider_mac.h"

#include "base/weak_ptr.h"
#include "translate_provider_mac_swift_bridge.h"

#include <cstdlib>
#include <memory>
#include <optional>

namespace Platform {
namespace {

class TranslateProvider final : public Ui::TranslateProvider
, public base::has_weak_ptr {
public:
	[[nodiscard]] bool supportsMessageId() const override {
		return false;
	}

	void request(
			Ui::TranslateProviderRequest request,
			LanguageId to,
			Fn<void(std::optional<TextWithEntities>)> done) override {
		if (request.text.text.isEmpty()) {
			done(std::nullopt);
			return;
		}
		const auto text = request.text.text.toUtf8();
		const auto target = to.twoLetterCode().toUtf8();
		if (target.isEmpty()) {
			done(std::nullopt);
			return;
		}
		struct CallbackContext {
			base::weak_ptr<TranslateProvider> provider;
			Fn<void(std::optional<TextWithEntities>)> done;
		};
		auto ownedContext = std::make_unique<CallbackContext>(CallbackContext{
			.provider = base::make_weak(this),
			.done = std::move(done),
		});
		TranslateProviderMacSwiftTranslate(
			text.constData(),
			target.constData(),
			ownedContext.release(),
			[](void *context, const char *resultUtf8, const char *errorUtf8) {
				auto guard = std::unique_ptr<CallbackContext>(
					static_cast<CallbackContext*>(context));
				auto done = std::move(guard->done);
				const auto isAlive = (guard->provider.get() != nullptr);
				auto translatedText = QString();
				auto hasError = (resultUtf8 == nullptr);
				if (resultUtf8 != nullptr) {
					translatedText = QString::fromUtf8(resultUtf8);
					std::free(const_cast<char*>(resultUtf8));
				}
				if (errorUtf8 != nullptr) {
					hasError = true;
					std::free(const_cast<char*>(errorUtf8));
				}
				if (!isAlive) {
					return;
				}
				crl::on_main([=,
						done = std::move(done),
						translatedText = std::move(translatedText)] {
					done(hasError
						? std::optional<TextWithEntities>()
						: std::optional<TextWithEntities>(TextWithEntities{
							.text = std::move(translatedText),
						}));
				});
			});
	}

};

} // namespace

std::unique_ptr<Ui::TranslateProvider> CreateTranslateProvider() {
	if (TranslateProviderMacSwiftIsAvailable()) {
		return std::make_unique<TranslateProvider>();
	}
	return nullptr;
}

bool IsTranslateProviderAvailable() {
	return TranslateProviderMacSwiftIsAvailable();
}

} // namespace Platform
