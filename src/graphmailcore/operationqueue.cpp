// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "operationqueue.h"
#include "blobstore.h"
#include "graphutil.h"
#include "graphmail_debug.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>

using namespace GraphCore;

namespace GraphMail
{

namespace
{
QString flagName(int flag)
{
    switch (flag) {
    case 1:
        return QStringLiteral("flagged");
    case 2:
        return QStringLiteral("complete");
    default:
        return QStringLiteral("notFlagged");
    }
}
// Retries: 30 s, 1 min, 2 min, ... up to an hour
constexpr qint64 firstRetryMs = 30 * 1000;
constexpr qint64 maxRetryMs = 3600 * 1000;

QJsonObject payloadOf(const OperationRow &op)
{
    return QJsonDocument::fromJson(op.payload.toUtf8()).object();
}

QString encode(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

// A permanently deleted message is removed from the cache at once; this snapshot brings it back
// if Graph refuses
QJsonObject snapshot(const MessageRow &m)
{
    return {{QStringLiteral("folder"), m.folder},
            {QStringLiteral("conversation"), m.conversation},
            {QStringLiteral("received"), m.received},
            {QStringLiteral("sent"), m.sent},
            {QStringLiteral("senderName"), m.senderName},
            {QStringLiteral("senderAddress"), m.senderAddress},
            {QStringLiteral("subject"), m.subject},
            {QStringLiteral("preview"), m.preview},
            {QStringLiteral("toList"), m.toList},
            {QStringLiteral("ccList"), m.ccList},
            {QStringLiteral("isRead"), m.isRead},
            {QStringLiteral("flag"), m.flag},
            {QStringLiteral("importance"), m.importance},
            {QStringLiteral("hasAttachments"), m.hasAttachments},
            {QStringLiteral("categories"), QJsonArray::fromStringList(m.categories)},
            {QStringLiteral("focused"), m.focused},
            {QStringLiteral("internetMessageId"), m.internetMessageId},
            {QStringLiteral("changeKey"), m.changeKey}};
}

MessageRow restore(const QString &graphId, const QJsonObject &s)
{
    MessageRow m;
    m.graphId = graphId;
    m.folder = s.value(QLatin1String("folder")).toInteger();
    m.conversation = s.value(QLatin1String("conversation")).toString();
    m.received = s.value(QLatin1String("received")).toInteger();
    m.sent = s.value(QLatin1String("sent")).toInteger();
    m.senderName = s.value(QLatin1String("senderName")).toString();
    m.senderAddress = s.value(QLatin1String("senderAddress")).toString();
    m.subject = s.value(QLatin1String("subject")).toString();
    m.preview = s.value(QLatin1String("preview")).toString();
    m.toList = s.value(QLatin1String("toList")).toString();
    m.ccList = s.value(QLatin1String("ccList")).toString();
    m.isRead = s.value(QLatin1String("isRead")).toBool();
    m.flag = s.value(QLatin1String("flag")).toInt();
    m.importance = s.value(QLatin1String("importance")).toInt();
    m.hasAttachments = s.value(QLatin1String("hasAttachments")).toBool();
    m.focused = s.value(QLatin1String("focused")).toBool(true);
    const QJsonArray categories = s.value(QLatin1String("categories")).toArray();
    for (const QJsonValue &c : categories) {
        m.categories.append(c.toString());
    }
    m.internetMessageId = s.value(QLatin1String("internetMessageId")).toString();
    m.changeKey = s.value(QLatin1String("changeKey")).toString();
    return m;
}
}

OperationQueue::OperationQueue(SyncContext *context, QObject *parent)
    : QObject(parent)
    , m_ctx(context)
{
}

int OperationQueue::inFlight() const
{
    return int(m_inFlight.size());
}

bool OperationQueue::markRead(qint64 message, bool read, bool receipt)
{
    const std::optional<MessageRow> m = m_ctx->store->message(message);
    const QJsonObject extra = read && !receipt ? QJsonObject{{QStringLiteral("noReceipt"), true}} : QJsonObject();
    return m && enqueue(message, QStringLiteral("read"), read, m->isRead, extra);
}

bool OperationQueue::setFlag(qint64 message, int flag, qint64 start, qint64 due)
{
    const std::optional<MessageRow> m = m_ctx->store->message(message);
    QJsonObject dates;
    if (flag == 1 && due > 0) {
        dates = {{QStringLiteral("start"), start > 0 ? start : due}, {QStringLiteral("due"), due}};
    }
    return m && flag >= 0 && flag < 3 && enqueue(message, QStringLiteral("flag"), flag, m->flag, dates);
}

bool OperationQueue::setCategories(qint64 message, const QStringList &categories)
{
    const std::optional<MessageRow> m = m_ctx->store->message(message);
    return m && enqueue(message, QStringLiteral("categories"), QJsonArray::fromStringList(categories), QJsonArray::fromStringList(m->categories));
}

bool OperationQueue::setFocused(qint64 message, bool focused)
{
    const std::optional<MessageRow> m = m_ctx->store->message(message);
    return m && enqueue(message, QStringLiteral("focus"), focused, m->focused);
}

bool OperationQueue::move(qint64 message, qint64 folder)
{
    const std::optional<MessageRow> m = m_ctx->store->message(message);
    if (!m || !m_ctx->store->folder(folder)) {
        return false;
    }
    return enqueue(message, QStringLiteral("move"), folder, m->folder);
}

bool OperationQueue::remove(qint64 message)
{
    const std::optional<MessageRow> m = m_ctx->store->message(message);
    if (!m) {
        return false;
    }
    const std::optional<FolderRow> deleted = m_ctx->store->folderByWellKnown(QStringLiteral("deleteditems"));
    if (deleted && deleted->id != m->folder) {
        return move(message, deleted->id);
    }
    return enqueue(message, QStringLiteral("delete"), true, snapshot(*m));
}

bool OperationQueue::enqueue(qint64 message, const QString &kind, const QJsonValue &value, const QJsonValue &previous, const QJsonObject &extra)
{
    Store *store = m_ctx->store;
    const std::optional<MessageRow> m = store->message(message);
    if (!m) {
        return false;
    }
    return store->inTransaction([&] {
        OperationRow op;
        op.kind = kind;
        op.target = m->graphId;
        if (const std::optional<OperationRow> queued = store->pendingOperation(m->graphId, kind); queued && kind != QLatin1String("delete")) {
            // Merge: keep the original "previous", take the new value
            QJsonObject payload = payloadOf(*queued);
            payload.remove(QStringLiteral("start"));
            payload.remove(QStringLiteral("due"));
            // A read receipt choice belongs to the newest request only (it comes again in extra)
            payload.remove(QStringLiteral("noReceipt"));
            for (auto it = extra.begin(); it != extra.end(); ++it) {
                payload.insert(it.key(), it.value());
            }
            if (payload.value(QLatin1String("previous")) == value && extra.isEmpty()) {
                // Back to where Graph still is: nothing to send
                op = *queued;
                payload.insert(QStringLiteral("value"), value);
                op.payload = encode(payload);
                if (!apply(op, false) || !store->removeOperation(queued->id)) {
                    return false;
                }
                return store->operationsFor(m->graphId).isEmpty() ? store->setPending(message, false) : true;
            }
            payload.insert(QStringLiteral("value"), value);
            op = *queued;
            op.payload = encode(payload);
            return store->updateOperationPayload(queued->id, op.payload) && apply(op, false);
        }
        op.created = QDateTime::currentMSecsSinceEpoch();
        QJsonObject fresh{{QStringLiteral("value"), value}, {QStringLiteral("previous"), previous}};
        for (auto it = extra.begin(); it != extra.end(); ++it) {
            fresh.insert(it.key(), it.value());
        }
        op.payload = encode(fresh);
        if (kind == QLatin1String("move")) {
            // The destination's graph id, resolved now: the folder row may change later
            QJsonObject payload = payloadOf(op);
            payload.insert(QStringLiteral("destination"), store->folder(value.toInteger())->graphId);
            op.payload = encode(payload);
        }
        op.id = store->addOperation(op);
        return op.id != 0 && apply(op, false) && (kind == QLatin1String("delete") || store->setPending(message, true));
    });
}

// The local effect of an operation: its new value, or its previous one for a rollback
bool OperationQueue::apply(const OperationRow &op, bool previous)
{
    Store *store = m_ctx->store;
    const QJsonObject payload = payloadOf(op);
    const QJsonValue value = payload.value(previous ? QLatin1String("previous") : QLatin1String("value"));
    if (op.kind == QLatin1String("delete")) {
        if (previous) {
            MessageRow row = restore(op.target, value.toObject());
            return store->upsertMessage(row) != Store::Upsert::Failed;
        }
        const RemovedMessage removed = store->removeMessage(op.target);
        for (const QString &blob : removed.blobs) {
            m_ctx->blobs->remove(blob);
        }
        return true;
    }
    const std::optional<MessageRow> m = store->messageByGraphId(op.target);
    if (!m) {
        return true; // gone meanwhile; nothing to show
    }
    if (op.kind == QLatin1String("read")) {
        return store->setRead(m->id, value.toBool());
    }
    if (op.kind == QLatin1String("flag")) {
        return store->setFlag(m->id, value.toInt());
    }
    if (op.kind == QLatin1String("categories")) {
        QStringList categories;
        const QJsonArray array = value.toArray();
        for (const QJsonValue &c : array) {
            categories.append(c.toString());
        }
        return store->setCategories(m->id, categories);
    }
    if (op.kind == QLatin1String("move")) {
        return store->setMessageFolder(m->id, value.toInteger());
    }
    if (op.kind == QLatin1String("focus")) {
        return store->setFocused(m->id, value.toBool());
    }
    return false;
}

void OperationQueue::reapply(const QString &graphId)
{
    const QList<OperationRow> ops = m_ctx->store->operationsFor(graphId);
    for (const OperationRow &op : ops) {
        apply(op, false);
    }
}

void OperationQueue::clearPendingIfDone(const QString &graphId)
{
    if (m_ctx->store->operationsFor(graphId).isEmpty()) {
        if (const std::optional<MessageRow> m = m_ctx->store->messageByGraphId(graphId)) {
            m_ctx->store->setPending(m->id, false);
        }
    }
}

void OperationQueue::process(int slots)
{
    const qint64 now = m_ctx->now ? m_ctx->now() : QDateTime::currentMSecsSinceEpoch();
    const QList<OperationRow> due = m_ctx->store->dueOperations(now, slots + int(m_inFlight.size()));
    for (const OperationRow &op : due) {
        if (slots <= 0) {
            break;
        }
        if (m_inFlight.contains(op.id)) {
            continue;
        }
        const QJsonObject payload = payloadOf(op);
        const QString base = m_ctx->root + QStringLiteral("/messages/") + op.target;
        GraphRequestSpec spec;
        spec.headers = {Prefer::header({Prefer::immutableId()}), {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}};
        QJsonObject body;
        if (op.kind == QLatin1String("read")) {
            spec.verb = "PATCH";
            body.insert(QStringLiteral("isRead"), payload.value(QLatin1String("value")).toBool());
            // Probed 2026-11-16: Exchange sends a requested read receipt as soon as isRead turns
            // true; with PR_READ_RECEIPT_REQUESTED cleared in the same PATCH it sends none
            if (payload.value(QLatin1String("noReceipt")).toBool() && payload.value(QLatin1String("value")).toBool()) {
                body.insert(QStringLiteral("singleValueExtendedProperties"),
                            QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("Boolean 0x0029")}, {QStringLiteral("value"), QStringLiteral("false")}}});
            }
        } else if (op.kind == QLatin1String("flag")) {
            spec.verb = "PATCH";
            QJsonObject flag{{QStringLiteral("flagStatus"), flagName(payload.value(QLatin1String("value")).toInt())}};
            // Follow-up dates (docs/PHASE9.md): whole days, in UTC as Outlook writes them
            if (payload.contains(QLatin1String("due"))) {
                auto day = [](qint64 ms) {
                    const QDate date = QDateTime::fromMSecsSinceEpoch(ms).date();
                    return QJsonObject{{QStringLiteral("dateTime"), QString(date.toString(Qt::ISODate) + QStringLiteral("T00:00:00"))}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}};
                };
                flag.insert(QStringLiteral("startDateTime"), day(payload.value(QLatin1String("start")).toInteger()));
                flag.insert(QStringLiteral("dueDateTime"), day(payload.value(QLatin1String("due")).toInteger()));
            }
            body.insert(QStringLiteral("flag"), flag);
        } else if (op.kind == QLatin1String("focus")) {
            spec.verb = "PATCH";
            body.insert(QStringLiteral("inferenceClassification"), payload.value(QLatin1String("value")).toBool() ? QStringLiteral("focused") : QStringLiteral("other"));
        } else if (op.kind == QLatin1String("categories")) {
            spec.verb = "PATCH";
            body.insert(QStringLiteral("categories"), payload.value(QLatin1String("value")));
        } else if (op.kind == QLatin1String("move")) {
            spec.verb = "POST";
            spec.pathOrUrl = base + QStringLiteral("/move");
            body.insert(QStringLiteral("destinationId"), payload.value(QLatin1String("destination")).toString());
        } else if (op.kind == QLatin1String("delete")) {
            spec.verb = "DELETE";
            spec.headers.removeLast();
        } else {
            qCWarning(GRAPHMAIL) << "Dropping an operation of unknown kind" << op.kind;
            m_ctx->store->removeOperation(op.id);
            continue;
        }
        if (spec.pathOrUrl.isEmpty()) {
            spec.pathOrUrl = base;
        }
        if (!body.isEmpty()) {
            spec.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
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

bool OperationQueue::isInFlight(qint64 op) const
{
    return m_inFlight.contains(op);
}

bool OperationQueue::discard(const OperationRow &op)
{
    if (isInFlight(op.id)) {
        return false;
    }
    const bool ok = m_ctx->store->inTransaction([&] {
        return apply(op, true) && m_ctx->store->removeOperation(op.id);
    });
    clearPendingIfDone(op.target);
    return ok;
}

void OperationQueue::finished(const OperationRow &op, const GraphResponse &response)
{
    Store *store = m_ctx->store;
    // The message is already gone on the server: a delete is done, anything else is moot
    if (response.ok() || response.status == 404) {
        // A move of a message gone on the server: the row kept for it (a folder's removal is
        // skipped while a move into it waits) goes too
        if (response.status == 404 && op.kind == QLatin1String("move")) {
            const RemovedMessage removed = store->removeMessage(op.target);
            for (const QString &blob : removed.blobs) {
                m_ctx->blobs->remove(blob);
            }
        }
        store->removeOperation(op.id);
        clearPendingIfDone(op.target);
        Q_EMIT completed(op.target, op.kind);
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
    // Refused for good (no permission, invalid folder, ...): show the old state again
    qCWarning(GRAPHMAIL).noquote() << "Graph refused" << op.kind << "for a message:" << response.describe();
    store->inTransaction([&] {
        return apply(op, true) && store->removeOperation(op.id);
    });
    clearPendingIfDone(op.target);
    Q_EMIT failed(op.target, op.kind, response.userMessage());
}

}
