// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "graphrequest.h"
#include "graphclient.h"
#include "graphcore_debug.h"
#include "graphutil.h"
#include "ratelimiter.h"
#include "throttle.h"
#include "tokenprovider.h"

#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QFutureWatcher>
#include <QNetworkReply>
#include <QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace GraphCore
{

namespace
{
// Waits are done in steps of at most this long, so cancelling is noticed quickly
constexpr int waitStepMs = 250;
// How often a request waiting for a free mailbox slot looks again
constexpr int slotPollMs = 50;
constexpr int maxRedirects = 5;

bool isThrottled(int status)
{
    return status == 429 || status == 503;
}

bool isRetryableStatus(int status)
{
    return status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

void emptySink(QIODevice *sink)
{
    if (auto *file = qobject_cast<QFile *>(sink)) {
        file->resize(0);
    } else if (auto *buffer = qobject_cast<QBuffer *>(sink)) {
        buffer->buffer().clear();
    }
    sink->seek(0);
}
}

GraphRequest::GraphRequest(GraphClient *client, const GraphRequestSpec &spec)
    : QObject(client)
    , m_client(client)
    , m_spec(spec)
{
    m_url = QUrl(spec.pathOrUrl.startsWith(QLatin1String("https://")) || spec.pathOrUrl.startsWith(QLatin1String("http://"))
                     ? spec.pathOrUrl
                     : client->baseUrl() + spec.pathOrUrl);
    m_throttleKey = throttleKeyFor(client->throttleScope(), m_url, client->graphHost());
    m_slotLimit = isMailboxKey(m_throttleKey) ? Throttle::maxConcurrentPerMailbox : 0;
    m_watchdog.setInterval(waitStepMs);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        if (m_reply && cancelled()) {
            m_reply->abort();
        }
    });
}

GraphRequest::~GraphRequest()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
    }
    releaseSlot();
}

bool GraphRequest::isFinished() const
{
    return m_finished;
}

GraphResponse GraphRequest::response() const
{
    return m_response;
}

void GraphRequest::abort()
{
    if (m_finished) {
        return;
    }
    m_aborted = true;
    if (m_reply) {
        // onReplyFinished() follows and finishes with "cancelled"
        m_reply->abort();
    } else {
        finish({0, {}, QStringLiteral("cancelled"), {}});
    }
}

bool GraphRequest::cancelled() const
{
    return m_aborted || (m_client->m_cancel && m_client->m_cancel->load());
}

bool GraphRequest::needsToken() const
{
    return m_spec.authenticated && m_url.host() == m_client->graphHost();
}

void GraphRequest::releaseSlot()
{
    if (m_hasSlot) {
        m_hasSlot = false;
        Throttle::release(m_throttleKey);
    }
}

void GraphRequest::next()
{
    if (m_finished) {
        return;
    }
    if (cancelled()) {
        finish({0, {}, QStringLiteral("cancelled"), {}});
        return;
    }
    // Our own backoff, or Retry-After for this mailbox (set by any request of the process)
    const qint64 waitMs = std::max(m_notBeforeMs - nowMs(), Throttle::waitMs(m_throttleKey));
    if (waitMs > 0) {
        QTimer::singleShot(std::min<qint64>(waitMs, waitStepMs), this, &GraphRequest::next);
        return;
    }
    if (m_slotLimit > 0 && !m_hasSlot) {
        if (!Throttle::tryAcquire(m_throttleKey, m_slotLimit)) {
            QTimer::singleShot(slotPollMs, this, &GraphRequest::next);
            return;
        }
        m_hasSlot = true;
    }
    // Upload limit: wait for this body's turn without blocking the other transfers
    if (m_spec.verb == "PUT" && !m_spec.body.isEmpty() && !m_paced) {
        m_paced = true;
        if (const qint64 delayMs = RateLimiter::reserveUpload(m_spec.body.size()); delayMs > 0) {
            m_notBeforeMs = nowMs() + delayMs;
            QTimer::singleShot(std::min<qint64>(delayMs, waitStepMs), this, &GraphRequest::next);
            return;
        }
    }

    if (!needsToken()) {
        sendWithToken({});
        return;
    }
    TokenProvider *tokens = m_client->m_tokens;
    const bool force = std::exchange(m_forceRefresh, false);
    if (!force) {
        if (const QString token = tokens->cachedAccessToken(); !token.isEmpty()) {
            sendWithToken(token);
            return;
        }
    }
    // A refresh may take a network round trip and the keychain: run it on the thread pool
    auto *watcher = new QFutureWatcher<QPair<QString, QString>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        watcher->deleteLater();
        if (m_finished) {
            return;
        }
        const auto [token, error] = watcher->result();
        if (token.isEmpty()) {
            finish({0, {}, QStringLiteral("no access token: ") + error, {}});
            return;
        }
        sendWithToken(token);
    });
    watcher->setFuture(QtConcurrent::run([tokens, force] {
        QString error;
        const QString token = tokens->accessToken(&error, force);
        return qMakePair(token, error);
    }));
}

void GraphRequest::sendWithToken(const QString &token)
{
    if (cancelled()) {
        finish({0, {}, QStringLiteral("cancelled"), {}});
        return;
    }
    m_paced = false;
    ++m_attempts;

    QNetworkRequest request(m_url);
    request.setTransferTimeout(m_client->m_policy.transferTimeoutMs);
    request.setHeader(QNetworkRequest::UserAgentHeader, m_client->m_userAgent);
    for (const auto &[name, value] : std::as_const(m_spec.headers)) {
        request.setRawHeader(name, value);
    }
    if (!token.isEmpty()) {
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + token.toLatin1());
    }
    if (m_spec.sink) {
        // A retried download starts over
        emptySink(m_spec.sink);
    }

    m_reply = m_client->m_nam.sendCustomRequest(request, m_spec.verb, m_spec.body);
    if (m_spec.sink) {
        connect(m_reply, &QNetworkReply::readyRead, this, &GraphRequest::drain);
    }
    connect(m_reply, &QNetworkReply::finished, this, &GraphRequest::onReplyFinished);
    if (m_client->m_cancel) {
        m_watchdog.start();
    }
}

// Moves file content from the reply into the sink and reports it. Only a 2xx answer is the file;
// redirect and error bodies are not.
void GraphRequest::drain()
{
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status < 200 || status >= 300) {
        return;
    }
    const QByteArray chunk = m_reply->readAll();
    m_spec.sink->write(chunk);
    if (m_client->m_observer) {
        m_client->m_observer(chunk.size());
    }
}

void GraphRequest::onReplyFinished()
{
    QNetworkReply *reply = std::exchange(m_reply, nullptr);
    reply->deleteLater();
    m_watchdog.stop();
    releaseSlot();

    GraphResponse response;
    response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError && response.ok()) {
        // The server answered 2xx but the transfer broke off (connection dropped, timeout): a
        // network failure to retry, never a finished download
        response.status = 0;
    }
    if (!reply->isOpen()) {
        // Aborted: nothing to read
    } else if (m_spec.sink && response.ok()) {
        const QByteArray rest = reply->readAll();
        m_spec.sink->write(rest);
        if (m_client->m_observer) {
            m_client->m_observer(rest.size());
        }
    } else {
        response.body = reply->readAll();
    }
    if (m_client->m_observer && m_spec.verb == "PUT" && response.ok()) {
        m_client->m_observer(m_spec.body.size());
    }
    response.errorString = reply->errorString();
    response.headers = reply->rawHeaderPairs();
    const QUrl location = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const QNetworkReply::NetworkError netError = reply->error();
    const RetryPolicy &policy = m_client->m_policy;

    // Redirects are followed by hand so the token never leaves the Graph host (downloads go to
    // SharePoint hosts; auto-expanded archive folders answer 308)
    if (response.status >= 300 && response.status < 400 && location.isValid() && m_redirects < maxRedirects) {
        ++m_redirects;
        --m_attempts; // following a redirect is not a failed attempt
        m_url = m_url.resolved(location);
        next();
        return;
    }

    if (response.status == 401 && needsToken() && !m_refreshedAfter401) {
        m_refreshedAfter401 = true;
        m_forceRefresh = true;
        qCInfo(GRAPHCORE) << "Graph returned 401, refreshing the access token once";
        next();
        return;
    }

    // Without an HTTP status the request never got an answer (timeouts, DNS, a server closing
    // the HTTP/2 connection with GOAWAY, ...): retry, unless it was cancelled
    const bool throttled = isThrottled(response.status);
    const bool transient = isRetryableStatus(response.status)
        || (response.status == 0 && netError != QNetworkReply::OperationCanceledError && !cancelled());
    const int attempts = m_spec.maxAttempts > 0 ? std::min(m_spec.maxAttempts, policy.maxAttempts) : policy.maxAttempts;
    if (transient && m_attempts < (throttled ? policy.maxThrottledAttempts : attempts)) {
        bool ok = false;
        const int serverWait = response.header("Retry-After").toInt(&ok);
        const qint64 delayMs = ok ? qint64(std::clamp(serverWait, 1, policy.maxRetryAfterSeconds)) * 1000
                                  : std::min<qint64>(qint64(policy.backoffBaseMs) << std::min(m_attempts, 20), policy.maxBackoffMs);
        if (throttled) {
            Throttle::note(m_throttleKey, delayMs);
            qCInfo(GRAPHCORE).noquote() << QStringLiteral("Throttled by Microsoft 365 (HTTP %1, %2), pausing it for %3 ms")
                                               .arg(response.status)
                                               .arg(m_throttleKey)
                                               .arg(delayMs);
        } else {
            qCInfo(GRAPHCORE) << m_spec.verb << m_url.path() << "->" << (response.status ? QString::number(response.status) : response.errorString)
                              << "retrying in" << delayMs << "ms";
        }
        if (response.status == 0) {
            // The kept-alive connection may be dead (network dropped): retry on a fresh one, or
            // every retry waits for the timeout again on the same dead socket
            m_client->m_nam.clearConnectionCache();
        }
        m_notBeforeMs = nowMs() + delayMs;
        next();
        return;
    }

    if (response.status == 0 && cancelled()) {
        response.errorString = QStringLiteral("cancelled");
    }
    finish(response);
}

void GraphRequest::finish(const GraphResponse &response)
{
    if (m_finished) {
        return;
    }
    releaseSlot();
    m_watchdog.stop();
    m_finished = true;
    m_response = response;
    Q_EMIT finished();
}

}
