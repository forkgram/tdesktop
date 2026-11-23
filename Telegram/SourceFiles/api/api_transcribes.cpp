/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_transcribes.h"

#include "apiwrap.h"
#include "api/api_text_entities.h"
#include "data/data_channel.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "media/audio/media_audio_edit.h"
#include "media/streaming/media_streaming_loader.h"
#include "media/streaming/media_streaming_reader.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "lang/lang_keys.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "spellcheck/spellcheck_types.h"
#include "storage/storage_account.h"
#include "storage/file_download.h"
#include "storage/streamed_file_downloader.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Api {
namespace {

constexpr auto kCloudflareEnabled = "fork_cf_stt_enabled"_cs;
constexpr auto kCloudflareAccount = "fork_cf_stt_account"_cs;
constexpr auto kCloudflareToken = "fork_cf_stt_token"_cs;
constexpr auto kCloudflareMaxAudio = 25 * 1024 * 1024;
constexpr auto kCloudflareMaxResponse = 1024 * 1024;
constexpr auto kCloudflareTimeout = 120 * 1000;

}

Transcribes::Transcribes(not_null<ApiWrap*> api)
: _session(&api->session())
, _api(&api->instance()) {
	_session->changes().realtimeMessageUpdates(
		Data::MessageUpdate::Flag::Destroyed
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		const auto id = update.item->fullId();
		const auto i = _cloudflareTasks.find(id);
		if (i != _cloudflareTasks.end()) {
			const auto task = i->second;
			_cloudflareTasks.erase(i);
			_map.remove(id);
			cancelCloudflare(task);
		}
	}, _lifetime);
}

bool Transcribes::isRated(not_null<HistoryItem*> item) const {
	if (entry(item).cloudflare) {
		return true;
	}
	const auto fullId = item->fullId();
	for (const auto &[transcribeId, id] : _ids) {
		if (id == fullId) {
			return _session->settings().isTranscriptionRated(transcribeId);
		}
	}
	return false;
}

void Transcribes::rate(not_null<HistoryItem*> item, bool isGood) {
	if (entry(item).cloudflare) {
		return;
	}
	const auto fullId = item->fullId();
	for (const auto &[transcribeId, id] : _ids) {
		if (id == fullId) {
			_api.request(MTPmessages_RateTranscribedAudio(
				item->history()->peer->input(),
				MTP_int(item->id),
				MTP_long(transcribeId),
				MTP_bool(isGood))).send();
			_session->settings().markTranscriptionAsRated(transcribeId);
			_session->saveSettings();
			return;
		}
	}
}

bool Transcribes::cloudflareFor(not_null<HistoryItem*> item) const {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	auto &local = _session->local();
	return document
		&& (document->isVoiceMessage() || document->isVideoMessage())
		&& item->allowsMediaDownloadControls()
		&& !document->forbidsFileSave()
		&& !media->ttlSeconds()
		&& local.readPref<bool>(kCloudflareEnabled)
		&& !local.readPref<QString>(kCloudflareAccount).trimmed().isEmpty()
		&& !local.readPref<QString>(kCloudflareToken).trimmed().isEmpty();
}

bool Transcribes::freeFor(not_null<HistoryItem*> item) const {
	if (const auto channel = item->history()->peer->asMegagroup()) {
		const auto owner = &channel->owner();
		return channel->levelHint() >= owner->groupFreeTranscribeLevel();
	}
	return false;
}

bool Transcribes::trialsSupport() {
	if (!_trialsSupport) {
		const auto count = _session->appConfig().get<int>(
			u"transcribe_audio_trial_weekly_number"_q,
			0);
		const auto until = _session->appConfig().get<int>(
			u"transcribe_audio_trial_cooldown_until"_q,
			0);
		_trialsSupport = (count > 0) || (until > 0);
	}
	return *_trialsSupport;
}

TimeId Transcribes::trialsRefreshAt() {
	if (_trialsRefreshAt < 0) {
		_trialsRefreshAt = _session->appConfig().get<int>(
			u"transcribe_audio_trial_cooldown_until"_q,
			0);
	}
	return _trialsRefreshAt;
}

int Transcribes::trialsCount() {
	if (_trialsCount < 0) {
		_trialsCount = _session->appConfig().get<int>(
			u"transcribe_audio_trial_weekly_number"_q,
			-1);
		return std::max(_trialsCount, 0);
	}
	return _trialsCount;
}

crl::time Transcribes::trialsMaxLengthMs() const {
	return 1000 * _session->appConfig().get<int>(
		u"transcribe_audio_trial_duration_max"_q,
		300);
}

void Transcribes::toggle(not_null<HistoryItem*> item) {
	const auto id = item->fullId();
	auto i = _map.find(id);
	if (i == _map.end()) {
		load(item);
		_session->data().requestItemResize(item);
	} else if (!i->second.requestId) {
		const auto hiddenRound = i->second.roundview && !i->second.shown;
		if (i->second.cloudflare
			&& ((i->second.failed && (!i->second.roundview || hiddenRound))
				|| (hiddenRound && !cloudflareFor(item)))) {
			load(item);
			_session->data().requestItemResize(item);
			return;
		}
		i->second.shown = !i->second.shown;
		if (i->second.roundview) {
			_session->data().requestItemViewRefresh(item);
		}
		_session->data().requestItemResize(item);
	}
}

void Transcribes::toggleSummary(not_null<HistoryItem*> item) {
	const auto id = item->fullId();
	auto i = _summaries.find(id);
	if (i == _summaries.end()) {
		summarize(item);
	} else if (!i->second.loading) {
		auto &entry = i->second;
		if (entry.result.empty()) {
			summarize(item);
		} else {
			entry.shown = entry.premiumRequired ? false : !entry.shown;
			_session->data().requestItemResize(item);
			if (entry.shown) {
				_session->data().requestItemShowHighlight(item);
			}
		}
	}
}

const Transcribes::Entry &Transcribes::entry(
		not_null<HistoryItem*> item) const {
	static auto empty = Entry();
	const auto i = _map.find(item->fullId());
	return (i != _map.end()) ? i->second : empty;
}

const SummaryEntry &Transcribes::summary(
		not_null<const HistoryItem*> item) const {
	static const auto empty = SummaryEntry();
	const auto i = _summaries.find(item->fullId());
	return (i != _summaries.end()) ? i->second : empty;
}

void Transcribes::apply(const MTPDupdateTranscribedAudio &update) {
	const auto id = update.vtranscription_id().v;
	const auto i = _ids.find(id);
	if (i == _ids.end()) {
		return;
	}
	const auto j = _map.find(i->second);
	if (j == _map.end() || j->second.cloudflare) {
		return;
	}
	const auto text = qs(update.vtext());
	j->second.result = text;
	j->second.pending = update.is_pending();
	if (const auto item = _session->data().message(i->second)) {
		if (j->second.roundview) {
			_session->data().requestItemViewRefresh(item);
		}
		_session->data().requestItemResize(item);
	}
}

void Transcribes::load(not_null<HistoryItem*> item) {
	if (!item->isHistoryEntry() || item->isLocal()) {
		return;
	}
	const auto id = item->fullId();
	for (auto i = _ids.begin(); i != _ids.end();) {
		if (i->second == id) {
			i = _ids.erase(i);
		} else {
			++i;
		}
	}
	if (cloudflareFor(item)) {
		loadCloudflare(item);
		return;
	}
	if (const auto i = _cloudflareTasks.find(id); i != _cloudflareTasks.end()) {
		const auto task = i->second;
		_cloudflareTasks.erase(i);
		cancelCloudflare(task);
	}
	const auto toggleRound = [](not_null<HistoryItem*> item, Entry &entry) {
		if (const auto media = item->media()) {
			if (const auto document = media->document()) {
				if (document->isVideoMessage()) {
					entry.roundview = true;
					document->owner().requestItemViewRefresh(item);
				}
			}
		}
	};
	const auto requestId = _api.request(MTPmessages_TranscribeAudio(
		item->history()->peer->input(),
		MTP_int(item->id)
	)).done([=](const MTPmessages_TranscribedAudio &result) {
		const auto &data = result.data();

		{
			const auto trialsCountChanged = data.vtrial_remains_num()
				&& (_trialsCount != data.vtrial_remains_num()->v);
			if (trialsCountChanged) {
				_trialsCount = data.vtrial_remains_num()->v;
			}
			const auto refreshAtChanged = data.vtrial_remains_until_date()
				&& (_trialsRefreshAt != data.vtrial_remains_until_date()->v);
			if (refreshAtChanged) {
				_trialsRefreshAt = data.vtrial_remains_until_date()->v;
			}
			if (trialsCountChanged) {
				ShowTrialTranscribesToast(_trialsCount, _trialsRefreshAt);
			}
		}

		auto &entry = _map[id];
		entry.requestId = 0;
		entry.pending = data.is_pending();
		entry.result = qs(data.vtext());
		_ids.emplace(data.vtranscription_id().v, id);
		if (const auto item = _session->data().message(id)) {
			toggleRound(item, entry);
			_session->data().requestItemResize(item);
		}
	}).fail([=](const MTP::Error &error) {
		auto &entry = _map[id];
		entry.requestId = 0;
		entry.pending = false;
		entry.failed = true;
		if (error.type() == u"MSG_VOICE_TOO_LONG"_q) {
			entry.toolong = true;
		}
		if (const auto item = _session->data().message(id)) {
			toggleRound(item, entry);
			_session->data().requestItemResize(item);
		}
	}).send();
	auto &entry = _map[id];
	entry = Entry();
	entry.requestId = requestId;
	entry.shown = true;
	entry.failed = false;
	entry.pending = false;
}

struct Transcribes::CloudflareTask {
	FullMsgId id;
	DocumentData *document = nullptr;
	QString account;
	QString token;
	std::shared_ptr<Data::DocumentMedia> view;
	std::unique_ptr<Storage::StreamedFileDownloader> loader;
	QPointer<QNetworkReply> reply;
	rpl::lifetime lifetime;
};

Transcribes::~Transcribes() {
	_lifetime.destroy();
	auto tasks = std::move(_cloudflareTasks);
	_cloudflareTasks.clear();
	for (const auto &entry : tasks) {
		cancelCloudflare(entry.second);
	}
}

void Transcribes::cancelCloudflare(
		const std::shared_ptr<CloudflareTask> &task) {
	task->lifetime.destroy();
	task->loader = nullptr;
	task->view = nullptr;
	const auto reply = task->reply.data();
	task->reply = nullptr;
	if (reply) {
		QObject::disconnect(reply, nullptr, reply, nullptr);
		reply->abort();
		reply->deleteLater();
	}
}

void Transcribes::settingsChanged() {
	auto hiddenRounds = std::vector<FullMsgId>();
	for (auto &[id, entry] : _map) {
		if (!entry.cloudflare || !entry.roundview || !entry.shown) {
			continue;
		}
		const auto item = _session->data().message(id);
		if (item && !cloudflareFor(item)) {
			entry.shown = false;
			hiddenRounds.push_back(id);
		}
	}
	auto tasks = std::move(_cloudflareTasks);
	_cloudflareTasks.clear();
	for (const auto &entry : tasks) {
		_map.remove(entry.first);
	}
	for (const auto &entry : tasks) {
		cancelCloudflare(entry.second);
	}
	for (const auto &entry : tasks) {
		if (const auto item = _session->data().message(entry.first)) {
			if (entry.second->document->isVideoMessage()) {
				_session->data().requestItemViewRefresh(item);
			}
			_session->data().requestItemResize(item);
		}
	}
	for (const auto id : hiddenRounds) {
		if (const auto item = _session->data().message(id)) {
			_session->data().requestItemViewRefresh(item);
			_session->data().requestItemResize(item);
		}
	}
}

bool Transcribes::validCloudflare(
		const std::shared_ptr<CloudflareTask> &task) {
	const auto i = _cloudflareTasks.find(task->id);
	if (i == _cloudflareTasks.end() || i->second != task) {
		return false;
	}
	const auto item = _session->data().message(task->id);
	const auto media = item ? item->media() : nullptr;
	auto &local = _session->local();
	if (item && media && media->document() == task->document
		&& cloudflareFor(item)
		&& local.readPref<QString>(kCloudflareAccount).trimmed() == task->account
		&& local.readPref<QString>(kCloudflareToken).trimmed() == task->token) {
		return true;
	}
	_cloudflareTasks.remove(task->id);
	_map.remove(task->id);
	cancelCloudflare(task);
	if (item) {
		if (task->document->isVideoMessage()) {
			_session->data().requestItemViewRefresh(item);
		}
		_session->data().requestItemResize(item);
	}
	return false;
}

void Transcribes::loadCloudflare(not_null<HistoryItem*> item) {
	const auto id = item->fullId();
	auto &entry = _map[id];
	entry = Entry();
	entry.cloudflare = true;
	entry.shown = true;
	entry.requestId = -1;

	const auto document = item->media()->document();
	auto &local = _session->local();
	const auto task = std::make_shared<CloudflareTask>();
	task->id = id;
	task->document = document;
	task->account = local.readPref<QString>(kCloudflareAccount).trimmed();
	task->token = local.readPref<QString>(kCloudflareToken).trimmed();
	task->view = document->createMediaView();
	_cloudflareTasks[id] = task;
	static const auto kAccountPattern = QRegularExpression(
		u"\\A[a-zA-Z0-9]+\\z"_q);
	static const auto kTokenPattern = QRegularExpression(
		u"\\A[a-zA-Z0-9_-]+\\z"_q);
	if (!kAccountPattern.match(task->account).hasMatch()
		|| !kTokenPattern.match(task->token).hasMatch()
		|| document->size <= 0
		|| document->size > kCloudflareMaxAudio) {
		finishCloudflare(task, {}, true);
		return;
	}
	const auto weak = base::make_weak(this);
	const auto weakTask = std::weak_ptr<CloudflareTask>(task);
	QTimer::singleShot(kCloudflareTimeout, [=] {
		if (const auto strong = weak.get()) {
			if (const auto task = weakTask.lock()) {
				strong->finishCloudflare(task, {}, true);
			}
		}
	});
	if (task->view->loaded(true)) {
		readCloudflare(task);
		return;
	}
	if (document->size >= Storage::kMaxFileInMemory) {
		finishCloudflare(task, {}, true);
		return;
	}
	if (document->loading()) {
		_session->data().documentLoadProgress(
		) | rpl::filter([=](not_null<DocumentData*> changed) {
			return changed == document;
		}) | rpl::on_next([=] {
			crl::on_main(weak, [=] {
				if (const auto task = weakTask.lock()) {
					if (!validCloudflare(task) || document->loading()) {
						return;
					} else if (task->view->loaded(true)) {
						task->lifetime.destroy();
						readCloudflare(task);
					} else {
						finishCloudflare(task, {}, true);
					}
				}
			});
		}, task->lifetime);
		document->permitLoadFromCloud();
		return;
	}
	const auto origin = Data::FileOrigin(id);
	auto loader = document->createStreamingLoader(origin, true);
	if (!loader) {
		finishCloudflare(task, {}, true);
		return;
	}
	auto reader = std::make_shared<Media::Streaming::Reader>(
		std::move(loader),
		&document->owner().cacheBigFile());
	task->loader = document->createStreamedDownloader(
		std::move(reader),
		origin,
		std::nullopt,
		QString(),
		LoadToCacheAsWell,
		LoadFromCloudOrLocal,
		false);
	task->loader->updates(
	) | rpl::on_next_error_done([] {
	}, [=](FileLoader::Error) {
		crl::on_main(weak, [=] {
			if (const auto task = weakTask.lock()) {
				finishCloudflare(task, {}, true);
			}
		});
	}, [=] {
		crl::on_main(weak, [=] {
			if (const auto task = weakTask.lock(); task && validCloudflare(task)) {
				task->view->setBytes(task->loader->bytes());
				readCloudflare(task);
			}
		});
	}, task->lifetime);
	task->loader->start();
}

void Transcribes::readCloudflare(const std::shared_ptr<CloudflareTask> &task) {
	if (!validCloudflare(task)) {
		return;
	}
	task->lifetime.destroy();
	task->loader = nullptr;
	const auto expectedSize = task->document->size;
	const auto isVideo = task->document->isVideoMessage();
	const auto bytes = task->view->bytes();
	const auto location = task->document->location(true);
	const auto weakTask = std::weak_ptr<CloudflareTask>(task);
	crl::async([
		=,
		weak = base::make_weak(this)
	]() mutable {
		auto audio = bytes;
		const auto access = audio.isEmpty() && location.accessEnable();
		if (access) {
			auto file = QFile(location.name());
			if (file.open(QIODevice::ReadOnly)
				&& file.size() > 0
				&& file.size() <= kCloudflareMaxAudio) {
				audio = file.read(kCloudflareMaxAudio + 1);
				if (file.error() != QFile::NoError) {
					audio.clear();
				}
			}
		}
		if (access) {
			location.accessDisable();
		}
		if (audio.isEmpty()
			|| audio.size() > kCloudflareMaxAudio
			|| audio.size() != expectedSize) {
			audio.clear();
		} else if (isVideo) {
			audio = Media::Audio::ExtractVideoAudio(audio);
		}
		auto body = QByteArray();
		if (!audio.isEmpty() && audio.size() <= kCloudflareMaxAudio) {
			body = QJsonDocument(QJsonObject{
				{ u"audio"_q, QString::fromLatin1(audio.toBase64()) },
				{ u"vad_filter"_q, false },
			}).toJson(QJsonDocument::Compact);
		}
		crl::on_main(weak, [=, body = std::move(body)] {
			const auto task = weakTask.lock();
			if (!task || !validCloudflare(task)) {
				return;
			} else if (body.isEmpty()) {
				finishCloudflare(task, {}, true);
				return;
			}
			if (!_cloudflareNetwork) {
				_cloudflareNetwork = std::make_unique<QNetworkAccessManager>();
			}
			const auto url = QUrl(
				u"https://api.cloudflare.com/client/v4/accounts/"_q
				+ task->account
				+ u"/ai/run/@cf/openai/whisper-large-v3-turbo"_q);
			auto request = QNetworkRequest(url);
			request.setAttribute(
				QNetworkRequest::RedirectPolicyAttribute,
				QNetworkRequest::ManualRedirectPolicy);
			request.setHeader(
				QNetworkRequest::ContentTypeHeader,
				u"application/json"_q);
			request.setRawHeader("Authorization", "Bearer " + task->token.toUtf8());
			const auto reply = _cloudflareNetwork->post(request, body);
			task->reply = reply;
			reply->setReadBufferSize(kCloudflareMaxResponse + 1);
			const auto response = std::make_shared<QByteArray>();
			QObject::connect(reply, &QNetworkReply::readyRead, reply, crl::guard(
				weak, [=] {
				const auto task = weakTask.lock();
				if (!task || !validCloudflare(task)) {
					return;
				}
				response->append(reply->read(
					kCloudflareMaxResponse + 1 - response->size()));
				if (response->size() > kCloudflareMaxResponse) {
					reply->abort();
				}
			}));
			QObject::connect(reply, &QNetworkReply::finished, reply, crl::guard(
				weak, [=] {
				if (response->size() <= kCloudflareMaxResponse) {
					response->append(reply->read(
						kCloudflareMaxResponse + 1 - response->size()));
				}
				const auto status = reply->attribute(
					QNetworkRequest::HttpStatusCodeAttribute).toInt();
				const auto json = QJsonDocument::fromJson(*response).object();
				const auto text = json.value(u"result"_q).toObject().value(
					u"text"_q);
				const auto failed = reply->error() != QNetworkReply::NoError
					|| status < 200 || status >= 300
					|| response->size() > kCloudflareMaxResponse
					|| !json.value(u"success"_q).toBool()
					|| !text.isString()
					|| text.toString().trimmed().isEmpty();
				if (const auto task = weakTask.lock()) {
					task->reply = nullptr;
					finishCloudflare(
						task,
						failed ? QString() : text.toString(),
						failed);
				}
				reply->deleteLater();
			}));
			QTimer::singleShot(kCloudflareTimeout, reply, [=] {
				reply->abort();
			});
		});
	});
}

void Transcribes::finishCloudflare(
		const std::shared_ptr<CloudflareTask> &task,
		const QString &result,
		bool failed) {
	if (!validCloudflare(task)) {
		return;
	}
	_cloudflareTasks.remove(task->id);
	auto &entry = _map[task->id];
	entry.requestId = 0;
	entry.pending = false;
	entry.failed = failed;
	entry.result = result;
	cancelCloudflare(task);
	if (const auto item = _session->data().message(task->id)) {
		if (task->document->isVideoMessage()) {
			entry.roundview = true;
			_session->data().requestItemViewRefresh(item);
		}
		_session->data().requestItemResize(item);
	}
}

void Transcribes::summarize(not_null<HistoryItem*> item) {
	if (!item->isHistoryEntry() || item->isLocal()) {
		return;
	}

	const auto id = item->fullId();
	const auto translatedTo = item->history()->translatedTo();
	const auto langCode = translatedTo
		? translatedTo.twoLetterCode()
		: QString();
	const auto requestId = _api.request(MTPmessages_SummarizeText(
		langCode.isEmpty()
			? MTP_flags(0)
			: MTP_flags(MTPmessages_summarizeText::Flag::f_to_lang),
		item->history()->peer->input(),
		MTP_int(item->id),
		langCode.isEmpty() ? MTPstring() : MTP_string(langCode),
		MTPstring() // tone
	)).done([=](const MTPTextWithEntities &result) {
		const auto &data = result.data();
		auto &entry = _summaries[id];
		entry.requestId = 0;
		entry.loading = false;
		entry.premiumRequired = false;
		entry.languageId = translatedTo;
		entry.result = TextWithEntities(
			qs(data.vtext()),
			Api::EntitiesFromMTP(_session, data.ventities().v));
		if (const auto item = _session->data().message(id)) {
			_session->data().requestItemTextRefresh(item);
			_session->data().requestItemShowHighlight(item);
		}
	}).fail([=](const MTP::Error &error) {
		auto &entry = _summaries[id];
		if (error.type() == u"SUMMARY_FLOOD_PREMIUM"_q) {
			entry.premiumRequired = true;
		}
		entry.requestId = 0;
		entry.shown = false;
		entry.loading = false;
		if (const auto item = _session->data().message(id)) {
			_session->data().requestItemTextRefresh(item);
		}
	}).send();

	auto &entry = _summaries.emplace(id).first->second;
	entry.requestId = requestId;
	entry.shown = true;
	entry.loading = true;

	item->setHasSummaryEntry();
	_session->data().requestItemResize(item);
}

void Transcribes::checkSummaryToTranslate(FullMsgId id) {
	const auto i = _summaries.find(id);
	if (i == _summaries.end() || i->second.result.empty()) {
		return;
	}
	const auto item = _session->data().message(id);
	if (!item) {
		return;
	}
	const auto translatedTo = item->history()->translatedTo();
	if (i->second.languageId != translatedTo) {
		i->second.result = tr::lng_contacts_loading(tr::now, tr::italic);
		summarize(item);
	}
}

} // namespace Api
