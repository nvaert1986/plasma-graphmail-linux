// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "graphclient.h"
#include "graphcore_debug.h"
#include "graphutil.h"
#include "throttle.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <numeric>

namespace GraphCore
{

namespace
{
QString defaultUserAgent()
{
    const QString name = QCoreApplication::applicationName();
    const QString version = QCoreApplication::applicationVersion();
    return (name.isEmpty() ? QStringLiteral("graphcore") : name) + QLatin1Char('/') + (version.isEmpty() ? QStringLiteral(GRAPHMAIL_VERSION) : version);
}
}

GraphClient::GraphClient(TokenProvider *tokens, QObject *parent)
    : QObject(parent)
    , m_tokens(tokens)
    , m_baseUrl(defaultBaseUrl())
    , m_userAgent(defaultUserAgent())
{
    // Redirects are followed by hand (GraphRequest) so the bearer token never leaves Graph
    m_nam.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
}

GraphClient::~GraphClient()
{
    // The requests are children, deleted only by ~QObject, after m_nam has deleted their network
    // replies: a request still running then aborted a freed reply (a mailbox removed or the
    // daemon stopped in the middle of a sync, phase 7). Delete them while m_nam still exists.
    const QList<GraphRequest *> requests = findChildren<GraphRequest *>(Qt::FindDirectChildrenOnly);
    qDeleteAll(requests);
}

QString GraphClient::defaultBaseUrl()
{
    return QStringLiteral("https://graph.microsoft.com/v1.0");
}

QString GraphClient::baseUrl() const
{
    return m_baseUrl;
}

void GraphClient::setBaseUrl(const QString &url)
{
    m_baseUrl = url;
}

QString GraphClient::graphHost() const
{
    return QUrl(m_baseUrl).host();
}

QString GraphClient::throttleScope() const
{
    return m_throttleScope;
}

void GraphClient::setThrottleScope(const QString &scope)
{
    m_throttleScope = scope;
}

void GraphClient::setUserAgent(const QString &userAgent)
{
    m_userAgent = userAgent;
}

RetryPolicy GraphClient::retryPolicy() const
{
    return m_policy;
}

void GraphClient::setRetryPolicy(const RetryPolicy &policy)
{
    m_policy = policy;
}

void GraphClient::setCancelFlag(const std::atomic_bool *flag)
{
    m_cancel = flag;
}

void GraphClient::setTransferObserver(std::function<void(qint64)> observer)
{
    m_observer = std::move(observer);
}

GraphRequest *GraphClient::start(const GraphRequestSpec &spec)
{
    auto *request = new GraphRequest(this, spec);
    QTimer::singleShot(0, request, &GraphRequest::next);
    return request;
}

GraphResponse GraphClient::send(const GraphRequestSpec &spec)
{
    GraphRequest *request = start(spec);
    QEventLoop loop;
    connect(request, &GraphRequest::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const GraphResponse response = request->response();
    delete request;
    return response;
}

GraphResponse GraphClient::get(const QString &pathOrUrl, const HeaderList &headers)
{
    return send({"GET", pathOrUrl, {}, headers, true, nullptr});
}

GraphResponse GraphClient::postJson(const QString &path, const QJsonObject &body, const HeaderList &headers)
{
    HeaderList all = headers;
    all.emplaceBack(QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json"));
    return send({"POST", path, QJsonDocument(body).toJson(QJsonDocument::Compact), all, true, nullptr});
}

GraphResponse GraphClient::patchJson(const QString &path, const QJsonObject &body, const HeaderList &headers)
{
    HeaderList all = headers;
    all.emplaceBack(QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json"));
    return send({"PATCH", path, QJsonDocument(body).toJson(QJsonDocument::Compact), all, true, nullptr});
}

GraphResponse GraphClient::put(const QString &path, const QByteArray &data, const HeaderList &headers)
{
    HeaderList all = headers;
    all.emplaceBack(QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/octet-stream"));
    return send({"PUT", path, data, all, true, nullptr});
}

GraphResponse GraphClient::deleteResource(const QString &path, const HeaderList &headers)
{
    return send({"DELETE", path, {}, headers, true, nullptr});
}

GraphResponse GraphClient::download(const QString &path, QIODevice *target)
{
    return send({"GET", path, {}, {}, true, target});
}

GraphResponse GraphClient::uploadFragment(const QString &uploadUrl, const QByteArray &data, qint64 offset, qint64 total)
{
    const QByteArray range = QByteArrayLiteral("bytes ") + QByteArray::number(offset) + '-' + QByteArray::number(offset + data.size() - 1) + '/'
        + QByteArray::number(total);
    return send({"PUT", uploadUrl, data, {{QByteArrayLiteral("Content-Range"), range}}, false, nullptr});
}

GraphResponse GraphClient::uploadSessionStatus(const QString &uploadUrl)
{
    return send({"GET", uploadUrl, {}, {}, false, nullptr});
}

GraphResponse GraphClient::cancelUploadSession(const QString &uploadUrl)
{
    return send({"DELETE", uploadUrl, {}, {}, false, nullptr});
}

void GraphClient::runParallel(QList<ParallelJob> jobs, int concurrency)
{
    if (jobs.isEmpty()) {
        return;
    }
    concurrency = std::max(1, concurrency);
    QEventLoop loop;
    int active = 0;
    qsizetype nextJob = 0;
    std::function<void()> pump = [&] {
        while (active < concurrency && nextJob < jobs.size()) {
            const qsizetype index = nextJob++;
            const ParallelJob &job = jobs.at(index);
            GraphRequest *request = start({job.verb, job.pathOrUrl, job.body, job.headers, true, job.sink});
            ++active;
            connect(request, &GraphRequest::finished, &loop, [&, request, index] {
                jobs[index].done(request->response());
                request->deleteLater();
                --active;
                pump();
            });
        }
        if (active == 0 && nextJob >= jobs.size()) {
            loop.quit();
        }
    };
    pump();
    if (active > 0) {
        loop.exec();
    }
}

void GraphClient::sleepCancellable(qint64 milliseconds)
{
    const qint64 until = QDateTime::currentMSecsSinceEpoch() + milliseconds;
    for (qint64 left = milliseconds; left > 0 && !(m_cancel && m_cancel->load()); left = until - QDateTime::currentMSecsSinceEpoch()) {
        QEventLoop pause;
        QTimer::singleShot(int(std::min<qint64>(left, 250)), &pause, &QEventLoop::quit);
        pause.exec();
    }
}

QList<GraphResponse> GraphClient::batch(const QList<BatchItem> &items)
{
    QList<GraphResponse> results(items.size());
    QList<QString> paths;
    QList<QString> keys;
    for (const BatchItem &item : items) {
        QString path = item.path;
        if (path.startsWith(m_baseUrl)) {
            path = path.mid(m_baseUrl.size());
        }
        paths.append(path);
        keys.append(throttleKeyFor(m_throttleScope, QUrl(m_baseUrl + path), graphHost()));
    }

    QList<qsizetype> pending(items.size());
    std::iota(pending.begin(), pending.end(), 0);
    for (int round = 0; !pending.isEmpty(); ++round) {
        QList<qsizetype> retry;
        qint64 retryAfterMs = 0;

        QList<qsizetype> remaining = pending;
        while (!remaining.isEmpty()) {
            if (m_cancel && m_cancel->load()) {
                for (qsizetype index : std::as_const(remaining)) {
                    results[index] = {0, {}, QStringLiteral("cancelled"), {}};
                }
                return results;
            }
            // One call: up to 20 items
            const QList<qsizetype> chunk = remaining.first(std::min<qsizetype>(maxBatchSize, remaining.size()));
            remaining.remove(0, chunk.size());
            QStringList mailboxes;
            for (qsizetype index : chunk) {
                if (isMailboxKey(keys.at(index)) && !mailboxes.contains(keys.at(index))) {
                    mailboxes.append(keys.at(index));
                }
            }
            mailboxes.sort(); // a fixed order, so two batches never wait for each other's half

            // Respect a Retry-After that any request of the process got for these mailboxes
            qint64 waitMs = 0;
            QJsonArray requests;
            for (qsizetype index : std::as_const(chunk)) {
                waitMs = std::max(waitMs, Throttle::waitMs(keys.at(index)));
                const BatchItem &item = items.at(index);
                QJsonObject request{{QStringLiteral("id"), QString::number(index)},
                                    {QStringLiteral("method"), QString::fromLatin1(item.verb)},
                                    {QStringLiteral("url"), paths.at(index)}};
                QJsonObject headers;
                for (const auto &[name, value] : item.headers) {
                    headers.insert(QString::fromLatin1(name), QString::fromLatin1(value));
                }
                if (!item.body.isUndefined()) {
                    request.insert(QStringLiteral("body"), item.body);
                    headers.insert(QStringLiteral("Content-Type"), QStringLiteral("application/json"));
                }
                if (!headers.isEmpty()) {
                    request.insert(QStringLiteral("headers"), headers);
                }
                requests.append(request);
            }
            if (waitMs > 0) {
                sleepCancellable(waitMs);
            }
            // All slots of these mailboxes, or none: no other request of the process runs against
            // them while the batch does
            while (!(m_cancel && m_cancel->load())) {
                qsizetype taken = 0;
                while (taken < mailboxes.size()
                       && Throttle::tryAcquire(mailboxes.at(taken), Throttle::maxConcurrentPerMailbox, Throttle::maxConcurrentPerMailbox)) {
                    ++taken;
                }
                if (taken == mailboxes.size()) {
                    break;
                }
                for (qsizetype i = 0; i < taken; ++i) {
                    Throttle::release(mailboxes.at(i), Throttle::maxConcurrentPerMailbox);
                }
                sleepCancellable(50);
            }
            if (m_cancel && m_cancel->load()) {
                for (qsizetype index : chunk) {
                    results[index] = {0, {}, QStringLiteral("cancelled"), {}};
                }
                continue;
            }

            const GraphResponse answer = postJson(QStringLiteral("/$batch"), {{QStringLiteral("requests"), requests}});
            for (const QString &mailbox : std::as_const(mailboxes)) {
                Throttle::release(mailbox, Throttle::maxConcurrentPerMailbox);
            }
            if (!answer.ok()) {
                for (qsizetype index : std::as_const(chunk)) {
                    results[index] = answer;
                }
                continue;
            }

            QSet<qsizetype> answered;
            const QJsonArray responses = answer.json().value(QLatin1String("responses")).toArray();
            for (const QJsonValue &value : responses) {
                const QJsonObject entry = value.toObject();
                bool ok = false;
                const qsizetype index = entry.value(QLatin1String("id")).toString().toLongLong(&ok);
                if (!ok || !chunk.contains(index)) {
                    continue;
                }
                answered.insert(index);
                GraphResponse result;
                result.status = entry.value(QLatin1String("status")).toInt();
                const QJsonObject headers = entry.value(QLatin1String("headers")).toObject();
                for (auto it = headers.begin(); it != headers.end(); ++it) {
                    result.headers.append({it.key().toLatin1(), it.value().toVariant().toString().toLatin1()});
                }
                const QJsonValue body = entry.value(QLatin1String("body"));
                if (body.isObject()) {
                    result.body = QJsonDocument(body.toObject()).toJson(QJsonDocument::Compact);
                } else if (body.isArray()) {
                    result.body = QJsonDocument(body.toArray()).toJson(QJsonDocument::Compact);
                } else if (body.isString()) {
                    // Non-JSON content comes base64-encoded
                    result.body = body.toString().toUtf8();
                }
                const bool throttled = result.status == 429 || result.status == 503;
                if (throttled && round + 1 < m_policy.maxThrottledAttempts) {
                    bool hasWait = false;
                    const int seconds = result.header("Retry-After").toInt(&hasWait);
                    const qint64 delayMs = hasWait ? qint64(std::clamp(seconds, 1, m_policy.maxRetryAfterSeconds)) * 1000
                                                   : std::min<qint64>(qint64(m_policy.backoffBaseMs) << std::min(round + 1, 20), m_policy.maxBackoffMs);
                    Throttle::note(keys.at(index), delayMs);
                    retryAfterMs = std::max(retryAfterMs, delayMs);
                    retry.append(index);
                    continue;
                }
                result.errorString = result.ok() ? QString() : QStringLiteral("batch item failed");
                results[index] = result;
            }
            for (qsizetype index : std::as_const(chunk)) {
                if (!answered.contains(index)) {
                    results[index] = {0, {}, QStringLiteral("no answer for this item in the batch response"), {}};
                }
            }
        }

        if (!retry.isEmpty()) {
            qCInfo(GRAPHCORE) << retry.size() << "batch item(s) throttled, sending them again in" << retryAfterMs << "ms";
            sleepCancellable(retryAfterMs);
        }
        pending = retry;
    }
    return results;
}

}
