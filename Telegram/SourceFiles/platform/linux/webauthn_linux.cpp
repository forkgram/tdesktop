/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/platform_webauthn.h"

#include "webauthn/webauthn_common.h"

namespace Platform::WebAuthn {

bool LocalOnlySupported() {
	return false;
}

bool HasLocalOnlyKeys(bool testServer) {
	return false;
}

void RegisterKeyLocalOnly(
		const Data::Passkey::RegisterData &data,
		bool testServer,
		Fn<void(RegisterResult result)> callback) {
	callback({});
}

void LoginLocalOnly(
		const Data::Passkey::LoginData &data,
		bool testServer,
		Fn<void(LoginResult result)> callback) {
	callback({});
}

void RemoveKeyLocalOnly(const QString &credentialId) {
}

} // namespace Platform::WebAuthn
