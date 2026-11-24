// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "eventqueue.h"
#include "fakegraphserver.h"
#include "graphclient.h"
#include "graphmapper.h"
#include "reminders.h"
#include "store.h"
#include "throttle.h"
#include "tokenprovider.h"

#include <QJsonArray>
#include <QDateTime>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

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

QJsonObject time(const QString &iso)
{
    return {{QStringLiteral("dateTime"), iso}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}};
}

QJsonObject makeEvent(const QString &id, const QString &type, const QString &subject)
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("changeKey"), QStringLiteral("ck")},
            {QStringLiteral("type"), type},
            {QStringLiteral("subject"), subject},
            {QStringLiteral("start"), time(QStringLiteral("2026-11-16T09:00:00"))},
            {QStringLiteral("end"), time(QStringLiteral("2026-11-16T10:00:00"))},
            {QStringLiteral("responseStatus"), QJsonObject{{QStringLiteral("response"), QStringLiteral("notResponded")}}}};
}

QByteArray pathOf(const FakeRequest &r)
{
    const qsizetype query = r.target.indexOf('?');
    return query < 0 ? r.target : r.target.left(query);
}
}

class EventQueueTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir dir;
    std::unique_ptr<FakeGraphServer> server;
    StaticTokens tokens;
    std::unique_ptr<GraphCore::GraphClient> client;
    std::unique_ptr<Store> store;
    SyncContext ctx;
    qint64 calendar = 0;
    int counter = 0;

    void run(EventQueue &queue)
    {
        QSignalSpy completed(&queue, &EventQueue::completed);
        QSignalSpy failed(&queue, &EventQueue::failed);
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
        QVERIFY(store->open(dir.path() + QStringLiteral("/events-%1.db").arg(++counter), Store::Mode::ReadWrite, &error));
        ctx = {};
        ctx.store = store.get();
        ctx.client = client.get();
        ctx.root = QStringLiteral("/me");
        ctx.now = [] {
            return qint64(1000000);
        };
        calendar = store->upsertCalendar({0, QStringLiteral("CAL"), QStringLiteral("Calendar"), QStringLiteral("me@example.com"), {}, true, true});
    }

    void cleanup()
    {
        store.reset();
    }

    void createReplacesTheLocalStandIn()
    {
        server->handler = [](const FakeRequest &r) -> FakeReply {
            if (r.verb == "POST" && pathOf(r) == "/v1.0/me/calendars/CAL/events") {
                QJsonObject made = makeEvent(QStringLiteral("NEW"), QStringLiteral("singleInstance"), QStringLiteral("[TEST] Planning"));
                return jsonReply(201, QJsonDocument(made).toJson(QJsonDocument::Compact));
            }
            return jsonReply(404, "{}");
        };
        EventQueue queue(&ctx);
        QJsonObject e = makeEvent({}, {}, QStringLiteral("[TEST] Planning"));
        e.remove(QStringLiteral("id"));
        e.remove(QStringLiteral("type"));
        QVERIFY(queue.create(calendar, e));
        // At once in the cache, under a local id
        QList<EventRow> shown = store->eventsBetween({calendar}, 0, std::numeric_limits<qint64>::max());
        QCOMPARE(shown.size(), 1);
        QVERIFY(shown.first().graphId.startsWith(QLatin1String("local:")));
        QCOMPARE(shown.first().response, 1); // the organizer
        run(queue);
        shown = store->eventsBetween({calendar}, 0, std::numeric_limits<qint64>::max());
        QCOMPARE(shown.size(), 1);
        QCOMPARE(shown.first().graphId, QStringLiteral("NEW"));
        // Times go to Graph as sent, answers come back in UTC
        QVERIFY(server->requests.first().headers.value("prefer").contains("outlook.timezone=\"UTC\""));
        QCOMPARE(QJsonDocument::fromJson(server->requests.first().body).object().value(QLatin1String("subject")).toString(), QStringLiteral("[TEST] Planning"));
    }

    void changingAnOccurrenceMakesAnException()
    {
        store->upsertItem(ItemKind::Event, calendar, Mapper::event(makeEvent(QStringLiteral("M"), QStringLiteral("seriesMaster"), QStringLiteral("Weekly"))));
        QJsonObject occurrence{{QStringLiteral("id"), QStringLiteral("O")},
                               {QStringLiteral("type"), QStringLiteral("occurrence")},
                               {QStringLiteral("seriesMasterId"), QStringLiteral("M")},
                               {QStringLiteral("start"), time(QStringLiteral("2026-11-16T09:00:00"))},
                               {QStringLiteral("end"), time(QStringLiteral("2026-11-16T10:00:00"))}};
        store->upsertItem(ItemKind::Event, calendar, Mapper::event(occurrence));
        server->handler = [](const FakeRequest &r) -> FakeReply {
            return r.verb == "PATCH" && pathOf(r) == "/v1.0/me/calendars/CAL/events/O" ? jsonReply(200, "{}") : jsonReply(404, "{}");
        };
        EventQueue queue(&ctx);
        const qint64 id = store->eventByGraphId(QStringLiteral("O"))->id;
        QVERIFY(queue.update(id, {{QStringLiteral("subject"), QStringLiteral("Weekly, moved")}}));
        const EventRow changed = *store->event(id);
        QCOMPARE(changed.subject, QStringLiteral("Weekly, moved"));
        QCOMPARE(changed.type, 2);
        // The series itself is untouched
        QCOMPARE(store->eventByGraphId(QStringLiteral("M"))->subject, QStringLiteral("Weekly"));
        run(queue);
        QCOMPARE(QJsonDocument::fromJson(server->requests.first().body).object().keys(), QStringList{QStringLiteral("subject")});
    }

    // Fast drags in the calendar (0.14.5): changes of one event go one at a time, each onto the
    // version the one before made, so none is refused or taken for a change made elsewhere
    void quickChangesGoOneAtATime()
    {
        QJsonObject event = makeEvent(QStringLiteral("E"), QStringLiteral("singleInstance"), QStringLiteral("Planning"));
        event.insert(QStringLiteral("@odata.etag"), QStringLiteral("v1"));
        store->upsertItem(ItemKind::Event, calendar, Mapper::event(event));
        // Exchange: a change only onto the current version; the answer in UTC, with a new tag
        auto current = std::make_shared<QJsonObject>(event);
        auto version = std::make_shared<int>(1);
        server->handler = [current, version](const FakeRequest &r) -> FakeReply {
            if (pathOf(r) != "/v1.0/me/calendars/CAL/events/E") {
                return jsonReply(404, "{}");
            }
            if (r.verb == "PATCH") {
                if (r.headers.value("if-match") != QStringLiteral("v%1").arg(*version).toUtf8()) {
                    return jsonReply(412, "{}");
                }
                const QJsonObject changes = QJsonDocument::fromJson(r.body).object();
                for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
                    // Graph's own form: UTC, seven digits
                    const qint64 at = Mapper::parseDateTimeTimeZone(it.value().toObject());
                    current->insert(it.key(), at ? QJsonValue(time(QDateTime::fromMSecsSinceEpoch(at, QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.0000000")))) : QJsonValue(it.value()));
                }
                current->insert(QStringLiteral("@odata.etag"), QStringLiteral("v%1").arg(++*version));
            }
            return jsonReply(200, QJsonDocument(*current).toJson(QJsonDocument::Compact));
        };
        EventQueue queue(&ctx);
        QSignalSpy conflicts(&queue, &EventQueue::conflict);
        const qint64 id = store->eventByGraphId(QStringLiteral("E"))->id;
        const auto local = [](int hour) {
            return QJsonObject{{QStringLiteral("dateTime"), QStringLiteral("2026-11-16T%1:00:00").arg(hour, 2, 10, QLatin1Char('0'))},
                               {QStringLiteral("timeZone"), QStringLiteral("W. Europe Standard Time")}};
        };
        QVERIFY(queue.update(id, {{QStringLiteral("start"), local(11)}, {QStringLiteral("end"), local(12)}}));
        QVERIFY(queue.update(id, {{QStringLiteral("start"), local(13)}, {QStringLiteral("end"), local(14)}}));
        // The second waits for the first
        run(queue);
        QCOMPARE(server->requests.size(), 1);
        run(queue);
        QCOMPARE(server->requests.size(), 2);
        QCOMPARE(server->requests.at(0).headers.value("if-match"), QByteArray("v1"));
        QCOMPARE(server->requests.at(1).headers.value("if-match"), QByteArray("v2"));
        QCOMPARE(conflicts.size(), 0);
        QCOMPARE(store->pendingOperationCount(), 0);
        // The cache holds Exchange's version: 13:00 in Amsterdam is 12:00 UTC, and the next change
        // goes onto its tag
        const QJsonObject cached = QJsonDocument::fromJson(store->eventByGraphId(QStringLiteral("E"))->json.toUtf8()).object();
        QCOMPARE(cached.value(QLatin1String("@odata.etag")).toString(), QStringLiteral("v3"));
        QCOMPARE(Mapper::parseDateTimeTimeZone(cached.value(QLatin1String("start")).toObject()), QDateTime(QDate(2026, 11, 16), QTime(12, 0), QTimeZone::UTC).toMSecsSinceEpoch());
        QVERIFY(queue.update(id, {{QStringLiteral("subject"), QStringLiteral("Planning, moved")}}));
        run(queue);
        QCOMPARE(server->requests.at(2).headers.value("if-match"), QByteArray("v3"));
        QCOMPARE(conflicts.size(), 0);
    }

    // Deleted while moves still waited (0.14.5): the moves go, the delete does not wait behind them,
    // and an event a sync brought back meanwhile leaves once the delete is done
    void deleteTakesWaitingChangesAlong()
    {
        store->upsertItem(ItemKind::Event, calendar, Mapper::event(makeEvent(QStringLiteral("E"), QStringLiteral("singleInstance"), QStringLiteral("Planning"))));
        server->handler = [](const FakeRequest &r) -> FakeReply {
            return r.verb == "DELETE" && pathOf(r) == "/v1.0/me/calendars/CAL/events/E" ? FakeReply{204, {}, {}} : jsonReply(404, "{}");
        };
        EventQueue queue(&ctx);
        const qint64 id = store->eventByGraphId(QStringLiteral("E"))->id;
        QVERIFY(queue.update(id, {{QStringLiteral("subject"), QStringLiteral("Planning, moved")}}));
        QVERIFY(queue.update(id, {{QStringLiteral("subject"), QStringLiteral("Planning, moved again")}}));
        QVERIFY(queue.remove(id, false, {}));
        const QList<OperationRow> ops = store->allOperations();
        QCOMPARE(ops.size(), 1);
        QCOMPARE(ops.first().kind, QStringLiteral("eventDelete"));
        // A sync before the delete reached Exchange
        store->upsertItem(ItemKind::Event, calendar, Mapper::event(makeEvent(QStringLiteral("E"), QStringLiteral("singleInstance"), QStringLiteral("Planning"))));
        run(queue);
        QCOMPARE(server->requests.size(), 1);
        QVERIFY(!store->eventByGraphId(QStringLiteral("E")));
    }

    // A change of an event Exchange has in Deleted Items: refused with 400 "non-calendar folder".
    // The event is gone, not put back (0.14.5: it came back and could not be moved)
    void changeOfAnEventInDeletedItems()
    {
        store->upsertItem(ItemKind::Event, calendar, Mapper::event(makeEvent(QStringLiteral("E"), QStringLiteral("singleInstance"), QStringLiteral("Planning"))));
        server->handler = [](const FakeRequest &) -> FakeReply {
            return jsonReply(400, R"({"error":{"code":"ErrorInvalidRequest","message":"Your request cannot be completed. This operation does not support binding to a non-calendar folder."}})");
        };
        EventQueue queue(&ctx);
        QSignalSpy failed(&queue, &EventQueue::failed);
        QVERIFY(queue.update(store->eventByGraphId(QStringLiteral("E"))->id, {{QStringLiteral("subject"), QStringLiteral("Planning, moved")}}));
        run(queue);
        QVERIFY(!store->eventByGraphId(QStringLiteral("E")));
        QCOMPARE(store->pendingOperationCount(), 0);
        QCOMPARE(failed.size(), 0);
        // Any other refusal still puts the event back as it was
        store->upsertItem(ItemKind::Event, calendar, Mapper::event(makeEvent(QStringLiteral("F"), QStringLiteral("singleInstance"), QStringLiteral("Review"))));
        server->handler = [](const FakeRequest &) -> FakeReply {
            return jsonReply(400, R"({"error":{"code":"ErrorInvalidRequest","message":"Invalid value."}})");
        };
        QVERIFY(queue.update(store->eventByGraphId(QStringLiteral("F"))->id, {{QStringLiteral("subject"), QStringLiteral("Review, moved")}}));
        run(queue);
        QCOMPARE(store->eventByGraphId(QStringLiteral("F"))->subject, QStringLiteral("Review"));
        QCOMPARE(failed.size(), 1);
    }

    // Moved right after creating it (0.14.5): the change waits for the create, then goes to the
    // event Exchange made, onto its version, and the calendar shows the move all along
    void changeRightAfterCreate()
    {
        server->handler = [](const FakeRequest &r) -> FakeReply {
            if (r.verb == "POST" && pathOf(r) == "/v1.0/me/calendars/CAL/events") {
                QJsonObject made = makeEvent(QStringLiteral("NEW"), QStringLiteral("singleInstance"), QStringLiteral("Planning"));
                made.insert(QStringLiteral("@odata.etag"), QStringLiteral("v1"));
                return jsonReply(201, QJsonDocument(made).toJson(QJsonDocument::Compact));
            }
            if (r.verb == "PATCH" && pathOf(r) == "/v1.0/me/calendars/CAL/events/NEW") {
                return r.headers.value("if-match") == "v1" ? jsonReply(200, "{}") : jsonReply(412, "{}");
            }
            return jsonReply(404, "{}");
        };
        EventQueue queue(&ctx);
        QJsonObject e = makeEvent({}, {}, QStringLiteral("Planning"));
        e.remove(QStringLiteral("id"));
        e.remove(QStringLiteral("type"));
        QVERIFY(queue.create(calendar, e));
        const EventRow standIn = store->eventsBetween({calendar}, 0, std::numeric_limits<qint64>::max()).first();
        QVERIFY(queue.update(standIn.id, {{QStringLiteral("start"), time(QStringLiteral("2026-11-16T11:00:00"))}, {QStringLiteral("end"), time(QStringLiteral("2026-11-16T12:00:00"))}}));
        run(queue);
        QCOMPARE(server->requests.size(), 1);
        // Created; the move waits, now for the real event, and is shown meanwhile
        const std::optional<EventRow> made = store->eventByGraphId(QStringLiteral("NEW"));
        QVERIFY(made);
        QCOMPARE(QDateTime::fromMSecsSinceEpoch(made->start, QTimeZone::UTC).time(), QTime(11, 0));
        QCOMPARE(store->allOperations().size(), 1);
        QCOMPARE(store->allOperations().first().target, QStringLiteral("NEW"));
        run(queue);
        QCOMPARE(server->requests.size(), 2);
        QCOMPARE(pathOf(server->requests.at(1)), QByteArray("/v1.0/me/calendars/CAL/events/NEW"));
        QCOMPARE(server->requests.at(1).headers.value("if-match"), QByteArray("v1"));
        QCOMPARE(store->pendingOperationCount(), 0);
    }

    void declineRemovesAndRefusalRestores()
    {
        store->upsertItem(ItemKind::Event, calendar, Mapper::event(makeEvent(QStringLiteral("INV"), QStringLiteral("singleInstance"), QStringLiteral("Invitation"))));
        server->handler = [](const FakeRequest &) -> FakeReply {
            return jsonReply(400, R"({"error":{"code":"ErrorInvalidRequest","message":"refused"}})");
        };
        EventQueue queue(&ctx);
        const qint64 id = store->eventByGraphId(QStringLiteral("INV"))->id;
        QVERIFY(queue.respond(id, QStringLiteral("decline"), QStringLiteral("Sorry"), true));
        QVERIFY(!store->eventByGraphId(QStringLiteral("INV")));
        QSignalSpy failed(&queue, &EventQueue::failed);
        run(queue);
        QCOMPARE(failed.size(), 1);
        // Refused: the invitation is back, as it was
        QCOMPARE(store->eventByGraphId(QStringLiteral("INV"))->subject, QStringLiteral("Invitation"));
        const QJsonObject sent = QJsonDocument::fromJson(server->requests.first().body).object();
        QCOMPARE(QString::fromLatin1(pathOf(server->requests.first())), QStringLiteral("/v1.0/me/calendars/CAL/events/INV/decline"));
        QCOMPARE(sent.value(QLatin1String("comment")).toString(), QStringLiteral("Sorry"));
        QVERIFY(sent.value(QLatin1String("sendResponse")).toBool());
        QCOMPARE(store->pendingOperationCount(), 0);
    }

    // Propose New Time (docs/PHASE9.md): Tentative with another time; the time goes as
    // proposedNewTime in UTC and the response is always sent
    void proposeNewTime()
    {
        store->upsertItem(ItemKind::Event, calendar, Mapper::event(makeEvent(QStringLiteral("PROP"), QStringLiteral("singleInstance"), QStringLiteral("Invitation"))));
        server->handler = [](const FakeRequest &) -> FakeReply {
            return jsonReply(202, "");
        };
        EventQueue queue(&ctx);
        const qint64 id = store->eventByGraphId(QStringLiteral("PROP"))->id;
        const qint64 start = QDateTime(QDate(2026, 11, 20), QTime(14, 0), QTimeZone::UTC).toMSecsSinceEpoch();
        QVERIFY(queue.respond(id, QStringLiteral("tentativelyAccept"), QStringLiteral("Later?"), false, start, start + 3600 * 1000));
        run(queue);
        QCOMPARE(QString::fromLatin1(pathOf(server->requests.first())), QStringLiteral("/v1.0/me/calendars/CAL/events/PROP/tentativelyAccept"));
        const QJsonObject sent = QJsonDocument::fromJson(server->requests.first().body).object();
        QVERIFY(sent.value(QLatin1String("sendResponse")).toBool());
        const QJsonObject proposed = sent.value(QLatin1String("proposedNewTime")).toObject();
        QCOMPARE(proposed.value(QLatin1String("start")).toObject().value(QLatin1String("dateTime")).toString().left(19), QStringLiteral("2026-11-20T14:00:00"));
        QCOMPARE(proposed.value(QLatin1String("end")).toObject().value(QLatin1String("dateTime")).toString().left(19), QStringLiteral("2026-11-20T15:00:00"));
        QCOMPARE(proposed.value(QLatin1String("end")).toObject().value(QLatin1String("timeZone")).toString(), QStringLiteral("UTC"));
        // A plain answer has none
        server->requests.clear();
        store->upsertItem(ItemKind::Event, calendar, Mapper::event(makeEvent(QStringLiteral("PLAIN"), QStringLiteral("singleInstance"), QStringLiteral("Invitation"))));
        QVERIFY(queue.respond(store->eventByGraphId(QStringLiteral("PLAIN"))->id, QStringLiteral("accept"), {}, true));
        run(queue);
        QVERIFY(!QJsonDocument::fromJson(server->requests.first().body).object().contains(QLatin1String("proposedNewTime")));
    }

    void overdueRemindersAndDismissAll()
    {
        // Exchange's reminder list: one event over, one still running
        auto time = [](const QString &iso) {
            return QJsonObject{{QStringLiteral("dateTime"), iso}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}};
        };
        const QJsonArray view{QJsonObject{{QStringLiteral("eventId"), QStringLiteral("OLD")}, {QStringLiteral("eventSubject"), QStringLiteral("Long gone")},
                                          {QStringLiteral("eventStartTime"), time(QStringLiteral("2026-03-27T11:00:00"))},
                                          {QStringLiteral("eventEndTime"), time(QStringLiteral("2026-03-27T12:00:00"))}},
                              QJsonObject{{QStringLiteral("eventId"), QStringLiteral("NOW")}, {QStringLiteral("eventSubject"), QStringLiteral("Running")},
                                          {QStringLiteral("eventStartTime"), time(QStringLiteral("2026-11-11T13:00:00"))},
                                          {QStringLiteral("eventEndTime"), time(QStringLiteral("2026-11-11T15:00:00"))}}};
        const qint64 now = QDateTime(QDate(2026, 11, 11), QTime(14, 0), QTimeZone::UTC).toMSecsSinceEpoch();
        const QList<OverdueReminder> overdue = Reminders::overdue(view, now);
        QCOMPARE(overdue.size(), 1);
        QCOMPARE(overdue.first().eventId, QStringLiteral("OLD"));
        QCOMPARE(overdue.first().subject, QStringLiteral("Long gone"));

        // Dismissing one outside the window goes through the mailbox's events
        server->handler = [](const FakeRequest &) -> FakeReply {
            return jsonReply(200, "{}");
        };
        EventQueue queue(&ctx);
        QVERIFY(queue.dismissReminderByGraphId(QStringLiteral("OLD")));
        run(queue);
        QCOMPARE(QString::fromLatin1(pathOf(server->requests.first())), QStringLiteral("/v1.0/me/events/OLD/dismissReminder"));
    }

    void exceptionsOfASeries()
    {
        auto occurrence = [](const QString &id, const QString &type) {
            return QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("type"), type}, {QStringLiteral("seriesMasterId"), QStringLiteral("M")},
                               {QStringLiteral("start"), QJsonObject{{QStringLiteral("dateTime"), QStringLiteral("2026-11-16T09:00:00")}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}}},
                               {QStringLiteral("end"), QJsonObject{{QStringLiteral("dateTime"), QStringLiteral("2026-11-16T10:00:00")}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}}}};
        };
        store->upsertItem(ItemKind::Event, calendar, Mapper::event(occurrence(QStringLiteral("O1"), QStringLiteral("occurrence"))));
        store->upsertItem(ItemKind::Event, calendar, Mapper::event(occurrence(QStringLiteral("O2"), QStringLiteral("exception"))));
        store->upsertItem(ItemKind::Event, calendar, Mapper::event(occurrence(QStringLiteral("O3"), QStringLiteral("exception"))));
        QCOMPARE(store->exceptionCount(QStringLiteral("M")), 2);
        QCOMPARE(store->exceptionCount(QStringLiteral("other")), 0);
    }

    void cancelGoesToTheAttendees()
    {
        store->upsertItem(ItemKind::Event, calendar, Mapper::event(makeEvent(QStringLiteral("MEET"), QStringLiteral("singleInstance"), QStringLiteral("Meeting"))));
        server->handler = [](const FakeRequest &) -> FakeReply {
            return jsonReply(202, "");
        };
        EventQueue queue(&ctx);
        QVERIFY(queue.remove(store->eventByGraphId(QStringLiteral("MEET"))->id, true, QStringLiteral("Moved to next week")));
        QVERIFY(!store->eventByGraphId(QStringLiteral("MEET")));
        run(queue);
        QCOMPARE(QString::fromLatin1(pathOf(server->requests.first())), QStringLiteral("/v1.0/me/calendars/CAL/events/MEET/cancel"));
        // Message changes are not picked up by the calendar queue, and the other way round
        QVERIFY(store->dueOperations(std::numeric_limits<qint64>::max(), 10).isEmpty());
    }
};

QTEST_GUILESS_MAIN(EventQueueTest)

#include "eventqueuetest.moc"
