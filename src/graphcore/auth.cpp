// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "auth.h"
#include "graphcore_debug.h"

#include <qt6keychain/keychain.h>

#include <QElapsedTimer>
#include <QEventLoop>
#include <functional>
#include <QThread>
#include <QMutex>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QOAuth2AuthorizationCodeFlow>
#include <QOAuthHttpServerReplyHandler>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QUrlQuery>

#include <memory>

namespace GraphCore
{

namespace
{
constexpr int loginTimeoutMs = 5 * 60 * 1000;
// An access token this close to expiry is refreshed first
constexpr int refreshMarginSeconds = 300;

// Blocking form POST to the identity platform; the JSON answer (also for errors)
QJsonObject postForm(const QUrl &url, const QUrlQuery &form, QString *networkError)
{
    QNetworkAccessManager nam;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setTransferTimeout(60 * 1000);
    QNetworkReply *reply = nam.post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();
    if (networkError) {
        *networkError = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
    }
    return QJsonDocument::fromJson(reply->readAll()).object();
}

QJsonObject jwtClaims(const QString &jwt)
{
    const QStringList parts = jwt.split(QLatin1Char('.'));
    if (parts.size() < 2) {
        return {};
    }
    const QByteArray payload = QByteArray::fromBase64(parts.at(1).toLatin1(), QByteArray::Base64UrlEncoding);
    return QJsonDocument::fromJson(payload).object();
}

// The first line of the identity platform's error description (the rest is trace and time stamps)
QString firstLine(const QJsonObject &answer)
{
    return answer.value(QLatin1String("error_description")).toString().section(QLatin1Char('\n'), 0, 0);
}

// QtKeychain jobs run on one thread of their own, one after the other: the secret service backend
// answers on the thread that first used it, so jobs started on the token refresh's pool threads
// got their answers on another thread and the daemon crashed as soon as two accounts were signed in
// (2026-11-11: "Cannot create children for a parent that is in a different thread", then a crash)
QObject *keychainContext()
{
    static QObject *context = [] {
        auto *thread = new QThread;
        thread->setObjectName(QStringLiteral("keychain"));
        thread->start();
        auto *object = new QObject;
        object->moveToThread(thread);
        return object;
    }();
    return context;
}

// Runs `work` on the keychain thread and waits for it
void onKeychainThread(const std::function<void()> &work)
{
    QObject *context = keychainContext();
    if (QThread::currentThread() == context->thread()) {
        work();
        return;
    }
    QMetaObject::invokeMethod(context, work, Qt::BlockingQueuedConnection);
}

// A QtKeychain job to completion, in a local event loop on the keychain thread
template<typename Job>
bool runKeychainJob(Job *job, QString *error)
{
    QEventLoop loop;
    QObject::connect(job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job->setAutoDelete(false);
    job->start();
    loop.exec();
    const bool ok = job->error() == QKeychain::NoError;
    if (!ok && error) {
        *error = job->errorString();
    }
    return ok;
}
}

Auth::Auth(const AuthSettings &settings)
    : m_settings(settings)
{
}

AuthSettings Auth::settingsFor(const Account &account, const QString &keychainService, const QStringList &scopes)
{
    return {keychainService, account.effectiveClientId(), account.tenantId, account.objectId, scopes};
}

Account Auth::accountFor(const SignedInUser &user, const QString &clientId)
{
    Account account;
    account.tenantId = user.tenantId;
    account.objectId = user.objectId;
    account.userPrincipalName = user.userPrincipalName;
    account.displayName = user.displayName;
    // Only remember a client ID that differs from the suite's default registration
    if (clientId != Accounts::defaultClientId()) {
        account.clientId = clientId;
    }
    return account;
}

bool Auth::isConfigured() const
{
    QMutexLocker locker(&m_refreshMutex);
    return !m_settings.clientId.isEmpty() && !m_settings.tenant.isEmpty();
}

AuthSettings Auth::settings() const
{
    QMutexLocker locker(&m_refreshMutex);
    return m_settings;
}

SignedInUser Auth::signedInUser() const
{
    QMutexLocker locker(&m_refreshMutex);
    return m_user;
}

QString Auth::authority() const
{
    return QStringLiteral("https://login.microsoftonline.com/%1/oauth2/v2.0").arg(m_settings.tenant);
}

QString Auth::scopeString() const
{
    QStringList scopes = m_settings.scopes;
    // Without these there is no refresh token or ID token
    for (const QString &required : {QStringLiteral("offline_access"), QStringLiteral("openid"), QStringLiteral("profile")}) {
        if (!scopes.contains(required)) {
            scopes.append(required);
        }
    }
    return scopes.join(QLatin1Char(' '));
}

QString Auth::keychainKey() const
{
    return m_settings.tenant + QLatin1Char(':') + m_settings.clientId + QLatin1Char(':') + m_settings.objectId;
}

void Auth::setToken(const QString &token, const QDateTime &expiresAt)
{
    QMutexLocker locker(&m_tokenMutex);
    m_accessToken = token;
    m_expiresAt = expiresAt;
}

bool Auth::completeSignIn(const QString &accessToken, qint64 expiresInSeconds, const QString &refreshToken, const QString &idToken, QString *error)
{
    const SignedInUser user = parseIdToken(idToken);
    if (refreshToken.isEmpty() || user.objectId.isEmpty() || user.tenantId.isEmpty()) {
        *error = refreshToken.isEmpty() ? QStringLiteral("no refresh token in the answer") : QStringLiteral("no usable ID token in the answer");
        return false;
    }
    // From now on this is the user's own tenant, also when the sign-in started at "organizations"
    m_settings.tenant = user.tenantId;
    m_settings.objectId = user.objectId;
    m_user = user;
    m_refreshToken = refreshToken;
    setToken(accessToken, QDateTime::currentDateTimeUtc().addSecs(expiresInSeconds));
    return writeRefreshToken(refreshToken, error);
}

bool Auth::interactiveLogin(QString *error, const QString &loginHint)
{
    QMutexLocker locker(&m_refreshMutex);
    if (m_settings.clientId.isEmpty() || m_settings.tenant.isEmpty()) {
        *error = QStringLiteral("no app registration (client ID) or tenant configured");
        return false;
    }

#if QT_VERSION < QT_VERSION_CHECK(6, 9, 0)
    Q_UNUSED(loginHint)
    *error = QStringLiteral("signing in through the browser needs Qt 6.9 or newer; sign in with a code instead");
    return false;
#else
    QOAuth2AuthorizationCodeFlow flow;
    flow.setAuthorizationUrl(QUrl(authority() + QStringLiteral("/authorize")));
    flow.setTokenUrl(QUrl(authority() + QStringLiteral("/token")));
    flow.setClientIdentifier(m_settings.clientId);
    QSet<QByteArray> scopeTokens;
    const QStringList scopes = scopeString().split(QLatin1Char(' '));
    for (const QString &scope : scopes) {
        scopeTokens.insert(scope.toLatin1());
    }
    flow.setRequestedScopeTokens(scopeTokens);
    flow.setPkceMethod(QOAuth2AuthorizationCodeFlow::PkceMethod::S256);
    flow.setModifyParametersFunction([loginHint](QAbstractOAuth::Stage stage, QMultiMap<QString, QVariant> *parameters) {
        if (stage == QAbstractOAuth::Stage::RequestingAuthorization) {
            parameters->insert(QStringLiteral("prompt"), QStringLiteral("select_account"));
            if (!loginHint.isEmpty()) {
                parameters->insert(QStringLiteral("login_hint"), loginHint);
            }
        }
    });

    // Entra ignores the port for http://localhost redirect URIs of public clients, so any
    // free port works; it must be "localhost" because that is what the app registration lists.
    auto *handler = new QOAuthHttpServerReplyHandler(QHostAddress::LocalHost, 0, &flow);
    handler->setCallbackHost(QStringLiteral("localhost"));
    if (!handler->isListening()) {
        *error = QStringLiteral("could not open a local port for the sign-in redirect");
        return false;
    }
    flow.setReplyHandler(handler);

    QEventLoop loop;
    QString failure;
    QObject::connect(&flow, &QAbstractOAuth::authorizeWithBrowser, &flow, [](const QUrl &url) {
        if (!QProcess::startDetached(QStringLiteral("xdg-open"), {url.toString(QUrl::FullyEncoded)})) {
            qCWarning(GRAPHCORE) << "Could not start xdg-open; open this URL manually:" << url.toString(QUrl::FullyEncoded);
        }
    });
    QObject::connect(&flow, &QAbstractOAuth::granted, &loop, &QEventLoop::quit);
    QObject::connect(&flow, &QAbstractOAuth2::serverReportedErrorOccurred, &loop, [&](const QString &code, const QString &description, const QUrl &) {
        failure = code + QStringLiteral(": ") + description;
        loop.quit();
    });
    QObject::connect(&flow, &QAbstractOAuth::requestFailed, &loop, [&](QAbstractOAuth::Error e) {
        failure = QStringLiteral("sign-in request failed (QAbstractOAuth::Error %1)").arg(static_cast<int>(e));
        loop.quit();
    });
    QTimer::singleShot(loginTimeoutMs, &loop, [&] {
        failure = QStringLiteral("no sign-in completed within five minutes");
        loop.quit();
    });

    flow.grant();
    loop.exec();

    if (flow.status() != QAbstractOAuth::Status::Granted) {
        *error = failure.isEmpty() ? QStringLiteral("sign-in did not complete") : failure;
        return false;
    }
    const qint64 expiresIn = std::max<qint64>(60, QDateTime::currentDateTimeUtc().secsTo(flow.expirationAt()));
    return completeSignIn(flow.token(), expiresIn, flow.refreshToken(), flow.idToken(), error);
#endif
}

bool Auth::deviceCodeLogin(const std::function<void(const QString &, const QString &)> &show, const std::atomic_bool *cancel, QString *error)
{
    QMutexLocker locker(&m_refreshMutex);
    QUrlQuery request;
    request.addQueryItem(QStringLiteral("client_id"), m_settings.clientId);
    request.addQueryItem(QStringLiteral("scope"), scopeString());
    QString networkError;
    const QJsonObject code = postForm(QUrl(authority() + QStringLiteral("/devicecode")), request, &networkError);
    const QString deviceCode = code.value(QLatin1String("device_code")).toString();
    if (deviceCode.isEmpty()) {
        const QString description = firstLine(code);
        *error = description.isEmpty() ? networkError : description;
        return false;
    }
    show(code.value(QLatin1String("user_code")).toString(), code.value(QLatin1String("verification_uri")).toString());

    int intervalSeconds = std::max(1, code.value(QLatin1String("interval")).toInt(5));
    QElapsedTimer age;
    age.start();
    const qint64 lifetimeMs = qint64(code.value(QLatin1String("expires_in")).toInt(900)) * 1000;
    while (age.elapsed() < lifetimeMs) {
        // Wait in small steps so cancelling is quick
        for (int waited = 0; waited < intervalSeconds * 1000; waited += 250) {
            if (cancel && cancel->load()) {
                *error = QStringLiteral("cancelled");
                return false;
            }
            QEventLoop pause;
            QTimer::singleShot(250, &pause, &QEventLoop::quit);
            pause.exec();
        }
        QUrlQuery poll;
        poll.addQueryItem(QStringLiteral("client_id"), m_settings.clientId);
        poll.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("urn:ietf:params:oauth:grant-type:device_code"));
        poll.addQueryItem(QStringLiteral("device_code"), deviceCode);
        const QJsonObject answer = postForm(QUrl(authority() + QStringLiteral("/token")), poll, &networkError);
        if (answer.contains(QLatin1String("access_token"))) {
            return completeSignIn(answer.value(QLatin1String("access_token")).toString(),
                                  answer.value(QLatin1String("expires_in")).toInt(3600),
                                  answer.value(QLatin1String("refresh_token")).toString(),
                                  answer.value(QLatin1String("id_token")).toString(),
                                  error);
        }
        const QString state = answer.value(QLatin1String("error")).toString();
        if (state == QLatin1String("authorization_pending")) {
            continue;
        }
        if (state == QLatin1String("slow_down")) {
            intervalSeconds += 5;
            continue;
        }
        if (state.isEmpty() && !networkError.isEmpty()) {
            // A network hiccup while waiting; try again
            continue;
        }
        const QString description = firstLine(answer);
        *error = description.isEmpty() ? state : description;
        return false;
    }
    *error = QStringLiteral("the code expired before sign-in completed");
    return false;
}

QString Auth::cachedAccessToken()
{
    QMutexLocker locker(&m_tokenMutex);
    const bool fresh = !m_accessToken.isEmpty() && m_expiresAt.isValid()
        && QDateTime::currentDateTimeUtc().secsTo(m_expiresAt) > refreshMarginSeconds;
    return fresh ? m_accessToken : QString();
}

QString Auth::accessToken(QString *error, bool forceRefresh)
{
    if (!forceRefresh) {
        if (QString token = cachedAccessToken(); !token.isEmpty()) {
            return token;
        }
    }
    QMutexLocker locker(&m_refreshMutex);
    // Another thread may have refreshed while this one waited for the lock
    if (!forceRefresh) {
        if (QString token = cachedAccessToken(); !token.isEmpty()) {
            return token;
        }
    }
    if (!refresh(error)) {
        return {};
    }
    QMutexLocker tokenLocker(&m_tokenMutex);
    return m_accessToken;
}

bool Auth::refresh(QString *error)
{
    if (m_refreshToken.isEmpty()) {
        m_refreshToken = readRefreshToken(error);
        if (m_refreshToken.isEmpty()) {
            if (error && error->isEmpty()) {
                *error = QStringLiteral("not signed in");
            }
            return false;
        }
    }

    QUrlQuery form;
    form.addQueryItem(QStringLiteral("client_id"), m_settings.clientId);
    form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    form.addQueryItem(QStringLiteral("refresh_token"), m_refreshToken);
    form.addQueryItem(QStringLiteral("scope"), scopeString());

    QString networkError;
    const QJsonObject body = postForm(QUrl(authority() + QStringLiteral("/token")), form, &networkError);
    if (!body.contains(QLatin1String("access_token"))) {
        const QString description = firstLine(body);
        // Callers tell these apart: only "sign-in expired" means signing in again; no answer at
        // all (offline, no DNS) is a connection problem that goes away by itself
        if (body.value(QLatin1String("error")).toString() == QLatin1String("invalid_grant")) {
            // The refresh token is dead (revoked, expired, password change)
            m_refreshToken.clear();
            *error = QStringLiteral("sign-in expired: ") + description;
        } else if (body.isEmpty()) {
            *error = QStringLiteral("no connection: ") + networkError;
        } else {
            *error = description.isEmpty() ? body.value(QLatin1String("error")).toString() : description;
        }
        return false;
    }

    setToken(body.value(QLatin1String("access_token")).toString(),
             QDateTime::currentDateTimeUtc().addSecs(body.value(QLatin1String("expires_in")).toInt(3600)));
    const QString rotated = body.value(QLatin1String("refresh_token")).toString();
    if (!rotated.isEmpty() && rotated != m_refreshToken) {
        m_refreshToken = rotated;
        QString writeError;
        if (!writeRefreshToken(rotated, &writeError)) {
            qCWarning(GRAPHCORE) << "Could not store the rotated refresh token:" << writeError;
        }
    }
    return true;
}

bool Auth::hasStoredRefreshToken()
{
    QMutexLocker locker(&m_refreshMutex);
    if (!m_refreshToken.isEmpty()) {
        return true;
    }
    QString error;
    m_refreshToken = readRefreshToken(&error);
    return !m_refreshToken.isEmpty();
}

void Auth::logout()
{
    QMutexLocker locker(&m_refreshMutex);
    setToken({}, {});
    m_refreshToken.clear();
    if (m_settings.objectId.isEmpty()) {
        return;
    }
    onKeychainThread([this] {
        QKeychain::DeletePasswordJob job(m_settings.keychainService);
        job.setKey(keychainKey());
        QString error;
        runKeychainJob(&job, &error);
    });
}

QUrl Auth::adminConsentUrl(const QString &tenant, const QString &clientId)
{
    QUrl url(QStringLiteral("https://login.microsoftonline.com/%1/adminconsent").arg(tenant.isEmpty() ? QStringLiteral("organizations") : tenant));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client_id"), clientId);
    url.setQuery(query);
    return url;
}

bool Auth::isGuid(const QString &text)
{
    static const QRegularExpression guid(QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return guid.match(text).hasMatch();
}

SignedInUser Auth::parseIdToken(const QString &idToken)
{
    const QJsonObject claims = jwtClaims(idToken);
    return {claims.value(QLatin1String("oid")).toString(),
            claims.value(QLatin1String("preferred_username")).toString(),
            claims.value(QLatin1String("name")).toString(),
            claims.value(QLatin1String("tid")).toString()};
}

Auth::SetupText Auth::parseSetupText(const QString &text)
{
    static const QRegularExpression client(QStringLiteral("ClientId\\s*[=:]\\s*([0-9a-fA-F-]{36})"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tenant(QStringLiteral("TenantId\\s*[=:]\\s*([^\\s;,]+)"), QRegularExpression::CaseInsensitiveOption);
    return {client.match(text).captured(1), tenant.match(text).captured(1)};
}

QString Auth::resolveTenant(const QString &input, bool *offline)
{
    *offline = false;
    QString value = input.trimmed();
    if (isGuid(value)) {
        return value.toLower();
    }
    // A work e-mail address stands for its domain
    value = value.section(QLatin1Char('@'), -1).toLower();
    static const QRegularExpression domain(QStringLiteral("^[a-z0-9-]+(\\.[a-z0-9-]+)+$"));
    if (!domain.match(value).hasMatch()) {
        return {};
    }

    QNetworkAccessManager nam;
    QNetworkRequest request(QUrl(QStringLiteral("https://login.microsoftonline.com/%1/v2.0/.well-known/openid-configuration").arg(value)));
    request.setTransferTimeout(30 * 1000);
    QNetworkReply *reply = nam.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0) {
        *offline = true;
        return {};
    }
    // "issuer": "https://login.microsoftonline.com/<tenant id>/v2.0"
    const QString issuer = QJsonDocument::fromJson(reply->readAll()).object().value(QLatin1String("issuer")).toString();
    const QString tenantId = QUrl(issuer).path().section(QLatin1Char('/'), 1, 1);
    return isGuid(tenantId) ? tenantId.toLower() : QString();
}

QString Auth::readRefreshToken(QString *error)
{
    if (m_settings.objectId.isEmpty()) {
        return {};
    }
    QString token;
    onKeychainThread([&] {
        QKeychain::ReadPasswordJob job(m_settings.keychainService);
        job.setKey(keychainKey());
        if (runKeychainJob(&job, error)) {
            token = job.textData();
        }
    });
    return token;
}

bool Auth::writeRefreshToken(const QString &token, QString *error)
{
    bool ok = false;
    onKeychainThread([&] {
        QKeychain::WritePasswordJob job(m_settings.keychainService);
        job.setKey(keychainKey());
        job.setTextData(token);
        ok = runKeychainJob(&job, error);
    });
    return ok;
}

}
