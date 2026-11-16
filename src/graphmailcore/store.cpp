// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "store.h"
#include "store_p.h"
#include "graphmail_debug.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>

namespace GraphMail
{

namespace
{
// The columns of MessageRow in the order readMessage() expects them
constexpr QLatin1StringView messageColumns(
    "id, graph_id, folder, conversation, received, sent, sender_name, sender_address, subject, preview, to_list, cc_list, "
    "is_read, flag, importance, has_attachments, categories, internet_message_id, change_key, body_blob, body_type, body_size, pending_op, item_type, focused");

QString encodeCategories(const QStringList &categories)
{
    return categories.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(categories)).toJson(QJsonDocument::Compact));
}

QStringList decodeCategories(const QString &json)
{
    QStringList categories;
    const QJsonArray array = QJsonDocument::fromJson(json.toUtf8()).array();
    for (const QJsonValue &value : array) {
        categories.append(value.toString());
    }
    return categories;
}

// NULL instead of an empty string, so optional text columns stay NULL in the database
QVariant text(const QString &value)
{
    return value.isEmpty() ? QVariant(QMetaType::fromType<QString>()) : QVariant(value);
}

MessageRow readMessage(const QSqlQuery &q)
{
    MessageRow m;
    m.id = q.value(0).toLongLong();
    m.graphId = q.value(1).toString();
    m.folder = q.value(2).toLongLong();
    m.conversation = q.value(3).toString();
    m.received = q.value(4).toLongLong();
    m.sent = q.value(5).toLongLong();
    m.senderName = q.value(6).toString();
    m.senderAddress = q.value(7).toString();
    m.subject = q.value(8).toString();
    m.preview = q.value(9).toString();
    m.toList = q.value(10).toString();
    m.ccList = q.value(11).toString();
    m.isRead = q.value(12).toBool();
    m.flag = q.value(13).toInt();
    m.importance = q.value(14).toInt();
    m.hasAttachments = q.value(15).toBool();
    m.categories = decodeCategories(q.value(16).toString());
    m.internetMessageId = q.value(17).toString();
    m.changeKey = q.value(18).toString();
    m.bodyBlob = q.value(19).toString();
    m.bodyType = q.value(20).toInt();
    m.bodySize = q.value(21).toLongLong();
    m.pendingOp = q.value(22).toLongLong();
    m.itemType = q.value(23).toInt();
    m.focused = q.value(24).toBool();
    return m;
}

FolderRow readFolder(const QSqlQuery &q)
{
    return {q.value(0).toLongLong(),
            q.value(1).toString(),
            q.value(2).toString(),
            q.value(3).toString(),
            q.value(4).toString(),
            q.value(5).toInt(),
            q.value(6).toInt()};
}
constexpr QLatin1StringView folderColumns("id, graph_id, parent_graph_id, name, well_known, total, unread");

// Schema version 1 (docs/PHASE2.md section 3). Calendar, contacts, To Do and the operation queue
// get their tables now, so steps 2.6 and 2.7 need no migration.
QStringList schemaV1()
{
    return {
        QStringLiteral("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT) WITHOUT ROWID"),
        QStringLiteral("CREATE TABLE folders("
                       "id INTEGER PRIMARY KEY, graph_id TEXT NOT NULL UNIQUE, parent_graph_id TEXT, name TEXT, well_known TEXT, "
                       "total INTEGER NOT NULL DEFAULT 0, unread INTEGER NOT NULL DEFAULT 0, "
                       "delta_link TEXT, recent_delta_link TEXT, initial_state INTEGER NOT NULL DEFAULT 0, "
                       "poll_class INTEGER NOT NULL DEFAULT 2, generation INTEGER NOT NULL DEFAULT 0, last_synced INTEGER, last_error TEXT)"),
        QStringLiteral("CREATE TABLE messages("
                       "id INTEGER PRIMARY KEY, graph_id TEXT NOT NULL UNIQUE, "
                       "folder INTEGER NOT NULL REFERENCES folders(id) ON DELETE CASCADE, conversation TEXT, "
                       "received INTEGER NOT NULL DEFAULT 0, sent INTEGER NOT NULL DEFAULT 0, sender_name TEXT, sender_address TEXT, "
                       "subject TEXT, preview TEXT, to_list TEXT, cc_list TEXT, is_read INTEGER NOT NULL DEFAULT 0, "
                       "flag INTEGER NOT NULL DEFAULT 0, importance INTEGER NOT NULL DEFAULT 1, has_attachments INTEGER NOT NULL DEFAULT 0, "
                       "categories TEXT, internet_message_id TEXT, change_key TEXT, "
                       "body_blob TEXT, body_type INTEGER NOT NULL DEFAULT 0, body_size INTEGER NOT NULL DEFAULT 0, "
                       "pending_op INTEGER, seen_generation INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE INDEX messages_by_folder ON messages(folder, received DESC, id DESC)"),
        QStringLiteral("CREATE INDEX messages_by_conversation ON messages(conversation)"),
        QStringLiteral("CREATE INDEX messages_unread ON messages(folder) WHERE is_read = 0"),
        QStringLiteral("CREATE INDEX messages_with_body ON messages(received) WHERE body_blob IS NOT NULL"),
        // The body prefetcher: newest messages still without a body
        QStringLiteral("CREATE INDEX messages_without_body ON messages(received DESC) WHERE body_blob IS NULL"),
        QStringLiteral("CREATE VIRTUAL TABLE messages_fts USING fts5(subject, sender_name, sender_address, preview, "
                       "content='messages', content_rowid='id', tokenize='unicode61 remove_diacritics 2')"),
        QStringLiteral("CREATE TRIGGER messages_fts_insert AFTER INSERT ON messages BEGIN "
                       "INSERT INTO messages_fts(rowid, subject, sender_name, sender_address, preview) "
                       "VALUES (new.id, new.subject, new.sender_name, new.sender_address, new.preview); END"),
        QStringLiteral("CREATE TRIGGER messages_fts_delete AFTER DELETE ON messages BEGIN "
                       "INSERT INTO messages_fts(messages_fts, rowid, subject, sender_name, sender_address, preview) "
                       "VALUES ('delete', old.id, old.subject, old.sender_name, old.sender_address, old.preview); END"),
        QStringLiteral("CREATE TRIGGER messages_fts_update AFTER UPDATE OF subject, sender_name, sender_address, preview ON messages BEGIN "
                       "INSERT INTO messages_fts(messages_fts, rowid, subject, sender_name, sender_address, preview) "
                       "VALUES ('delete', old.id, old.subject, old.sender_name, old.sender_address, old.preview); "
                       "INSERT INTO messages_fts(rowid, subject, sender_name, sender_address, preview) "
                       "VALUES (new.id, new.subject, new.sender_name, new.sender_address, new.preview); END"),
        QStringLiteral("CREATE TABLE attachments("
                       "id INTEGER PRIMARY KEY, message INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE, graph_id TEXT, "
                       "name TEXT, content_type TEXT, size INTEGER NOT NULL DEFAULT 0, is_inline INTEGER NOT NULL DEFAULT 0, "
                       "content_id TEXT, blob TEXT)"),
        QStringLiteral("CREATE INDEX attachments_by_message ON attachments(message)"),
        QStringLiteral("CREATE TABLE calendars("
                       "id INTEGER PRIMARY KEY, graph_id TEXT NOT NULL UNIQUE, name TEXT, owner_address TEXT, color TEXT, "
                       "can_edit INTEGER NOT NULL DEFAULT 0, delta_link TEXT, window_start INTEGER, window_end INTEGER, "
                       "generation INTEGER NOT NULL DEFAULT 0, last_synced INTEGER, last_error TEXT)"),
        QStringLiteral("CREATE TABLE events("
                       "id INTEGER PRIMARY KEY, graph_id TEXT NOT NULL UNIQUE, "
                       "calendar INTEGER NOT NULL REFERENCES calendars(id) ON DELETE CASCADE, series_master TEXT, "
                       "start_time INTEGER NOT NULL DEFAULT 0, end_time INTEGER NOT NULL DEFAULT 0, all_day INTEGER NOT NULL DEFAULT 0, "
                       "subject TEXT, location TEXT, organizer TEXT, show_as INTEGER, response INTEGER, "
                       "is_online_meeting INTEGER NOT NULL DEFAULT 0, type INTEGER, change_key TEXT, "
                       "seen_generation INTEGER NOT NULL DEFAULT 0, json TEXT)"),
        QStringLiteral("CREATE INDEX events_by_time ON events(calendar, start_time)"),
        QStringLiteral("CREATE TABLE contact_folders("
                       "id INTEGER PRIMARY KEY, graph_id TEXT NOT NULL UNIQUE, name TEXT, delta_link TEXT, "
                       "generation INTEGER NOT NULL DEFAULT 0, last_synced INTEGER, last_error TEXT)"),
        QStringLiteral("CREATE TABLE contacts("
                       "id INTEGER PRIMARY KEY, graph_id TEXT NOT NULL UNIQUE, "
                       "folder INTEGER NOT NULL REFERENCES contact_folders(id) ON DELETE CASCADE, display_name TEXT, emails TEXT, "
                       "change_key TEXT, seen_generation INTEGER NOT NULL DEFAULT 0, json TEXT)"),
        QStringLiteral("CREATE INDEX contacts_by_name ON contacts(display_name)"),
        QStringLiteral("CREATE TABLE todo_lists("
                       "id INTEGER PRIMARY KEY, graph_id TEXT NOT NULL UNIQUE, name TEXT, well_known TEXT, delta_link TEXT, "
                       "generation INTEGER NOT NULL DEFAULT 0, last_synced INTEGER, last_error TEXT)"),
        QStringLiteral("CREATE TABLE tasks("
                       "id INTEGER PRIMARY KEY, graph_id TEXT NOT NULL UNIQUE, "
                       "list INTEGER NOT NULL REFERENCES todo_lists(id) ON DELETE CASCADE, title TEXT, status INTEGER, "
                       "due INTEGER, reminder INTEGER, change_key TEXT, seen_generation INTEGER NOT NULL DEFAULT 0, json TEXT)"),
        QStringLiteral("CREATE INDEX tasks_by_due ON tasks(list, due)"),
        QStringLiteral("CREATE TABLE ops("
                       "id INTEGER PRIMARY KEY, created INTEGER NOT NULL, kind TEXT NOT NULL, target TEXT, payload TEXT, "
                       "state INTEGER NOT NULL DEFAULT 0, attempts INTEGER NOT NULL DEFAULT 0, next_attempt INTEGER NOT NULL DEFAULT 0, "
                       "last_error TEXT)"),
        QStringLiteral("CREATE INDEX ops_due ON ops(state, next_attempt)"),
    };
}

// Phase 4: messages being written and sent (Outgoing; the JSON holds the whole record)
QStringList schemaV2()
{
    return {
        QStringLiteral("CREATE TABLE outgoing("
                       "uuid TEXT PRIMARY KEY, state INTEGER NOT NULL, draft_id TEXT, next_attempt INTEGER NOT NULL DEFAULT 0, "
                       "updated INTEGER NOT NULL, json TEXT NOT NULL) WITHOUT ROWID"),
        QStringLiteral("CREATE INDEX outgoing_draft ON outgoing(draft_id)"),
    };
}

// Phase 5: the default calendar among several, the kind of a message (invitations), reminders shown
QStringList schemaV3()
{
    return {
        QStringLiteral("ALTER TABLE calendars ADD COLUMN is_default INTEGER NOT NULL DEFAULT 0"),
        QStringLiteral("ALTER TABLE messages ADD COLUMN item_type INTEGER NOT NULL DEFAULT 0"),
        QStringLiteral("CREATE TABLE reminders(key TEXT PRIMARY KEY, due INTEGER NOT NULL, state INTEGER NOT NULL, snooze_until INTEGER NOT NULL DEFAULT 0) WITHOUT ROWID"),
    };
}

// Schema 4 (docs/PHASE8.md): the search index also holds the recipients and the text of every
// body that is offline (body_text, filled when a body is stored, cleared when it leaves the window)
QStringList schemaV4()
{
    return {
        QStringLiteral("ALTER TABLE messages ADD COLUMN body_text TEXT"),
        QStringLiteral("DROP TRIGGER messages_fts_insert"),
        QStringLiteral("DROP TRIGGER messages_fts_delete"),
        QStringLiteral("DROP TRIGGER messages_fts_update"),
        QStringLiteral("DROP TABLE messages_fts"),
        QStringLiteral("CREATE VIRTUAL TABLE messages_fts USING fts5(subject, sender_name, sender_address, to_list, cc_list, preview, body_text, content='messages', content_rowid='id', tokenize='unicode61 remove_diacritics 2')"),
        QStringLiteral("CREATE TRIGGER messages_fts_insert AFTER INSERT ON messages BEGIN "
                       "INSERT INTO messages_fts(rowid, subject, sender_name, sender_address, to_list, cc_list, preview, body_text) VALUES (new.id, new.subject, new.sender_name, new.sender_address, new.to_list, new.cc_list, new.preview, new.body_text); END"),
        QStringLiteral("CREATE TRIGGER messages_fts_delete AFTER DELETE ON messages BEGIN "
                       "INSERT INTO messages_fts(messages_fts, rowid, subject, sender_name, sender_address, to_list, cc_list, preview, body_text) VALUES ('delete', old.id, old.subject, old.sender_name, old.sender_address, old.to_list, old.cc_list, old.preview, old.body_text); END"),
        QStringLiteral("CREATE TRIGGER messages_fts_update AFTER UPDATE OF subject, sender_name, sender_address, to_list, cc_list, preview, body_text ON messages BEGIN "
                       "INSERT INTO messages_fts(messages_fts, rowid, subject, sender_name, sender_address, to_list, cc_list, preview, body_text) VALUES ('delete', old.id, old.subject, old.sender_name, old.sender_address, old.to_list, old.cc_list, old.preview, old.body_text); "
                       "INSERT INTO messages_fts(rowid, subject, sender_name, sender_address, to_list, cc_list, preview, body_text) VALUES (new.id, new.subject, new.sender_name, new.sender_address, new.to_list, new.cc_list, new.preview, new.body_text); END"),
        QStringLiteral("INSERT INTO messages_fts(messages_fts) VALUES ('rebuild')"),
        // The background fill: bodies stored before schema 4 still need their text
        QStringLiteral("CREATE INDEX messages_body_without_text ON messages(received DESC) WHERE body_blob IS NOT NULL AND body_text IS NULL"),
    };
}

// Schema 5 (docs/PHASE9.md): Focused Inbox. Existing rows start as Focused; the daemon marks the
// Inbox's Other messages once (meta "focusFilled")
QStringList schemaV5()
{
    return {
        QStringLiteral("ALTER TABLE messages ADD COLUMN focused INTEGER NOT NULL DEFAULT 1"),
        QStringLiteral("CREATE INDEX messages_by_focus ON messages(folder, focused, received DESC, id DESC)"),
    };
}

// Schema 6 (docs/PHASE9B.md 9b.22): a mailing list's List-Unsubscribe header, kept with the body
QStringList schemaV6()
{
    return {
        QStringLiteral("ALTER TABLE messages ADD COLUMN list_unsubscribe TEXT"),
        QStringLiteral("ALTER TABLE messages ADD COLUMN unsubscribe_one_click INTEGER NOT NULL DEFAULT 0"),
    };
}

// Schema 7 (docs/PHASE9B.md 9b.12): whether the sender asked for a read receipt, kept with the body
QStringList schemaV7()
{
    return {QStringLiteral("ALTER TABLE messages ADD COLUMN read_receipt INTEGER")};
}
}

QList<QStringList> Store::migrations()
{
    return {schemaV1(), schemaV2(), schemaV3(), schemaV4(), schemaV5(), schemaV6(), schemaV7()};
}

// Shared with storeextra.cpp (store_p.h)
namespace StoreRows
{
const QLatin1StringView listColumns("id, sender_name, sender_address, subject, preview, received, is_read, flag, importance, has_attachments, to_list, categories");

ListRow readListRow(const QSqlQuery &q)
{
    ListRow row{q.value(0).toLongLong(),
                q.value(1).toString(),
                q.value(2).toString(),
                q.value(3).toString(),
                q.value(4).toString(),
                q.value(5).toLongLong(),
                q.value(6).toBool(),
                q.value(7).toInt(),
                q.value(8).toInt(),
                q.value(9).toBool(),
                q.value(10).toString(),
                0,
                categoriesFromJson(q.value(11).toString())};
    return row;
}
}
using StoreRows::listColumns;
using StoreRows::readListRow;

Store::Store() = default;

Store::~Store()
{
    close();
}

bool Store::fail(const QString &what, const QString &detail) const
{
    m_lastError = what + QStringLiteral(": ") + detail;
    qCWarning(GRAPHMAIL).noquote() << m_lastError;
    return false;
}

QString Store::lastError() const
{
    return m_lastError;
}

bool Store::isOpen() const
{
    return m_db.isOpen();
}

int Store::version() const
{
    return m_version;
}

bool Store::exec(const QString &sql)
{
    QSqlQuery q(m_db);
    if (!q.exec(sql)) {
        return fail(sql.left(60), q.lastError().text());
    }
    return true;
}

bool Store::open(const QString &path, Mode mode, QString *error, const QList<QStringList> *steps)
{
    close();
    m_mode = mode;
    m_connection = QStringLiteral("graphmailcore-") + QUuid::createUuid().toString(QUuid::Id128);
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    m_db.setDatabaseName(path);
    // The daemon writes in short transactions; a reader that hits one waits instead of failing
    m_db.setConnectOptions(mode == Mode::ReadOnly ? QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000;QSQLITE_OPEN_READONLY")
                                                  : QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!m_db.open()) {
        *error = QStringLiteral("cannot open %1: %2").arg(path, m_db.lastError().text());
        close();
        return false;
    }

    QSqlQuery q(m_db);
    m_version = q.exec(QStringLiteral("PRAGMA user_version")) && q.next() ? q.value(0).toInt() : -1;
    q.finish();
    const QList<QStringList> all = steps ? *steps : migrations();
    const int target = int(all.size());

    if (mode == Mode::ReadOnly) {
        if (m_version != target) {
            *error = QStringLiteral("the cache has schema version %1, this program needs %2").arg(m_version).arg(target);
            close();
            return false;
        }
        return true;
    }

    if (m_version > target) {
        *error = QStringLiteral("the cache was written by a newer version (schema %1, this program knows up to %2)").arg(m_version).arg(target);
        close();
        return false;
    }
    bool ok = exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    if (ok && m_version == 0) {
        // Only possible before the first table exists
        ok = exec(QStringLiteral("PRAGMA page_size = 8192"));
    }
    // WAL: the UI reads while the daemon writes; NORMAL is safe with WAL (a crash loses at most
    // the last transactions, never consistency) and much faster than FULL
    ok = ok && exec(QStringLiteral("PRAGMA journal_mode = WAL")) && exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    ok = ok && migrate(all);
    if (!ok) {
        *error = m_lastError;
        close();
        return false;
    }
    return true;
}

bool Store::migrate(const QList<QStringList> &steps)
{
    for (int version = m_version; version < steps.size(); ++version) {
        if (!m_db.transaction()) {
            return fail(QStringLiteral("migration"), m_db.lastError().text());
        }
        bool ok = true;
        for (const QString &statement : steps.at(version)) {
            if (!exec(statement)) {
                ok = false;
                break;
            }
        }
        ok = ok && exec(QStringLiteral("PRAGMA user_version = %1").arg(version + 1));
        if (!ok) {
            m_db.rollback();
            m_lastError = QStringLiteral("migration to schema %1 failed: ").arg(version + 1) + m_lastError;
            return false;
        }
        if (!m_db.commit()) {
            return fail(QStringLiteral("migration commit"), m_db.lastError().text());
        }
        m_version = version + 1;
        qCInfo(GRAPHMAIL) << "Cache migrated to schema" << m_version;
    }
    return true;
}

void Store::close()
{
    if (m_connection.isEmpty()) {
        return;
    }
    m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connection);
    m_connection.clear();
    m_version = 0;
}

bool Store::inTransaction(const std::function<bool()> &work)
{
    if (!m_db.transaction()) {
        return fail(QStringLiteral("begin"), m_db.lastError().text());
    }
    if (!work()) {
        m_db.rollback();
        return false;
    }
    if (!m_db.commit()) {
        m_db.rollback();
        return fail(QStringLiteral("commit"), m_db.lastError().text());
    }
    return true;
}

QString Store::meta(const QString &key) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
    q.addBindValue(key);
    return q.exec() && q.next() ? q.value(0).toString() : QString();
}

bool Store::setMeta(const QString &key, const QString &value)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO meta(key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    q.addBindValue(key);
    q.addBindValue(value);
    return q.exec() || fail(QStringLiteral("setMeta"), q.lastError().text());
}

qint64 Store::upsertFolder(const FolderRow &folder)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT INTO folders(graph_id, parent_graph_id, name, well_known, total, unread) VALUES (?, ?, ?, ?, ?, ?) "
                             "ON CONFLICT(graph_id) DO UPDATE SET parent_graph_id = excluded.parent_graph_id, name = excluded.name, "
                             "well_known = COALESCE(excluded.well_known, folders.well_known), total = excluded.total, unread = excluded.unread "
                             "RETURNING id"));
    q.addBindValue(folder.graphId);
    q.addBindValue(text(folder.parentGraphId));
    q.addBindValue(folder.name);
    q.addBindValue(text(folder.wellKnown));
    q.addBindValue(folder.total);
    q.addBindValue(folder.unread);
    if (!q.exec() || !q.next()) {
        fail(QStringLiteral("upsertFolder"), q.lastError().text());
        return 0;
    }
    return q.value(0).toLongLong();
}

std::optional<FolderRow> Store::folder(qint64 id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM folders WHERE id = ?").arg(folderColumns));
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        return readFolder(q);
    }
    return std::nullopt;
}

std::optional<FolderRow> Store::folderByGraphId(const QString &graphId) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM folders WHERE graph_id = ?").arg(folderColumns));
    q.addBindValue(graphId);
    if (q.exec() && q.next()) {
        return readFolder(q);
    }
    return std::nullopt;
}

QList<FolderRow> Store::folders() const
{
    QList<FolderRow> result;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT %1 FROM folders ORDER BY id").arg(folderColumns))) {
        while (q.next()) {
            result.append(readFolder(q));
        }
    }
    return result;
}

bool Store::setFolderCounts(qint64 id, int total, int unread)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE folders SET total = ?, unread = ? WHERE id = ?"));
    q.addBindValue(total);
    q.addBindValue(unread);
    q.addBindValue(id);
    return q.exec() || fail(QStringLiteral("setFolderCounts"), q.lastError().text());
}

SyncState Store::syncState(qint64 folder) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT delta_link, recent_delta_link, initial_state, last_synced, last_error FROM folders WHERE id = ?"));
    q.addBindValue(folder);
    if (!q.exec() || !q.next()) {
        return {};
    }
    return {q.value(0).toString(), q.value(1).toString(), q.value(2).toInt(), q.value(3).toLongLong(), q.value(4).toString()};
}

bool Store::setSyncState(qint64 folder, const SyncState &state)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE folders SET delta_link = ?, recent_delta_link = ?, initial_state = ?, last_synced = ?, last_error = ? WHERE id = ?"));
    q.addBindValue(text(state.deltaLink));
    q.addBindValue(text(state.recentDeltaLink));
    q.addBindValue(state.initialState);
    q.addBindValue(state.lastSynced);
    q.addBindValue(text(state.lastError));
    q.addBindValue(folder);
    return q.exec() || fail(QStringLiteral("setSyncState"), q.lastError().text());
}

QStringList Store::blobsOfMessages(const QString &where, const QVariantList &values) const
{
    QStringList blobs;
    QSqlQuery q(m_db);
    // `where` refers to the messages table as m
    q.prepare(QStringLiteral("SELECT m.body_blob FROM messages m WHERE m.body_blob IS NOT NULL AND (%1) "
                             "UNION ALL SELECT a.blob FROM attachments a JOIN messages m ON m.id = a.message WHERE a.blob IS NOT NULL AND (%1)")
                  .arg(where));
    // The condition appears twice
    for (int round = 0; round < 2; ++round) {
        for (const QVariant &value : values) {
            q.addBindValue(value);
        }
    }
    if (q.exec()) {
        while (q.next()) {
            blobs.append(q.value(0).toString());
        }
    } else {
        fail(QStringLiteral("blobsOfMessages"), q.lastError().text());
    }
    return blobs;
}

QStringList Store::removeFolder(qint64 id)
{
    const QStringList blobs = blobsOfMessages(QStringLiteral("m.folder = ?"), {id});
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM folders WHERE id = ?"));
    q.addBindValue(id);
    if (!q.exec()) {
        fail(QStringLiteral("removeFolder"), q.lastError().text());
        return {};
    }
    return blobs;
}

Store::Upsert Store::upsertMessage(const MessageRow &m, int generation)
{
    QSqlQuery find(m_db);
    find.prepare(QStringLiteral("SELECT id, change_key, folder, focused FROM messages WHERE graph_id = ?"));
    find.addBindValue(m.graphId);
    if (!find.exec()) {
        fail(QStringLiteral("upsertMessage"), find.lastError().text());
        return Upsert::Failed;
    }
    const bool exists = find.next();
    const qint64 id = exists ? find.value(0).toLongLong() : 0;
    // Focused/Other counts too: a cache from before schema 5 has the same change key with the
    // default value, and the Inbox's fresh chain brings the real one
    const bool focusDiffers = exists && m.focusKnown && find.value(3).toBool() != m.focused;
    if (exists && !focusDiffers && !m.changeKey.isEmpty() && find.value(1).toString() == m.changeKey && find.value(2).toLongLong() == m.folder) {
        // Nothing changed (a delta page may repeat items): only mark it seen for a running resync
        QSqlQuery seen(m_db);
        seen.prepare(QStringLiteral("UPDATE messages SET seen_generation = ? WHERE id = ?"));
        seen.addBindValue(generation);
        seen.addBindValue(id);
        return seen.exec() ? Upsert::Unchanged : (fail(QStringLiteral("upsertMessage"), seen.lastError().text()), Upsert::Failed);
    }

    QSqlQuery q(m_db);
    if (exists) {
        q.prepare(QStringLiteral("UPDATE messages SET folder = ?, conversation = ?, received = ?, sent = ?, sender_name = ?, sender_address = ?, "
                                 "subject = ?, preview = ?, to_list = ?, cc_list = ?, is_read = ?, flag = ?, importance = ?, has_attachments = ?, "
                                 "categories = ?, internet_message_id = ?, change_key = ?, seen_generation = ?, item_type = ?, focused = ? WHERE id = ?"));
    } else {
        q.prepare(QStringLiteral("INSERT INTO messages(folder, conversation, received, sent, sender_name, sender_address, subject, preview, "
                                 "to_list, cc_list, is_read, flag, importance, has_attachments, categories, internet_message_id, change_key, "
                                 "seen_generation, item_type, focused, graph_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    }
    q.addBindValue(m.folder);
    q.addBindValue(text(m.conversation));
    q.addBindValue(m.received);
    q.addBindValue(m.sent);
    q.addBindValue(text(m.senderName));
    q.addBindValue(text(m.senderAddress));
    q.addBindValue(text(m.subject));
    q.addBindValue(text(m.preview));
    q.addBindValue(text(m.toList));
    q.addBindValue(text(m.ccList));
    q.addBindValue(m.isRead ? 1 : 0);
    q.addBindValue(m.flag);
    q.addBindValue(m.importance);
    q.addBindValue(m.hasAttachments ? 1 : 0);
    q.addBindValue(text(encodeCategories(m.categories)));
    q.addBindValue(text(m.internetMessageId));
    q.addBindValue(text(m.changeKey));
    q.addBindValue(generation);
    q.addBindValue(m.itemType);
    q.addBindValue((m.focusKnown || !exists ? m.focused : find.value(3).toBool()) ? 1 : 0);
    if (exists) {
        q.addBindValue(id);
    } else {
        q.addBindValue(m.graphId);
    }
    if (!q.exec()) {
        fail(QStringLiteral("upsertMessage"), q.lastError().text());
        return Upsert::Failed;
    }
    return exists ? Upsert::Updated : Upsert::Inserted;
}

RemovedMessage Store::removeMessage(const QString &graphId, qint64 onlyFromFolder)
{
    QSqlQuery find(m_db);
    find.prepare(QStringLiteral("SELECT id, folder FROM messages WHERE graph_id = ?"));
    find.addBindValue(graphId);
    if (!find.exec() || !find.next()) {
        return {};
    }
    const qint64 id = find.value(0).toLongLong();
    if (onlyFromFolder != 0 && find.value(1).toLongLong() != onlyFromFolder) {
        // Already moved on: the target folder's chain reported it there first
        return {};
    }
    RemovedMessage result;
    result.blobs = blobsOfMessages(QStringLiteral("m.id = ?"), {id});
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM messages WHERE id = ?"));
    q.addBindValue(id);
    if (!q.exec()) {
        fail(QStringLiteral("removeMessage"), q.lastError().text());
        return {};
    }
    result.removed = true;
    return result;
}

std::optional<MessageRow> Store::message(qint64 id) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM messages WHERE id = ?").arg(messageColumns));
    q.addBindValue(id);
    if (q.exec() && q.next()) {
        return readMessage(q);
    }
    return std::nullopt;
}

std::optional<MessageRow> Store::messageByGraphId(const QString &graphId) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM messages WHERE graph_id = ?").arg(messageColumns));
    q.addBindValue(graphId);
    if (q.exec() && q.next()) {
        return readMessage(q);
    }
    return std::nullopt;
}

bool Store::setBody(qint64 message, const QString &blob, int type, qint64 size, const QString &bodyText)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET body_blob = ?, body_type = ?, body_size = ?, body_text = ? WHERE id = ?"));
    q.addBindValue(text(blob));
    q.addBindValue(blob.isEmpty() ? 0 : type);
    q.addBindValue(blob.isEmpty() ? 0 : size);
    // An empty string, not NULL, for a body without text: the background fill skips it
    q.addBindValue(blob.isEmpty() ? QVariant() : QVariant(bodyText.isNull() ? QString(QLatin1String("")) : bodyText));
    q.addBindValue(message);
    return q.exec() || fail(QStringLiteral("setBody"), q.lastError().text());
}

QList<BodyWithoutText> Store::bodiesWithoutText(int limit) const
{
    QList<BodyWithoutText> result;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, body_blob, body_type FROM messages WHERE body_blob IS NOT NULL AND body_text IS NULL ORDER BY received DESC LIMIT ?"));
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            result.append({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toInt()});
        }
    }
    return result;
}

bool Store::setBodyText(qint64 message, const QString &bodyText)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET body_text = ? WHERE id = ? AND body_blob IS NOT NULL"));
    q.addBindValue(bodyText.isNull() ? QString(QLatin1String("")) : bodyText);
    q.addBindValue(message);
    return q.exec() || fail(QStringLiteral("setBodyText"), q.lastError().text());
}

bool Store::setUnsubscribe(qint64 message, const QString &header, bool oneClick)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET list_unsubscribe = ?, unsubscribe_one_click = ? WHERE id = ?"));
    q.addBindValue(header.isNull() ? QString(QLatin1String("")) : header);
    q.addBindValue(oneClick ? 1 : 0);
    q.addBindValue(message);
    return q.exec() || fail(QStringLiteral("setUnsubscribe"), q.lastError().text());
}

bool Store::setReadReceiptRequested(qint64 message, bool requested)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET read_receipt = ? WHERE id = ?"));
    q.addBindValue(requested ? 1 : 0);
    q.addBindValue(message);
    return q.exec() || fail(QStringLiteral("setReadReceiptRequested"), q.lastError().text());
}

std::optional<bool> Store::readReceiptRequested(qint64 message) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT read_receipt FROM messages WHERE id = ?"));
    q.addBindValue(message);
    if (!q.exec() || !q.next() || q.value(0).isNull()) {
        return std::nullopt;
    }
    return q.value(0).toInt() != 0;
}

QString Store::unsubscribe(qint64 message, bool *oneClick) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT list_unsubscribe, unsubscribe_one_click FROM messages WHERE id = ?"));
    q.addBindValue(message);
    if (!q.exec() || !q.next()) {
        return {};
    }
    if (oneClick) {
        *oneClick = q.value(1).toInt() != 0;
    }
    return q.value(0).isNull() ? QString() : q.value(0).toString();
}

QHash<QString, qint64> Store::messageIdsByGraphIds(const QStringList &graphIds) const
{
    QHash<QString, qint64> ids;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM messages WHERE graph_id = ?"));
    for (const QString &graphId : graphIds) {
        q.addBindValue(graphId);
        if (q.exec() && q.next()) {
            ids.insert(graphId, q.value(0).toLongLong());
        }
    }
    return ids;
}

bool Store::setRead(qint64 message, bool read)
{
    // The folder's unread count follows at once (the next folder tree sync sets Graph's number)
    QSqlQuery count(m_db);
    count.prepare(QStringLiteral("UPDATE folders SET unread = MAX(0, unread + ?) WHERE id = (SELECT folder FROM messages WHERE id = ? AND is_read <> ?)"));
    count.addBindValue(read ? -1 : 1);
    count.addBindValue(message);
    count.addBindValue(read ? 1 : 0);
    if (!count.exec()) {
        return fail(QStringLiteral("setRead"), count.lastError().text());
    }
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE messages SET is_read = ? WHERE id = ?"));
    q.addBindValue(read ? 1 : 0);
    q.addBindValue(message);
    return q.exec() || fail(QStringLiteral("setRead"), q.lastError().text());
}

QList<ListRow> Store::listPage(qint64 folder, const std::optional<PageCursor> &after, int limit, int focus) const
{
    QList<ListRow> rows;
    QSqlQuery q(m_db);
    const QString focusFilter = focus >= 0 ? QStringLiteral(" AND focused = %1").arg(focus > 0 ? 1 : 0) : QString();
    if (after) {
        q.prepare(QStringLiteral("SELECT %1 FROM messages WHERE folder = ?%2 AND (received < ? OR (received = ? AND id < ?)) "
                                 "ORDER BY received DESC, id DESC LIMIT ?")
                      .arg(listColumns, focusFilter));
        q.addBindValue(folder);
        q.addBindValue(after->received);
        q.addBindValue(after->received);
        q.addBindValue(after->id);
    } else {
        q.prepare(QStringLiteral("SELECT %1 FROM messages WHERE folder = ?%2 ORDER BY received DESC, id DESC LIMIT ?").arg(listColumns, focusFilter));
        q.addBindValue(folder);
    }
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            rows.append(readListRow(q));
        }
    } else {
        fail(QStringLiteral("listPage"), q.lastError().text());
    }
    return rows;
}

QString Store::ftsQuery(const QString &text)
{
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    QStringList words = text.split(whitespace, Qt::SkipEmptyParts);
    // A word without letters or digits ("-", "&") has no tokens and would break the query
    words.removeIf([](const QString &word) {
        return std::none_of(word.cbegin(), word.cend(), [](QChar c) {
            return c.isLetterOrNumber();
        });
    });
    if (words.isEmpty()) {
        return {};
    }
    QStringList terms;
    for (const QString &word : std::as_const(words)) {
        // Quoted, so FTS5 operators the user typed (AND, NOT, NEAR, "-", ":") are just text
        QString quoted = word;
        terms.append(QLatin1Char('"') + quoted.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"'));
    }
    // While typing, the last word is probably incomplete
    if (!text.isEmpty() && !text.back().isSpace()) {
        terms.last().append(QLatin1Char('*'));
    }
    return terms.join(QLatin1Char(' '));
}

QList<ListRow> Store::search(const SearchQuery &query, const QList<qint64> &folders, int limit, const QList<qint64> &onlyIds) const
{
    if (query.isEmpty() && onlyIds.isEmpty()) {
        return {};
    }
    const QString match = query.fts();
    QString sql = QStringLiteral("SELECT m.id, m.sender_name, m.sender_address, m.subject, m.preview, m.received, m.is_read, m.flag, m.importance, "
                                 "m.has_attachments, m.to_list, m.categories, m.folder FROM messages m");
    QStringList where;
    QVariantList values;
    if (!match.isEmpty()) {
        sql += QStringLiteral(" JOIN messages_fts ON messages_fts.rowid = m.id");
        where << QStringLiteral("messages_fts MATCH ?");
        values << match;
    }
    if (!folders.isEmpty()) {
        QStringList marks;
        for (const qint64 folder : folders) {
            marks << QStringLiteral("?");
            values << folder;
        }
        where << QStringLiteral("m.folder IN (") + marks.join(QLatin1Char(',')) + QLatin1Char(')');
    }
    if (!onlyIds.isEmpty()) {
        QStringList marks;
        for (const qint64 id : onlyIds) {
            marks << QStringLiteral("?");
            values << id;
        }
        where << QStringLiteral("m.id IN (") + marks.join(QLatin1Char(',')) + QLatin1Char(')');
    }
    if (query.unread) {
        where << (*query.unread ? QStringLiteral("m.is_read = 0") : QStringLiteral("m.is_read = 1"));
    }
    if (query.flagged) {
        where << (*query.flagged ? QStringLiteral("m.flag = 1") : QStringLiteral("m.flag != 1"));
    }
    if (query.attachments) {
        where << (*query.attachments ? QStringLiteral("m.has_attachments = 1") : QStringLiteral("m.has_attachments = 0"));
    }
    if (query.highImportance) {
        where << QStringLiteral("m.importance = 2");
    }
    // Categories are kept as a JSON array of names
    for (const QString &category : query.categories) {
        where << QStringLiteral("m.categories LIKE ? ESCAPE '\\'");
        QString like = category;
        like.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('%'), QStringLiteral("\\%")).replace(QLatin1Char('_'), QStringLiteral("\\_"));
        values << QString(QStringLiteral("%\"") + like + QStringLiteral("\"%"));
    }
    // An attachment's name: only those whose list was fetched; Graph's search finds the rest
    for (const QString &name : query.attachmentNames) {
        where << QStringLiteral("EXISTS (SELECT 1 FROM attachments a WHERE a.message = m.id AND a.name LIKE ? ESCAPE '\\')");
        QString like = name;
        like.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('%'), QStringLiteral("\\%")).replace(QLatin1Char('_'), QStringLiteral("\\_"));
        values << QString(QLatin1Char('%') + like + QLatin1Char('%'));
    }
    if (query.receivedFrom > 0) {
        where << QStringLiteral("m.received >= ?");
        values << query.receivedFrom;
    }
    if (query.receivedTo > 0) {
        where << QStringLiteral("m.received < ?");
        values << query.receivedTo;
    }
    if (!where.isEmpty()) {
        sql += QStringLiteral(" WHERE ") + where.join(QStringLiteral(" AND "));
    }
    sql += QStringLiteral(" ORDER BY m.received DESC, m.id DESC LIMIT ?");
    values << limit;
    QList<ListRow> rows;
    QSqlQuery q(m_db);
    q.prepare(sql);
    for (const QVariant &v : std::as_const(values)) {
        q.addBindValue(v);
    }
    if (q.exec()) {
        while (q.next()) {
            ListRow row = readListRow(q);
            row.folder = q.value(12).toLongLong();
            rows.append(row);
        }
    } else {
        fail(QStringLiteral("search"), q.lastError().text());
    }
    return rows;
}

QList<ListRow> Store::search(const QString &text, qint64 folder, int limit) const
{
    const QString query = ftsQuery(text);
    if (query.isEmpty()) {
        return {};
    }
    QList<ListRow> rows;
    QSqlQuery q(m_db);
    QString sql = QStringLiteral("SELECT m.id, m.sender_name, m.sender_address, m.subject, m.preview, m.received, m.is_read, m.flag, m.importance, "
                                 "m.has_attachments, m.to_list, m.categories FROM messages_fts JOIN messages m ON m.id = messages_fts.rowid WHERE messages_fts MATCH ?");
    if (folder != 0) {
        sql += QStringLiteral(" AND m.folder = ?");
    }
    sql += QStringLiteral(" ORDER BY m.received DESC, m.id DESC LIMIT ?");
    q.prepare(sql);
    q.addBindValue(query);
    if (folder != 0) {
        q.addBindValue(folder);
    }
    q.addBindValue(limit);
    if (q.exec()) {
        while (q.next()) {
            rows.append(readListRow(q));
        }
    } else {
        fail(QStringLiteral("search"), q.lastError().text());
    }
    return rows;
}

QHash<qint64, int> Store::unreadCounts() const
{
    QHash<qint64, int> counts;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT folder, COUNT(*) FROM messages WHERE is_read = 0 GROUP BY folder"))) {
        while (q.next()) {
            counts.insert(q.value(0).toLongLong(), q.value(1).toInt());
        }
    }
    return counts;
}

QList<qint64> Store::messagesBefore(qint64 folder, qint64 beforeMs) const
{
    QList<qint64> ids;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM messages WHERE folder = ? AND received > 0 AND received < ? AND flag != 1 ORDER BY received, id"));
    q.addBindValue(folder);
    q.addBindValue(beforeMs);
    if (q.exec()) {
        while (q.next()) {
            ids.append(q.value(0).toLongLong());
        }
    }
    return ids;
}

QList<qint64> Store::messagesFrom(qint64 folder, const QString &address) const
{
    QList<qint64> ids;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM messages WHERE folder = ? AND lower(sender_address) = ? ORDER BY received DESC, id DESC"));
    q.addBindValue(folder);
    q.addBindValue(address.trimmed().toLower());
    if (q.exec()) {
        while (q.next()) {
            ids.append(q.value(0).toLongLong());
        }
    }
    return ids;
}

QList<qint64> Store::conversation(const QString &conversationId) const
{
    QList<qint64> ids;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM messages WHERE conversation = ? ORDER BY received, id"));
    q.addBindValue(conversationId);
    if (q.exec()) {
        while (q.next()) {
            ids.append(q.value(0).toLongLong());
        }
    }
    return ids;
}

QList<Store::ConversationMessage> Store::conversationMessages(const QStringList &conversations, const QSet<qint64> &skipFolders) const
{
    QList<ConversationMessage> result;
    if (conversations.isEmpty()) {
        return result;
    }
    QStringList marks;
    marks.reserve(conversations.size());
    for (qsizetype i = 0; i < conversations.size(); ++i) {
        marks.append(QStringLiteral("?"));
    }
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id, folder, pending_op FROM messages WHERE conversation IN (%1)").arg(marks.join(QLatin1Char(','))));
    for (const QString &c : conversations) {
        q.addBindValue(c);
    }
    if (q.exec()) {
        while (q.next()) {
            const qint64 folder = q.value(1).toLongLong();
            if (!skipFolders.contains(folder)) {
                result.append({q.value(0).toLongLong(), folder, q.value(2).toLongLong() != 0});
            }
        }
    }
    return result;
}

int Store::generation(qint64 folder) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT generation FROM folders WHERE id = ?"));
    q.addBindValue(folder);
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

int Store::beginResync(qint64 folder)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("UPDATE folders SET generation = generation + 1 WHERE id = ? RETURNING generation"));
    q.addBindValue(folder);
    if (!q.exec() || !q.next()) {
        fail(QStringLiteral("beginResync"), q.lastError().text());
        return -1;
    }
    return q.value(0).toInt();
}

QStringList Store::sweep(qint64 folder, int generation)
{
    const QStringList blobs = blobsOfMessages(QStringLiteral("m.folder = ? AND m.seen_generation < ?"), {folder, generation});
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM messages WHERE folder = ? AND seen_generation < ?"));
    q.addBindValue(folder);
    q.addBindValue(generation);
    if (!q.exec()) {
        fail(QStringLiteral("sweep"), q.lastError().text());
        return {};
    }
    return blobs;
}

QStringList Store::referencedBlobs() const
{
    QStringList blobs;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT body_blob FROM messages WHERE body_blob IS NOT NULL UNION SELECT blob FROM attachments WHERE blob IS NOT NULL"))) {
        while (q.next()) {
            blobs.append(q.value(0).toString());
        }
    }
    return blobs;
}

}
