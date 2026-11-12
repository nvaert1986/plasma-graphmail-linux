// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fakegraphserver.h"
#include "graphclient.h"
#include "graphutil.h"
#include "throttle.h"
#include "tokenprovider.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

using namespace GraphCore;

namespace
{
class FakeTokens : public TokenProvider
{
public:
    QString accessToken(QString *error, bool forceRefresh) override
    {
        QMutexLocker locker(&mutex);
        ++calls;
        if (forceRefresh) {
            ++forced;
        }
        refreshThread = QThread::currentThread();
        if (fail) {
            *error = QStringLiteral("sign-in expired: test");
            return {};
        }
        return QStringLiteral("token-%1").arg(forced);
    }
    QString cachedAccessToken() override
    {
        QMutexLocker locker(&mutex);
        return cached ? QStringLiteral("token-%1").arg(forced) : QString();
    }

    QMutex mutex;
    int calls = 0;
    int forced = 0;
    bool cached = true;
    bool fail = false;
    QThread *refreshThread = nullptr;
};

// Fast retries: the tests check behaviour, not patience
RetryPolicy fastPolicy()
{
    RetryPolicy policy;
    policy.backoffBaseMs = 10;
    policy.maxBackoffMs = 50;
    policy.maxAttempts = 3;
    policy.maxThrottledAttempts = 4;
    policy.transferTimeoutMs = 5000;
    return policy;
}
}

class GraphClientTest : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<FakeGraphServer> server;
    std::unique_ptr<FakeTokens> tokens;
    std::unique_ptr<GraphClient> client;

private Q_SLOTS:
    void init()
    {
        Throttle::resetForTests();
        server = std::make_unique<FakeGraphServer>();
        tokens = std::make_unique<FakeTokens>();
        client = std::make_unique<GraphClient>(tokens.get());
        client->setBaseUrl(server->baseUrl());
        client->setThrottleScope(QStringLiteral("acc"));
        client->setRetryPolicy(fastPolicy());
    }

    void getSendsTokenAndHeaders()
    {
        server->handler = [](const FakeRequest &) {
            return jsonReply(200, R"({"value":[{"id":"a"}]})");
        };
        const GraphResponse response = client->get(QStringLiteral("/me/messages?$top=1"), {Prefer::header({Prefer::immutableId(), Prefer::maxPageSize(10)})});
        QVERIFY(response.ok());
        QCOMPARE(response.json().value(QLatin1String("value")).toArray().size(), 1);
        QCOMPARE(server->requests.size(), 1);
        const FakeRequest &request = server->requests.first();
        QCOMPARE(request.target, QByteArray("/v1.0/me/messages?$top=1"));
        QCOMPARE(request.headers.value("authorization"), QByteArray("Bearer token-0"));
        QCOMPARE(request.headers.value("prefer"), QByteArray("IdType=\"ImmutableId\", odata.maxpagesize=10"));
        QCOMPARE(response.header("content-type"), QByteArray("application/json"));
    }

    // A write the user waits for in a dialog: few attempts, then the answer (phase 7: a 500 on
    // a sharing change was retried for minutes while the dialog waited)
    void attemptsPerRequest()
    {
        server->handler = [](const FakeRequest &) {
            return jsonReply(500, R"({"error":{"code":"InternalServerError","message":"m"}})");
        };
        GraphRequestSpec spec{"POST", QStringLiteral("/me/calendar/calendarPermissions"), "{}", {}, true, nullptr};
        spec.maxAttempts = 2;
        const GraphResponse response = client->send(spec);
        QCOMPARE(response.status, 500);
        QCOMPARE(server->requests.size(), 2);
    }

    void notFoundIsNotRetried()
    {
        server->handler = [](const FakeRequest &) {
            return jsonReply(404, R"({"error":{"code":"ErrorItemNotFound","message":"gone"}})");
        };
        const GraphResponse response = client->get(QStringLiteral("/me/messages/x"));
        QCOMPARE(response.status, 404);
        QCOMPARE(response.graphErrorCode(), QStringLiteral("ErrorItemNotFound"));
        QCOMPARE(server->requests.size(), 1);
    }

    void throttlingWaitsForRetryAfter()
    {
        int calls = 0;
        server->handler = [&calls](const FakeRequest &) {
            return ++calls == 1 ? jsonReply(429, "{}", {{"Retry-After", "1"}}) : jsonReply(200, "{}");
        };
        QElapsedTimer timer;
        timer.start();
        const GraphResponse response = client->get(QStringLiteral("/me/mailFolders('inbox')/messages"));
        QVERIFY(response.ok());
        QCOMPARE(server->requests.size(), 2);
        QVERIFY2(timer.elapsed() >= 900, qPrintable(QString::number(timer.elapsed())));
    }

    void throttlingPausesOnlyThatMailbox()
    {
        Throttle::note(QStringLiteral("acc|me"), 1500);
        QElapsedTimer timer;
        timer.start();
        // Another mailbox and a non-mailbox resource go ahead at once
        QVERIFY(client->get(QStringLiteral("/users/shared@example.com/messages")).ok());
        QVERIFY(client->get(QStringLiteral("/drives/abc/root")).ok());
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
        // The throttled mailbox waits
        QVERIFY(client->get(QStringLiteral("/me/messages")).ok());
        QVERIFY2(timer.elapsed() >= 1300, qPrintable(QString::number(timer.elapsed())));
    }

    void refreshesTokenOnceAfter401()
    {
        server->handler = [](const FakeRequest &request) {
            return request.headers.value("authorization") == "Bearer token-1" ? jsonReply(200, "{}") : jsonReply(401, "{}");
        };
        QVERIFY(client->get(QStringLiteral("/me")).ok());
        QCOMPARE(tokens->forced, 1);
        QCOMPARE(server->requests.size(), 2);

        // A second 401 after the refresh is final
        server->handler = [](const FakeRequest &) {
            return jsonReply(401, "{}");
        };
        QCOMPARE(client->get(QStringLiteral("/me")).status, 401);
        QCOMPARE(tokens->forced, 2);
    }

    void tokenNeverLeavesGraphHost()
    {
        const QString blob = server->otherHostUrl(QStringLiteral("/blob"));
        server->handler = [blob](const FakeRequest &request) {
            if (request.target == "/blob") {
                return FakeReply{200, {{"Content-Type", "application/octet-stream"}}, "file content", 0, false};
            }
            return FakeReply{302, {{"Location", blob.toLatin1()}}, {}, 0, false};
        };
        QBuffer sink;
        sink.open(QIODevice::ReadWrite);
        const GraphResponse response = client->download(QStringLiteral("/drives/a/items/b/content"), &sink);
        QVERIFY(response.ok());
        QCOMPARE(sink.data(), QByteArray("file content"));
        QCOMPARE(server->requests.size(), 2);
        QVERIFY(server->requests.at(0).headers.contains("authorization"));
        QVERIFY(!server->requests.at(1).headers.contains("authorization"));
    }

    void droppedConnectionIsRetried()
    {
        int calls = 0;
        server->handler = [&calls](const FakeRequest &) {
            FakeReply reply = jsonReply(200, R"({"ok":true})");
            reply.drop = ++calls == 1;
            return reply;
        };
        const GraphResponse response = client->get(QStringLiteral("/me"));
        QVERIFY(response.ok());
        QCOMPARE(server->requests.size(), 2);
    }

    void networkErrorAfterRetriesIsStatusZero()
    {
        server->handler = [](const FakeRequest &) {
            FakeReply reply;
            reply.drop = true;
            return reply;
        };
        const GraphResponse response = client->get(QStringLiteral("/me"));
        QVERIFY(response.isNetworkError());
        // Qt itself sends a request once more when the connection closes without an answer, so
        // each of our attempts may reach the server twice
        const int attempts = fastPolicy().maxAttempts;
        QVERIFY2(server->requests.size() >= attempts && server->requests.size() <= 2 * attempts, qPrintable(QString::number(server->requests.size())));
    }

    void mailboxConcurrencyIsCapped()
    {
        server->handler = [](const FakeRequest &) {
            FakeReply reply = jsonReply(200, "{}");
            reply.delayMs = 150;
            return reply;
        };
        QList<ParallelJob> jobs;
        int done = 0;
        for (int i = 0; i < 10; ++i) {
            ParallelJob job;
            job.pathOrUrl = QStringLiteral("/me/messages/%1").arg(i);
            job.done = [&done](const GraphResponse &response) {
                QVERIFY(response.ok());
                ++done;
            };
            jobs.append(job);
        }
        client->runParallel(jobs, 8);
        QCOMPARE(done, 10);
        QVERIFY2(server->maxActive <= Throttle::maxConcurrentPerMailbox, qPrintable(QString::number(server->maxActive)));
        QCOMPARE(Throttle::running(QStringLiteral("acc|me")), 0);

        // Not a mailbox (OneDrive): no cap of our own
        server->maxActive = 0;
        for (ParallelJob &job : jobs) {
            job.pathOrUrl.replace(QLatin1String("/me/messages/"), QLatin1String("/drives/d/items/"));
        }
        client->runParallel(jobs, 8);
        QVERIFY2(server->maxActive > Throttle::maxConcurrentPerMailbox, qPrintable(QString::number(server->maxActive)));
    }

    void asyncRequestRefreshesOffTheCallingThread()
    {
        tokens->cached = false;
        server->handler = [](const FakeRequest &) {
            return jsonReply(200, R"({"id":"me"})");
        };
        GraphRequest *request = client->start({"GET", QStringLiteral("/me"), {}, {}, true, nullptr});
        QSignalSpy spy(request, &GraphRequest::finished);
        QVERIFY(spy.wait(5000));
        QVERIFY(request->response().ok());
        QCOMPARE(tokens->calls, 1);
        QVERIFY(tokens->refreshThread != QThread::currentThread());
        request->deleteLater();
    }

    void tokenFailureEndsTheRequest()
    {
        tokens->cached = false;
        tokens->fail = true;
        const GraphResponse response = client->get(QStringLiteral("/me"));
        QVERIFY(response.isNetworkError());
        QVERIFY(response.errorString.contains(QLatin1String("sign-in expired")));
        QVERIFY(server->requests.isEmpty());
    }

    void cancelStopsAWait()
    {
        std::atomic_bool cancel{false};
        client->setCancelFlag(&cancel);
        Throttle::note(QStringLiteral("acc|me"), 60 * 1000);
        QTimer::singleShot(300, client.get(), [&cancel] {
            cancel = true;
        });
        QElapsedTimer timer;
        timer.start();
        const GraphResponse response = client->get(QStringLiteral("/me/messages"));
        QCOMPARE(response.errorString, QStringLiteral("cancelled"));
        QVERIFY(timer.elapsed() < 2000);
        QVERIFY(server->requests.isEmpty());
    }

    // Deleting the client with a request on the wire must not touch the freed network reply
    // (ASan found it when a mailbox was removed in the middle of a sync)
    void deletingTheClientEndsRunningRequests()
    {
        server->handler = [](const FakeRequest &) {
            FakeReply reply = jsonReply(200, "{}");
            reply.delayMs = 2000;
            return reply;
        };
        GraphRequest *request = client->start({"GET", QStringLiteral("/me"), {}, {}, true, nullptr});
        QPointer<GraphRequest> guard(request);
        QTest::qWait(300);
        client.reset();
        QVERIFY(guard.isNull());
        QTest::qWait(100);
    }

    void abortFinishesOnce()
    {
        server->handler = [](const FakeRequest &) {
            FakeReply reply = jsonReply(200, "{}");
            reply.delayMs = 2000;
            return reply;
        };
        GraphRequest *request = client->start({"GET", QStringLiteral("/me"), {}, {}, true, nullptr});
        QSignalSpy spy(request, &GraphRequest::finished);
        QTimer::singleShot(200, request, &GraphRequest::abort);
        QVERIFY(spy.wait(3000));
        QCOMPARE(request->response().errorString, QStringLiteral("cancelled"));
        QTest::qWait(100);
        QCOMPARE(spy.count(), 1);
        request->deleteLater();
    }

    void batchSplitsPerMailboxAndRetriesThrottledItems()
    {
        int posts = 0;
        bool throttledOnce = false;
        server->handler = [&](const FakeRequest &request) {
            if (request.target != "/v1.0/$batch") {
                return jsonReply(400, "{}");
            }
            ++posts;
            const QJsonArray requests = QJsonDocument::fromJson(request.body).object().value(QLatin1String("requests")).toArray();
            QJsonArray responses;
            for (const QJsonValue &value : requests) {
                const QJsonObject item = value.toObject();
                const QString id = item.value(QLatin1String("id")).toString();
                if (id == QLatin1String("3") && !throttledOnce) {
                    throttledOnce = true;
                    responses.append(QJsonObject{{QStringLiteral("id"), id},
                                                 {QStringLiteral("status"), 429},
                                                 {QStringLiteral("headers"), QJsonObject{{QStringLiteral("Retry-After"), QStringLiteral("1")}}},
                                                 {QStringLiteral("body"), QJsonObject{}}});
                    continue;
                }
                responses.append(QJsonObject{{QStringLiteral("id"), id},
                                             {QStringLiteral("status"), 200},
                                             {QStringLiteral("body"), QJsonObject{{QStringLiteral("url"), item.value(QLatin1String("url"))}}}});
            }
            // Answers in reverse order: the client must match them by id
            QJsonArray reversed;
            for (qsizetype i = responses.size() - 1; i >= 0; --i) {
                reversed.append(responses.at(i));
            }
            return jsonReply(200, QJsonDocument(QJsonObject{{QStringLiteral("responses"), reversed}}).toJson(QJsonDocument::Compact));
        };

        QList<BatchItem> items;
        for (int i = 0; i < 10; ++i) {
            items.append({"GET", QStringLiteral("/me/messages/m%1").arg(i), QJsonValue::Undefined, {}});
        }
        for (int i = 0; i < 25; ++i) {
            items.append({"GET", QStringLiteral("/drives/d/items/i%1").arg(i), QJsonValue::Undefined, {}});
        }
        const QList<GraphResponse> results = client->batch(items);
        QCOMPARE(results.size(), items.size());
        for (qsizetype i = 0; i < items.size(); ++i) {
            QVERIFY2(results.at(i).ok(), qPrintable(QString::number(i)));
            QCOMPARE(results.at(i).json().value(QLatin1String("url")).toString(), items.at(i).path);
        }
        QVERIFY(throttledOnce);
        // 35 items at 20 per call: 2 calls, plus 1 for the retry
        QCOMPARE(posts, 3);
        // The batch gave its mailbox slots back
        QCOMPARE(Throttle::running(QStringLiteral("acc|me")), 0);
    }

    void batchWaitsForTheMailboxToBeFree()
    {
        // Another request holds a slot of the mailbox: the batch waits until it is released
        QVERIFY(Throttle::tryAcquire(QStringLiteral("acc|me"), Throttle::maxConcurrentPerMailbox));
        QTimer::singleShot(400, client.get(), [] {
            Throttle::release(QStringLiteral("acc|me"));
        });
        server->handler = [](const FakeRequest &) {
            return jsonReply(200, R"({"responses":[{"id":"0","status":200,"body":{}}]})");
        };
        QElapsedTimer timer;
        timer.start();
        const QList<GraphResponse> results = client->batch({{"GET", QStringLiteral("/me/messages/a"), QJsonValue::Undefined, {}}});
        QVERIFY(results.first().ok());
        QVERIFY2(timer.elapsed() >= 350, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(Throttle::running(QStringLiteral("acc|me")), 0);
    }

    void batchCallFailureAppliesToItsItems()
    {
        server->handler = [](const FakeRequest &) {
            return jsonReply(403, R"({"error":{"code":"Forbidden","message":"no"}})");
        };
        const QList<GraphResponse> results = client->batch({{"GET", QStringLiteral("/me"), QJsonValue::Undefined, {}}});
        QCOMPARE(results.size(), 1);
        QCOMPARE(results.first().status, 403);
    }
};

QTEST_GUILESS_MAIN(GraphClientTest)

#include "graphclienttest.moc"
