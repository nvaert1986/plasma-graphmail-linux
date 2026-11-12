// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "graphrequest.h"
#include "graphresponse.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

#include <atomic>
#include <functional>

class QFile;

namespace GraphCore
{

class TokenProvider;

struct RetryPolicy {
    int maxAttempts = 6;
    // Throttling (429/503) is not a failure of the request: it gets more patience
    int maxThrottledAttempts = 20;
    int maxRetryAfterSeconds = 120;
    // Backoff without Retry-After: base * 2^attempt, capped
    int backoffBaseMs = 1000;
    int maxBackoffMs = 60 * 1000;
    // Without any bytes moving for this long a transfer is given up (and retried). A dropped
    // connection gives no error by itself, so this decides how long a dead link goes unnoticed.
    int transferTimeoutMs = 2 * 60 * 1000;
};

// One request for GraphClient::runParallel. done() runs in the calling thread when the job
// has finished, including after retries and redirects.
struct ParallelJob {
    QByteArray verb = "GET";
    QString pathOrUrl;
    QByteArray body;
    HeaderList headers;
    QFile *sink = nullptr; // receives the body of a 2xx answer (downloads)
    std::function<void(const GraphResponse &)> done;
};

// One request inside a $batch call
struct BatchItem {
    QByteArray verb = "GET";
    // Relative to the version root: "/me/messages/{id}" (absolute Graph URLs are cut down)
    QString path;
    // Sent as JSON when not undefined
    QJsonValue body = QJsonValue::Undefined;
    HeaderList headers;
};

// Microsoft Graph client for one account. Each instance owns its network manager and must only be
// used from the thread that created it: the UI thread (asynchronous start()) or a sync worker or
// command-line tool (the blocking calls, which run a local event loop and so must not be used on
// the UI thread). Both go through the same GraphRequest rules.
class GraphClient : public QObject
{
    Q_OBJECT

public:
    explicit GraphClient(TokenProvider *tokens, QObject *parent = nullptr);
    // Ends the requests still running before the network manager goes
    ~GraphClient() override;

    static QString defaultBaseUrl();
    QString baseUrl() const;
    // Tests (a local fake server) and national clouds
    void setBaseUrl(const QString &url);

    // Throttling is kept per mailbox under this scope, normally the account id (see throttleKeyFor())
    QString throttleScope() const;
    void setThrottleScope(const QString &scope);

    void setUserAgent(const QString &userAgent);
    RetryPolicy retryPolicy() const;
    void setRetryPolicy(const RetryPolicy &policy);
    // Set from another thread to make waits and long transfers give up early (pause, shutdown).
    void setCancelFlag(const std::atomic_bool *flag);
    // Called with the number of bytes each time file content was received or an upload body
    // was accepted (transfer progress)
    void setTransferObserver(std::function<void(qint64)> observer);

    // Asynchronous: starts once control is back in the event loop, so connect to finished() first.
    // The request belongs to this client; deleteLater() it after reading the response.
    GraphRequest *start(const GraphRequestSpec &spec);

    // Blocking (local event loop)
    GraphResponse send(const GraphRequestSpec &spec);
    GraphResponse get(const QString &pathOrUrl, const HeaderList &headers = {});
    GraphResponse postJson(const QString &path, const QJsonObject &body, const HeaderList &headers = {});
    GraphResponse patchJson(const QString &path, const QJsonObject &body, const HeaderList &headers = {});
    GraphResponse put(const QString &path, const QByteArray &data, const HeaderList &headers = {});
    GraphResponse deleteResource(const QString &path, const HeaderList &headers = {});
    // Streams the body of a GET into target (follows redirects to download hosts)
    GraphResponse download(const QString &path, QIODevice *target);
    // PUT one fragment to a pre-authenticated upload-session URL; must not carry our token.
    GraphResponse uploadFragment(const QString &uploadUrl, const QByteArray &data, qint64 offset, qint64 total);
    // State (nextExpectedRanges) and cancellation of an upload session; no token either.
    GraphResponse uploadSessionStatus(const QString &uploadUrl);
    GraphResponse cancelUploadSession(const QString &uploadUrl);

    // Runs jobs with up to `concurrency` requests in flight (requests to one mailbox are capped at
    // four anyway). Returns when every job is done (or cancelled).
    void runParallel(QList<ParallelJob> jobs, int concurrency);

    // JSON batching: up to 20 requests per call. While a call runs it holds all concurrency slots of
    // every mailbox it touches: Exchange runs a batch's items concurrently, and in phase 0 one batch
    // of 20 same-mailbox requests at a time was never throttled, while three such batches at once
    // got 12 of 60 items throttled. Items answered 429/503 are sent again after their Retry-After.
    // Results come back in the order of `items`; an item whose batch call failed as a whole gets
    // that call's response.
    QList<GraphResponse> batch(const QList<BatchItem> &items);

    static constexpr int maxBatchSize = 20;

private:
    friend class GraphRequest;

    void sleepCancellable(qint64 milliseconds);
    QString graphHost() const;

    TokenProvider *m_tokens;
    QNetworkAccessManager m_nam;
    QString m_baseUrl;
    QString m_throttleScope;
    QString m_userAgent;
    RetryPolicy m_policy;
    const std::atomic_bool *m_cancel = nullptr;
    std::function<void(qint64)> m_observer;
};

}
