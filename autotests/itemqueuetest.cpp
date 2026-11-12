// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fakegraphserver.h"
#include "graphclient.h"
#include "graphmapper.h"
#include "conflict.h"
#include "itemqueue.h"
#include "store.h"
#include "throttle.h"
#include "tokenprovider.h"

#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <limits>

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

QString call(const FakeRequest &r)
{
    return QString::fromLatin1(r.verb) + QLatin1Char(' ') + QString::fromLatin1(pathOf(r));
}
}

class ItemQueueTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir dir;
    std::unique_ptr<FakeGraphServer> server;
    StaticTokens tokens;
    std::unique_ptr<GraphCore::GraphClient> client;
    std::unique_ptr<Store> store;
    SyncContext ctx;
    qint64 folder = 0;
    qint64 list = 0;
    int counter = 0;

    void run(ItemQueue &queue)
    {
        QSignalSpy completed(&queue, &ItemQueue::completed);
        QSignalSpy failed(&queue, &ItemQueue::failed);
        queue.process(4);
        QTRY_VERIFY(queue.inFlight() == 0 && (completed.size() + failed.size()) > 0);
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
        client->setRetryPolicy(policy);
        store = std::make_unique<Store>();
        QString error;
        QVERIFY(store->open(dir.path() + QStringLiteral("/items-%1.db").arg(++counter), Store::Mode::ReadWrite, &error));
        ctx = {};
        ctx.store = store.get();
        ctx.client = client.get();
        ctx.root = QStringLiteral("/me");
        ctx.now = [] {
            return qint64(1000000);
        };
        folder = store->upsertParent(ItemKind::Contact, {0, QStringLiteral("contacts"), QStringLiteral("Contacts"), {}, {}, 0, 0, 0});
        list = store->upsertParent(ItemKind::Task, {0, QStringLiteral("LIST"), QStringLiteral("Tasks"), QStringLiteral("defaultList"), {}, 0, 0, 0});
    }

    void cleanup()
    {
        store.reset();
    }

    void contactCreateUpdateDelete()
    {
        server->handler = [](const FakeRequest &r) -> FakeReply {
            if (r.verb == "POST") {
                return jsonReply(201, R"({"id":"C1","changeKey":"k1","displayName":"[TEST] Jane Doe","emailAddresses":[{"address":"jane@example.com"}]})");
            }
            return r.verb == "DELETE" ? jsonReply(204, "") : jsonReply(200, "{}");
        };
        ItemQueue queue(&ctx);
        QVERIFY(queue.create(ItemKind::Contact, folder, {{QStringLiteral("displayName"), QStringLiteral("[TEST] Jane Doe")}}));
        QCOMPARE(store->items(ItemKind::Contact).size(), 1);
        QVERIFY(store->items(ItemKind::Contact).first().graphId.startsWith(QLatin1String("local:")));
        run(queue);
        // The default folder goes through /contacts; the stand-in became Graph's contact
        QCOMPARE(call(server->requests.at(0)), QStringLiteral("POST /v1.0/me/contacts"));
        QVERIFY(server->requests.at(0).headers.value("prefer").contains("ImmutableId"));
        const StoredItem made = store->items(ItemKind::Contact).first();
        QCOMPARE(made.graphId, QStringLiteral("C1"));
        QVERIFY(queue.update(ItemKind::Contact, made.id, {{QStringLiteral("jobTitle"), QStringLiteral("Tester")}}));
        QVERIFY(store->item(ItemKind::Contact, made.id)->json.contains(QLatin1String("Tester")));
        run(queue);
        QCOMPARE(call(server->requests.at(1)), QStringLiteral("PATCH /v1.0/me/contacts/C1"));
        QCOMPARE(QJsonDocument::fromJson(server->requests.at(1).body).object().keys(), QStringList{QStringLiteral("jobTitle")});
        QVERIFY(queue.remove(ItemKind::Contact, made.id));
        QVERIFY(store->items(ItemKind::Contact).isEmpty());
        run(queue);
        QCOMPARE(call(server->requests.at(2)), QStringLiteral("DELETE /v1.0/me/contacts/C1"));
    }

    void taskCompletedAndRefusalRestores()
    {
        store->upsertItem(ItemKind::Task, list, Mapper::task(QJsonObject{{QStringLiteral("id"), QStringLiteral("T1")}, {QStringLiteral("title"), QStringLiteral("[TEST] Task")},
                                                                         {QStringLiteral("status"), QStringLiteral("notStarted")}, {QStringLiteral("lastModifiedDateTime"), QStringLiteral("a")}}));
        server->handler = [](const FakeRequest &) -> FakeReply {
            return jsonReply(400, R"({"error":{"code":"invalidRequest","message":"refused"}})");
        };
        ItemQueue queue(&ctx);
        const qint64 id = store->itemByGraphId(ItemKind::Task, QStringLiteral("T1"))->id;
        QVERIFY(queue.update(ItemKind::Task, id, {{QStringLiteral("status"), QStringLiteral("completed")}}));
        QVERIFY(store->item(ItemKind::Task, id)->json.contains(QLatin1String("completed")));
        run(queue);
        QCOMPARE(call(server->requests.first()), QStringLiteral("PATCH /v1.0/me/todo/lists/LIST/tasks/T1"));
        // To Do is synced without immutable ids, so its writes must not ask for them
        QVERIFY(!server->requests.first().headers.value("prefer").contains("ImmutableId"));
        // Refused: back to not started
        QVERIFY(store->item(ItemKind::Task, id)->json.contains(QLatin1String("notStarted")));
    }

    void listsAndSteps()
    {
        server->handler = [](const FakeRequest &r) -> FakeReply {
            if (r.verb == "POST" && pathOf(r) == "/v1.0/me/todo/lists") {
                return jsonReply(201, R"({"id":"L2","displayName":"[TEST] Shopping"})");
            }
            return jsonReply(r.verb == "POST" ? 201 : 200, "{}");
        };
        ItemQueue queue(&ctx);
        QVERIFY(queue.createList(QStringLiteral("[TEST] Shopping")));
        run(queue);
        const QList<ParentRow> lists = store->parents(ItemKind::Task);
        QCOMPARE(lists.size(), 2);
        QCOMPARE(lists.last().name, QStringLiteral("[TEST] Shopping"));
        QVERIFY(queue.renameList(lists.last().id, QStringLiteral("[TEST] Groceries")));
        QCOMPARE(store->parents(ItemKind::Task).last().name, QStringLiteral("[TEST] Groceries"));
        run(queue);
        QCOMPARE(call(server->requests.at(1)), QStringLiteral("PATCH /v1.0/me/todo/lists/L2"));
        // To Do's own lists cannot be deleted
        QVERIFY(!queue.deleteList(list));
        store->upsertItem(ItemKind::Task, list, Mapper::task(QJsonObject{{QStringLiteral("id"), QStringLiteral("T2")}, {QStringLiteral("title"), QStringLiteral("[TEST] Steps")}}));
        QVERIFY(queue.step(store->itemByGraphId(ItemKind::Task, QStringLiteral("T2"))->id, QStringLiteral("create"), {}, {{QStringLiteral("displayName"), QStringLiteral("Step one")}}));
        run(queue);
        QCOMPARE(call(server->requests.at(2)), QStringLiteral("POST /v1.0/me/todo/lists/LIST/tasks/T2/checklistItems"));
        // The three queues keep to their own kinds
        QVERIFY(store->dueOperations(std::numeric_limits<qint64>::max(), 10).isEmpty());
        QVERIFY(store->dueOperations(std::numeric_limits<qint64>::max(), 10, QStringLiteral("event")).isEmpty());
    }

    void dueTaskReminders()
    {
        auto task = [](const QString &id, const QString &status, const QString &reminder) {
            QJsonObject t{{QStringLiteral("id"), id}, {QStringLiteral("title"), id}, {QStringLiteral("status"), status}, {QStringLiteral("isReminderOn"), true}};
            if (!reminder.isEmpty()) {
                t.insert(QStringLiteral("reminderDateTime"), QJsonObject{{QStringLiteral("dateTime"), reminder}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}});
            }
            return t;
        };
        store->upsertItem(ItemKind::Task, list, Mapper::task(task(QStringLiteral("due"), QStringLiteral("notStarted"), QStringLiteral("2026-11-11T09:00:00"))));
        store->upsertItem(ItemKind::Task, list, Mapper::task(task(QStringLiteral("later"), QStringLiteral("notStarted"), QStringLiteral("2026-11-20T09:00:00"))));
        store->upsertItem(ItemKind::Task, list, Mapper::task(task(QStringLiteral("done"), QStringLiteral("completed"), QStringLiteral("2026-11-11T09:00:00"))));
        store->upsertItem(ItemKind::Task, list, Mapper::task(task(QStringLiteral("none"), QStringLiteral("notStarted"), {})));
        const qint64 now = QDateTime(QDate(2026, 11, 11), QTime(10, 0), QTimeZone::UTC).toMSecsSinceEpoch();
        const QList<StoredItem> due = store->dueTaskReminders(now);
        QCOMPARE(due.size(), 1);
        QCOMPARE(due.first().graphId, QStringLiteral("due"));
    }

    void conflictingFieldsOnlyWhereBothChanged()
    {
        const QJsonObject previous{{QStringLiteral("title"), QStringLiteral("A")}, {QStringLiteral("importance"), QStringLiteral("normal")}};
        const QJsonObject changes{{QStringLiteral("importance"), QStringLiteral("high")}};
        // Elsewhere the title changed: no conflict, a merge
        QVERIFY(conflictingFields(changes, previous, {{QStringLiteral("title"), QStringLiteral("B")}, {QStringLiteral("importance"), QStringLiteral("normal")}}).isEmpty());
        // Elsewhere the same field changed to the same value: no conflict either
        QVERIFY(conflictingFields(changes, previous, {{QStringLiteral("importance"), QStringLiteral("high")}}).isEmpty());
        // The same field to another value: a conflict
        QCOMPARE(conflictingFields(changes, previous, {{QStringLiteral("importance"), QStringLiteral("low")}}), QStringList{QStringLiteral("importance")});
        QCOMPARE(ifMatch({{QStringLiteral("@odata.etag"), QStringLiteral("v1")}}), QByteArray("v1"));
    }

    // Changed elsewhere meanwhile (412): other fields = merge onto the new version
    void taskChangedElsewhereIsMerged()
    {
        store->upsertItem(ItemKind::Task, list, Mapper::task(QJsonObject{{QStringLiteral("id"), QStringLiteral("T1")}, {QStringLiteral("title"), QStringLiteral("[TEST] A")},
                                                                         {QStringLiteral("importance"), QStringLiteral("normal")}, {QStringLiteral("@odata.etag"), QStringLiteral("v1")},
                                                                         {QStringLiteral("lastModifiedDateTime"), QStringLiteral("a")}}));
        server->handler = [](const FakeRequest &r) -> FakeReply {
            if (r.verb == "GET") {
                return jsonReply(200, R"({"id":"T1","title":"[TEST] B","importance":"normal","@odata.etag":"v2"})");
            }
            return r.headers.value("if-match") == "v2" ? jsonReply(200, "{}") : jsonReply(412, R"({"error":{"code":"ErrorIrresolvableConflict","message":"m"}})");
        };
        ItemQueue queue(&ctx);
        QSignalSpy conflicts(&queue, &ItemQueue::conflict);
        const qint64 id = store->itemByGraphId(ItemKind::Task, QStringLiteral("T1"))->id;
        QVERIFY(queue.update(ItemKind::Task, id, {{QStringLiteral("importance"), QStringLiteral("high")}}));
        queue.process(4);
        QTRY_VERIFY(server->requests.size() >= 2 && queue.inFlight() == 0);
        // The merged change goes out again on v2 and is accepted
        queue.process(4);
        QTRY_VERIFY(store->dueOperations(std::numeric_limits<qint64>::max(), 10, QStringLiteral("item")).isEmpty() && queue.inFlight() == 0);
        QCOMPARE(server->requests.first().headers.value("if-match"), QByteArray("v1"));
        QCOMPARE(server->requests.last().headers.value("if-match"), QByteArray("v2"));
        QCOMPARE(conflicts.size(), 0);
    }

    // The same field changed elsewhere: Microsoft 365's version stays, the user is told
    void taskConflictKeepsTheServersVersion()
    {
        store->upsertItem(ItemKind::Task, list, Mapper::task(QJsonObject{{QStringLiteral("id"), QStringLiteral("T2")}, {QStringLiteral("title"), QStringLiteral("[TEST] A")},
                                                                         {QStringLiteral("importance"), QStringLiteral("normal")}, {QStringLiteral("@odata.etag"), QStringLiteral("v1")},
                                                                         {QStringLiteral("lastModifiedDateTime"), QStringLiteral("a")}}));
        server->handler = [](const FakeRequest &r) -> FakeReply {
            if (r.verb == "GET") {
                return jsonReply(200, R"({"id":"T2","title":"[TEST] A","importance":"low","@odata.etag":"v2","lastModifiedDateTime":"b"})");
            }
            return jsonReply(412, R"({"error":{"code":"ErrorIrresolvableConflict","message":"m"}})");
        };
        ItemQueue queue(&ctx);
        QSignalSpy conflicts(&queue, &ItemQueue::conflict);
        const qint64 id = store->itemByGraphId(ItemKind::Task, QStringLiteral("T2"))->id;
        QVERIFY(queue.update(ItemKind::Task, id, {{QStringLiteral("importance"), QStringLiteral("high")}}));
        queue.process(4);
        QTRY_COMPARE(conflicts.size(), 1);
        QCOMPARE(conflicts.first().at(0).toString(), QStringLiteral("task"));
        QCOMPARE(conflicts.first().at(3).toStringList(), QStringList{QStringLiteral("importance")});
        QVERIFY(store->item(ItemKind::Task, id)->json.contains(QLatin1String("\"low\"")));
        QVERIFY(store->dueOperations(std::numeric_limits<qint64>::max(), 10, QStringLiteral("item")).isEmpty());
    }

    // Graph hands a due date set in Amsterdam back as the previous day 23:00 UTC
    void dueDates()
    {
        auto at = [](const QString &dateTime, const QString &zone) {
            return QJsonObject{{QStringLiteral("dateTime"), dateTime}, {QStringLiteral("timeZone"), zone}};
        };
        qputenv("TZ", "Europe/Amsterdam");
        QCOMPARE(Mapper::dueDate(at(QStringLiteral("2026-11-19T23:00:00.0000000"), QStringLiteral("UTC"))), QDate(2026, 11, 20));
        QCOMPARE(Mapper::dueDate(at(QStringLiteral("2026-11-20T00:00:00.0000000"), QStringLiteral("UTC"))), QDate(2026, 11, 20));
        QCOMPARE(Mapper::dueDate(at(QStringLiteral("2026-11-20T00:00:00"), QStringLiteral("W. Europe Standard Time"))), QDate(2026, 11, 20));
        QCOMPARE(Mapper::dueDate({}), QDate());
        const QJsonObject written = Mapper::dueDateTime(QDate(2026, 11, 20));
        QCOMPARE(written.value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-11-20T00:00:00"));
        QCOMPARE(Mapper::dueDate(written), QDate(2026, 11, 20));
        qunsetenv("TZ");
    }
};

QTEST_GUILESS_MAIN(ItemQueueTest)

#include "itemqueuetest.moc"
