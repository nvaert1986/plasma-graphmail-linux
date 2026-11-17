// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "blobstore.h"
#include "bodyfetcher.h"
#include "fakegraphserver.h"
#include "graphclient.h"
#include "operationqueue.h"
#include "throttle.h"
#include "tokenprovider.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace GraphMail;

namespace
{
class StaticTokens : public GraphCore::TokenProvider
{
public:
    QString accessToken(QString *, bool) override
    {
        return QStringLiteral("token");
    }
    QString cachedAccessToken() override
    {
        return QStringLiteral("token");
    }
};
}

class OperationsTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir dir;
    std::unique_ptr<FakeGraphServer> server;
    StaticTokens tokens;
    std::unique_ptr<GraphCore::GraphClient> client;
    std::unique_ptr<Store> store;
    std::unique_ptr<BlobStore> blobs;
    SyncContext ctx;
    qint64 inbox = 0;
    qint64 archive = 0;
    qint64 deleted = 0;
    qint64 message = 0;
    int counter = 0;
    qint64 clock = 1000000;

private Q_SLOTS:
    void init()
    {
        GraphCore::Throttle::resetForTests();
        server = std::make_unique<FakeGraphServer>();
        client = std::make_unique<GraphCore::GraphClient>(&tokens);
        client->setBaseUrl(server->baseUrl());
        GraphCore::RetryPolicy policy;
        policy.maxAttempts = 2;
        policy.backoffBaseMs = 10;
        policy.maxBackoffMs = 20;
        policy.maxThrottledAttempts = 2;
        client->setRetryPolicy(policy);
        store = std::make_unique<Store>();
        QString error;
        QVERIFY(store->open(dir.path() + QStringLiteral("/ops-%1.db").arg(++counter), Store::Mode::ReadWrite, &error));
        blobs = std::make_unique<BlobStore>(dir.path() + QStringLiteral("/blobs-%1").arg(counter));
        ctx = {};
        ctx.store = store.get();
        ctx.blobs = blobs.get();
        ctx.client = client.get();
        ctx.root = QStringLiteral("/me");
        ctx.now = [this] {
            return clock;
        };
        inbox = store->upsertFolder({0, QStringLiteral("F-inbox"), {}, QStringLiteral("Inbox"), QStringLiteral("inbox"), 0, 0});
        archive = store->upsertFolder({0, QStringLiteral("F-archive"), {}, QStringLiteral("Archive"), QStringLiteral("archive"), 0, 0});
        deleted = store->upsertFolder({0, QStringLiteral("F-deleted"), {}, QStringLiteral("Deleted Items"), QStringLiteral("deleteditems"), 0, 0});
        MessageRow m;
        m.graphId = QStringLiteral("m1");
        m.folder = inbox;
        m.received = clock - 1000;
        m.subject = QStringLiteral("Hello");
        m.changeKey = QStringLiteral("k1");
        QCOMPARE(store->upsertMessage(m), Store::Upsert::Inserted);
        message = store->messageByGraphId(QStringLiteral("m1"))->id;
    }

    void cleanup()
    {
        store.reset();
    }

    void markReadIsLocalThenSent()
    {
        server->handler = [](const FakeRequest &) {
            return jsonReply(200, R"({"id":"m1"})");
        };
        OperationQueue queue(&ctx);
        QSignalSpy completed(&queue, &OperationQueue::completed);
        QVERIFY(queue.markRead(message, true));
        // Shown at once, queued, marked pending
        QVERIFY(store->message(message)->isRead);
        QCOMPARE(store->pendingOperationCount(), 1);
        QCOMPARE(store->message(message)->pendingOp, 1);

        queue.process(2);
        QVERIFY(completed.wait(5000));
        QCOMPARE(server->requests.size(), 1);
        QCOMPARE(server->requests.first().verb, QByteArray("PATCH"));
        QCOMPARE(server->requests.first().target, QByteArray("/v1.0/me/messages/m1"));
        QCOMPARE(QJsonDocument::fromJson(server->requests.first().body).object().value(QLatin1String("isRead")).toBool(), true);
        QCOMPARE(store->pendingOperationCount(), 0);
        QCOMPARE(store->message(message)->pendingOp, 0);
    }

    // Move to Other: local at once, then inferenceClassification in Graph
    void moveToOther()
    {
        server->handler = [](const FakeRequest &) {
            return jsonReply(200, R"({"id":"m1"})");
        };
        OperationQueue queue(&ctx);
        QSignalSpy completed(&queue, &OperationQueue::completed);
        QVERIFY(store->message(message)->focused);
        QVERIFY(queue.setFocused(message, false));
        QVERIFY(!store->message(message)->focused);
        queue.process(2);
        QVERIFY(completed.wait(5000));
        QCOMPARE(QJsonDocument::fromJson(server->requests.first().body).object().value(QLatin1String("inferenceClassification")).toString(), QStringLiteral("other"));
        QCOMPARE(store->pendingOperationCount(), 0);
    }

    // A follow-up flag with dates: whole days in the PATCH
    void flagWithDates()
    {
        server->handler = [](const FakeRequest &) {
            return jsonReply(200, R"({"id":"m1"})");
        };
        OperationQueue queue(&ctx);
        QSignalSpy completed(&queue, &OperationQueue::completed);
        const qint64 start = QDateTime(QDate(2026, 11, 16), QTime(12, 0)).toMSecsSinceEpoch();
        const qint64 due = QDateTime(QDate(2026, 11, 20), QTime(12, 0)).toMSecsSinceEpoch();
        QVERIFY(queue.setFlag(message, 1, start, due));
        QCOMPARE(store->message(message)->flag, 1);
        queue.process(2);
        QVERIFY(completed.wait(5000));
        const QJsonObject flag = QJsonDocument::fromJson(server->requests.first().body).object().value(QLatin1String("flag")).toObject();
        QCOMPARE(flag.value(QLatin1String("flagStatus")).toString(), QStringLiteral("flagged"));
        QCOMPARE(flag.value(QLatin1String("startDateTime")).toObject().value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-11-16T00:00:00"));
        QCOMPARE(flag.value(QLatin1String("dueDateTime")).toObject().value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-11-20T00:00:00"));
    }

    void changeBackCancels()
    {
        OperationQueue queue(&ctx);
        QVERIFY(queue.markRead(message, true));
        QVERIFY(queue.markRead(message, false));
        QCOMPARE(store->pendingOperationCount(), 0);
        QVERIFY(!store->message(message)->isRead);
        QCOMPARE(store->message(message)->pendingOp, 0);
        // Two flag changes merge into one operation with the last value
        QVERIFY(queue.setFlag(message, 1));
        QVERIFY(queue.setFlag(message, 2));
        QCOMPARE(store->pendingOperationCount(), 1);
        QCOMPARE(store->message(message)->flag, 2);
    }

    void refusedChangeRollsBack()
    {
        server->handler = [](const FakeRequest &) {
            return jsonReply(403, R"({"error":{"code":"ErrorAccessDenied","message":"no"}})");
        };
        OperationQueue queue(&ctx);
        QSignalSpy failed(&queue, &OperationQueue::failed);
        QVERIFY(queue.markRead(message, true));
        queue.process(2);
        QVERIFY(failed.wait(5000));
        QVERIFY(!store->message(message)->isRead);
        QCOMPARE(store->pendingOperationCount(), 0);
    }

    void serverTroubleRetriesLater()
    {
        server->handler = [](const FakeRequest &) {
            return jsonReply(503, "{}", {{"Retry-After", "0"}});
        };
        OperationQueue queue(&ctx);
        QVERIFY(queue.setFlag(message, 1));
        queue.process(2);
        QTRY_COMPARE_WITH_TIMEOUT(queue.inFlight(), 0, 5000);
        // Still queued, still shown, not due before its retry time
        QCOMPARE(store->pendingOperationCount(), 1);
        QCOMPARE(store->message(message)->flag, 1);
        QVERIFY(store->dueOperations(clock, 10).isEmpty());
        QCOMPARE(store->dueOperations(clock + 31 * 1000, 10).size(), 1);
    }

    void queuedChangeSurvivesAGraphUpdate()
    {
        OperationQueue queue(&ctx);
        ctx.reapplyPending = [&queue](const QString &graphId) {
            queue.reapply(graphId);
        };
        QVERIFY(queue.markRead(message, true));
        // A delta page brings the message with isRead false (the change is not sent yet)
        MessageRow fromGraph = store->message(message).value();
        fromGraph.isRead = false;
        fromGraph.changeKey = QStringLiteral("k2");
        QCOMPARE(store->upsertMessage(fromGraph), Store::Upsert::Updated);
        ctx.reapplyPending(QStringLiteral("m1"));
        QVERIFY(store->message(message)->isRead);
    }

    void moveAndDelete()
    {
        server->handler = [](const FakeRequest &) {
            return jsonReply(201, R"({"id":"m1"})");
        };
        OperationQueue queue(&ctx);
        QSignalSpy completed(&queue, &OperationQueue::completed);
        QVERIFY(queue.move(message, archive));
        QCOMPARE(store->message(message)->folder, archive);
        queue.process(2);
        QVERIFY(completed.wait(5000));
        QCOMPARE(server->requests.first().target, QByteArray("/v1.0/me/messages/m1/move"));
        QCOMPARE(QJsonDocument::fromJson(server->requests.first().body).object().value(QLatin1String("destinationId")).toString(), QStringLiteral("F-archive"));

        // Delete: first to Deleted Items
        QVERIFY(queue.remove(message));
        QCOMPARE(store->message(message)->folder, deleted);
        queue.process(2);
        QVERIFY(completed.wait(5000));

        // From Deleted Items: permanently; gone at once, back when Graph refuses
        server->handler = [](const FakeRequest &) {
            return jsonReply(403, R"({"error":{"code":"ErrorAccessDenied"}})");
        };
        QSignalSpy failed(&queue, &OperationQueue::failed);
        QVERIFY(queue.remove(message));
        QVERIFY(!store->messageByGraphId(QStringLiteral("m1")).has_value());
        queue.process(2);
        QVERIFY(failed.wait(5000));
        QCOMPARE(server->requests.constLast().verb, QByteArray("DELETE"));
        const std::optional<MessageRow> back = store->messageByGraphId(QStringLiteral("m1"));
        QVERIFY(back.has_value());
        QCOMPARE(back->subject, QStringLiteral("Hello"));
        QCOMPARE(back->folder, deleted);
    }

    void moveOfAMessageGoneOnTheServer()
    {
        // 404 on a move: the message is gone in Exchange, its row goes too (no row kept for ever)
        server->handler = [](const FakeRequest &) {
            return jsonReply(404, R"({"error":{"code":"ErrorItemNotFound"}})");
        };
        OperationQueue queue(&ctx);
        QSignalSpy completed(&queue, &OperationQueue::completed);
        QVERIFY(queue.move(message, archive));
        queue.process(2);
        QVERIFY(completed.wait(5000));
        QVERIFY(!store->messageByGraphId(QStringLiteral("m1")).has_value());
        QCOMPARE(store->pendingOperationCount(), 0);
    }

    void bodiesAndInlineImages()
    {
        const QByteArray image("\x89PNG fake image bytes", 21);
        bool askedForHtml = false;
        server->handler = [&image, &askedForHtml](const FakeRequest &r) -> FakeReply {
            const QString target = QString::fromUtf8(r.target);
            if (target.startsWith(QLatin1String("/v1.0/me/messages/m1?"))) {
                askedForHtml = r.headers.value("prefer").contains("outlook.body-content-type=\"html\"");
                // No backslashes in raw strings: moc reads them as ordinary strings
                return jsonReply(200, R"({"body":{"contentType":"html","content":"<p>Hi <img src='cid:logo'></p>"},"hasAttachments":true})");
            }
            if (target.startsWith(QLatin1String("/v1.0/me/messages/m1/attachments?"))) {
                return jsonReply(200, R"({"value":[{"id":"a1","name":"logo.png","contentType":"image/png","size":21,"isInline":true},
                                                   {"id":"a2","name":"report.pdf","contentType":"application/pdf","size":900000,"isInline":false}]})");
            }
            if (target == QLatin1String("/v1.0/me/messages/m1/attachments/a1")) {
                const QJsonObject attachment{{QStringLiteral("id"), QStringLiteral("a1")},
                                             {QStringLiteral("contentId"), QStringLiteral("logo")},
                                             {QStringLiteral("contentBytes"), QString::fromLatin1(image.toBase64())}};
                return jsonReply(200, QJsonDocument(attachment).toJson(QJsonDocument::Compact));
            }
            return jsonReply(404, "{}");
        };
        BodyFetcher fetcher(&ctx);
        fetcher.setWindowDays(30);
        fetcher.process(2);
        QTRY_VERIFY_WITH_TIMEOUT(!store->message(message)->bodyBlob.isEmpty(), 5000);
        QString error;
        QCOMPARE(blobs->get(store->message(message)->bodyBlob, &error).value(), QByteArray("<p>Hi <img src='cid:logo'></p>"));
        QCOMPARE(store->message(message)->bodyType, 1);
        QVERIFY(askedForHtml);
        QTRY_VERIFY_WITH_TIMEOUT(!store->attachments(message).isEmpty() && !store->attachments(message).first().blob.isEmpty(), 5000);
        const QList<AttachmentRow> attachments = store->attachments(message);
        QCOMPARE(attachments.size(), 2);
        QCOMPARE(attachments.first().contentId, QStringLiteral("logo"));
        QCOMPARE(blobs->get(attachments.first().blob, &error).value(), image);
        // The PDF is not inline and attachments offline is off: metadata only
        QVERIFY(attachments.at(1).blob.isEmpty());
        QVERIFY(!server->requests.constLast().target.contains("a2"));

        // A month later the body left the 30-day window: the sweep drops it and its image
        clock += 31LL * 24 * 3600 * 1000;
        const QString bodyBlob = store->message(message)->bodyBlob;
        const QString orphan = blobs->put("left behind", &error);
        fetcher.sweep();
        QVERIFY(store->message(message)->bodyBlob.isEmpty());
        QVERIFY(!blobs->contains(bodyBlob));
        QVERIFY(!blobs->contains(attachments.first().blob));
        QVERIFY(!blobs->contains(orphan));
        QVERIFY(blobs->allIds().isEmpty());
    }

    void inlineImagesWithoutHasAttachments()
    {
        // Graph says hasAttachments false when a message only has inline images (a signature logo,
        // found on the user's real mail 2026-11-11): the cid: in the body must still fetch them
        server->handler = [](const FakeRequest &r) -> FakeReply {
            const QString target = QString::fromUtf8(r.target);
            if (target.startsWith(QLatin1String("/v1.0/me/messages/m1?"))) {
                return jsonReply(200, R"({"body":{"contentType":"html","content":"<p>Regards <img src='cid:logo@01'></p>"},"hasAttachments":false})");
            }
            if (target.startsWith(QLatin1String("/v1.0/me/messages/m1/attachments?"))) {
                return jsonReply(200, R"({"value":[{"id":"a1","name":"image001.png","contentType":"image/png","size":10,"isInline":true}]})");
            }
            if (target == QLatin1String("/v1.0/me/messages/m1/attachments/a1")) {
                return jsonReply(200, R"({"id":"a1","contentId":"logo@01","contentBytes":"aW1hZ2U="})");
            }
            return jsonReply(404, "{}");
        };
        BodyFetcher fetcher(&ctx);
        QSignalSpy ready(&fetcher, &BodyFetcher::bodyReady);
        fetcher.fetchNow(message);
        fetcher.process(1);
        QVERIFY(ready.wait(5000));
        // bodyReady only after the inline image is stored, so the pane shows it at once
        const QList<AttachmentRow> attachments = store->attachments(message);
        QCOMPARE(attachments.size(), 1);
        QCOMPARE(attachments.first().contentId, QStringLiteral("logo@01"));
        QString error;
        QCOMPARE(blobs->get(attachments.first().blob, &error).value(), QByteArray("image"));

        // A body cached before the fix (no attachment rows): opening it fetches the images
        store->replaceAttachments(message, {});
        ready.clear();
        fetcher.fetchNow(message);
        QVERIFY(ready.wait(5000));
        QCOMPARE(store->attachments(message).size(), 1);
        QVERIFY(!store->attachments(message).first().blob.isEmpty());
    }

    void openedMessageJumpsTheQueue()
    {
        server->handler = [](const FakeRequest &) {
            return jsonReply(200, R"({"body":{"contentType":"text","content":"plain"},"hasAttachments":false})");
        };
        BodyFetcher fetcher(&ctx);
        fetcher.setWindowDays(0); // no prefetch at all
        QSignalSpy ready(&fetcher, &BodyFetcher::bodyReady);
        fetcher.process(2);
        QTest::qWait(100);
        QVERIFY(server->requests.isEmpty());
        fetcher.fetchNow(message);
        fetcher.process(0);
        QVERIFY(ready.wait(5000));
        QCOMPARE(ready.first().first().toLongLong(), message);
        QCOMPARE(store->message(message)->bodyType, 2);
    }
};

QTEST_GUILESS_MAIN(OperationsTest)

#include "operationstest.moc"
