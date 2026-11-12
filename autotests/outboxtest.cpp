// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "blobstore.h"
#include "fakegraphserver.h"
#include "graphclient.h"
#include "outboxengine.h"
#include "store.h"
#include "throttle.h"
#include "tokenprovider.h"

#include <QDateTime>
#include <QTimeZone>
#include <QJsonArray>
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

QJsonObject bodyOf(const FakeRequest &r)
{
    return QJsonDocument::fromJson(r.body).object();
}

QByteArray pathOf(const FakeRequest &r)
{
    const qsizetype query = r.target.indexOf('?');
    return query < 0 ? r.target : r.target.left(query);
}

FakeReply json(int status, const QJsonObject &body, QList<QPair<QByteArray, QByteArray>> headers = {})
{
    return jsonReply(status, QJsonDocument(body).toJson(QJsonDocument::Compact), headers);
}
}

class OutboxTest : public QObject
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
    int counter = 0;
    static constexpr qint64 fixedNow = 1000000;

    QString url(const QString &path) const
    {
        return server->baseUrl() + path;
    }

    // Runs the engine until the message is saved, sent, failed or discarded, or the run stops
    QString runEngine(OutboxEngine &engine)
    {
        QSignalSpy changed(&engine, &OutboxEngine::changed);
        engine.process();
        for (int i = 0; i < 200 && changed.isEmpty() && engine.busy(); ++i) {
            changed.wait(50);
        }
        return changed.isEmpty() ? QString() : changed.constLast().at(1).toString();
    }

    Outgoing message(const QString &uuid)
    {
        Outgoing o;
        o.uuid = uuid;
        o.to = {{QStringLiteral("Test Person"), QStringLiteral("person@example.com")}};
        o.subject = QStringLiteral("[TEST] Subject");
        o.body = QStringLiteral("<p>Hello</p>");
        return o;
    }

    OutgoingAttachment attachment(const QString &key, const QString &name, const QByteArray &data)
    {
        QString error;
        OutgoingAttachment a;
        a.key = key;
        a.name = name;
        a.contentType = QStringLiteral("application/octet-stream");
        a.blob = blobs->put(data, &error);
        a.size = data.size();
        return a;
    }

    QStringList calls() const
    {
        QStringList list;
        for (const FakeRequest &r : server->requests) {
            list.append(QString::fromLatin1(r.verb) + QLatin1Char(' ') + QString::fromLatin1(pathOf(r)));
        }
        return list;
    }

private Q_SLOTS:
    void init()
    {
        GraphCore::Throttle::resetForTests();
        server = std::make_unique<FakeGraphServer>();
        client = std::make_unique<GraphCore::GraphClient>(&tokens);
        client->setBaseUrl(server->baseUrl());
        GraphCore::RetryPolicy policy;
        policy.maxAttempts = 1;
        policy.backoffBaseMs = 10;
        policy.maxBackoffMs = 20;
        client->setRetryPolicy(policy);
        store = std::make_unique<Store>();
        QString error;
        QVERIFY(store->open(dir.path() + QStringLiteral("/outbox-%1.db").arg(++counter), Store::Mode::ReadWrite, &error));
        QCOMPARE(store->version(), Store::schemaVersion);
        blobs = std::make_unique<BlobStore>(dir.path() + QStringLiteral("/blobs-%1").arg(counter));
        ctx = {};
        ctx.store = store.get();
        ctx.blobs = blobs.get();
        ctx.client = client.get();
        ctx.root = QStringLiteral("/me");
        ctx.now = [] {
            return fixedNow;
        };
    }

    void cleanup()
    {
        store.reset();
    }

    void newMessageWithAttachmentsIsSent()
    {
        Outgoing o = message(QStringLiteral("u1"));
        o.send = true;
        o.attachments = {attachment(QStringLiteral("k1"), QStringLiteral("small.txt"), QByteArray("small content")),
                         attachment(QStringLiteral("k2"), QStringLiteral("big.bin"), QByteArray(5 * 1024 * 1024, 'x'))};
        const QString smallBlob = o.attachments.at(0).blob;
        QVERIFY(store->saveOutgoing(o));

        QList<QByteArray> ranges;
        bool tokenOnUpload = false;
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            const QByteArray path = pathOf(r);
            if (r.verb == "POST" && path == "/v1.0/me/messages") {
                return json(201, {{QStringLiteral("id"), QStringLiteral("d1")}, {QStringLiteral("internetMessageId"), QStringLiteral("<m1@example.com>")}});
            }
            if (r.verb == "GET" && path == "/v1.0/me/messages/d1/attachments") {
                return json(200, {{QStringLiteral("value"), QJsonArray()}});
            }
            if (r.verb == "POST" && path == "/v1.0/me/messages/d1/attachments") {
                return json(201, {{QStringLiteral("id"), QStringLiteral("a1")}});
            }
            if (r.verb == "POST" && path == "/v1.0/me/messages/d1/attachments/createUploadSession") {
                return json(201, {{QStringLiteral("uploadUrl"), url(QStringLiteral("/upload/1"))}});
            }
            if (r.verb == "PUT" && path == "/v1.0/upload/1") {
                tokenOnUpload = tokenOnUpload || r.headers.contains("authorization");
                ranges.append(r.headers.value("content-range"));
                if (!r.headers.value("content-range").endsWith(QByteArrayLiteral("5242879/5242880"))) {
                    return json(200, {{QStringLiteral("nextExpectedRanges"), QJsonArray{QStringLiteral("3932160-")}}});
                }
                return json(201, {}, {{"Location", "https://outlook.office.com/api/v2.0/Users('u')/Messages('d1')/Attachments('a2')"}});
            }
            if (r.verb == "POST" && path == "/v1.0/me/messages/d1/send") {
                return jsonReply(202, "");
            }
            return jsonReply(404, R"({"error":{"code":"unexpected"}})");
        };

        OutboxEngine engine(&ctx);
        QCOMPARE(runEngine(engine), QStringLiteral("sent"));
        QCOMPARE(calls(),
                 (QStringList{QStringLiteral("POST /v1.0/me/messages"),
                              QStringLiteral("GET /v1.0/me/messages/d1/attachments"),
                              QStringLiteral("POST /v1.0/me/messages/d1/attachments"),
                              QStringLiteral("POST /v1.0/me/messages/d1/attachments/createUploadSession"),
                              QStringLiteral("PUT /v1.0/upload/1"),
                              QStringLiteral("PUT /v1.0/upload/1"),
                              QStringLiteral("POST /v1.0/me/messages/d1/send")}));
        // The new message carries its content at once
        const QJsonObject created = bodyOf(server->requests.at(0));
        QCOMPARE(created.value(QLatin1String("subject")).toString(), QStringLiteral("[TEST] Subject"));
        QCOMPARE(created.value(QLatin1String("toRecipients")).toArray().first().toObject().value(QLatin1String("emailAddress")).toObject().value(QLatin1String("address")).toString(),
                 QStringLiteral("person@example.com"));
        QVERIFY(!created.contains(QLatin1String("from")));
        const QJsonObject small = bodyOf(server->requests.at(2));
        QCOMPARE(QByteArray::fromBase64(small.value(QLatin1String("contentBytes")).toString().toLatin1()), QByteArray("small content"));
        // Chunks of 12 x 320 KiB, without our token
        QCOMPARE(ranges, (QList<QByteArray>{"bytes 0-3932159/5242880", "bytes 3932160-5242879/5242880"}));
        QVERIFY(!tokenOnUpload);
        // Sent: the record and its content are gone
        QVERIFY(!store->outgoing(QStringLiteral("u1")));
        QVERIFY(!blobs->contains(smallBlob));
    }

    void replyStaysDraftThenGetsAnAttachment()
    {
        Outgoing o = message(QStringLiteral("u2"));
        o.kind = QStringLiteral("reply");
        o.source = QStringLiteral("s1");
        QVERIFY(store->saveOutgoing(o));
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            const QByteArray path = pathOf(r);
            if (r.verb == "POST" && path == "/v1.0/me/messages/s1/createReply") {
                return json(201, {{QStringLiteral("id"), QStringLiteral("d2")}});
            }
            if (r.verb == "PATCH" && path == "/v1.0/me/messages/d2") {
                return json(200, {{QStringLiteral("id"), QStringLiteral("d2")}, {QStringLiteral("internetMessageId"), QStringLiteral("<m2@example.com>")}});
            }
            if (r.verb == "GET" && path == "/v1.0/me/messages/d2/attachments") {
                return json(200, {{QStringLiteral("value"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("inline1")}, {QStringLiteral("name"), QStringLiteral("logo.png")}}}}});
            }
            if (r.verb == "POST" && path == "/v1.0/me/messages/d2/attachments") {
                return json(201, {{QStringLiteral("id"), QStringLiteral("a3")}});
            }
            return jsonReply(404, R"({"error":{"code":"unexpected"}})");
        };
        OutboxEngine engine(&ctx);
        QCOMPARE(runEngine(engine), QStringLiteral("saved"));
        QCOMPARE(calls(), (QStringList{QStringLiteral("POST /v1.0/me/messages/s1/createReply"), QStringLiteral("PATCH /v1.0/me/messages/d2")}));
        // The PATCH sets what the user wrote
        QCOMPARE(bodyOf(server->requests.at(1)).value(QLatin1String("body")).toObject().value(QLatin1String("content")).toString(), QStringLiteral("<p>Hello</p>"));
        std::optional<Outgoing> stored = store->outgoing(QStringLiteral("u2"));
        QCOMPARE(int(stored->state), int(Outgoing::State::Saved));
        QCOMPARE(stored->draftId, QStringLiteral("d2"));
        QCOMPARE(stored->internetMessageId, QStringLiteral("<m2@example.com>"));

        // The composer saves again with an attachment: only the PATCH and the upload; the inline
        // image Exchange copied into the reply stays
        Outgoing next = message(QStringLiteral("u2"));
        next.kind = QStringLiteral("reply");
        next.source = QStringLiteral("s1");
        next.attachments = {attachment(QStringLiteral("k3"), QStringLiteral("notes.txt"), QByteArray("notes"))};
        Outgoing merged = Outgoing::merged(*stored, next);
        merged.state = Outgoing::State::Pending;
        QVERIFY(store->saveOutgoing(merged));
        server->requests.clear();
        QCOMPARE(runEngine(engine), QStringLiteral("saved"));
        QCOMPARE(calls(),
                 (QStringList{QStringLiteral("PATCH /v1.0/me/messages/d2"),
                              QStringLiteral("GET /v1.0/me/messages/d2/attachments"),
                              QStringLiteral("POST /v1.0/me/messages/d2/attachments")}));
        QCOMPARE(store->outgoing(QStringLiteral("u2"))->attachments.first().graphId, QStringLiteral("a3"));
    }

    void forwardDropsARemovedOriginalAttachment()
    {
        Outgoing o = message(QStringLiteral("u3"));
        o.kind = QStringLiteral("forward");
        o.source = QStringLiteral("s2");
        o.dropSourceAttachments = {QStringLiteral("drop.pdf")};
        QVERIFY(store->saveOutgoing(o));
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            const QByteArray path = pathOf(r);
            if (r.verb == "POST" && path == "/v1.0/me/messages/s2/createForward") {
                return json(201, {{QStringLiteral("id"), QStringLiteral("d3")}});
            }
            if (r.verb == "PATCH") {
                return json(200, {{QStringLiteral("id"), QStringLiteral("d3")}});
            }
            if (r.verb == "GET" && path == "/v1.0/me/messages/d3/attachments") {
                return json(200,
                            {{QStringLiteral("value"),
                              QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("x1")}, {QStringLiteral("name"), QStringLiteral("keep.pdf")}},
                                         QJsonObject{{QStringLiteral("id"), QStringLiteral("x2")}, {QStringLiteral("name"), QStringLiteral("drop.pdf")}}}}});
            }
            if (r.verb == "DELETE") {
                return jsonReply(204, "");
            }
            return jsonReply(404, R"({"error":{"code":"unexpected"}})");
        };
        OutboxEngine engine(&ctx);
        QCOMPARE(runEngine(engine), QStringLiteral("saved"));
        QVERIFY(calls().contains(QStringLiteral("DELETE /v1.0/me/messages/d3/attachments/x2")));
        QVERIFY(!calls().join(QLatin1Char(' ')).contains(QLatin1String("x1")));
        QVERIFY(store->outgoing(QStringLiteral("u3"))->dropSourceAttachments.isEmpty());
    }

    void sendAsDeniedKeepsTheDraft()
    {
        Outgoing o = message(QStringLiteral("u4"));
        o.send = true;
        o.fromAddress = QStringLiteral("shared@example.com");
        o.fromMailbox = QStringLiteral("oid-shared");
        QVERIFY(store->saveOutgoing(o));
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            const QByteArray path = pathOf(r);
            if (r.verb == "POST" && path == "/v1.0/me/messages") {
                return json(201, {{QStringLiteral("id"), QStringLiteral("d4")}});
            }
            if (r.verb == "POST" && path == "/v1.0/me/messages/d4/send") {
                return jsonReply(403, R"({"error":{"code":"ErrorSendAsDenied","message":"The user account which was used to submit this request does not have the right to send mail on behalf of the specified sending account."}})");
            }
            return jsonReply(404, R"({"error":{"code":"unexpected"}})");
        };
        OutboxEngine engine(&ctx);
        QSignalSpy learned(&engine, &OutboxEngine::sendRightLearned);
        QCOMPARE(runEngine(engine), QStringLiteral("failed"));
        // From went along in the new draft
        QCOMPARE(bodyOf(server->requests.at(0)).value(QLatin1String("from")).toObject().value(QLatin1String("emailAddress")).toObject().value(QLatin1String("address")).toString(),
                 QStringLiteral("shared@example.com"));
        QCOMPARE(learned.size(), 1);
        QCOMPARE(learned.first().at(0).toString(), QStringLiteral("oid-shared"));
        QCOMPARE(learned.first().at(1).toString(), QStringLiteral("denied"));
        const std::optional<Outgoing> stored = store->outgoing(QStringLiteral("u4"));
        QCOMPARE(int(stored->state), int(Outgoing::State::Failed));
        QVERIFY(!stored->send);
        QVERIFY(stored->lastError.contains(QLatin1String("ErrorSendAsDenied")));
        QCOMPARE(stored->draftId, QStringLiteral("d4"));
        // A failed one is not retried by itself
        server->requests.clear();
        engine.process();
        QVERIFY(!engine.busy());
        QVERIFY(server->requests.isEmpty());
    }

    void learnsOnBehalfFromTheSentCopy()
    {
        Outgoing o = message(QStringLiteral("u5"));
        o.send = true;
        o.fromAddress = QStringLiteral("shared@example.com");
        o.fromMailbox = QStringLiteral("oid-shared");
        QVERIFY(store->saveOutgoing(o));
        int lookups = 0;
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            const QByteArray path = pathOf(r);
            if (r.verb == "POST" && path == "/v1.0/me/messages") {
                return json(201, {{QStringLiteral("id"), QStringLiteral("d5")}, {QStringLiteral("internetMessageId"), QStringLiteral("<m5@example.com>")}});
            }
            if (r.verb == "POST" && path == "/v1.0/me/messages/d5/send") {
                return jsonReply(202, "");
            }
            if (r.verb == "GET" && path == "/v1.0/me/mailFolders/sentitems/messages") {
                // Not there yet the first time
                if (++lookups == 1) {
                    return json(200, {{QStringLiteral("value"), QJsonArray()}});
                }
                auto address = [](const QString &a) {
                    return QJsonObject{{QStringLiteral("emailAddress"), QJsonObject{{QStringLiteral("address"), a}}}};
                };
                return json(200,
                            {{QStringLiteral("value"),
                              QJsonArray{QJsonObject{{QStringLiteral("from"), address(QStringLiteral("shared@example.com"))},
                                                     {QStringLiteral("sender"), address(QStringLiteral("me@example.com"))}}}}});
            }
            return jsonReply(404, R"({"error":{"code":"unexpected"}})");
        };
        OutboxEngine engine(&ctx);
        engine.setLearnDelay(10, 3);
        QSignalSpy learned(&engine, &OutboxEngine::sendRightLearned);
        QCOMPARE(runEngine(engine), QStringLiteral("sent"));
        QVERIFY(learned.wait(3000));
        QCOMPARE(learned.first().at(1).toString(), QStringLiteral("behalf"));
        QCOMPARE(lookups, 2);
        // The filter asks for the message by its internet message id
        QVERIFY(QUrl::fromPercentEncoding(server->requests.constLast().target).contains(QLatin1String("internetMessageId eq '<m5@example.com>'")));
    }

    void discardDeletesTheDraft()
    {
        Outgoing o = message(QStringLiteral("u6"));
        o.draftId = QStringLiteral("d6");
        o.state = Outgoing::State::Discard;
        o.attachments = {attachment(QStringLiteral("k6"), QStringLiteral("a.txt"), QByteArray("a"))};
        const QString blob = o.attachments.first().blob;
        QVERIFY(store->saveOutgoing(o));
        server->handler = [&](const FakeRequest &) -> FakeReply {
            return jsonReply(204, "");
        };
        OutboxEngine engine(&ctx);
        QCOMPARE(runEngine(engine), QStringLiteral("discarded"));
        QCOMPARE(calls(), (QStringList{QStringLiteral("DELETE /v1.0/me/messages/d6")}));
        QVERIFY(!store->outgoing(QStringLiteral("u6")));
        QVERIFY(!blobs->contains(blob));
    }

    void offlineRetriesLater()
    {
        Outgoing o = message(QStringLiteral("u7"));
        o.send = true;
        QVERIFY(store->saveOutgoing(o));
        server->handler = [&](const FakeRequest &) -> FakeReply {
            FakeReply reply;
            reply.drop = true;
            return reply;
        };
        OutboxEngine engine(&ctx);
        QSignalSpy network(&engine, &OutboxEngine::networkError);
        QCOMPARE(runEngine(engine), QString());
        QTRY_VERIFY(!engine.busy());
        QCOMPARE(network.size(), 1);
        const std::optional<Outgoing> stored = store->outgoing(QStringLiteral("u7"));
        QCOMPARE(int(stored->state), int(Outgoing::State::Pending));
        QVERIFY(stored->send);
        QCOMPARE(stored->attempts, 1);
        QVERIFY(stored->nextAttempt > fixedNow);
        // Not due yet
        server->requests.clear();
        engine.process();
        QVERIFY(!engine.busy());
    }

    void draftDeletedInOutlookIsCreatedAgain()
    {
        Outgoing o = message(QStringLiteral("u8"));
        o.draftId = QStringLiteral("gone");
        o.attachments = {attachment(QStringLiteral("k8"), QStringLiteral("b.txt"), QByteArray("b"))};
        o.attachments.first().graphId = QStringLiteral("old");
        QVERIFY(store->saveOutgoing(o));
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            const QByteArray path = pathOf(r);
            if (r.verb == "PATCH" && path == "/v1.0/me/messages/gone") {
                return jsonReply(404, R"({"error":{"code":"ErrorItemNotFound"}})");
            }
            if (r.verb == "POST" && path == "/v1.0/me/messages") {
                return json(201, {{QStringLiteral("id"), QStringLiteral("d8")}});
            }
            if (r.verb == "GET") {
                return json(200, {{QStringLiteral("value"), QJsonArray()}});
            }
            if (r.verb == "POST" && path == "/v1.0/me/messages/d8/attachments") {
                return json(201, {{QStringLiteral("id"), QStringLiteral("a8")}});
            }
            return jsonReply(404, R"({"error":{"code":"unexpected"}})");
        };
        OutboxEngine engine(&ctx);
        QCOMPARE(runEngine(engine), QStringLiteral("saved"));
        const std::optional<Outgoing> stored = store->outgoing(QStringLiteral("u8"));
        QCOMPARE(stored->draftId, QStringLiteral("d8"));
        QCOMPARE(stored->attachments.first().graphId, QStringLiteral("a8"));
    }

    // Changed in Outlook since Graph Mail saved it (412 on If-Match): Outlook's version stays and
    // this one becomes a second draft (docs/PHASE8.md)
    void draftChangedInOutlookTooKeepsBoth()
    {
        Outgoing o = message(QStringLiteral("u9"));
        o.draftId = QStringLiteral("d9");
        o.draftEtag = QStringLiteral("v1");
        QVERIFY(store->saveOutgoing(o));
        QByteArray sentIfMatch;
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            const QByteArray path = pathOf(r);
            if (r.verb == "PATCH" && path == "/v1.0/me/messages/d9") {
                sentIfMatch = r.headers.value("if-match");
                return jsonReply(412, R"({"error":{"code":"ErrorIrresolvableConflict"}})");
            }
            if (r.verb == "POST" && path == "/v1.0/me/messages") {
                return json(201, {{QStringLiteral("id"), QStringLiteral("d10")}, {QStringLiteral("@odata.etag"), QStringLiteral("v5")}});
            }
            if (r.verb == "GET") {
                return json(200, {{QStringLiteral("value"), QJsonArray()}});
            }
            return jsonReply(404, R"({"error":{"code":"unexpected"}})");
        };
        OutboxEngine engine(&ctx);
        QSignalSpy conflicts(&engine, &OutboxEngine::draftConflict);
        QCOMPARE(runEngine(engine), QStringLiteral("saved"));
        QCOMPARE(sentIfMatch, QByteArray("v1"));
        QCOMPARE(conflicts.size(), 1);
        const std::optional<Outgoing> stored = store->outgoing(QStringLiteral("u9"));
        QCOMPARE(stored->draftId, QStringLiteral("d10"));
        QCOMPARE(stored->draftEtag, QStringLiteral("v5"));
    }

    // Undo send (docs/PHASE9.md): the draft goes up at once with the new options, the send waits
    // for sendAfter; taken back meanwhile, it stays a draft
    void undoSendHoldsTheMessage()
    {
        qint64 clock = fixedNow;
        ctx.now = [&clock] {
            return clock;
        };
        Outgoing o = message(QStringLiteral("u20"));
        o.send = true;
        o.sendAfter = fixedNow + 10000;
        o.readReceipt = true;
        o.sensitivity = 3;
        o.deferUntil = QDateTime(QDate(2030, 1, 1), QTime(9, 0), QTimeZone::UTC).toMSecsSinceEpoch();
        QVERIFY(store->saveOutgoing(o));
        Outgoing back = message(QStringLiteral("u21"));
        back.send = true;
        back.sendAfter = fixedNow + 10000;
        QVERIFY(store->saveOutgoing(back));
        int drafts = 0;
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            const QByteArray path = pathOf(r);
            if (r.verb == "POST" && path == "/v1.0/me/messages") {
                ++drafts;
                return json(201, {{QStringLiteral("id"), QStringLiteral("d%1").arg(drafts)}});
            }
            if (r.verb == "PATCH" || r.verb == "GET") {
                return json(200, {{QStringLiteral("value"), QJsonArray()}});
            }
            if (r.verb == "POST" && path.endsWith("/send")) {
                return jsonReply(202, "");
            }
            return jsonReply(404, R"({"error":{"code":"unexpected"}})");
        };
        OutboxEngine engine(&ctx);
        QCOMPARE(runEngine(engine), QStringLiteral("held"));
        QCOMPARE(runEngine(engine), QStringLiteral("held"));
        QVERIFY(!calls().join(QLatin1Char(' ')).contains(QLatin1String("/send")));
        // The options are on the draft
        const QJsonObject created = bodyOf(server->requests.at(0));
        QVERIFY(created.value(QLatin1String("isReadReceiptRequested")).toBool());
        QVERIFY(!created.value(QLatin1String("isDeliveryReceiptRequested")).toBool());
        const QJsonArray extended = created.value(QLatin1String("singleValueExtendedProperties")).toArray();
        QCOMPARE(extended.size(), 2);
        QCOMPARE(extended.at(0).toObject().value(QLatin1String("value")).toString(), QStringLiteral("3"));
        QCOMPARE(extended.at(1).toObject().value(QLatin1String("id")).toString(), QStringLiteral("SystemTime 0x3FEF"));
        QCOMPARE(extended.at(1).toObject().value(QLatin1String("value")).toString(), QStringLiteral("2030-01-01T09:00:00Z"));
        // Not due yet
        QCOMPARE(store->outgoing(QStringLiteral("u20"))->nextAttempt, fixedNow + 10000);
        engine.process();
        QVERIFY(!engine.busy());

        // The user takes u21 back (what the worker's undoSend does)
        Outgoing taken = *store->outgoing(QStringLiteral("u21"));
        taken.send = false;
        taken.sendAfter = 0;
        taken.nextAttempt = 0;
        QVERIFY(store->saveOutgoing(taken));
        QCOMPARE(runEngine(engine), QStringLiteral("saved"));
        QCOMPARE(int(store->outgoing(QStringLiteral("u21"))->state), int(Outgoing::State::Saved));

        // Time is up: u20 goes
        server->requests.clear();
        clock = fixedNow + 10000;
        QCOMPARE(runEngine(engine), QStringLiteral("sent"));
        QVERIFY(calls().constLast().endsWith(QLatin1String("/send")));
        QVERIFY(!store->outgoing(QStringLiteral("u20")));
    }

    // A delay taken away again: Graph cannot remove the property, a past time sends at once
    void deferredSendTakenAway()
    {
        Outgoing o = message(QStringLiteral("u22"));
        o.deferUntil = -1;
        const QJsonArray extended = o.graphMessage().value(QLatin1String("singleValueExtendedProperties")).toArray();
        QCOMPARE(extended.size(), 2);
        QCOMPARE(extended.at(1).toObject().value(QLatin1String("value")).toString(), QStringLiteral("2000-01-01T00:00:00Z"));
        o.deferUntil = 0;
        QCOMPARE(o.graphMessage().value(QLatin1String("singleValueExtendedProperties")).toArray().size(), 1);
        // Kept through the record
        o.sensitivity = 2;
        o.deliveryReceipt = true;
        o.sendAfter = 42;
        const Outgoing again = Outgoing::fromJson(o.toJson());
        QCOMPARE(again.sensitivity, 2);
        QVERIFY(again.deliveryReceipt);
        QCOMPARE(again.sendAfter, 42);
    }

    void mergeKeepsUploadsAndMarksRemovals()
    {
        Outgoing stored = message(QStringLiteral("u9"));
        stored.draftId = QStringLiteral("d9");
        stored.revision = 3;
        OutgoingAttachment kept;
        kept.key = QStringLiteral("keep");
        kept.blob = QStringLiteral("b1");
        kept.graphId = QStringLiteral("g1");
        OutgoingAttachment gone = kept;
        gone.key = QStringLiteral("gone");
        gone.graphId = QStringLiteral("g2");
        OutgoingAttachment notUploaded = kept;
        notUploaded.key = QStringLiteral("local");
        notUploaded.graphId.clear();
        stored.attachments = {kept, gone, notUploaded};

        Outgoing incoming = message(QStringLiteral("u9"));
        OutgoingAttachment keptAgain;
        keptAgain.key = QStringLiteral("keep");
        incoming.attachments = {keptAgain};
        const Outgoing merged = Outgoing::merged(stored, incoming);
        QCOMPARE(merged.draftId, QStringLiteral("d9"));
        QCOMPARE(merged.revision, 4);
        QCOMPARE(merged.attachments.size(), 2);
        QCOMPARE(merged.attachments.at(0).graphId, QStringLiteral("g1"));
        QCOMPARE(merged.attachments.at(0).blob, QStringLiteral("b1"));
        QCOMPARE(merged.attachments.at(1).key, QStringLiteral("gone"));
        QVERIFY(merged.attachments.at(1).removed);
        // JSON round trip
        const Outgoing back = Outgoing::fromJson(merged.toJson());
        QCOMPARE(back.toJson(), merged.toJson());
    }
};

QTEST_GUILESS_MAIN(OutboxTest)

#include "outboxtest.moc"
