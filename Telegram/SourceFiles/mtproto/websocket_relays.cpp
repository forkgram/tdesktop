/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/websocket_relays.h"

#include "base/const_string.h"
#include "base/invoke_queued.h"
#include "base/options.h"
#include "base/random.h"

#include <QtCore/QCoreApplication>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace MTP {
namespace {

base::options::toggle OptionWebSocketTransport({
	.id = kOptionWebSocketTransport,
	.name = "Connect through a WebSocket relay",
	.description = "Tunnel MTProto inside ordinary HTTPS to a"
		" Cloudflare-fronted relay, which can get through where a direct"
		" connection is blocked.",
});

base::options::option<QString> OptionWebSocketDomain({
	.id = kOptionWebSocketDomain,
	.name = "WebSocket relay domain",
	.description = "A Cloudflare worker of your own. Empty means the"
		" automatic public relay list.",
});

base::options::toggle OptionWebSocketMediaMultiplexing({
	.id = kOptionWebSocketMediaMultiplexing,
	.name = "Media multiplexing (HTTP/2)",
	.description = "Send media through one HTTP/2 connection to the relay"
		" instead of a separate WebSocket for every media connection.",
	.defaultValue = true,
});

constexpr auto kCooldownBase = crl::time(45 * 1000);
constexpr auto kCooldownMax = crl::time(300 * 1000);
constexpr auto kFailuresBeforeSuppress = 6;
constexpr auto kSuppressDuration = crl::time(60 * 1000);
constexpr auto kFetchTimeout = crl::time(8 * 1000);
constexpr auto kFetchTtl = crl::time(12 * 60 * 60 * 1000);
constexpr auto kForcedFetchInterval = crl::time(5 * 60 * 1000);
constexpr auto kMinFetchedDomains = 3;

const auto kDirectDomain = u"web.telegram.org"_q;

const auto kMirrors = std::array{
	"https://raw.githubusercontent.com/Flowseal/tg-ws-proxy/main/.github/cfproxy-domains.txt"_cs,
	"https://gcore.jsdelivr.net/gh/Flowseal/tg-ws-proxy@main/.github/cfproxy-domains.txt"_cs,
	"https://fastly.jsdelivr.net/gh/Flowseal/tg-ws-proxy@main/.github/cfproxy-domains.txt"_cs,
	"https://cdn.jsdelivr.net/gh/Flowseal/tg-ws-proxy@main/.github/cfproxy-domains.txt"_cs,
	"https://raw.githack.com/Flowseal/tg-ws-proxy/main/.github/cfproxy-domains.txt"_cs,
};

const auto kFallbackDomains = std::array{
	"virkgj.com"_cs,
	"vmmzovy.com"_cs,
	"mkuosckvso.com"_cs,
	"zaewayzmplad.com"_cs,
	"twdmbzcm.com"_cs,
	"awzwsldi.com"_cs,
	"clngqrflngqin.com"_cs,
	"tjacxbqtj.com"_cs,
	"bxaxtxmrw.com"_cs,
	"dmohrsgmohcrwb.com"_cs,
	"vwbmtmoi.com"_cs,
	"khgrre.com"_cs,
	"ulihssf.com"_cs,
	"tmhqsdqmfpmk.com"_cs,
	"xwuwoqbm.com"_cs,
	"orgcnunpj.com"_cs,
	"zhkuldz.com"_cs,
	"zypoljnslxa.com"_cs,
	"efabnxaowuzs.com"_cs,
	"zaftuzsftqdq.com"_cs,
};

[[nodiscard]] crl::time EscalatedCooldown(int strikes) {
	auto cooldown = kCooldownBase;
	for (auto i = 1; i != strikes && cooldown < kCooldownMax; ++i) {
		cooldown *= 2;
	}
	return std::min(cooldown, kCooldownMax);
}

struct ParsedHost {
	int dc = 0;
	bool dash1 = false;
	QString domain;
};

// Hosts are built by routeForDc as kws<dc>[-1].<domain>.
[[nodiscard]] ParsedHost ParseHost(const QString &host) {
	if (!host.startsWith(u"kws"_q)) {
		return {};
	}
	auto index = 3;
	const auto size = int(host.size());
	auto dc = 0;
	while (index < size && host.at(index).isDigit()) {
		dc = dc * 10 + (host.at(index).unicode() - u'0');
		++index;
	}
	if (!dc) {
		return {};
	}
	auto result = ParsedHost();
	result.dc = dc;
	if (host.mid(index, 2) == u"-1"_q) {
		result.dash1 = true;
		index += 2;
	}
	if (index >= size || host.at(index) != u'.') {
		return {};
	}
	result.domain = host.mid(index + 1);
	return result;
}

[[nodiscard]] std::vector<QString> DecodeList(const QByteArray &content) {
	auto result = std::vector<QString>();
	const auto lines = QString::fromUtf8(content).split(
		'\n',
		Qt::SkipEmptyParts);
	for (const auto &line : lines) {
		const auto decoded = DecodeRelayDomain(line);
		if (!decoded.isEmpty()) {
			result.push_back(decoded);
		}
	}
	return result;
}

} // namespace

const char kOptionWebSocketTransport[] = "websocket-transport";
const char kOptionWebSocketDomain[] = "websocket-relay-domain";
const char kOptionWebSocketMediaMultiplexing[] = "websocket-media-multiplexing";

QString DecodeRelayDomain(const QString &encoded) {
	const auto value = encoded.trimmed().toLower();
	if (value.endsWith(u".co.uk"_q)) {
		return value;
	} else if (!value.endsWith(u".com"_q)) {
		return QString();
	}
	const auto body = value.chopped(4);
	auto letters = 0;
	for (const auto ch : body) {
		if (ch >= u'a' && ch <= u'z') {
			++letters;
		}
	}
	const auto shift = letters % 26;
	auto result = QString();
	result.reserve(body.size() + 6);
	for (const auto ch : body) {
		if (ch >= u'a' && ch <= u'z') {
			const auto index = ch.unicode() - u'a';
			result.append(QChar(u'a' + ((index - shift + 26) % 26)));
		} else {
			result.append(ch);
		}
	}
	return result.isEmpty() ? QString() : (result + u".co.uk"_q);
}

WebSocketRelays::WebSocketRelays()
// Instance() may well be first reached from a session thread, while the
// timer is only ever used from the main one, so bind it explicitly.
: _timeoutTimer(QCoreApplication::instance()->thread()) {
}

WebSocketRelays &WebSocketRelays::Instance() {
	static auto result = WebSocketRelays();
	return result;
}

void WebSocketRelays::applyConfig() {
	const auto enabled = OptionWebSocketTransport.value();
	const auto userDomain = OptionWebSocketDomain.value().trimmed().toLower();
	const auto multiplexing = OptionWebSocketMediaMultiplexing.value();

	auto locker = QMutexLocker(&_mutex);
	const auto changed = (_enabled != enabled) || (_userDomain != userDomain);
	_enabled = enabled;
	_userDomain = userDomain;
	_mediaMultiplexing = multiplexing;
	if (changed) {
		rebuildPool();

		_consecutiveFailures = 0;
		_suppressedUntil = 0;
	}
}

void WebSocketRelays::rebuildPool() {
	auto pool = DecodeList(_fetchedList);
	for (const auto &entry : kFallbackDomains) {
		const auto decoded = DecodeRelayDomain(entry.utf16());
		if (!decoded.isEmpty() && !ranges::contains(pool, decoded)) {
			pool.push_back(decoded);
		}
	}
	_pool = std::move(pool);
	_dcPick.clear();
	_states.clear();
	_direct.clear();
	_dash1.clear();
	_rotateIndex = 0;
}

bool WebSocketRelays::enabled() const {
	auto locker = QMutexLocker(&_mutex);
	return _enabled;
}

bool WebSocketRelays::isUserDomain(const QString &domain) const {
	auto locker = QMutexLocker(&_mutex);
	return !domain.isEmpty() && (domain == _userDomain);
}

bool WebSocketRelays::suppressedLocked() {
	if (!_suppressedUntil) {
		return false;
	} else if (crl::now() < _suppressedUntil) {
		return true;
	}
	_suppressedUntil = 0;
	return false;
}

bool WebSocketRelays::directCooledLocked(int slot) const {
	const auto i = _direct.find(slot);
	return (i != _direct.end()) && (crl::now() < i->second.cooldownUntil);
}

bool WebSocketRelays::dash1CooledLocked(int bareDcId) const {
	const auto i = _dash1.find(bareDcId);
	return (i != _dash1.end()) && (crl::now() < i->second.cooldownUntil);
}

QString WebSocketRelays::soonestDomainLocked() {
	if (_pool.empty()) {
		return QString();
	}
	auto best = _pool.front();
	auto bestUntil = std::numeric_limits<crl::time>::max();
	for (const auto &domain : _pool) {
		const auto i = _states.find(domain);
		const auto until = (i != _states.end())
			? i->second.cooldownUntil
			: crl::time(0);
		if (until < bestUntil) {
			bestUntil = until;
			best = domain;
		}
	}
	return best;
}

RelayRoute WebSocketRelays::routeForDc(
		int bareDcId,
		bool media,
		bool testMode) {
	auto locker = QMutexLocker(&_mutex);
	if (!_enabled || suppressedLocked()) {
		return {};
	}
	const auto domainKind = (media && _mediaMultiplexing && !testMode)
		? RelayRoute::Kind::Http
		: RelayRoute::Kind::WebSocket;
	const auto pick = [&](
			const QString &domain,
			bool dash1,
			RelayRoute::Kind kind) {
		auto result = RelayRoute();
		result.kind = kind;
		result.host = u"kws%1%2.%3"_q
			.arg(bareDcId)
			.arg(dash1 ? u"-1"_q : QString())
			.arg(domain);
		result.path = (kind == RelayRoute::Kind::Http)
			? u"/api"_q
			: (testMode ? u"/apiws_test"_q : u"/apiws"_q);
		return result;
	};

	if (!_userDomain.isEmpty()) {
		return pick(_userDomain, false, domainKind);
	}
	const auto now = crl::now();
	const auto slot = bareDcId * 2 + (media ? 1 : 0);
	// The endpoint of Telegram itself is not rate limited by Cloudflare, so
	// it goes first while it works; a failure cools down this slot only.
	if (!directCooledLocked(slot)) {
		return pick(
			kDirectDomain,
			media && !dash1CooledLocked(bareDcId),
			RelayRoute::Kind::WebSocket);
	}
	const auto cooled = [&](const QString &domain) {
		const auto i = _states.find(domain);
		return (i != _states.end()) && (now < i->second.cooldownUntil);
	};
	// Main and media connections of one datacenter share a single domain.
	const auto current = _dcPick.find(bareDcId);
	if (current != _dcPick.end() && !cooled(current->second)) {
		return pick(current->second, false, domainKind);
	}
	for (auto j = 0; j != int(_pool.size()); ++j) {
		const auto &domain = _pool[(_rotateIndex + j) % _pool.size()];
		if (!cooled(domain)) {
			_rotateIndex = (_rotateIndex + j + 1) % _pool.size();
			_dcPick[bareDcId] = domain;
			return pick(domain, false, domainKind);
		}
	}
	if (_pool.empty()) {
		return {};
	}
	const auto picked = soonestDomainLocked();
	_dcPick[bareDcId] = picked;
	return pick(picked, false, domainKind);
}

void WebSocketRelays::markDirectFailureLocked(
		int slot,
		int bareDcId,
		bool dash1) {
	const auto now = crl::now();
	if (dash1) {
		auto &state = _dash1[bareDcId];
		++state.strikes;
		state.cooldownUntil = now + EscalatedCooldown(state.strikes);
		return;
	}
	auto &state = _direct[slot];
	++state.strikes;
	state.cooldownUntil = now + EscalatedCooldown(state.strikes);
	_dash1.remove(bareDcId);
}

void WebSocketRelays::markDomainFailureLocked(const QString &domain) {
	auto &state = _states[domain];
	++state.strikes;
	state.cooldownUntil = crl::now() + EscalatedCooldown(state.strikes);
	for (auto i = _dcPick.begin(); i != _dcPick.end();) {
		if (i->second == domain) {
			i = _dcPick.erase(i);
		} else {
			++i;
		}
	}
}

void WebSocketRelays::markResult(
		const QString &host,
		bool media,
		bool success) {
	auto locker = QMutexLocker(&_mutex);
	const auto parsed = ParseHost(host);
	if (!parsed.dc) {
		return;
	}
	const auto direct = (parsed.domain == kDirectDomain);
	const auto slot = parsed.dc * 2 + ((media || parsed.dash1) ? 1 : 0);
	if (success) {
		_consecutiveFailures = 0;
		_suppressedUntil = 0;
		if (direct) {
			_direct.remove(slot);
			_dash1.remove(parsed.dc);
		} else if (!parsed.domain.isEmpty()) {
			_states.remove(parsed.domain);
		}
		return;
	}
	if (direct) {
		markDirectFailureLocked(slot, parsed.dc, parsed.dash1);
	} else if (!parsed.domain.isEmpty()) {
		markDomainFailureLocked(parsed.domain);
	}
	if (++_consecutiveFailures >= kFailuresBeforeSuppress) {
		_suppressedUntil = crl::now() + kSuppressDuration;
		if (_userDomain.isEmpty()) {
			// The whole pool keeps failing, it has likely rotated.
			InvokeQueued(QCoreApplication::instance(), [] {
				Instance().refresh(true);
			});
		}
	}
}

void WebSocketRelays::refresh(bool force) {
	if (_fetching) {
		return;
	} else if (force) {
		if (_lastFetchStartedAt
			&& (crl::now() - _lastFetchStartedAt) < kForcedFetchInterval) {
			return;
		}
	} else if (!_fetchedList.isEmpty()
		&& (crl::now() - _fetchedAt) < kFetchTtl) {
		return;
	}
	{
		auto locker = QMutexLocker(&_mutex);
		// A domain of the user's own replaces the public pool entirely.
		if (!_enabled || !_userDomain.isEmpty()) {
			return;
		}
	}
	if (!_manager) {
		_manager = std::make_unique<QNetworkAccessManager>();
	}
	_fetching = true;
	_lastFetchStartedAt = crl::now();
	_mirrorIndex = 0;
	_timeoutTimer.setCallback([=] { fetchFailed(); });
	fetchNext();
}

void WebSocketRelays::fetchNext() {
	if (_mirrorIndex >= int(kMirrors.size())) {
		_fetching = false;
		_timeoutTimer.cancel();
		return;
	}
	const auto url = QUrl(
		QString(kMirrors[_mirrorIndex++].utf16())
		+ u"?%1"_q.arg(base::RandomValue<quint32>()));
	auto request = QNetworkRequest(url);
	request.setHeader(
		QNetworkRequest::UserAgentHeader,
		u"tg-ws-proxy"_q);
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	_reply = _manager->get(request);
	const auto reply = _reply;
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		if (_reply != reply) {
			return;
		}
		_reply = nullptr;
		_timeoutTimer.cancel();
		const auto content = (reply->error() == QNetworkReply::NoError)
			? reply->readAll()
			: QByteArray();
		reply->deleteLater();
		if (!applyFetched(content)) {
			fetchNext();
		} else {
			_fetching = false;
		}
	});
	_timeoutTimer.callOnce(kFetchTimeout);
}

void WebSocketRelays::fetchFailed() {
	if (const auto reply = base::take(_reply)) {
		reply->abort();
		reply->deleteLater();
	}
	fetchNext();
}

bool WebSocketRelays::applyFetched(const QByteArray &content) {
	// A truncated or hijacked response must not replace a working pool.
	if (DecodeList(content).size() < kMinFetchedDomains) {
		return false;
	}
	_fetchedList = content;
	_fetchedAt = crl::now();

	auto locker = QMutexLocker(&_mutex);
	const auto was = _pool;
	rebuildPool();
	if (_pool != was) {
		// An unchanged list must not lift the suppression, or the client
		// would hammer the same dead pool right away.
		_consecutiveFailures = 0;
		_suppressedUntil = 0;
	}
	return true;
}

} // namespace MTP
