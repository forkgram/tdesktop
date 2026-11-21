/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"
#include "mtproto/sender.h"
#include "spellcheck/spellcheck_types.h"

class ApiWrap;
class DocumentData;
class QNetworkAccessManager;

namespace Main {
class Session;
} // namespace Main

namespace Api {

struct SummaryEntry {
	TextWithEntities result;
	LanguageId languageId;
	bool shown = false;
	bool loading = false;
	bool premiumRequired = false;
	mtpRequestId requestId = 0;
};

class Transcribes final : public base::has_weak_ptr {
public:
	explicit Transcribes(not_null<ApiWrap*> api);
	~Transcribes();

	struct Entry {
		QString result;
		bool shown = false;
		bool failed = false;
		bool toolong = false;
		bool pending = false;
		bool roundview = false;
		bool cloudflare = false;
		mtpRequestId requestId = 0;
	};

	void toggle(not_null<HistoryItem*> item);
	void settingsChanged();
	[[nodiscard]] const Entry &entry(not_null<HistoryItem*> item) const;

	void toggleSummary(not_null<HistoryItem*> item);
	[[nodiscard]] const SummaryEntry &summary(
		not_null<const HistoryItem*> item) const;
	void checkSummaryToTranslate(FullMsgId id);

	void apply(const MTPDupdateTranscribedAudio &update);

	[[nodiscard]] bool cloudflareFor(not_null<HistoryItem*> item) const;
	[[nodiscard]] bool freeFor(not_null<HistoryItem*> item) const;
	[[nodiscard]] bool isRated(not_null<HistoryItem*> item) const;
	void rate(not_null<HistoryItem*> item, bool isGood);

	[[nodiscard]] bool trialsSupport();
	[[nodiscard]] TimeId trialsRefreshAt();
	[[nodiscard]] int trialsCount();
	[[nodiscard]] crl::time trialsMaxLengthMs() const;

private:
	void load(not_null<HistoryItem*> item);
	void loadCloudflare(not_null<HistoryItem*> item);
	struct CloudflareTask;
	void cancelCloudflare(const std::shared_ptr<CloudflareTask> &task);
	[[nodiscard]] bool validCloudflare(
		const std::shared_ptr<CloudflareTask> &task);
	void readCloudflare(const std::shared_ptr<CloudflareTask> &task);
	void finishCloudflare(
		const std::shared_ptr<CloudflareTask> &task,
		const QString &result,
		bool failed);
	void summarize(not_null<HistoryItem*> item);

	const not_null<Main::Session*> _session;
	MTP::Sender _api;

	int _trialsCount = -1;
	std::optional<bool> _trialsSupport;
	TimeId _trialsRefreshAt = -1;

	base::flat_map<FullMsgId, Entry> _map;
	base::flat_map<uint64, FullMsgId> _ids;

	base::flat_map<FullMsgId, SummaryEntry> _summaries;
	std::unique_ptr<QNetworkAccessManager> _cloudflareNetwork;
	base::flat_map<FullMsgId, std::shared_ptr<CloudflareTask>> _cloudflareTasks;
	rpl::lifetime _lifetime;

};

} // namespace Api
