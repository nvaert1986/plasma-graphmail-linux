// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fakegraphserver.h"
#include "graphclient.h"
#include "mailboxaccess.h"
#include "throttle.h"
#include "tokenprovider.h"

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

FakeReply graphError(int status, const char *code)
{
    return jsonReply(status, QByteArray(R"({"error":{"code":")") + code + R"(","message":"m"}})");
}

const QByteArray user = R"({"value":[{"id":"U1","mail":"box@example.com","userPrincipalName":"box@example.com","displayName":"Box"}]})";
}

// The answers are the ones the phase 7 probe saw (docs/PHASE7.md findings)
class MailboxAccessTest : public QObject
{
    Q_OBJECT

private:
    StaticTokens tokens;

    MailboxAccess check(const FakeGraphServer::Handler &handler)
    {
        GraphCore::Throttle::resetForTests();
        FakeGraphServer server;
        server.handler = handler;
        GraphCore::GraphClient client(&tokens);
        client.setBaseUrl(server.baseUrl());
        GraphCore::RetryPolicy policy;
        policy.maxAttempts = 1;
        client.setRetryPolicy(policy);
        return checkMailboxAccess(client, QStringLiteral("box@example.com"));
    }

private Q_SLOTS:
    void fullAccess()
    {
        const MailboxAccess a = check([](const FakeRequest &r) {
            if (r.target.startsWith("/v1.0/users?")) {
                return jsonReply(200, user);
            }
            return r.target.contains("/calendar?") ? jsonReply(200, R"({"id":"C","canEdit":true})") : jsonReply(200, R"({"value":[]})");
        });
        QVERIFY(a.problem.isEmpty());
        QCOMPARE(a.id, QStringLiteral("U1"));
        QCOMPARE(a.parts(), (QStringList{QStringLiteral("mail"), QStringLiteral("calendar"), QStringLiteral("contacts")}));
        QVERIFY(a.calendarEdit);
    }

    void calendarOnly()
    {
        const MailboxAccess a = check([](const FakeRequest &r) {
            if (r.target.startsWith("/v1.0/users?")) {
                return jsonReply(200, user);
            }
            return r.target.contains("/calendar?") ? jsonReply(200, R"({"id":"C","canEdit":false})") : graphError(404, "ErrorItemNotFound");
        });
        QVERIFY(a.problem.isEmpty());
        QCOMPARE(a.parts(), QStringList{QStringLiteral("calendar")});
        QVERIFY(!a.calendarEdit);
    }

    // Not shared at all: found, nothing reachable (the app offers free/busy)
    void noAccess()
    {
        const MailboxAccess a = check([](const FakeRequest &r) {
            return r.target.startsWith("/v1.0/users?") ? jsonReply(200, user) : graphError(404, "ErrorItemNotFound");
        });
        QVERIFY(a.problem.isEmpty());
        QVERIFY(!a.any());
    }

    void unknownAddress()
    {
        const MailboxAccess a = check([](const FakeRequest &) {
            return jsonReply(200, R"({"value":[]})");
        });
        QCOMPARE(a.problem, QStringLiteral("noSuchMailbox"));
    }

    // Any other refusal is not "no access": the check fails as a whole
    void serverErrorIsNotNoAccess()
    {
        const MailboxAccess a = check([](const FakeRequest &r) {
            return r.target.startsWith("/v1.0/users?") ? jsonReply(200, user) : graphError(400, "ErrorInvalidRequest");
        });
        QCOMPARE(a.problem, QStringLiteral("error"));
    }

    void problemCodes()
    {
        GraphCore::GraphResponse r;
        r.status = 404;
        r.body = R"({"error":{"code":"ErrorInvalidUser"}})";
        QCOMPARE(accessProblem(r), QStringLiteral("noSuchMailbox"));
        r.body = R"({"error":{"code":"ErrorItemNotFound"}})";
        QCOMPARE(accessProblem(r), QStringLiteral("noAccess"));
        r.status = 403;
        r.body = R"({"error":{"code":"ErrorSendAsDenied"}})";
        QCOMPARE(accessProblem(r), QStringLiteral("sendAsDenied"));
        r.body = R"({"error":{"code":"ErrorAccessDenied"}})";
        QCOMPARE(accessProblem(r), QStringLiteral("denied"));
        // A calendar share for a shared mailbox (live answer, phase 7)
        r.status = 500;
        r.body = R"({"error":{"code":"ErrorInternalServerError","message":"m","innererror":{"message":"No sharing handler is available for recipient: box@example.com."}}})";
        QCOMPARE(accessProblem(r), QStringLiteral("notShareable"));
        r.status = 0;
        QCOMPARE(accessProblem(r), QStringLiteral("network"));
        r.status = 200;
        QVERIFY(accessProblem(r).isEmpty());
    }
};

QTEST_GUILESS_MAIN(MailboxAccessTest)

#include "mailboxaccesstest.moc"
