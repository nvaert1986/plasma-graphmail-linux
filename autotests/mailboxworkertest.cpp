// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fakegraphserver.h"
#include "mailboxworker.h"
#include "throttle.h"
#include "tokenprovider.h"

#include <QJsonDocument>
#include <QJsonObject>
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

QString lastState(const QSignalSpy &spy)
{
    return spy.isEmpty() ? QString() : QJsonDocument::fromJson(spy.last().at(2).toString().toUtf8()).object().value(QLatin1String("state")).toString();
}
}

// A whole worker against the fake server, for what only shows over several sync rounds
class MailboxWorkerTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir dir;
    StaticTokens tokens;
    int counter = 0;

    // A colleague's calendar-only mailbox (docs/PHASE7.md): no mail, no contacts, no To Do
    MailboxConfig calendarOnly(const FakeGraphServer &server)
    {
        MailboxConfig config;
        config.account = QStringLiteral("acc");
        config.mailbox = QStringLiteral("abc");
        config.root = QStringLiteral("/users/abc");
        config.cacheDir = dir.path() + QStringLiteral("/worker-%1").arg(++counter);
        config.baseUrl = server.baseUrl();
        config.todo = false;
        config.mail = false;
        config.contacts = false;
        return config;
    }

private Q_SLOTS:
    void init()
    {
        GraphCore::Throttle::resetForTests();
    }

    // Without mail there is no folder tree to wait for: the calendar syncs at once
    void calendarOnlySyncsWithoutAFolderTree()
    {
        FakeGraphServer server;
        server.handler = [&server](const FakeRequest &r) {
            if (r.target.contains("/calendars?")) {
                return jsonReply(200, R"({"value":[{"id":"C1","name":"Calendar","canEdit":false,"isDefaultCalendar":true,"owner":{"address":"abc@example.com"}}]})");
            }
            if (r.target.contains("calendarView/delta")) {
                const QByteArray link = server.baseUrl().toUtf8() + "/users/abc/calendarView/delta?$deltatoken=t1";
                return jsonReply(200, R"({"value":[],"@odata.deltaLink":")" + link + R"("})");
            }
            return jsonReply(404, R"({"error":{"code":"ErrorItemNotFound","message":"m"}})");
        };
        MailboxWorker worker(calendarOnly(server), &tokens);
        QSignalSpy status(&worker, &MailboxWorker::statusChanged);
        worker.start();
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(server.requests.cbegin(), server.requests.cend(),
                                             [](const FakeRequest &r) {
                                                 return r.target.contains("calendarView/delta");
                                             }),
                                 10000);
        QTRY_COMPARE_WITH_TIMEOUT(lastState(status), QStringLiteral("idle"), 10000);
        for (const FakeRequest &r : std::as_const(server.requests)) {
            QVERIFY2(!r.target.contains("mailFolders") && !r.target.contains("contact"), r.target.constData());
        }
        worker.stop();
    }

    // The colleague takes the share away: Graph answers 404 ErrorItemNotFound. The worker says
    // "no access" and does not keep asking (the next try is an hour later)
    void calendarShareTakenAway()
    {
        FakeGraphServer server;
        server.handler = [](const FakeRequest &) {
            return jsonReply(404, R"({"error":{"code":"ErrorItemNotFound","message":"The specified object was not found in the store."}})");
        };
        MailboxWorker worker(calendarOnly(server), &tokens);
        QSignalSpy status(&worker, &MailboxWorker::statusChanged);
        worker.start();
        QTRY_COMPARE_WITH_TIMEOUT(lastState(status), QStringLiteral("noAccess"), 10000);
        const qsizetype asked = server.requests.size();
        QTest::qWait(4000);
        QCOMPARE(server.requests.size(), asked);
        for (const FakeRequest &r : std::as_const(server.requests)) {
            QVERIFY2(!r.target.contains("mailFolders") && !r.target.contains("contact"), r.target.constData());
        }
        worker.stop();
    }

    // Inbox rules (docs/PHASE9.md): the list, a new rule, a change and a delete go to the Inbox's
    // messageRules with the rule as the body
    void messageRules()
    {
        FakeGraphServer server;
        server.handler = [](const FakeRequest &r) {
            if (r.target.contains("/users/abc/mailFolders/inbox/messageRules")) {
                if (r.verb == "GET") {
                    return jsonReply(200, R"({"value":[{"id":"R1","displayName":"A","sequence":1}]})");
                }
                if (r.verb == "DELETE") {
                    return FakeReply{204, {}, {}};
                }
                return jsonReply(r.verb == "POST" ? 201 : 200, r.body);
            }
            return jsonReply(404, R"({"error":{"code":"ErrorItemNotFound","message":"m"}})");
        };
        MailboxWorker worker(calendarOnly(server), &tokens);
        QSignalSpy answers(&worker, &MailboxWorker::queryAnswered);
        worker.start();
        auto answer = [&answers](int i) {
            return QJsonDocument::fromJson(answers.at(i).at(1).toString().toUtf8()).object();
        };
        worker.query(1, QStringLiteral("messageRules"), QStringLiteral("{}"));
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 1, 10000);
        QCOMPARE(answer(0).value(QLatin1String("body")).toObject().value(QLatin1String("value")).toArray().size(), 1);
        worker.query(2, QStringLiteral("messageRuleCreate"), QStringLiteral(R"({"rule":{"displayName":"B","actions":{"markAsRead":true}}})"));
        worker.query(3, QStringLiteral("messageRuleUpdate"), QStringLiteral(R"({"id":"R1","rule":{"isEnabled":false}})"));
        worker.query(4, QStringLiteral("messageRuleDelete"), QStringLiteral(R"({"id":"R1"})"));
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 4, 10000);
        for (int i = 1; i < 4; ++i) {
            QVERIFY(answer(i).value(QLatin1String("ok")).toBool());
        }
        QHash<QByteArray, QByteArray> seen;
        for (const FakeRequest &r : std::as_const(server.requests)) {
            if (r.target.contains("messageRules")) {
                seen.insert(r.verb, r.target + ' ' + r.body);
            }
        }
        QVERIFY(seen.value("POST").endsWith(R"( {"actions":{"markAsRead":true},"displayName":"B"})"));
        QVERIFY(seen.value("PATCH").contains("/messageRules/R1 {\"isEnabled\":false}"));
        QVERIFY(seen.value("DELETE").contains("/messageRules/R1"));
        worker.stop();
    }

    // Automatic replies (docs/PHASE9.md): read and saved through /mailboxSettings; the answer
    // carries Graph's body or the reason it was refused
    void mailboxSettings()
    {
        FakeGraphServer server;
        server.handler = [](const FakeRequest &r) {
            if (r.target.endsWith("/users/abc/mailboxSettings")) {
                if (r.verb == "PATCH") {
                    return jsonReply(200, r.body);
                }
                return jsonReply(200, R"({"automaticRepliesSetting":{"status":"disabled","externalAudience":"all"}})");
            }
            return jsonReply(404, R"({"error":{"code":"ErrorItemNotFound","message":"m"}})");
        };
        MailboxWorker worker(calendarOnly(server), &tokens);
        QSignalSpy answers(&worker, &MailboxWorker::queryAnswered);
        worker.start();
        worker.query(1, QStringLiteral("mailboxSettings"), QStringLiteral("{}"));
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 1, 10000);
        QJsonObject answer = QJsonDocument::fromJson(answers.at(0).at(1).toString().toUtf8()).object();
        QVERIFY(answer.value(QLatin1String("ok")).toBool());
        QCOMPARE(answer.value(QLatin1String("body")).toObject().value(QLatin1String("automaticRepliesSetting")).toObject().value(QLatin1String("status")).toString(),
                 QStringLiteral("disabled"));

        const QByteArray settings = R"({"settings":{"automaticRepliesSetting":{"status":"alwaysEnabled","internalReplyMessage":"<p>Away</p>"}}})";
        worker.query(2, QStringLiteral("mailboxSettingsUpdate"), QString::fromUtf8(settings));
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 2, 10000);
        answer = QJsonDocument::fromJson(answers.at(1).at(1).toString().toUtf8()).object();
        QVERIFY(answer.value(QLatin1String("ok")).toBool());
        const auto patch = std::find_if(server.requests.cbegin(), server.requests.cend(), [](const FakeRequest &r) {
            return r.verb == "PATCH";
        });
        QVERIFY(patch != server.requests.cend());
        QCOMPARE(QJsonDocument::fromJson(patch->body).object().value(QLatin1String("automaticRepliesSetting")).toObject().value(QLatin1String("status")).toString(),
                 QStringLiteral("alwaysEnabled"));

        // Another mailbox's settings: Graph refuses them
        server.handler = [](const FakeRequest &) {
            return jsonReply(403, R"({"error":{"code":"ErrorAccessDenied","message":"Access is denied."}})");
        };
        worker.query(3, QStringLiteral("mailboxSettings"), QStringLiteral("{}"));
        QTRY_COMPARE_WITH_TIMEOUT(answers.size(), 3, 10000);
        answer = QJsonDocument::fromJson(answers.at(2).at(1).toString().toUtf8()).object();
        QVERIFY(!answer.value(QLatin1String("ok")).toBool());
        QVERIFY(!answer.value(QLatin1String("problem")).toString().isEmpty());
        worker.stop();
    }
};

QTEST_GUILESS_MAIN(MailboxWorkerTest)

#include "mailboxworkertest.moc"
