/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/details/mtproto_abstract_socket.h"

#include <QtNetwork/QSslSocket>

namespace MTP::details {

class WebSocketSocket final : public AbstractSocket {
public:
	WebSocketSocket(
		not_null<QThread*> thread,
		const QNetworkProxy &proxy,
		bool protocolForFiles,
		const QString &path);

	void connectToHost(const QString &address, int port) override;
	bool isGoodStartNonce(bytes::const_span nonce) override;
	void timedOut() override;
	bool isConnected() override;
	bool hasBytesAvailable() override;
	int64 read(bytes::span buffer) override;
	void write(bytes::const_span prefix, bytes::const_span buffer) override;

	int32 debugState() override;
	QString debugPostfix() const override;

private:
	enum class State {
		NotConnected,
		Connecting,
		WaitingUpgrade,
		Connected,
		Error,
	};

	void sslConnected();
	void plainDisconnected();
	void plainReadyRead();
	void handleError(int errorCode = 0);

	void sendUpgradeRequest();
	[[nodiscard]] bool readUpgradeResponse();
	[[nodiscard]] bool parseFrames();
	void writeFrame(bytes::const_span payload, quint8 opcode);

	const QString _path;
	QString _host;
	QString _domain;
	QSslSocket _socket;
	State _state = State::NotConnected;
	QByteArray _key;
	QByteArray _incoming;
	QByteArray _readBuffer;
	int64 _readOffset = 0;
	bool _verifyCertificate = true;

};

} // namespace MTP::details
