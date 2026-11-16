// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Store: local changes, bodies and attachments, the operation queue, and calendar/contacts/To Do
// items. The message and folder basics are in store.cpp.

#include "store.h"
#include "store_p.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>

namespace GraphMail
{

namespace
{
QVariant text(const QString &value)
{
    return value.isEmpty() ? QVariant(QMetaType::fromType<QString>()) : QVariant(value);
}

struct KindTables {
    QString items;
    QString parentColumn;
    QString parents;
};

KindTables tables(ItemKind kind)
{
    switch (kind) {
    case ItemKind::Event:
        return {QStringLiteral("events"), QStringLiteral("calendar"), QStringLiteral("calendars")};
    case ItemKind::Contact:
        return {QStringLiteral("contacts"), QStringLiteral("folder"), QStringLiteral("contact_folders")};
    case ItemKind::Task:
        break;
    }
    return {QStringLiteral("tasks"), QStringLiteral("list"), QStringLiteral("todo_lists")};
}

OperationRow readOperation(const QSqlQuery &q)
{
    return {q.value(0).toLongLong(),
            q.value(1).toLongLong(),
            q.value(2).toString(),
            q.value(3).toString(),
            q.value(4).toString(),
            q.value(5).toInt(),
            q.value(6).toInt(),
            q.value(7).toLongLong(),
            q.value(8).toString()};
}
constexpr QLatin1StringView operationColumns("id, created, kind, target, payload, state, attempts, next_attempt, last_error");
}

bool Store::setFocused(qint64 message, bool focused)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET focused = ? WHERE id = ?"));
    q.addBindValue(focused ? 1 : 0);
    q.addBindValue(message);
    return q.exec() || fail(QStringLiteral("setFocused"), q.lastError().text());
}

bool Store::setFlag(qint64 message, int flag)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET flag = ? WHERE id = ?"));
    q.addBindValue(flag);
    q.addBindValue(message);
    return q.exec() || fail(QStringLiteral("setFlag"), q.lastError().text());
}

bool Store::setCategories(qint64 message, const QStringList &categories)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET categories = ? WHERE id = ?"));
    q.addBindValue(text(categories.isEmpty() ? QString()
                                             : QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(categories)).toJson(QJsonDocument::Compact))));
    q.addBindValue(message);
    return q.exec() || fail(QStringLiteral("setCategories"), q.lastError().text());
}

bool Store::setMessageFolder(qint64 message, qint64 folder)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET folder = ? WHERE id = ?"));
    q.addBindValue(folder);
    q.addBindValue(message);
    return q.exec() || fail(QStringLiteral("setMessageFolder"), q.lastError().text());
}

bool Store::setPending(qint64 message, bool pending)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET pending_op = ? WHERE id = ?"));
    q.addBindValue(pending ? QVariant(1) : QVariant(QMetaType::fromType<qlonglong>()));
    q.addBindValue(message);
    return q.exec() || fail(QStringLiteral("setPending"), q.lastError().text());
}

bool Store::setFolderWellKnown(const QString &graphId, const QString &wellKnown)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE folders SET well_known = ? WHERE graph_id = ?"));
    q.addBindValue(wellKnown);
    q.addBindValue(graphId);
    return q.exec() || fail(QStringLiteral("setFolderWellKnown"), q.lastError().text());
}

std::optional<FolderRow> Store::folderByWellKnown(const QString &wellKnown) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, graph_id FROM folders WHERE well_known = ?"));
    q.addBindValue(wellKnown);
    if (q.exec() && q.next()) {
        return folder(q.value(0).toLongLong());
    }
    return std::nullopt;
}

qint64 Store::messageCount(qint64 folder) const
{
    QSqlQuery q(m_db);
    if (folder == 0) {
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM messages"));
    } else {
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM messages WHERE folder = ?"));
        q.addBindValue(folder);
    }
    return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
}

QList<QPair<qint64, QString>> Store::messagesWithoutBody(qint64 since, int limit) const
{
    QList<QPair<qint64, QString>> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, graph_id FROM messages WHERE body_blob IS NULL AND received >= ? ORDER BY received DESC LIMIT ?"));
    q.addBindValue(since);
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            result.append({q.value(0).toLongLong(), q.value(1).toString()});
        }
    }
    return result;
}

bool Store::replaceAttachments(qint64 message, const QList<AttachmentRow> &attachments)
{
    QSqlQuery remove(m_db);
    remove.prepare(QStringLiteral("DELETE FROM attachments WHERE message = ?"));
    remove.addBindValue(message);
    if (!remove.exec()) {
        return fail(QStringLiteral("replaceAttachments"), remove.lastError().text());
    }
    for (const AttachmentRow &a : attachments) {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral("INSERT INTO attachments(message, graph_id, name, content_type, size, is_inline, content_id, blob) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
        q.addBindValue(message);
        q.addBindValue(a.graphId);
        q.addBindValue(text(a.name));
        q.addBindValue(text(a.contentType));
        q.addBindValue(a.size);
        q.addBindValue(a.isInline ? 1 : 0);
        q.addBindValue(text(a.contentId));
        q.addBindValue(text(a.blob));
        if (!q.exec()) {
            return fail(QStringLiteral("replaceAttachments"), q.lastError().text());
        }
    }
    return true;
}

QList<AttachmentRow> Store::attachments(qint64 message) const
{
    QList<AttachmentRow> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, graph_id, name, content_type, size, is_inline, content_id, blob FROM attachments WHERE message = ? ORDER BY id"));
    q.addBindValue(message);
    if (q.exec()) {
        while (q.next()) {
            result.append({q.value(0).toLongLong(),
                           q.value(1).toString(),
                           q.value(2).toString(),
                           q.value(3).toString(),
                           q.value(4).toLongLong(),
                           q.value(5).toBool(),
                           q.value(6).toString(),
                           q.value(7).toString()});
        }
    }
    return result;
}

QString Store::blobOfContentId(const QString &contentId) const
{
    QString bare = contentId;
    if (bare.startsWith(QLatin1Char('<')) && bare.endsWith(QLatin1Char('>'))) {
        bare = bare.mid(1, bare.size() - 2);
    }
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT blob FROM attachments WHERE (content_id = ? OR content_id = ?) AND coalesce(blob, '') <> '' ORDER BY id DESC LIMIT 1"));
    q.addBindValue(bare);
    q.addBindValue(QString(QLatin1Char('<') + bare + QLatin1Char('>')));
    return q.exec() && q.next() ? q.value(0).toString() : QString();
}

bool Store::setAttachmentBlob(qint64 attachment, const QString &blob, const QString &contentId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE attachments SET blob = ?, content_id = COALESCE(?, content_id) WHERE id = ?"));
    q.addBindValue(text(blob));
    q.addBindValue(text(contentId));
    q.addBindValue(attachment);
    return q.exec() || fail(QStringLiteral("setAttachmentBlob"), q.lastError().text());
}

QList<ConversationRow> Store::conversationPage(qint64 folder, const std::optional<ConversationCursor> &after, int limit, int focus) const
{
    // Messages without a conversation id stand alone
    const QString key = QStringLiteral("COALESCE(conversation, 'msg:' || id)");
    const QString focusFilter = focus >= 0 ? QStringLiteral(" AND focused = %1").arg(focus > 0 ? 1 : 0) : QString();
    QString sql = QStringLiteral("SELECT %1 AS k, MAX(received) AS latest, COUNT(*), SUM(is_read = 0), SUM(flag = 1), SUM(has_attachments) "
                                 "FROM messages WHERE folder = ?%2 GROUP BY k")
                      .arg(key, focusFilter);
    if (after) {
        sql += QStringLiteral(" HAVING latest < ? OR (latest = ? AND k < ?)");
    }
    sql += QStringLiteral(" ORDER BY latest DESC, k DESC LIMIT ?");
    QSqlQuery q(m_db);
    q.prepare(sql);
    q.addBindValue(folder);
    if (after) {
        q.addBindValue(after->latest);
        q.addBindValue(after->latest);
        q.addBindValue(after->key);
    }
    q.addBindValue(limit);
    QList<ConversationRow> rows;
    if (!q.exec()) {
        fail(QStringLiteral("conversationPage"), q.lastError().text());
        return rows;
    }
    while (q.next()) {
        ConversationRow row;
        row.key = q.value(0).toString();
        row.count = q.value(2).toInt();
        row.anyUnread = q.value(3).toInt() > 0;
        row.anyFlagged = q.value(4).toInt() > 0;
        row.anyAttachments = q.value(5).toInt() > 0;
        rows.append(row);
    }
    // The newest message of each (a second query per screen of 200 is cheap; joining is not)
    for (ConversationRow &row : rows) {
        const QList<ListRow> messages = conversationMessages(folder, row.key, focus);
        if (!messages.isEmpty()) {
            row.latest = messages.constFirst();
        }
    }
    return rows;
}

QList<ListRow> Store::conversationMessages(qint64 folder, const QString &key, int focus) const
{
    QList<ListRow> rows;
    QSqlQuery q(m_db);
    const QString focusFilter = focus >= 0 ? QStringLiteral(" AND focused = %1").arg(focus > 0 ? 1 : 0) : QString();
    if (key.startsWith(QLatin1String("msg:"))) {
        q.prepare(QStringLiteral("SELECT %1 FROM messages WHERE id = ? AND folder = ?").arg(StoreRows::listColumns) + focusFilter);
        q.addBindValue(key.mid(4).toLongLong());
    } else {
        // SQLite prefers the folder index here and then scans the whole folder (2.3 s for 200
        // conversations in a 56k folder); a conversation has few messages
        q.prepare(QStringLiteral("SELECT %1 FROM messages INDEXED BY messages_by_conversation WHERE conversation = ? AND folder = ?").arg(StoreRows::listColumns) + focusFilter
                  + QStringLiteral(" ORDER BY received DESC, id DESC"));
        q.addBindValue(key);
    }
    q.addBindValue(folder);
    if (q.exec()) {
        while (q.next()) {
            rows.append(StoreRows::readListRow(q));
        }
    }
    return rows;
}

std::optional<qint64> Store::messageOfAttachment(qint64 attachment) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT message FROM attachments WHERE id = ?"));
    q.addBindValue(attachment);
    if (q.exec() && q.next()) {
        return q.value(0).toLongLong();
    }
    return std::nullopt;
}

QStringList Store::sweepBodies(qint64 before)
{
    QStringList blobs;
    QSqlQuery select(m_db);
    select.prepare(QStringLiteral("SELECT m.body_blob FROM messages m WHERE m.body_blob IS NOT NULL AND m.received < ? AND m.flag != 1 "
                                  "UNION ALL SELECT a.blob FROM attachments a JOIN messages m ON m.id = a.message "
                                  "WHERE a.blob IS NOT NULL AND m.received < ? AND m.flag != 1"));
    select.addBindValue(before);
    select.addBindValue(before);
    if (!select.exec()) {
        fail(QStringLiteral("sweepBodies"), select.lastError().text());
        return {};
    }
    while (select.next()) {
        blobs.append(select.value(0).toString());
    }
    QSqlQuery attachments(m_db);
    attachments.prepare(QStringLiteral("UPDATE attachments SET blob = NULL WHERE blob IS NOT NULL AND message IN "
                                       "(SELECT id FROM messages WHERE received < ? AND flag != 1)"));
    attachments.addBindValue(before);
    QSqlQuery bodies(m_db);
    bodies.prepare(
        QStringLiteral("UPDATE messages SET body_blob = NULL, body_type = 0, body_size = 0, body_text = NULL WHERE body_blob IS NOT NULL AND received < ? AND flag != 1"));
    bodies.addBindValue(before);
    if (!attachments.exec() || !bodies.exec()) {
        fail(QStringLiteral("sweepBodies"), attachments.lastError().text() + bodies.lastError().text());
        return {};
    }
    return blobs;
}

qint64 Store::addOperation(const OperationRow &op)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO ops(created, kind, target, payload, state, attempts, next_attempt) VALUES (?, ?, ?, ?, 0, 0, ?) RETURNING id"));
    q.addBindValue(op.created);
    q.addBindValue(op.kind);
    q.addBindValue(op.target);
    q.addBindValue(op.payload);
    q.addBindValue(op.nextAttempt);
    if (!q.exec() || !q.next()) {
        fail(QStringLiteral("addOperation"), q.lastError().text());
        return 0;
    }
    return q.value(0).toLongLong();
}

bool Store::updateOperationPayload(qint64 id, const QString &payload)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE ops SET payload = ?, attempts = 0, next_attempt = 0, last_error = NULL WHERE id = ?"));
    q.addBindValue(payload);
    q.addBindValue(id);
    return q.exec() || fail(QStringLiteral("updateOperationPayload"), q.lastError().text());
}

std::optional<OperationRow> Store::pendingOperation(const QString &target, const QString &kind) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM ops WHERE target = ? AND kind = ? AND state = 0 ORDER BY id LIMIT 1").arg(operationColumns));
    q.addBindValue(target);
    q.addBindValue(kind);
    if (q.exec() && q.next()) {
        return readOperation(q);
    }
    return std::nullopt;
}

QList<OperationRow> Store::operationsFor(const QString &target) const
{
    QList<OperationRow> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM ops WHERE target = ? AND state = 0 ORDER BY id").arg(operationColumns));
    q.addBindValue(target);
    if (q.exec()) {
        while (q.next()) {
            result.append(readOperation(q));
        }
    }
    return result;
}

QList<OperationRow> Store::dueOperations(qint64 now, int limit, const QString &family) const
{
    QList<OperationRow> result;
    QSqlQuery q(m_db);
    const QString which = family == QLatin1String("event") ? QStringLiteral("kind LIKE 'event%'")
        : family == QLatin1String("item")                  ? QStringLiteral("kind LIKE 'item%'")
                                                           : QStringLiteral("kind NOT LIKE 'event%' AND kind NOT LIKE 'item%'");
    q.prepare(QStringLiteral("SELECT %1 FROM ops WHERE state = 0 AND next_attempt <= ? AND %2 ORDER BY id LIMIT ?").arg(operationColumns, which));
    q.addBindValue(now);
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            result.append(readOperation(q));
        }
    }
    return result;
}

bool Store::removeOperation(qint64 id)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM ops WHERE id = ?"));
    q.addBindValue(id);
    return q.exec() || fail(QStringLiteral("removeOperation"), q.lastError().text());
}

bool Store::retryOperationLater(qint64 id, int attempts, qint64 nextAttempt, const QString &error)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE ops SET attempts = ?, next_attempt = ?, last_error = ? WHERE id = ?"));
    q.addBindValue(attempts);
    q.addBindValue(nextAttempt);
    q.addBindValue(text(error));
    q.addBindValue(id);
    return q.exec() || fail(QStringLiteral("retryOperationLater"), q.lastError().text());
}

QList<OperationRow> Store::allOperations() const
{
    QList<OperationRow> result;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT %1 FROM ops ORDER BY id").arg(operationColumns))) {
        while (q.next()) {
            result.append(readOperation(q));
        }
    }
    return result;
}

std::optional<OperationRow> Store::operation(qint64 id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM ops WHERE id = ?").arg(operationColumns));
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        return readOperation(q);
    }
    return std::nullopt;
}

bool Store::retryAllNow()
{
    QSqlQuery q(m_db);
    return q.exec(QStringLiteral("UPDATE ops SET next_attempt = 0 WHERE state = 0")) || fail(QStringLiteral("retryAllNow"), q.lastError().text());
}

int Store::pendingOperationCount() const
{
    QSqlQuery q(m_db);
    return q.exec(QStringLiteral("SELECT COUNT(*) FROM ops WHERE state = 0")) && q.next() ? q.value(0).toInt() : 0;
}

QStringList Store::itemColumns(ItemKind kind)
{
    switch (kind) {
    case ItemKind::Event:
        return {QStringLiteral("series_master"), QStringLiteral("start_time"), QStringLiteral("end_time"), QStringLiteral("all_day"),
                QStringLiteral("subject"),       QStringLiteral("location"),   QStringLiteral("organizer"), QStringLiteral("show_as"),
                QStringLiteral("response"),      QStringLiteral("is_online_meeting"), QStringLiteral("type")};
    case ItemKind::Contact:
        return {QStringLiteral("display_name"), QStringLiteral("emails")};
    case ItemKind::Task:
        break;
    }
    return {QStringLiteral("title"), QStringLiteral("status"), QStringLiteral("due"), QStringLiteral("reminder")};
}

qint64 Store::upsertParent(ItemKind kind, const ParentRow &parent)
{
    const KindTables t = tables(kind);
    QSqlQuery q(m_db);
    if (kind == ItemKind::Task) {
        q.prepare(QStringLiteral("INSERT INTO todo_lists(graph_id, name, well_known) VALUES (?, ?, ?) ON CONFLICT(graph_id) DO UPDATE SET "
                                 "name = excluded.name, well_known = excluded.well_known RETURNING id"));
        q.addBindValue(parent.graphId);
        q.addBindValue(parent.name);
        q.addBindValue(text(parent.wellKnown));
    } else {
        q.prepare(QStringLiteral("INSERT INTO %1(graph_id, name) VALUES (?, ?) ON CONFLICT(graph_id) DO UPDATE SET name = excluded.name RETURNING id")
                      .arg(t.parents));
        q.addBindValue(parent.graphId);
        q.addBindValue(parent.name);
    }
    if (!q.exec() || !q.next()) {
        fail(QStringLiteral("upsertParent"), q.lastError().text());
        return 0;
    }
    return q.value(0).toLongLong();
}

QList<ParentRow> Store::parents(ItemKind kind) const
{
    const KindTables t = tables(kind);
    QList<ParentRow> result;
    QSqlQuery q(m_db);
    const QString extra = kind == ItemKind::Event ? QStringLiteral(", window_start, window_end")
                                                  : (kind == ItemKind::Task ? QStringLiteral(", well_known") : QString());
    if (q.exec(QStringLiteral("SELECT id, graph_id, name, delta_link, generation%1 FROM %2 ORDER BY id").arg(extra, t.parents))) {
        while (q.next()) {
            ParentRow p;
            p.id = q.value(0).toLongLong();
            p.graphId = q.value(1).toString();
            p.name = q.value(2).toString();
            p.deltaLink = q.value(3).toString();
            p.generation = q.value(4).toInt();
            if (kind == ItemKind::Event) {
                p.windowStart = q.value(5).toLongLong();
                p.windowEnd = q.value(6).toLongLong();
            } else if (kind == ItemKind::Task) {
                p.wellKnown = q.value(5).toString();
            }
            result.append(p);
        }
    }
    return result;
}

bool Store::setParentDeltaLink(ItemKind kind, qint64 parent, const QString &deltaLink, qint64 lastSynced, const QString &lastError)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE %1 SET delta_link = ?, last_synced = ?, last_error = ? WHERE id = ?").arg(tables(kind).parents));
    q.addBindValue(text(deltaLink));
    q.addBindValue(lastSynced);
    q.addBindValue(text(lastError));
    q.addBindValue(parent);
    return q.exec() || fail(QStringLiteral("setParentDeltaLink"), q.lastError().text());
}

bool Store::setCalendarWindow(qint64 calendar, qint64 start, qint64 end)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE calendars SET window_start = ?, window_end = ? WHERE id = ?"));
    q.addBindValue(start);
    q.addBindValue(end);
    q.addBindValue(calendar);
    return q.exec() || fail(QStringLiteral("setCalendarWindow"), q.lastError().text());
}

bool Store::removeParent(ItemKind kind, qint64 parent)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM %1 WHERE id = ?").arg(tables(kind).parents));
    q.addBindValue(parent);
    return q.exec() || fail(QStringLiteral("removeParent"), q.lastError().text());
}

Store::Upsert Store::upsertItem(ItemKind kind, qint64 parent, const ItemRow &item, int generation)
{
    const KindTables t = tables(kind);
    const QStringList allowed = itemColumns(kind);
    for (auto it = item.columns.cbegin(); it != item.columns.cend(); ++it) {
        if (!allowed.contains(it.key())) {
            fail(QStringLiteral("upsertItem"), QStringLiteral("unknown column ") + it.key());
            return Upsert::Failed;
        }
    }

    QSqlQuery find(m_db);
    find.prepare(QStringLiteral("SELECT id, change_key, %1 FROM %2 WHERE graph_id = ?").arg(t.parentColumn, t.items));
    find.addBindValue(item.graphId);
    if (!find.exec()) {
        fail(QStringLiteral("upsertItem"), find.lastError().text());
        return Upsert::Failed;
    }
    const bool exists = find.next();
    if (exists && !item.changeKey.isEmpty() && find.value(1).toString() == item.changeKey && find.value(2).toLongLong() == parent) {
        QSqlQuery seen(m_db);
        seen.prepare(QStringLiteral("UPDATE %1 SET seen_generation = ? WHERE id = ?").arg(t.items));
        seen.addBindValue(generation);
        seen.addBindValue(find.value(0));
        return seen.exec() ? Upsert::Unchanged : Upsert::Failed;
    }

    QStringList names = item.columns.keys();
    names << t.parentColumn << QStringLiteral("change_key") << QStringLiteral("seen_generation") << QStringLiteral("json");
    QVariantList values;
    for (auto it = item.columns.cbegin(); it != item.columns.cend(); ++it) {
        values.append(it.value());
    }
    values << parent << text(item.changeKey) << generation << text(item.json);

    QSqlQuery q(m_db);
    if (exists) {
        QStringList assignments;
        for (const QString &name : std::as_const(names)) {
            assignments.append(name + QStringLiteral(" = ?"));
        }
        q.prepare(QStringLiteral("UPDATE %1 SET %2 WHERE id = ?").arg(t.items, assignments.join(QStringLiteral(", "))));
        values.append(find.value(0));
    } else {
        names.append(QStringLiteral("graph_id"));
        values.append(item.graphId);
        q.prepare(QStringLiteral("INSERT INTO %1(%2) VALUES (%3)")
                      .arg(t.items, names.join(QStringLiteral(", ")), QStringList(names.size(), QStringLiteral("?")).join(QStringLiteral(", "))));
    }
    for (const QVariant &value : std::as_const(values)) {
        q.addBindValue(value);
    }
    if (!q.exec()) {
        fail(QStringLiteral("upsertItem"), q.lastError().text());
        return Upsert::Failed;
    }
    return exists ? Upsert::Updated : Upsert::Inserted;
}

bool Store::removeItem(ItemKind kind, const QString &graphId)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM %1 WHERE graph_id = ?").arg(tables(kind).items));
    q.addBindValue(graphId);
    if (!q.exec()) {
        return fail(QStringLiteral("removeItem"), q.lastError().text());
    }
    // A series goes with its occurrences and exceptions: the delta names only the series (they
    // stayed as "(no subject)" items that Exchange no longer knows)
    if (kind == ItemKind::Event) {
        QSqlQuery occurrences(m_db);
        occurrences.prepare(QStringLiteral("DELETE FROM events WHERE series_master = ?"));
        occurrences.addBindValue(graphId);
        if (!occurrences.exec()) {
            return fail(QStringLiteral("removeItem"), occurrences.lastError().text());
        }
    }
    return true;
}

int Store::sweepOrphanOccurrences(qint64 calendar)
{
    // Occurrences and exceptions whose series is gone (a series removed before the fix above)
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM events WHERE calendar = ? AND coalesce(series_master, '') <> '' "
                             "AND NOT EXISTS (SELECT 1 FROM events m WHERE m.graph_id = events.series_master)"));
    q.addBindValue(calendar);
    if (!q.exec()) {
        fail(QStringLiteral("sweepOrphanOccurrences"), q.lastError().text());
        return 0;
    }
    return q.numRowsAffected();
}

int Store::beginParentResync(ItemKind kind, qint64 parent)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE %1 SET generation = generation + 1 WHERE id = ? RETURNING generation").arg(tables(kind).parents));
    q.addBindValue(parent);
    if (!q.exec() || !q.next()) {
        fail(QStringLiteral("beginParentResync"), q.lastError().text());
        return -1;
    }
    return q.value(0).toInt();
}

bool Store::sweepItems(ItemKind kind, qint64 parent, int generation)
{
    const KindTables t = tables(kind);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM %1 WHERE %2 = ? AND seen_generation < ?").arg(t.items, t.parentColumn));
    q.addBindValue(parent);
    q.addBindValue(generation);
    return q.exec() || fail(QStringLiteral("sweepItems"), q.lastError().text());
}

qint64 Store::itemCount(ItemKind kind, qint64 parent) const
{
    const KindTables t = tables(kind);
    QSqlQuery q(m_db);
    if (parent == 0) {
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM %1").arg(t.items));
    } else {
        q.prepare(QStringLiteral("SELECT COUNT(*) FROM %1 WHERE %2 = ?").arg(t.items, t.parentColumn));
        q.addBindValue(parent);
    }
    return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
}

}

namespace GraphMail
{

namespace
{
std::optional<Outgoing> outgoingFrom(QSqlQuery &q)
{
    if (!q.next()) {
        return std::nullopt;
    }
    return Outgoing::fromJson(QJsonDocument::fromJson(q.value(0).toString().toUtf8()).object());
}
}

bool Store::saveOutgoing(const Outgoing &outgoing)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO outgoing(uuid, state, draft_id, next_attempt, updated, json) VALUES (?, ?, ?, ?, ?, ?)"));
    q.addBindValue(outgoing.uuid);
    q.addBindValue(int(outgoing.state));
    q.addBindValue(outgoing.draftId.isEmpty() ? QVariant() : QVariant(outgoing.draftId));
    q.addBindValue(outgoing.nextAttempt);
    q.addBindValue(outgoing.updated);
    q.addBindValue(QString::fromUtf8(QJsonDocument(outgoing.toJson()).toJson(QJsonDocument::Compact)));
    return q.exec() || fail(QStringLiteral("saveOutgoing"), q.lastError().text());
}

std::optional<Outgoing> Store::outgoing(const QString &uuid) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT json FROM outgoing WHERE uuid = ?"));
    q.addBindValue(uuid);
    if (!q.exec()) {
        fail(QStringLiteral("outgoing"), q.lastError().text());
        return std::nullopt;
    }
    return outgoingFrom(q);
}

std::optional<Outgoing> Store::outgoingForDraft(const QString &draftId) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT json FROM outgoing WHERE draft_id = ? ORDER BY updated DESC LIMIT 1"));
    q.addBindValue(draftId);
    if (!q.exec()) {
        fail(QStringLiteral("outgoingForDraft"), q.lastError().text());
        return std::nullopt;
    }
    return outgoingFrom(q);
}

QList<Outgoing> Store::dueOutgoing(qint64 now) const
{
    QList<Outgoing> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT json FROM outgoing WHERE state IN (?, ?) AND next_attempt <= ? ORDER BY updated"));
    q.addBindValue(int(Outgoing::State::Pending));
    q.addBindValue(int(Outgoing::State::Discard));
    q.addBindValue(now);
    if (!q.exec()) {
        fail(QStringLiteral("dueOutgoing"), q.lastError().text());
        return result;
    }
    while (std::optional<Outgoing> o = outgoingFrom(q)) {
        result.append(*o);
    }
    return result;
}

QList<Outgoing> Store::allOutgoing() const
{
    QList<Outgoing> result;
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT json FROM outgoing ORDER BY updated"))) {
        fail(QStringLiteral("allOutgoing"), q.lastError().text());
        return result;
    }
    while (std::optional<Outgoing> o = outgoingFrom(q)) {
        result.append(*o);
    }
    return result;
}

bool Store::removeOutgoing(const QString &uuid)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM outgoing WHERE uuid = ?"));
    q.addBindValue(uuid);
    return q.exec() || fail(QStringLiteral("removeOutgoing"), q.lastError().text());
}


QList<OutgoingRecipient> Store::knownAddresses(int recent) const
{
    QList<OutgoingRecipient> result;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT display_name, emails FROM contacts"))) {
        while (q.next()) {
            const QStringList emails = q.value(1).toString().split(QStringLiteral("; "), Qt::SkipEmptyParts);
            for (const QString &email : emails) {
                result.append({q.value(0).toString(), email});
            }
        }
    }
    q.prepare(QStringLiteral("SELECT sender_name, sender_address FROM messages WHERE sender_address <> '' "
                             "GROUP BY lower(sender_address) ORDER BY MAX(received) DESC LIMIT ?"));
    q.addBindValue(recent);
    if (q.exec()) {
        while (q.next()) {
            result.append({q.value(0).toString(), q.value(1).toString()});
        }
    }
    q.prepare(QStringLiteral("SELECT to_list, cc_list FROM messages WHERE folder IN (SELECT id FROM folders WHERE well_known = 'sentitems') "
                             "ORDER BY received DESC LIMIT ?"));
    q.addBindValue(recent);
    if (q.exec()) {
        while (q.next()) {
            result += parseRecipients(q.value(0).toString() + QLatin1Char(';') + q.value(1).toString());
        }
    }
    return result;
}


namespace
{
constexpr QLatin1StringView eventColumns(
    "id, graph_id, calendar, series_master, start_time, end_time, all_day, subject, location, organizer, show_as, response, is_online_meeting, type, json");

EventRow readEvent(const QSqlQuery &q)
{
    EventRow e;
    e.id = q.value(0).toLongLong();
    e.graphId = q.value(1).toString();
    e.calendar = q.value(2).toLongLong();
    e.seriesMaster = q.value(3).toString();
    e.start = q.value(4).toLongLong();
    e.end = q.value(5).toLongLong();
    e.allDay = q.value(6).toBool();
    e.subject = q.value(7).toString();
    e.location = q.value(8).toString();
    e.organizer = q.value(9).toString();
    e.showAs = q.value(10).isNull() ? 2 : q.value(10).toInt();
    e.response = q.value(11).toInt();
    e.isOnlineMeeting = q.value(12).toBool();
    e.type = q.value(13).toInt();
    e.json = q.value(14).toString();
    return e;
}

CalendarRow readCalendar(const QSqlQuery &q)
{
    return {q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toString(), q.value(3).toString(), q.value(4).toString(), q.value(5).toBool(), q.value(6).toBool()};
}

// An occurrence knows only its times: the rest comes from its series master
void fillFromMaster(EventRow &occurrence, const EventRow &master)
{
    occurrence.subject = master.subject;
    occurrence.location = master.location;
    occurrence.organizer = master.organizer;
    occurrence.showAs = master.showAs;
    occurrence.response = master.response;
    occurrence.isOnlineMeeting = master.isOnlineMeeting;
    occurrence.allDay = master.allDay;
    occurrence.json = master.json;
}
}

qint64 Store::upsertCalendar(const CalendarRow &calendar)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO calendars(graph_id, name, owner_address, color, can_edit, is_default) VALUES (?, ?, ?, ?, ?, ?) "
                             "ON CONFLICT(graph_id) DO UPDATE SET name = excluded.name, owner_address = excluded.owner_address, color = excluded.color, "
                             "can_edit = excluded.can_edit, is_default = excluded.is_default RETURNING id"));
    q.addBindValue(calendar.graphId);
    q.addBindValue(calendar.name);
    q.addBindValue(text(calendar.owner));
    q.addBindValue(text(calendar.color));
    q.addBindValue(calendar.canEdit ? 1 : 0);
    q.addBindValue(calendar.isDefault ? 1 : 0);
    if (!q.exec() || !q.next()) {
        fail(QStringLiteral("upsertCalendar"), q.lastError().text());
        return 0;
    }
    return q.value(0).toLongLong();
}

QList<CalendarRow> Store::calendars() const
{
    QList<CalendarRow> result;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT id, graph_id, name, owner_address, color, can_edit, is_default FROM calendars ORDER BY is_default DESC, id"))) {
        while (q.next()) {
            result.append(readCalendar(q));
        }
    }
    return result;
}

std::optional<CalendarRow> Store::calendar(qint64 id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, graph_id, name, owner_address, color, can_edit, is_default FROM calendars WHERE id = ?"));
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        return readCalendar(q);
    }
    return std::nullopt;
}

QList<EventRow> Store::eventsBetween(const QList<qint64> &calendars, qint64 start, qint64 end) const
{
    QList<EventRow> result;
    QSqlQuery q(m_db);
    QString filter;
    if (!calendars.isEmpty()) {
        QStringList ids;
        for (qint64 c : calendars) {
            ids.append(QString::number(c));
        }
        filter = QStringLiteral(" AND calendar IN (%1)").arg(ids.join(QLatin1Char(',')));
    }
    // An all-day event or one that ends exactly at `start` must not show on the day before
    q.prepare(QStringLiteral("SELECT %1 FROM events WHERE type <> 3 AND start_time < ? AND end_time > ?%2 ORDER BY start_time").arg(eventColumns, filter));
    q.addBindValue(end);
    q.addBindValue(start);
    if (!q.exec()) {
        fail(QStringLiteral("eventsBetween"), q.lastError().text());
        return result;
    }
    QHash<QString, std::optional<EventRow>> masters;
    while (q.next()) {
        EventRow e = readEvent(q);
        if (e.type == 1 && !e.seriesMaster.isEmpty()) {
            auto it = masters.find(e.seriesMaster);
            if (it == masters.end()) {
                it = masters.insert(e.seriesMaster, eventByGraphId(e.seriesMaster));
            }
            if (it.value()) {
                fillFromMaster(e, *it.value());
            }
        }
        result.append(e);
    }
    return result;
}

std::optional<EventRow> Store::event(qint64 id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM events WHERE id = ?").arg(eventColumns));
    q.addBindValue(id);
    if (!q.exec() || !q.next()) {
        return std::nullopt;
    }
    EventRow e = readEvent(q);
    if (e.type == 1 && !e.seriesMaster.isEmpty()) {
        if (const std::optional<EventRow> master = eventByGraphId(e.seriesMaster)) {
            fillFromMaster(e, *master);
        }
    }
    return e;
}

std::optional<EventRow> Store::eventByGraphId(const QString &graphId) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM events WHERE graph_id = ?").arg(eventColumns));
    q.addBindValue(graphId);
    if (q.exec() && q.next()) {
        return readEvent(q);
    }
    return std::nullopt;
}

int Store::reminderState(const QString &key, qint64 *snoozeUntil) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT state, snooze_until FROM reminders WHERE key = ?"));
    q.addBindValue(key);
    if (q.exec() && q.next()) {
        if (snoozeUntil) {
            *snoozeUntil = q.value(1).toLongLong();
        }
        return q.value(0).toInt();
    }
    return 0;
}

bool Store::setReminderState(const QString &key, qint64 due, int state, qint64 snoozeUntil)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO reminders(key, due, state, snooze_until) VALUES (?, ?, ?, ?)"));
    q.addBindValue(key);
    q.addBindValue(due);
    q.addBindValue(state);
    q.addBindValue(snoozeUntil);
    return q.exec() || fail(QStringLiteral("setReminderState"), q.lastError().text());
}

bool Store::sweepReminders(qint64 before)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM reminders WHERE due < ?"));
    q.addBindValue(before);
    return q.exec() || fail(QStringLiteral("sweepReminders"), q.lastError().text());
}


int Store::exceptionCount(const QString &seriesMaster) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM events WHERE series_master = ? AND type = 2"));
    q.addBindValue(seriesMaster);
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}


namespace
{
StoredItem readStored(const QSqlQuery &q)
{
    return {q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toLongLong(), q.value(3).toString()};
}
}

QList<StoredItem> Store::items(ItemKind kind, qint64 parent) const
{
    const KindTables t = tables(kind);
    QList<StoredItem> result;
    QSqlQuery q(m_db);
    const QString order = kind == ItemKind::Contact ? QStringLiteral("display_name COLLATE NOCASE") : QStringLiteral("id");
    q.prepare(QStringLiteral("SELECT id, graph_id, %1, json FROM %2 %3 ORDER BY %4")
                  .arg(t.parentColumn, t.items, parent != 0 ? QStringLiteral("WHERE %1 = ?").arg(t.parentColumn) : QString(), order));
    if (parent != 0) {
        q.addBindValue(parent);
    }
    if (q.exec()) {
        while (q.next()) {
            result.append(readStored(q));
        }
    }
    return result;
}

std::optional<StoredItem> Store::item(ItemKind kind, qint64 id) const
{
    const KindTables t = tables(kind);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, graph_id, %1, json FROM %2 WHERE id = ?").arg(t.parentColumn, t.items));
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        return readStored(q);
    }
    return std::nullopt;
}

std::optional<StoredItem> Store::itemByGraphId(ItemKind kind, const QString &graphId) const
{
    const KindTables t = tables(kind);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, graph_id, %1, json FROM %2 WHERE graph_id = ?").arg(t.parentColumn, t.items));
    q.addBindValue(graphId);
    if (q.exec() && q.next()) {
        return readStored(q);
    }
    return std::nullopt;
}

QList<StoredItem> Store::dueTaskReminders(qint64 now) const
{
    QList<StoredItem> result;
    QSqlQuery q(m_db);
    // status 2 is completed
    q.prepare(QStringLiteral("SELECT id, graph_id, list, json FROM tasks WHERE reminder > 0 AND reminder <= ? AND status <> 2"));
    q.addBindValue(now);
    if (q.exec()) {
        while (q.next()) {
            result.append(readStored(q));
        }
    }
    return result;
}

bool Store::vacuum()
{
    // VACUUM writes the whole database through the write-ahead log: without the checkpoint the
    // -wal file holds a second copy and the space grows instead (phase 8 live test)
    QSqlQuery q(m_db);
    return (q.exec(QStringLiteral("VACUUM")) && q.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"))) || fail(QStringLiteral("vacuum"), q.lastError().text());
}

QStringList categoriesFromJson(const QString &json)
{
    QStringList names;
    if (json.isEmpty()) {
        return names;
    }
    const QJsonArray array = QJsonDocument::fromJson(json.toUtf8()).array();
    for (const QJsonValue &v : array) {
        names.append(v.toString());
    }
    return names;
}

}
