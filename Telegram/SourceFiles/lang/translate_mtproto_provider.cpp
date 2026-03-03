/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lang/translate_mtproto_provider.h"

#include "api/api_text_entities.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "mtproto/sender.h"
#include "spellcheck/platform/platform_language.h"

namespace Ui {
namespace {

class MTProtoTranslateProvider final : public TranslateProvider {
public:
	explicit MTProtoTranslateProvider(not_null<Main::Session*> session)
	: _session(session)
	, _api(&session->mtp()) {
	}

	[[nodiscard]] bool supportsMessageId() const override {
		return true;
	}

	void request(
			TranslateProviderRequest request,
			LanguageId to,
			Fn<void(TranslateProviderResult)> done) override {
		const auto msgId = MsgId(request.msgId);
		const auto peerId = PeerId(PeerIdHelper(request.peerId));
		const auto peer = msgId
			? _session->data().peerLoaded(peerId)
			: nullptr;
		using Flag = MTPmessages_TranslateText::Flag;
		const auto flags = msgId
			? (Flag::f_peer | Flag::f_id)
			: !request.text.text.isEmpty()
			? Flag::f_text
			: Flag(0);
		if (!flags || (msgId && !peer)) {
			done(TranslateProviderResult{
				.error = TranslateProviderError::Unknown,
			});
			return;
		}
		_api.request(MTPmessages_TranslateText(
			MTP_flags(flags),
			msgId ? peer->input() : MTP_inputPeerEmpty(),
			(msgId
				? MTP_vector<MTPint>(1, MTP_int(msgId.bare))
				: MTPVector<MTPint>()),
			(msgId
				? MTPVector<MTPTextWithEntities>()
				: MTP_vector<MTPTextWithEntities>(1, MTP_textWithEntities(
					MTP_string(request.text.text),
					Api::EntitiesToMTP(
						_session,
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
						_session,
						list.front()),
				});
		}).fail([=](const MTP::Error &) {
			done(TranslateProviderResult{
				.error = TranslateProviderError::Unknown,
			});
		}).send();
	}

private:
	const not_null<Main::Session*> _session;
	MTP::Sender _api;

};

} // namespace

std::unique_ptr<TranslateProvider> CreateMTProtoTranslateProvider(
		not_null<Main::Session*> session) {
	return std::make_unique<MTProtoTranslateProvider>(session);
}

} // namespace Ui
