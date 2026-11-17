// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "itemqueue.h"
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

QString kindName(ItemKind kind)
{
    return kind == ItemKind::Contact ? QStringLiteral("contact") : QStringLiteral("task");
}

ItemKind kindOf(const QString &name)
{
    return name == QLatin1String("contact") ? ItemKind::Contact : ItemKind::Task;
}

ItemRow mapped(ItemKind kind, QJsonObject json)
{
    // Changed here first: without its change key the cache writes it (it skips known keys)
    json.remove(QStringLiteral("changeKey"));
    if (kind == ItemKind::Task) {
        json.remove(QStringLiteral("lastModifiedDateTime"));
    }
    return kind == ItemKind::Contact ? Mapper::contact(json) : Mapper::task(json);
}
}

ItemQueue::ItemQueue(SyncContext *context, QObject *parent)
    : QObject(parent)
    , m_ctx(context)
{
}

int ItemQueue::inFlight() const
{
    return int(m_inFlight.size());
}

QString ItemQueue::collectionPath(ItemKind kind, qint64 parent) const
{
    const QList<ParentRow> parents = m_ctx->store->parents(kind);
    for (const ParentRow &p : parents) {
        if (p.id != parent) {
            continue;
        }
        if (kind == ItemKind::Task) {
            return QStringLiteral("/me/todo/lists/") + p.graphId + QStringLiteral("/tasks");
        }
        // The default folder is synced through /contacts
        if (p.graphId == QLatin1String("contacts")) {
            return m_ctx->root + QStringLiteral("/contacts");
        }
        return m_ctx->root + QStringLiteral("/contactFolders/") + p.graphId + QStringLiteral("/contacts");
    }
    return {};
}

bool ItemQueue::enqueue(const QString &kind, const QString &target, const QJsonObject &payload)
{
    OperationRow op;
    op.created = m_ctx->now ? m_ctx->now() : QDateTime::currentMSecsSinceEpoch();
    op.kind = kind;
    op.target = target;
    op.payload = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    return m_ctx->store->addOperation(op) != 0;
}

bool ItemQueue::create(ItemKind kind, qint64 parent, const QJsonObject &json)
{
    const QString path = collectionPath(kind, parent);
    if (path.isEmpty()) {
        return false;
    }
    Store *store = m_ctx->store;
    const QString localId = QStringLiteral("local:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject shown = json;
    shown.insert(QStringLiteral("id"), localId);
    return store->inTransaction([&] {
        return store->upsertItem(kind, parent, mapped(kind, shown)) != Store::Upsert::Failed
            && enqueue(QStringLiteral("itemCreate"),
                       localId,
                       {{QStringLiteral("kind"), kindName(kind)}, {QStringLiteral("parent"), parent}, {QStringLiteral("path"), path}, {QStringLiteral("body"), json}});
    });
}

bool ItemQueue::update(ItemKind kind, qint64 item, const QJsonObject &changes)
{
    Store *store = m_ctx->store;
    const std::optional<StoredItem> stored = store->item(kind, item);
    if (!stored) {
        return false;
    }
    const QString path = collectionPath(kind, stored->parent);
    const QJsonObject previous = QJsonDocument::fromJson(stored->json.toUtf8()).object();
    QJsonObject next = previous;
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
        next.insert(it.key(), it.value());
    }
    return store->inTransaction([&] {
        return store->upsertItem(kind, stored->parent, mapped(kind, next)) != Store::Upsert::Failed
            && enqueue(QStringLiteral("itemUpdate"),
                       stored->graphId,
                       {{QStringLiteral("kind"), kindName(kind)},
                        {QStringLiteral("parent"), stored->parent},
                        {QStringLiteral("path"), QString(path + QLatin1Char('/') + stored->graphId)},
                        {QStringLiteral("body"), changes},
                        {QStringLiteral("previous"), previous}});
    });
}

bool ItemQueue::remove(ItemKind kind, qint64 item)
{
    Store *store = m_ctx->store;
    const std::optional<StoredItem> stored = store->item(kind, item);
    if (!stored) {
        return false;
    }
    const QString path = collectionPath(kind, stored->parent);
    return store->inTransaction([&] {
        return store->removeItem(kind, stored->graphId)
            && enqueue(QStringLiteral("itemDelete"),
                       stored->graphId,
                       {{QStringLiteral("kind"), kindName(kind)},
                        {QStringLiteral("parent"), stored->parent},
                        {QStringLiteral("path"), QString(path + QLatin1Char('/') + stored->graphId)},
                        {QStringLiteral("previous"), QJsonDocument::fromJson(stored->json.toUtf8()).object()}});
    });
}

bool ItemQueue::createList(const QString &name)
{
    return enqueue(QStringLiteral("itemListCreate"), {}, {{QStringLiteral("path"), QStringLiteral("/me/todo/lists")}, {QStringLiteral("body"), QJsonObject{{QStringLiteral("displayName"), name}}}});
}

bool ItemQueue::renameList(qint64 list, const QString &name)
{
    const QList<ParentRow> lists = m_ctx->store->parents(ItemKind::Task);
    for (const ParentRow &l : lists) {
        if (l.id == list) {
            ParentRow renamed = l;
            renamed.name = name;
            m_ctx->store->upsertParent(ItemKind::Task, renamed);
            return enqueue(QStringLiteral("itemListUpdate"),
                           l.graphId,
                           {{QStringLiteral("path"), QString(QStringLiteral("/me/todo/lists/") + l.graphId)},
                            {QStringLiteral("body"), QJsonObject{{QStringLiteral("displayName"), name}}},
                            {QStringLiteral("previousName"), l.name}});
        }
    }
    return false;
}

bool ItemQueue::deleteList(qint64 list)
{
    const QList<ParentRow> lists = m_ctx->store->parents(ItemKind::Task);
    for (const ParentRow &l : lists) {
        // To Do's own lists (Tasks, Flagged Emails) cannot be deleted
        if (l.id == list && l.wellKnown.isEmpty()) {
            return m_ctx->store->removeParent(ItemKind::Task, l.id)
                && enqueue(QStringLiteral("itemListDelete"), l.graphId, {{QStringLiteral("path"), QString(QStringLiteral("/me/todo/lists/") + l.graphId)}});
        }
    }
    return false;
}

bool ItemQueue::step(qint64 task, const QString &action, const QString &stepId, const QJsonObject &json)
{
    const std::optional<StoredItem> stored = m_ctx->store->item(ItemKind::Task, task);
    if (!stored) {
        return false;
    }
    const QString path = collectionPath(ItemKind::Task, stored->parent) + QLatin1Char('/') + stored->graphId + QStringLiteral("/checklistItems")
        + (stepId.isEmpty() ? QString() : QLatin1Char('/') + stepId);
    const QByteArray verb = action == QLatin1String("create") ? "POST" : action == QLatin1String("delete") ? "DELETE" : "PATCH";
    return enqueue(QStringLiteral("itemStep"), stored->graphId, {{QStringLiteral("path"), path}, {QStringLiteral("verb"), QString::fromLatin1(verb)}, {QStringLiteral("body"), json}});
}

void ItemQueue::process(int slots)
{
    const qint64 now = m_ctx->now ? m_ctx->now() : QDateTime::currentMSecsSinceEpoch();
    const QList<OperationRow> due = m_ctx->store->dueOperations(now, slots + int(m_inFlight.size()), QStringLiteral("item"));
    for (const OperationRow &op : due) {
        if (slots <= 0) {
            break;
        }
        if (m_inFlight.contains(op.id)) {
            continue;
        }
        const QJsonObject payload = payloadOf(op);
        GraphRequestSpec spec;
        spec.pathOrUrl = payload.value(QLatin1String("path")).toString();
        if (op.kind == QLatin1String("itemCreate") || op.kind == QLatin1String("itemListCreate")) {
            spec.verb = "POST";
        } else if (op.kind == QLatin1String("itemUpdate") || op.kind == QLatin1String("itemListUpdate")) {
            spec.verb = "PATCH";
        } else if (op.kind == QLatin1String("itemDelete") || op.kind == QLatin1String("itemListDelete")) {
            spec.verb = "DELETE";
        } else if (op.kind == QLatin1String("itemStep")) {
            spec.verb = payload.value(QLatin1String("verb")).toString().toLatin1();
        } else {
            qCWarning(GRAPHMAIL) << "Dropping a contact or task operation of unknown kind" << op.kind;
            m_ctx->store->removeOperation(op.id);
            continue;
        }
        // Contacts are read with immutable ids. To Do is read without them (TodoSync), and a task
        // POSTed with the header comes back under an id the delta never uses: the cache then held
        // the new task twice (phase 6 live test, 2026-11-11).
        if (!payload.value(QLatin1String("path")).toString().startsWith(QLatin1String("/me/todo/"))) {
            spec.headers = {Prefer::header({Prefer::immutableId()})};
        }
        if (spec.verb == "POST" || spec.verb == "PATCH") {
            spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
            const QJsonObject body = payload.value(QLatin1String("body")).toObject();
            spec.body = body.isEmpty() ? QByteArray("{}") : QJsonDocument(body).toJson(QJsonDocument::Compact);
        }
        // Only onto the version the change was made on: a change elsewhere meanwhile gives 412
        if (op.kind == QLatin1String("itemUpdate")) {
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

void ItemQueue::resolveConflict(const OperationRow &op)
{
    const QJsonObject payload = payloadOf(op);
    const QString path = payload.value(QLatin1String("path")).toString();
    GraphRequestSpec spec;
    spec.pathOrUrl = path;
    if (!path.startsWith(QLatin1String("/me/todo/"))) {
        spec.headers = {Prefer::header({Prefer::immutableId()})};
    }
    m_inFlight.insert(op.id);
    GraphRequest *request = m_ctx->client->start(spec);
    connect(request, &GraphRequest::finished, this, [this, request, op, payload] {
        const GraphResponse response = request->response();
        request->deleteLater();
        m_inFlight.remove(op.id);
        Store *store = m_ctx->store;
        if (!response.ok()) {
            // Gone or unreadable meanwhile: as a refusal
            finished(op, response.isNetworkError() ? response : GraphResponse{400, response.body, response.errorString, response.headers});
            return;
        }
        const QJsonObject server = response.json();
        const QJsonObject changes = payload.value(QLatin1String("body")).toObject();
        const QStringList both = conflictingFields(changes, payload.value(QLatin1String("previous")).toObject(), server);
        const ItemKind kind = kindOf(payload.value(QLatin1String("kind")).toString());
        if (both.isEmpty()) {
            // Other fields changed there: the user's fields go onto that version
            QJsonObject next = payload;
            next.insert(QStringLiteral("previous"), server);
            store->updateOperationPayload(op.id, QString::fromUtf8(QJsonDocument(next).toJson(QJsonDocument::Compact)));
            store->retryOperationLater(op.id, op.attempts + 1, 0, QStringLiteral("merged with a change made elsewhere"));
            qCInfo(GRAPHMAIL) << "Merged a change with one made elsewhere:" << changes.keys();
            return;
        }
        // The same field changed there: Microsoft 365's version stays, the user is asked
        store->inTransaction([&] {
            return store->upsertItem(kind, payload.value(QLatin1String("parent")).toInteger(), mapped(kind, server)) != Store::Upsert::Failed
                && store->removeOperation(op.id);
        });
        const std::optional<StoredItem> row = store->itemByGraphId(kind, op.target);
        const QString title = server.value(kind == ItemKind::Contact ? QLatin1String("displayName") : QLatin1String("title")).toString();
        qCInfo(GRAPHMAIL) << "Conflict on" << both << "- Microsoft 365's version kept";
        Q_EMIT conflict(kindName(kind), row ? row->id : 0, title, both, changes);
    });
}

void ItemQueue::finished(const OperationRow &op, const GraphResponse &response)
{
    Store *store = m_ctx->store;
    const QJsonObject payload = payloadOf(op);
    if (response.status == 412 && op.kind == QLatin1String("itemUpdate")) {
        resolveConflict(op);
        return;
    }
    const bool creating = op.kind == QLatin1String("itemCreate") || op.kind == QLatin1String("itemListCreate");
    if (response.ok() || (response.status == 404 && !creating)) {
        store->inTransaction([&] {
            if (op.kind == QLatin1String("itemCreate") && response.ok()) {
                // The local stand-in makes way for what Graph made
                const ItemKind kind = kindOf(payload.value(QLatin1String("kind")).toString());
                store->removeItem(kind, op.target);
                const QJsonObject made = response.json();
                if (!made.value(QLatin1String("id")).toString().isEmpty()) {
                    store->upsertItem(kind, payload.value(QLatin1String("parent")).toInteger(), kind == ItemKind::Contact ? Mapper::contact(made) : Mapper::task(made));
                }
            } else if (op.kind == QLatin1String("itemListCreate") && response.ok()) {
                const QJsonObject made = response.json();
                store->upsertParent(ItemKind::Task, {0, made.value(QLatin1String("id")).toString(), made.value(QLatin1String("displayName")).toString(), {}, {}, 0, 0, 0});
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
    qCWarning(GRAPHMAIL).noquote() << "Graph refused" << op.kind << "for a contact or task:" << response.describe();
    store->inTransaction([&] {
        rollback(op);
        return store->removeOperation(op.id);
    });
    Q_EMIT failed(op.target, op.kind, response.userMessage());
}

bool ItemQueue::isInFlight(qint64 op) const
{
    return m_inFlight.contains(op);
}

bool ItemQueue::discard(const OperationRow &op)
{
    if (isInFlight(op.id)) {
        return false;
    }
    return m_ctx->store->inTransaction([&] {
        rollback(op);
        return m_ctx->store->removeOperation(op.id);
    });
}

void ItemQueue::rollback(const OperationRow &op)
{
    Store *store = m_ctx->store;
    const QJsonObject payload = payloadOf(op);
    const ItemKind kind = kindOf(payload.value(QLatin1String("kind")).toString());
    const qint64 parent = payload.value(QLatin1String("parent")).toInteger();
    if (op.kind == QLatin1String("itemCreate")) {
        store->removeItem(kind, op.target);
    } else if (op.kind == QLatin1String("itemUpdate") || op.kind == QLatin1String("itemDelete")) {
        const QJsonObject previous = payload.value(QLatin1String("previous")).toObject();
        if (!previous.isEmpty()) {
            store->upsertItem(kind, parent, mapped(kind, previous));
        }
    } else if (op.kind == QLatin1String("itemListUpdate")) {
        const QList<ParentRow> lists = store->parents(ItemKind::Task);
        for (const ParentRow &l : lists) {
            if (l.graphId == op.target) {
                ParentRow back = l;
                back.name = payload.value(QLatin1String("previousName")).toString();
                store->upsertParent(ItemKind::Task, back);
            }
        }
    }
    // A deleted list comes back with the next To Do sync
}

}
