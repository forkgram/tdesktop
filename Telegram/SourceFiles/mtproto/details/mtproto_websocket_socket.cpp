/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/details/mtproto_websocket_socket.h"

#include "base/invoke_queued.h"
#include "mtproto/websocket_relays.h"

#include <QtCore/QCryptographicHash>

namespace MTP::details {
namespace {

constexpr auto kOpcodeContinuation = quint8(0x0);
constexpr auto kOpcodeText = quint8(0x1);
constexpr auto kOpcodeBinary = quint8(0x2);
constexpr auto kOpcodeClose = quint8(0x8);
constexpr auto kOpcodePing = quint8(0x9);
constexpr auto kOpcodePong = quint8(0xA);

constexpr auto kMaxFramePayload = quint64(16 * 1024 * 1024);
constexpr auto kMaxUpgradeResponse = 8 * 1024;

const auto kAcceptGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

const auto kOrigin = "https://web.telegram.org";

} // namespace

WebSocketSocket::WebSocketSocket(
	not_null<QThread*> thread,
	const QNetworkProxy &proxy,
	bool protocolForFiles,
	const QString &path)
: AbstractSocket(thread)
, _path(path) {
	_socket.moveToThread(thread);
	_socket.setProxy(proxy);
	if (protocolForFiles) {
		_socket.setSocketOption(
			QAbstractSocket::SendBufferSizeSocketOption,
			kFilesSendBufferSize);
		_socket.setSocketOption(
			QAbstractSocket::ReceiveBufferSizeSocketOption,
			kFilesReceiveBufferSize);
	}
	const auto wrap = [&](auto handler) {
		return [=](auto &&...args) {
			InvokeQueued(this, [=] { handler(args...); });
		};
	};
	using Error = QAbstractSocket::SocketError;
	connect(
		&_socket,
		&QSslSocket::encrypted,
		wrap([=] { sslConnected(); }));
	connect(
		&_socket,
		&QSslSocket::disconnected,
		wrap([=] { plainDisconnected(); }));
	connect(
		&_socket,
		&QSslSocket::readyRead,
		wrap([=] { plainReadyRead(); }));
	connect(
		&_socket,
		&QAbstractSocket::errorOccurred,
		wrap([=](Error e) { handleError(e); }));

	connect(
		&_socket,
		QOverload<const QList<QSslError>&>::of(&QSslSocket::sslErrors),
		[=](const QList<QSslError>&) {
			if (!_verifyCertificate) {
				_socket.ignoreSslErrors();
			}
		});
}

WebSocketSocket::~WebSocketSocket() {
	// Connected, but no packet ever passed the msg_key check - fed garbage.
	if (_state == State::Connected) {
		reportResult(false);
	}
}

void WebSocketSocket::connectToHost(const QString &address, int port) {
	Expects(_state == State::NotConnected);

	_host = address;

	_domain = address.section('.', 1);

	// Custom workers may sit on deep subdomains their certificate can't cover.
	_verifyCertificate = !WebSocketRelays::Instance().isUserDomain(_domain);
	if (!_verifyCertificate) {
		_socket.setPeerVerifyMode(QSslSocket::VerifyNone);
	}
	_state = State::Connecting;
	_socket.connectToHostEncrypted(address, port);
}

void WebSocketSocket::sslConnected() {
	if (_state != State::Connecting) {
		return;
	}
	_state = State::WaitingUpgrade;
	sendUpgradeRequest();
}

void WebSocketSocket::sendUpgradeRequest() {
	auto keyBytes = bytes::vector(16);
	bytes::set_random(keyBytes);
	_key = QByteArray(
		reinterpret_cast<const char*>(keyBytes.data()),
		int(keyBytes.size())
	).toBase64();

	// Without the "binary" subprotocol the upgrade is answered 404, not 101.
	const auto request = u"GET %1 HTTP/1.1\r\n"
		"Host: %2\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: %3\r\n"
		"Sec-WebSocket-Version: 13\r\n"
		"Sec-WebSocket-Protocol: binary\r\n"
		"Origin: %4\r\n"
		"\r\n"_q
		.arg(_path)
		.arg(_host)
		.arg(QString::fromLatin1(_key))
		.arg(QString::fromLatin1(kOrigin));
	_socket.write(request.toLatin1());
}

bool WebSocketSocket::readUpgradeResponse() {
	const auto end = _incoming.indexOf("\r\n\r\n");
	if (end < 0) {
		return (_incoming.size() < kMaxUpgradeResponse);
	}
	const auto head = _incoming.left(end);
	const auto lines = head.split('\n');
	const auto status = lines.isEmpty()
		? QByteArray()
		: lines.front().trimmed();
	if (!status.contains(" 101")) {
		logError(888, u"Bad WebSocket upgrade status: %1."_q.arg(
			QString::fromLatin1(status)));
		return false;
	}
	const auto expected = QCryptographicHash::hash(
		_key + kAcceptGuid,
		QCryptographicHash::Sha1).toBase64();
	auto accepted = false;
	for (const auto &line : lines) {
		const auto colon = line.indexOf(':');
		if (colon < 0) {
			continue;
		} else if (line.left(colon).trimmed().toLower()
			!= "sec-websocket-accept") {
			continue;
		}
		accepted = (line.mid(colon + 1).trimmed() == expected);
		break;
	}
	if (!accepted) {
		logError(888, u"Bad WebSocket accept key."_q);
		return false;
	}
	_incoming.remove(0, end + 4);
	_state = State::Connected;
	_connected.fire({});
	return true;
}

void WebSocketSocket::plainReadyRead() {
	if (_state == State::WaitingUpgrade) {
		_incoming.append(_socket.readAll());
		if (!readUpgradeResponse()) {
			handleError();
			return;
		} else if (_state != State::Connected) {
			return;
		}
	} else if (_state == State::Connected) {
		_incoming.append(_socket.readAll());
	} else {
		return;
	}
	if (!parseFrames()) {
		handleError();
	} else if (hasBytesAvailable()) {
		_readyRead.fire({});
	}
}

bool WebSocketSocket::parseFrames() {
	auto offset = 0;
	const auto size = _incoming.size();
	const auto data = reinterpret_cast<const uchar*>(_incoming.constData());
	while (true) {
		if (size - offset < 2) {
			break;
		}
		const auto first = data[offset];
		const auto second = data[offset + 1];
		const auto opcode = quint8(first & 0x0F);
		const auto masked = ((second & 0x80) != 0);
		auto length = quint64(second & 0x7F);
		auto header = 2;
		if (length == 126) {
			if (size - offset < 4) {
				break;
			}
			length = (quint64(data[offset + 2]) << 8)
				| quint64(data[offset + 3]);
			header = 4;
		} else if (length == 127) {
			if (size - offset < 10) {
				break;
			}
			length = 0;
			for (auto i = 0; i != 8; ++i) {
				length = (length << 8) | quint64(data[offset + 2 + i]);
			}
			header = 10;
		}
		if (length > kMaxFramePayload) {
			logError(888, u"Too large WebSocket frame."_q);
			return false;
		}
		if (masked) {
			header += 4;
		}
		if (quint64(size - offset) < quint64(header) + length) {
			break;
		}
		auto payload = QByteArray(
			_incoming.constData() + offset + header,
			int(length));
		if (masked) {
			const auto mask = data + offset + header - 4;
			for (auto i = 0; i != int(length); ++i) {
				payload[i] = char(payload[i] ^ char(mask[i % 4]));
			}
		}
		offset += header + int(length);

		switch (opcode) {
		case kOpcodeContinuation:
		case kOpcodeBinary:
			if (!payload.isEmpty()) {
				if (_readOffset > 0) {
					_readBuffer.remove(0, base::take(_readOffset));
				}
				_readBuffer.append(payload);
			}
			break;
		case kOpcodeClose:
			logError(888, u"WebSocket closed by the relay."_q);
			return false;
		case kOpcodePing:
			writeFrame(bytes::make_span(payload), kOpcodePong);
			break;
		case kOpcodePong:
		case kOpcodeText:
			break;
		default:
			logError(888, u"Bad WebSocket opcode %1."_q.arg(opcode));
			return false;
		}
	}
	if (offset > 0) {
		_incoming.remove(0, offset);
	}
	return true;
}

void WebSocketSocket::writeFrame(bytes::const_span payload, quint8 opcode) {
	const auto length = quint64(payload.size());
	auto frame = QByteArray();
	frame.reserve(int(length) + 14);
	frame.append(char(0x80 | opcode));
	if (length < 126) {
		frame.append(char(0x80 | char(length)));
	} else if (length <= 0xFFFF) {
		frame.append(char(0x80 | 126));
		frame.append(char((length >> 8) & 0xFF));
		frame.append(char(length & 0xFF));
	} else {
		frame.append(char(0x80 | 127));
		for (auto i = 7; i >= 0; --i) {
			frame.append(char((length >> (i * 8)) & 0xFF));
		}
	}
	auto maskBytes = bytes::vector(4);
	bytes::set_random(maskBytes);
	const auto mask = reinterpret_cast<const char*>(maskBytes.data());
	frame.append(mask, 4);

	const auto from = reinterpret_cast<const char*>(payload.data());
	for (auto i = 0; i != int(length); ++i) {
		frame.append(char(from[i] ^ mask[i % 4]));
	}
	_socket.write(frame);
}

void WebSocketSocket::write(
		bytes::const_span prefix,
		bytes::const_span buffer) {
	Expects(!buffer.empty());

	if (!isConnected()) {
		return;
	}

	// One message per packet, never coalesced - the relay closes otherwise.
	if (!prefix.empty()) {
		writeFrame(prefix, kOpcodeBinary);
	}
	writeFrame(buffer, kOpcodeBinary);
}

void WebSocketSocket::reportResult(bool success) {
	if (_resultReported) {
		return;
	}
	_resultReported = true;
	WebSocketRelays::Instance().markResult(_domain, success);
}

void WebSocketSocket::plainDisconnected() {
	reportResult(false);
	_state = State::NotConnected;
	_disconnected.fire({});
}

void WebSocketSocket::handleError(int errorCode) {
	if (errorCode) {
		logError(errorCode, _socket.errorString());
	}
	reportResult(false);
	_state = State::Error;
	_error.fire({});
}

bool WebSocketSocket::isGoodStartNonce(bytes::const_span nonce) {
	// Opaque inside the frames, the raw-Tcp marker check does not apply.
	return true;
}

void WebSocketSocket::timedOut() {
	reportResult(false);
	_syncTimeRequests.fire({});
}

void WebSocketSocket::verifiedDataReceived() {
	reportResult(true);
}

bool WebSocketSocket::isConnected() {
	return (_state == State::Connected);
}

bool WebSocketSocket::hasBytesAvailable() {
	return (_readOffset < _readBuffer.size());
}

int64 WebSocketSocket::read(bytes::span buffer) {
	const auto available = int64(_readBuffer.size()) - _readOffset;
	const auto write = std::min(available, int64(buffer.size()));
	if (write <= 0) {
		return 0;
	}
	bytes::copy(
		buffer,
		bytes::make_span(_readBuffer).subspan(_readOffset, write));
	_readOffset += write;
	if (_readOffset == _readBuffer.size()) {
		_readBuffer.clear();
		_readOffset = 0;
	}
	return write;
}

int32 WebSocketSocket::debugState() {
	return _socket.state();
}

QString WebSocketSocket::debugPostfix() const {
	return u"_ws"_q;
}

} // namespace MTP::details
