// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fakegraphserver.h"
#include "graphclient.h"
#include "messageimport.h"
#include "throttle.h"
#include "tokenprovider.h"

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

QByteArray pathOf(const FakeRequest &r)
{
    const qsizetype query = r.target.indexOf('?');
    return query < 0 ? r.target : r.target.left(query);
}

FakeReply json(int status, const QJsonObject &body)
{
    return jsonReply(status, QJsonDocument(body).toJson(QJsonDocument::Compact));
}
}

class MessageImportTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir dir;
    std::unique_ptr<FakeGraphServer> server;
    StaticTokens tokens;
    std::unique_ptr<GraphCore::GraphClient> client;

    // A request file with a message, a small and a large attachment and an attached message
    QString requestFile()
    {
        QFile small(dir.filePath(QStringLiteral("small")));
        if (!small.open(QIODevice::WriteOnly)) {
            return {};
        }
        small.write("Example attachment text");
        small.close();
        QFile large(dir.filePath(QStringLiteral("large")));
        if (!large.open(QIODevice::WriteOnly)) {
            return {};
        }
        large.write(QByteArray(5 * 1024 * 1024, 'x'));
        large.close();
        const QJsonObject request{
            {QStringLiteral("message"), QJsonObject{{QStringLiteral("subject"), QStringLiteral("Example subject")}}},
            {QStringLiteral("files"),
             QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("notes.txt")},
                                    {QStringLiteral("contentType"), QStringLiteral("text/plain")},
                                    {QStringLiteral("contentId"), QStringLiteral("notes@example")},
                                    {QStringLiteral("isInline"), true},
                                    {QStringLiteral("path"), small.fileName()}},
                        QJsonObject{{QStringLiteral("name"), QStringLiteral("big.bin")},
                                    {QStringLiteral("contentType"), QStringLiteral("application/octet-stream")},
                                    {QStringLiteral("path"), large.fileName()}}}},
            {QStringLiteral("items"),
             QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("Attached example")},
                                    {QStringLiteral("item"), QJsonObject{{QStringLiteral("subject"), QStringLiteral("Attached example")}}}}}}};
        QFile f(dir.filePath(QStringLiteral("request.json")));
        if (!f.open(QIODevice::WriteOnly)) {
            return {};
        }
        f.write(QJsonDocument(request).toJson());
        return f.fileName();
    }

    QJsonObject run(const QString &file, bool skipDuplicates = false)
    {
        auto *job = new MessageImport(client.get(), QStringLiteral("/me"), QStringLiteral("f1"), file);
        job->setSkipDuplicates(skipDuplicates);
        QSignalSpy finished(job, &MessageImport::finished);
        job->start();
        for (int i = 0; i < 200 && finished.isEmpty(); ++i) {
            finished.wait(50);
        }
        return finished.isEmpty() ? QJsonObject() : finished.first().first().toJsonObject();
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
    }

    // The message, then the small attachment inline, the large one in chunks, the attached message
    void importsEverything()
    {
        QList<QByteArray> ranges;
        bool tokenOnUpload = false;
        QJsonObject smallBody;
        QJsonObject itemBody;
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            const QByteArray path = pathOf(r);
            if (r.verb == "POST" && path == "/v1.0/me/mailFolders/f1/messages") {
                return json(201, {{QStringLiteral("id"), QStringLiteral("m1")}});
            }
            if (r.verb == "POST" && path == "/v1.0/me/messages/m1/attachments") {
                const QJsonObject body = QJsonDocument::fromJson(r.body).object();
                (body.value(QLatin1String("@odata.type")).toString().contains(QLatin1String("item")) ? itemBody : smallBody) = body;
                return json(201, {{QStringLiteral("id"), QStringLiteral("a1")}});
            }
            if (r.verb == "POST" && path == "/v1.0/me/messages/m1/attachments/createUploadSession") {
                return json(201, {{QStringLiteral("uploadUrl"), QString(server->baseUrl() + QStringLiteral("/upload/1"))}});
            }
            if (r.verb == "PUT" && path == "/v1.0/upload/1") {
                tokenOnUpload = tokenOnUpload || r.headers.contains("authorization");
                ranges.append(r.headers.value("content-range"));
                return json(ranges.size() < 2 ? 200 : 201, {});
            }
            return json(404, {});
        };
        const QJsonObject answer = run(requestFile());
        QVERIFY(answer.value(QLatin1String("ok")).toBool());
        QCOMPARE(answer.value(QLatin1String("id")).toString(), QStringLiteral("m1"));
        QCOMPARE(calls(),
                 (QStringList{QStringLiteral("POST /v1.0/me/mailFolders/f1/messages"), QStringLiteral("POST /v1.0/me/messages/m1/attachments"),
                              QStringLiteral("POST /v1.0/me/messages/m1/attachments/createUploadSession"), QStringLiteral("PUT /v1.0/upload/1"),
                              QStringLiteral("PUT /v1.0/upload/1"), QStringLiteral("POST /v1.0/me/messages/m1/attachments")}));
        QCOMPARE(smallBody.value(QLatin1String("contentId")).toString(), QStringLiteral("notes@example"));
        QVERIFY(smallBody.value(QLatin1String("isInline")).toBool());
        QCOMPARE(QByteArray::fromBase64(smallBody.value(QLatin1String("contentBytes")).toString().toLatin1()), QByteArrayLiteral("Example attachment text"));
        QCOMPARE(ranges, (QList<QByteArray>{"bytes 0-3932159/5242880", "bytes 3932160-5242879/5242880"}));
        QVERIFY(!tokenOnUpload);
        QCOMPARE(itemBody.value(QLatin1String("item")).toObject().value(QLatin1String("subject")).toString(), QStringLiteral("Attached example"));
    }

    // An attachment refused: the half message is deleted, the answer says why
    void failureRemovesTheMessage()
    {
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            const QByteArray path = pathOf(r);
            if (r.verb == "POST" && path == "/v1.0/me/mailFolders/f1/messages") {
                return json(201, {{QStringLiteral("id"), QStringLiteral("m1")}});
            }
            if (r.verb == "DELETE") {
                return json(204, {});
            }
            return json(400, {{QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), QStringLiteral("ErrorInvalidRequest")}}}});
        };
        const QJsonObject answer = run(requestFile());
        QVERIFY(!answer.value(QLatin1String("ok")).toBool());
        QVERIFY(!answer.value(QLatin1String("detail")).toString().isEmpty());
        QCOMPARE(calls().constLast(), QStringLiteral("DELETE /v1.0/me/messages/m1"));
    }

    // With skipDuplicates a message whose Message-ID is in the folder already is left out; one that
    // is not there (or without a Message-ID) is created as usual
    void skipsDuplicates()
    {
        const QString file = dir.filePath(QStringLiteral("duplicate.json"));
        const auto write = [&](const QString &messageId) {
            QJsonObject message{{QStringLiteral("subject"), QStringLiteral("Example duplicate")}};
            if (!messageId.isEmpty()) {
                message.insert(QStringLiteral("internetMessageId"), messageId);
            }
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QJsonDocument(QJsonObject{{QStringLiteral("message"), message}}).toJson());
        };
        QByteArray filter;
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            if (r.verb == "GET" && pathOf(r) == "/v1.0/me/mailFolders/f1/messages") {
                filter = QByteArray::fromPercentEncoding(r.target.mid(r.target.indexOf("$filter=") + 8));
                const bool known = filter.contains("<known@example.com>");
                return json(200, {{QStringLiteral("value"), known ? QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("old")}}} : QJsonArray()}});
            }
            if (r.verb == "POST" && pathOf(r) == "/v1.0/me/mailFolders/f1/messages") {
                return json(201, {{QStringLiteral("id"), QStringLiteral("new")}});
            }
            return json(404, {});
        };

        write(QStringLiteral("<known@example.com>"));
        QJsonObject answer = run(file, true);
        QVERIFY(answer.value(QLatin1String("ok")).toBool());
        QVERIFY(answer.value(QLatin1String("duplicate")).toBool());
        QCOMPARE(answer.value(QLatin1String("id")).toString(), QStringLiteral("old"));
        QCOMPARE(filter, QByteArray("internetMessageId eq '<known@example.com>'"));
        QCOMPARE(calls(), QStringList{QStringLiteral("GET /v1.0/me/mailFolders/f1/messages")});

        // Not in the folder: created; a quote in the ID is doubled in the filter
        server->requests.clear();
        write(QStringLiteral("<o'new@example.com>"));
        answer = run(file, true);
        QVERIFY(!answer.value(QLatin1String("duplicate")).toBool());
        QCOMPARE(answer.value(QLatin1String("id")).toString(), QStringLiteral("new"));
        QCOMPARE(filter, QByteArray("internetMessageId eq '<o''new@example.com>'"));
        QCOMPARE(calls().constLast(), QStringLiteral("POST /v1.0/me/mailFolders/f1/messages"));

        // Without a Message-ID, or without the option: no lookup
        server->requests.clear();
        write(QString());
        QVERIFY(run(file, true).value(QLatin1String("ok")).toBool());
        write(QStringLiteral("<known@example.com>"));
        QVERIFY(!run(file, false).value(QLatin1String("duplicate")).toBool());
        for (const QString &call : calls()) {
            QVERIFY(!call.startsWith(QLatin1String("GET")));
        }
    }

    // The message itself refused: nothing to clean up
    void refusedMessage()
    {
        server->handler = [&](const FakeRequest &) -> FakeReply {
            return json(403, {{QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), QStringLiteral("ErrorAccessDenied")}}}});
        };
        const QJsonObject answer = run(requestFile());
        QVERIFY(!answer.value(QLatin1String("ok")).toBool());
        QCOMPARE(calls().size(), 1);
        QVERIFY(!run(dir.filePath(QStringLiteral("missing.json"))).value(QLatin1String("ok")).toBool());
    }
};

QTEST_GUILESS_MAIN(MessageImportTest)
#include "messageimporttest.moc"
