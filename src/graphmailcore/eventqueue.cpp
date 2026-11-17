// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "eventqueue.h"
#include "conflict.h"
#include "graphmail_debug.h"
#include "graphmapper.h"
#include "graphrequest.h"
#include "graphutil.h"
#include "store.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QUuid>

using namespace GraphCore;

namespace GraphMail
{

namespace
{
constexpr qint64 firstRetryMs = 30 * 1000;
constexpr qint64 maxRetryMs = 30 * 60 * 1000;

QJsonObject payloadOf(const OperationRow &op)
{
    return QJsonDocument::fromJson(op.payload.toUtf8()).object();
}

QJsonObject jsonOf(const EventRow &e)
{
    return QJsonDocument::fromJson(e.json.toUtf8()).object();
}

QString isoUtc(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"));
}

// The event JSON the cache keeps for an event it does not hold whole (an occurrence)
QJsonObject cachedJson(Store *store, const EventRow &e)
{
    const std::optional<EventRow> own = store->eventByGraphId(e.graphId);
    return own ? jsonOf(*own) : jsonOf(e);
}
}

EventQueue::EventQueue(SyncContext *context, QObject *parent)
    : QObject(parent)
    , m_ctx(context)
{
}

int EventQueue::inFlight() const
{
    return int(m_inFlight.size());
}

QString EventQueue::eventPath(const QString &calendarGraphId, const QString &graphId) const
{
    // Without a known calendar (a meeting request's event): the mailbox's events
    QString events = m_ctx->root + QStringLiteral("/events");
    if (!calendarGraphId.isEmpty()) {
        events = m_ctx->root + QStringLiteral("/calendars/") + calendarGraphId + QStringLiteral("/events");
    }
    return graphId.isEmpty() ? events : events + QLatin1Char('/') + graphId;
}

bool EventQueue::enqueue(const QString &kind, const QString &target, const QJsonObject &payload)
{
    OperationRow op;
    op.created = m_ctx->now ? m_ctx->now() : QDateTime::currentMSecsSinceEpoch();
    op.kind = kind;
    op.target = target;
    op.payload = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    return m_ctx->store->addOperation(op) != 0;
}

bool EventQueue::create(qint64 calendar, const QJsonObject &event)
{
    Store *store = m_ctx->store;
    const std::optional<CalendarRow> cal = store->calendar(calendar);
    if (!cal) {
        return false;
    }
    const QString localId = QStringLiteral("local:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject shown = event;
    shown.insert(QStringLiteral("id"), localId);
    shown.insert(QStringLiteral("type"), event.contains(QLatin1String("recurrence")) && !event.value(QLatin1String("recurrence")).isNull() ? QStringLiteral("seriesMaster") : QStringLiteral("singleInstance"));
    // Whoever makes an event organizes it
    shown.insert(QStringLiteral("responseStatus"), QJsonObject{{QStringLiteral("response"), QStringLiteral("organizer")}});
    return store->inTransaction([&] {
        return store->upsertItem(ItemKind::Event, calendar, Mapper::event(shown)) != Store::Upsert::Failed
            && enqueue(QStringLiteral("eventCreate"), localId, {{QStringLiteral("calendar"), cal->graphId}, {QStringLiteral("calendarRow"), calendar}, {QStringLiteral("event"), event}});
    });
}

bool EventQueue::update(qint64 event, const QJsonObject &changes)
{
    Store *store = m_ctx->store;
    const std::optional<EventRow> e = store->event(event);
    const std::optional<CalendarRow> cal = e ? store->calendar(e->calendar) : std::nullopt;
    if (!e || !cal) {
        return false;
    }
    const QJsonObject previous = cachedJson(store, *e);
    QJsonObject next = jsonOf(*e);
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
        next.insert(it.key(), it.value());
    }
    // The row keeps its own identity (an occurrence keeps its id, type and master)
    const QJsonObject own = previous;
    for (const char *key : {"id", "type", "seriesMasterId"}) {
        if (own.contains(QLatin1String(key))) {
            next.insert(QLatin1String(key), own.value(QLatin1String(key)));
        }
    }
    if (e->type == 1) {
        // Changing one occurrence makes it an exception
        next.insert(QStringLiteral("type"), QStringLiteral("exception"));
    }
    // The cache skips rows whose change key it has seen: this one changed here first
    next.remove(QStringLiteral("changeKey"));
    return store->inTransaction([&] {
        return store->upsertItem(ItemKind::Event, e->calendar, Mapper::event(next)) != Store::Upsert::Failed
            && enqueue(QStringLiteral("eventUpdate"),
                       e->graphId,
                       {{QStringLiteral("calendar"), cal->graphId}, {QStringLiteral("calendarRow"), e->calendar}, {QStringLiteral("changes"), changes}, {QStringLiteral("previous"), previous}});
    });
}

bool EventQueue::remove(qint64 event, bool cancel, const QString &comment)
{
    Store *store = m_ctx->store;
    const std::optional<EventRow> e = store->event(event);
    const std::optional<CalendarRow> cal = e ? store->calendar(e->calendar) : std::nullopt;
    if (!e || !cal) {
        return false;
    }
    const QJsonObject previous = cachedJson(store, *e);
    return store->inTransaction([&] {
        return store->removeItem(ItemKind::Event, e->graphId)
            && enqueue(cancel ? QStringLiteral("eventCancel") : QStringLiteral("eventDelete"),
                       e->graphId,
                       {{QStringLiteral("calendar"), cal->graphId}, {QStringLiteral("calendarRow"), e->calendar}, {QStringLiteral("comment"), comment}, {QStringLiteral("previous"), previous}});
    });
}

bool EventQueue::respond(qint64 event, const QString &action, const QString &comment, bool sendResponse, qint64 proposedStart, qint64 proposedEnd)
{
    Store *store = m_ctx->store;
    const std::optional<EventRow> e = store->event(event);
    const std::optional<CalendarRow> cal = e ? store->calendar(e->calendar) : std::nullopt;
    if (!e || !cal) {
        return false;
    }
    const QJsonObject previous = cachedJson(store, *e);
    QJsonObject next = previous;
    const QString response = action == QLatin1String("accept") ? QStringLiteral("accepted")
        : action == QLatin1String("decline")                   ? QStringLiteral("declined")
                                                                : QStringLiteral("tentativelyAccepted");
    next.insert(QStringLiteral("responseStatus"), QJsonObject{{QStringLiteral("response"), response}});
    if (e->type == 1) {
        next.insert(QStringLiteral("type"), QStringLiteral("exception"));
    }
    next.remove(QStringLiteral("changeKey"));
    QJsonObject payload{{QStringLiteral("calendar"), cal->graphId},
                        {QStringLiteral("calendarRow"), e->calendar},
                        {QStringLiteral("action"), action},
                        {QStringLiteral("comment"), comment},
                        {QStringLiteral("sendResponse"), sendResponse},
                        {QStringLiteral("previous"), previous}};
    if (proposedStart > 0 && proposedEnd > proposedStart) {
        payload.insert(QStringLiteral("proposedStart"), proposedStart);
        payload.insert(QStringLiteral("proposedEnd"), proposedEnd);
    }
    return store->inTransaction([&] {
        // A declined meeting leaves the calendar, as in Outlook
        const bool local = action == QLatin1String("decline") ? store->removeItem(ItemKind::Event, e->graphId)
                                                               : store->upsertItem(ItemKind::Event, e->calendar, Mapper::event(next)) != Store::Upsert::Failed;
        return local && enqueue(QStringLiteral("eventRespond"), e->graphId, payload);
    });
}

bool EventQueue::respondByGraphId(const QString &graphId, const QString &calendarGraphId, const QString &action, const QString &comment, bool sendResponse, qint64 proposedStart,
                                  qint64 proposedEnd)
{
    if (const std::optional<EventRow> e = m_ctx->store->eventByGraphId(graphId)) {
        return respond(e->id, action, comment, sendResponse, proposedStart, proposedEnd);
    }
    QJsonObject payload{{QStringLiteral("calendar"), calendarGraphId},
                        {QStringLiteral("action"), action},
                        {QStringLiteral("comment"), comment},
                        {QStringLiteral("sendResponse"), sendResponse}};
    if (proposedStart > 0 && proposedEnd > proposedStart) {
        payload.insert(QStringLiteral("proposedStart"), proposedStart);
        payload.insert(QStringLiteral("proposedEnd"), proposedEnd);
    }
    return enqueue(QStringLiteral("eventRespond"), graphId, payload);
}

bool EventQueue::dismissReminder(qint64 event)
{
    const std::optional<EventRow> e = m_ctx->store->event(event);
    const std::optional<CalendarRow> cal = e ? m_ctx->store->calendar(e->calendar) : std::nullopt;
    return e && cal && enqueue(QStringLiteral("eventDismiss"), e->graphId, {{QStringLiteral("calendar"), cal->graphId}});
}

bool EventQueue::dismissReminderByGraphId(const QString &graphId)
{
    // No calendar: the mailbox's /events path finds it
    return !graphId.isEmpty() && enqueue(QStringLiteral("eventDismiss"), graphId, {{QStringLiteral("calendar"), QString()}});
}

bool EventQueue::snoozeReminder(qint64 event, qint64 until)
{
    const std::optional<EventRow> e = m_ctx->store->event(event);
    const std::optional<CalendarRow> cal = e ? m_ctx->store->calendar(e->calendar) : std::nullopt;
    return e && cal && enqueue(QStringLiteral("eventSnooze"), e->graphId, {{QStringLiteral("calendar"), cal->graphId}, {QStringLiteral("until"), until}});
}

void EventQueue::process(int slots)
{
    const qint64 now = m_ctx->now ? m_ctx->now() : QDateTime::currentMSecsSinceEpoch();
    const QList<OperationRow> due = m_ctx->store->dueOperations(now, slots + int(m_inFlight.size()), QStringLiteral("event"));
    for (const OperationRow &op : due) {
        if (slots <= 0) {
            break;
        }
        if (m_inFlight.contains(op.id)) {
            continue;
        }
        const QJsonObject payload = payloadOf(op);
        const QString calendar = payload.value(QLatin1String("calendar")).toString();
        GraphRequestSpec spec;
        spec.headers = {Prefer::header({Prefer::immutableId(), QByteArrayLiteral("outlook.timezone=\"UTC\"")}),
                        {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}};
        QJsonObject body;
        if (op.kind == QLatin1String("eventCreate")) {
            spec.verb = "POST";
            spec.pathOrUrl = eventPath(calendar, {});
            body = payload.value(QLatin1String("event")).toObject();
        } else if (op.kind == QLatin1String("eventUpdate")) {
            spec.verb = "PATCH";
            spec.pathOrUrl = eventPath(calendar, op.target);
            body = payload.value(QLatin1String("changes")).toObject();
        } else if (op.kind == QLatin1String("eventDelete")) {
            spec.verb = "DELETE";
            spec.pathOrUrl = eventPath(calendar, op.target);
            spec.headers.removeLast();
        } else if (op.kind == QLatin1String("eventCancel")) {
            spec.verb = "POST";
            spec.pathOrUrl = eventPath(calendar, op.target) + QStringLiteral("/cancel");
            body = {{QStringLiteral("comment"), payload.value(QLatin1String("comment")).toString()}};
        } else if (op.kind == QLatin1String("eventRespond")) {
            spec.verb = "POST";
            spec.pathOrUrl = eventPath(calendar, op.target) + QLatin1Char('/') + payload.value(QLatin1String("action")).toString();
            body = {{QStringLiteral("comment"), payload.value(QLatin1String("comment")).toString()},
                    {QStringLiteral("sendResponse"), payload.value(QLatin1String("sendResponse")).toBool(true)}};
            // Propose New Time (docs/PHASE9.md): only with Tentative or Decline, and sent
            if (payload.contains(QLatin1String("proposedStart")) && payload.value(QLatin1String("action")).toString() != QLatin1String("accept")) {
                body.insert(QStringLiteral("proposedNewTime"),
                            QJsonObject{{QStringLiteral("start"),
                                         QJsonObject{{QStringLiteral("dateTime"), isoUtc(payload.value(QLatin1String("proposedStart")).toInteger())}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}}},
                                        {QStringLiteral("end"),
                                         QJsonObject{{QStringLiteral("dateTime"), isoUtc(payload.value(QLatin1String("proposedEnd")).toInteger())}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}}}});
                body.insert(QStringLiteral("sendResponse"), true);
            }
        } else if (op.kind == QLatin1String("eventDismiss")) {
            spec.verb = "POST";
            spec.pathOrUrl = eventPath(calendar, op.target) + QStringLiteral("/dismissReminder");
        } else if (op.kind == QLatin1String("eventSnooze")) {
            spec.verb = "POST";
            spec.pathOrUrl = eventPath(calendar, op.target) + QStringLiteral("/snoozeReminder");
            body = {{QStringLiteral("newReminderTime"),
                     QJsonObject{{QStringLiteral("dateTime"), isoUtc(payload.value(QLatin1String("until")).toInteger())}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}}}};
        } else {
            qCWarning(GRAPHMAIL) << "Dropping a calendar operation of unknown kind" << op.kind;
            m_ctx->store->removeOperation(op.id);
            continue;
        }
        if (spec.verb == "POST" || spec.verb == "PATCH") {
            spec.body = body.isEmpty() ? QByteArray("{}") : QJsonDocument(body).toJson(QJsonDocument::Compact);
        }
        // Only onto the version the change was made on: a change elsewhere meanwhile gives 412
        if (op.kind == QLatin1String("eventUpdate")) {
            if (const QByteArray tag = ifMatch(payload.value(QLatin1String("previous")).toObject()); !tag.isEmpty()) {
                spec.headers.append({QByteArrayLiteral("If-Match"), tag});
            }
        }
        m_inFlight.insert(op.id);
        --slots;
        GraphRequest *request = m_ctx->client->start(spec);
        connect(request, &GraphRequest::finished, this, [this, request, op] {
            const GraphResponse response = request->response();
            request->deleteLater();
            m_inFlight.remove(op.id);
            finished(op, response);
        });
    }
}

void EventQueue::resolveConflict(const OperationRow &op)
{
    const QJsonObject payload = payloadOf(op);
    GraphRequestSpec spec;
    spec.pathOrUrl = eventPath(payload.value(QLatin1String("calendar")).toString(), op.target);
    spec.headers = {Prefer::header({Prefer::immutableId(), QByteArrayLiteral("outlook.timezone=\"UTC\"")})};
    m_inFlight.insert(op.id);
    GraphRequest *request = m_ctx->client->start(spec);
    connect(request, &GraphRequest::finished, this, [this, request, op, payload] {
        const GraphResponse response = request->response();
        request->deleteLater();
        m_inFlight.remove(op.id);
        Store *store = m_ctx->store;
        if (!response.ok()) {
            finished(op, response.isNetworkError() ? response : GraphResponse{400, response.body, response.errorString, response.headers});
            return;
        }
        const QJsonObject server = response.json();
        const QJsonObject changes = payload.value(QLatin1String("changes")).toObject();
        const QStringList both = conflictingFields(changes, payload.value(QLatin1String("previous")).toObject(), server);
        if (both.isEmpty()) {
            QJsonObject next = payload;
            next.insert(QStringLiteral("previous"), server);
            store->updateOperationPayload(op.id, QString::fromUtf8(QJsonDocument(next).toJson(QJsonDocument::Compact)));
            store->retryOperationLater(op.id, op.attempts + 1, 0, QStringLiteral("merged with a change made elsewhere"));
            qCInfo(GRAPHMAIL) << "Merged an event change with one made elsewhere:" << changes.keys();
            return;
        }
        store->inTransaction([&] {
            return store->upsertItem(ItemKind::Event, payload.value(QLatin1String("calendarRow")).toInteger(), Mapper::event(server)) != Store::Upsert::Failed
                && store->removeOperation(op.id);
        });
        const std::optional<EventRow> row = store->eventByGraphId(op.target);
        qCInfo(GRAPHMAIL) << "Conflict on" << both << "- Microsoft 365's version of the event kept";
        Q_EMIT conflict(QStringLiteral("event"), row ? row->id : 0, server.value(QLatin1String("subject")).toString(), both, changes);
    });
}

void EventQueue::finished(const OperationRow &op, const GraphResponse &response)
{
    Store *store = m_ctx->store;
    const QJsonObject payload = payloadOf(op);
    if (response.status == 412 && op.kind == QLatin1String("eventUpdate")) {
        resolveConflict(op);
        return;
    }
    if (response.ok() || (response.status == 404 && op.kind != QLatin1String("eventCreate"))) {
        store->inTransaction([&] {
            if (op.kind == QLatin1String("eventCreate") && response.ok()) {
                // The local stand-in makes way for the event Exchange made
                store->removeItem(ItemKind::Event, op.target);
                const QJsonObject created = response.json();
                if (!created.value(QLatin1String("id")).toString().isEmpty()) {
                    store->upsertItem(ItemKind::Event, payload.value(QLatin1String("calendarRow")).toInteger(), Mapper::event(created));
                }
            }
            return store->removeOperation(op.id);
        });
        Q_EMIT completed(op.kind);
        return;
    }
    const bool transient = response.isNetworkError() || response.status == 408 || response.status == 429 || response.status >= 500;
    if (transient) {
        const qint64 delay = std::min(maxRetryMs, firstRetryMs << std::min(op.attempts, 10));
        const qint64 now = m_ctx->now ? m_ctx->now() : QDateTime::currentMSecsSinceEpoch();
        store->retryOperationLater(op.id, op.attempts + 1, now + delay, response.userMessage());
        if (response.isNetworkError()) {
            Q_EMIT networkError();
        }
        return;
    }
    qCWarning(GRAPHMAIL).noquote() << "Graph refused" << op.kind << "for an event:" << response.describe();
    store->inTransaction([&] {
        rollback(op);
        return store->removeOperation(op.id);
    });
    Q_EMIT failed(op.target, op.kind, response.userMessage());
}

bool EventQueue::isInFlight(qint64 op) const
{
    return m_inFlight.contains(op);
}

bool EventQueue::discard(const OperationRow &op)
{
    if (isInFlight(op.id)) {
        return false;
    }
    return m_ctx->store->inTransaction([&] {
        rollback(op);
        return m_ctx->store->removeOperation(op.id);
    });
}

void EventQueue::rollback(const OperationRow &op)
{
    Store *store = m_ctx->store;
    const QJsonObject payload = payloadOf(op);
    const qint64 calendar = payload.value(QLatin1String("calendarRow")).toInteger();
    const QJsonObject previous = payload.value(QLatin1String("previous")).toObject();
    if (op.kind == QLatin1String("eventCreate")) {
        store->removeItem(ItemKind::Event, op.target);
    } else if (!previous.isEmpty() && calendar != 0) {
        QJsonObject restored = previous;
        restored.remove(QStringLiteral("changeKey"));
        store->upsertItem(ItemKind::Event, calendar, Mapper::event(restored));
    }
}

}
