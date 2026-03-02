/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lang/translate_mtproto_provider.h"

#include "api/api_text_entities.h"
#include "data/data_peer.h"
#include "main/main_session.h"
#include "mtproto/sender.h"

namespace Ui {
namespace {

class MTProtoTranslateProvider final : public TranslateProvider {
public:
	explicit MTProtoTranslateProvider(not_null<Main::Session*> session)
	: _api(&session->mtp()) {
	}

	[[nodiscard]] bool supportsMessageId() const override {
		return true;
	}

	void request(
			TranslateProviderRequest request,
			LanguageId to,
			Fn<void(TranslateProviderResult)> done) override {
		using Flag = MTPmessages_TranslateText::Flag;
		const auto flags = request.msgId
			? (Flag::f_peer | Flag::f_id)
			: !request.text.text.isEmpty()
			? Flag::f_text
			: Flag(0);
		if (!flags) {
			done(TranslateProviderResult{
				.error = TranslateProviderError::Unknown,
			});
			return;
		}
		_api.request(MTPmessages_TranslateText(
			MTP_flags(flags),
			request.msgId ? request.peer->input() : MTP_inputPeerEmpty(),
			(request.msgId
				? MTP_vector<MTPint>(1, MTP_int(request.msgId))
				: MTPVector<MTPint>()),
			(request.msgId
				? MTPVector<MTPTextWithEntities>()
				: MTP_vector<MTPTextWithEntities>(1, MTP_textWithEntities(
					MTP_string(request.text.text),
					Api::EntitiesToMTP(
						&request.peer->session(),
						request.text.entities,
						Api::ConvertOption::SkipLocal)))),
			MTP_string(to.twoLetterCode())
		)).done([=](const MTPmessages_TranslatedText &result) {
			const auto &data = result.data();
			const auto &list = data.vresult().v;
			done(list.isEmpty()
				? TranslateProviderResult{
					.error = TranslateProviderError::Unknown,
				}
				: TranslateProviderResult{
					.text = Api::ParseTextWithEntities(
						&request.peer->session(),
						list.front()),
				});
		}).fail([=](const MTP::Error &) {
			done(TranslateProviderResult{
				.error = TranslateProviderError::Unknown,
			});
		}).send();
	}

private:
	MTP::Sender _api;

};

} // namespace

std::unique_ptr<TranslateProvider> CreateMTProtoTranslateProvider(
		not_null<Main::Session*> session) {
	return std::make_unique<MTProtoTranslateProvider>(session);
}

} // namespace Ui
