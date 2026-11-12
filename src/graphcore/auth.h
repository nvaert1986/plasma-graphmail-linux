// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "accounts.h"
#include "tokenprovider.h"

#include <QDateTime>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <atomic>
#include <functional>

namespace GraphCore
{

struct AuthSettings {
    // Keychain service the refresh token is stored under (one per app until the account service
    // owns the tokens)
    QString keychainService;
    QString clientId;
    // A tenant ID, or "organizations" for a first sign-in where the user picks any work account
    QString tenant;
    // The user's object id; empty until signed in
    QString objectId;
    QStringList scopes;
};

// Who signed in, from the ID token
struct SignedInUser {
    QString objectId;
    QString userPrincipalName;
    QString displayName;
    QString tenantId;
};

// Entra ID sign-in for one account with a public client (authorization code + PKCE, or a device
// code). The refresh token lives in the system keychain (KWallet / Secret Service) under
// tenant:client:user; access tokens only in memory. Thread-safe: sync workers call accessToken()
// from their own threads, the UI calls cachedAccessToken(), which never waits for a refresh.
class Auth : public TokenProvider
{
public:
    explicit Auth(const AuthSettings &settings);

    // Settings for an account that signed in before
    static AuthSettings settingsFor(const Account &account, const QString &keychainService, const QStringList &scopes);
    // The account to store after a sign-in (organisation still empty: it comes from Graph)
    static Account accountFor(const SignedInUser &user, const QString &clientId);

    bool isConfigured() const;
    AuthSettings settings() const;

    // Opens the browser and blocks (with a local event loop) until the redirect arrives.
    // Must run in a thread with an event loop. loginHint pre-fills the account picker.
    bool interactiveLogin(QString *error, const QString &loginHint = {});
    // Signing in through the browser needs Qt 6.9 (scope tokens, the "localhost" callback host, the
    // ID token); older systems sign in with a code only
    static constexpr bool browserLoginAvailable()
    {
        return QT_VERSION >= QT_VERSION_CHECK(6, 9, 0);
    }
    // Sign-in with a short code entered at the shown address, on any device. show() gets the code
    // and the address as soon as they are known; then this blocks until the user is done, the
    // code expires or cancel is set. Needs "Allow public client flows" on the app registration.
    bool deviceCodeLogin(const std::function<void(const QString &code, const QString &address)> &show, const std::atomic_bool *cancel, QString *error);
    // After a sign-in: the user, whose tenant and object id the settings now carry
    SignedInUser signedInUser() const;

    QString accessToken(QString *error, bool forceRefresh = false) override;
    QString cachedAccessToken() override;

    bool hasStoredRefreshToken();
    void logout();

    // Microsoft's page where a tenant administrator approves every permission of the app
    // registration for the whole organisation, once
    static QUrl adminConsentUrl(const QString &tenant, const QString &clientId);
    // The tenant ID for what the user typed: a tenant ID, a domain or a work e-mail address.
    // Domains are looked up in the identity platform's public OpenID document (no sign-in
    // needed). Blocking. Empty when not found; *offline tells a missing connection apart
    static QString resolveTenant(const QString &input, bool *offline);
    static bool isGuid(const QString &text);
    // The claims of an ID token we care about; empty fields when it cannot be read
    static SignedInUser parseIdToken(const QString &idToken);

    // Client ID and tenant from a setup text handed out by IT ("ClientId=... TenantId=...", in
    // any layout); fields not found stay empty
    struct SetupText {
        QString clientId;
        QString tenant;
    };
    static SetupText parseSetupText(const QString &text);

private:
    QString authority() const;
    QString scopeString() const;
    QString keychainKey() const;
    bool refresh(QString *error);
    bool completeSignIn(const QString &accessToken, qint64 expiresInSeconds, const QString &refreshToken, const QString &idToken, QString *error);
    void setToken(const QString &token, const QDateTime &expiresAt);

    QString readRefreshToken(QString *error);
    bool writeRefreshToken(const QString &token, QString *error);

    // m_refreshMutex serialises sign-ins and refreshes (network round trips); m_tokenMutex only
    // guards the cached access token, so cachedAccessToken() never waits for a refresh. Recursive:
    // a sign-in runs a local event loop, and code called from it on the same thread (a timer
    // asking isConfigured()) must not deadlock.
    mutable QRecursiveMutex m_refreshMutex;
    mutable QMutex m_tokenMutex;
    AuthSettings m_settings;
    SignedInUser m_user;
    QString m_refreshToken;
    QString m_accessToken;
    QDateTime m_expiresAt;
};

}
