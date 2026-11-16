// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "outgoing.h"
#include "searchquery.h"

#include <QHash>
#include <QList>
#include <QPair>
#include <QSqlDatabase>
#include <QVariantMap>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <functional>
#include <optional>

namespace GraphMail
{

// Times are UTC milliseconds since the epoch; graphId is always the immutable id.

struct FolderRow {
    qint64 id = 0;
    QString graphId;
    // The parent's graph id: the hierarchy delta may deliver a child before its parent
    QString parentGraphId;
    QString name;
    // inbox, sentitems, deleteditems, ...; never rely on names (duplicate folders in two languages)
    QString wellKnown;
    int total = 0;
    int unread = 0;
};

// The sync state of a collection (a mail folder here)
struct SyncState {
    QString deltaLink;
    // The recent-window chain of the initial sync (docs/PHASE2.md section 4)
    QString recentDeltaLink;
    int initialState = 0;
    qint64 lastSynced = 0;
    QString lastError;
};

struct MessageRow {
    qint64 id = 0;
    QString graphId;
    qint64 folder = 0;
    QString conversation;
    qint64 received = 0;
    qint64 sent = 0;
    QString senderName;
    QString senderAddress;
    QString subject;
    QString preview;
    QString toList;
    QString ccList;
    bool isRead = false;
    int flag = 0; // 0 not flagged, 1 flagged, 2 complete
    int importance = 1; // 0 low, 1 normal, 2 high
    bool hasAttachments = false;
    QStringList categories;
    QString internetMessageId;
    QString changeKey;
    // Kept by the store on updates from Graph: only setBody() and the operation queue change these
    QString bodyBlob;
    int bodyType = 0; // 0 none, 1 HTML, 2 text
    qint64 bodySize = 0;
    qint64 pendingOp = 0;
    // 0 mail, 1 meeting request, 2 meeting response, 3 other meeting message (update, cancellation)
    int itemType = 0;
    // Focused Inbox (docs/PHASE9.md): Exchange's inferenceClassification, "other" = false.
    // focusKnown: Graph's item carried it; a delta link from before schema 5 asks without it,
    // and then the stored value stays
    bool focused = true;
    bool focusKnown = true;
};

// One line of the message list: sender, subject with the date, one preview line
struct ListRow {
    qint64 id = 0;
    QString senderName;
    QString senderAddress;
    QString subject;
    QString preview;
    qint64 received = 0;
    bool isRead = false;
    int flag = 0;
    int importance = 1;
    bool hasAttachments = false;
    // Shown instead of the sender in Sent Items, Drafts and the Outbox
    QString toList;
    // Only from search(SearchQuery): the folder the message is in
    qint64 folder = 0;
    // The message's categories (names; their colours are the mailbox's master categories)
    QStringList categories;
};

// A categories column (a JSON array of names) as a list
QStringList categoriesFromJson(const QString &json);

struct BodyWithoutText {
    qint64 id = 0;
    QString blob;
    int type = 0;
};

// Where the previous screen of a list ended; lists are newest first with the id as tie-breaker.
// Keyset paging stays fast deep in a 250k-message folder, OFFSET does not (docs/PHASE2.md section 9).
struct PageCursor {
    qint64 received = 0;
    qint64 id = 0;
};

struct AttachmentRow {
    qint64 id = 0;
    QString graphId;
    QString name;
    QString contentType;
    qint64 size = 0;
    bool isInline = false;
    QString contentId;
    QString blob;
};

// A queued change to a message (docs/PHASE2.md section 6)
struct OperationRow {
    qint64 id = 0;
    qint64 created = 0;
    QString kind; // read, flag, categories, move, delete
    QString target; // the message's graph id
    QString payload; // JSON: the new value and "previous" for a rollback
    int state = 0; // 0 pending, 2 failed
    int attempts = 0;
    qint64 nextAttempt = 0;
    QString lastError;
};

// A contact or a task as the windows read it: the row, its folder or list, its Graph JSON
struct StoredItem {
    qint64 id = 0;
    QString graphId;
    qint64 parent = 0;
    QString json;
};

// A calendar of a mailbox: its own, or one a colleague shared with the user
struct CalendarRow {
    qint64 id = 0;
    QString graphId;
    QString name;
    QString owner;
    // "#rrggbb", or empty for Outlook's "auto"
    QString color;
    bool canEdit = false;
    bool isDefault = false;
};

// An event as the views show it: an occurrence carries its series master's details
struct EventRow {
    qint64 id = 0;
    QString graphId;
    qint64 calendar = 0;
    QString seriesMaster;
    qint64 start = 0;
    qint64 end = 0;
    bool allDay = false;
    QString subject;
    QString location;
    QString organizer;
    // free 0, tentative 1, busy 2, oof 3, workingElsewhere 4, unknown -1
    int showAs = 2;
    // none 0, organizer 1, tentativelyAccepted 2, accepted 3, declined 4, notResponded 5
    int response = 0;
    bool isOnlineMeeting = false;
    // singleInstance 0, occurrence 1, exception 2, seriesMaster 3
    int type = 0;
    // The event's JSON (an occurrence: its master's, with the occurrence's own start and end)
    QString json;
};

// Calendar events, contacts and To Do tasks share one shape: indexed columns plus the JSON
enum class ItemKind { Event, Contact, Task };

struct ParentRow {
    qint64 id = 0;
    QString graphId;
    QString name;
    QString wellKnown; // To Do lists only
    QString deltaLink;
    int generation = 0;
    qint64 windowStart = 0; // calendars only
    qint64 windowEnd = 0;
};

struct ItemRow {
    QString graphId;
    QString changeKey;
    // Kind-specific columns (see Store::itemColumns()); unknown names are refused
    QVariantMap columns;
    QString json;
};

// One conversation of a folder in the conversation view: its newest message, how many messages it
// has in the folder, and whether any of them is unread, flagged or has attachments
struct ConversationRow {
    // The conversation id, or "msg:<id>" for a message without one
    QString key;
    ListRow latest;
    int count = 0;
    bool anyUnread = false;
    bool anyFlagged = false;
    bool anyAttachments = false;
};

// Where the previous screen of conversations ended (newest message time, key)
struct ConversationCursor {
    qint64 latest = 0;
    QString key;
};

// What a removed message leaves behind in the blob store
struct RemovedMessage {
    bool removed = false;
    QStringList blobs;
};

// The SQLite cache of one mailbox. The daemon opens it read-write (the only writer); the UI opens
// it read-only next to it (WAL). One instance per thread: the connection belongs to the thread
// that called open().
class Store
{
public:
    enum class Mode { ReadWrite, ReadOnly };
    enum class Upsert { Inserted, Updated, Unchanged, Failed };

    static constexpr int schemaVersion = 7;
    // Each entry brings the database from version i to i + 1 (statements run in one transaction)
    static QList<QStringList> migrations();

    Store();
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    // Read-write creates the file and migrates it to schemaVersion; read-only refuses a database
    // of another version (the daemon migrates it). `steps` replaces migrations() in tests.
    bool open(const QString &path, Mode mode, QString *error, const QList<QStringList> *steps = nullptr);
    void close();
    bool isOpen() const;
    int version() const;
    QString lastError() const;

    // Runs work in one transaction; commits when it returns true, rolls back otherwise
    bool inTransaction(const std::function<bool()> &work);

    QString meta(const QString &key) const;
    bool setMeta(const QString &key, const QString &value);

    // Folders
    qint64 upsertFolder(const FolderRow &folder);
    std::optional<FolderRow> folder(qint64 id) const;
    std::optional<FolderRow> folderByGraphId(const QString &graphId) const;
    QList<FolderRow> folders() const;
    bool setFolderCounts(qint64 id, int total, int unread);
    SyncState syncState(qint64 folder) const;
    bool setSyncState(qint64 folder, const SyncState &state);
    // Removes the folder with all its messages; returns their blobs for the blob store
    QStringList removeFolder(qint64 id);

    // Messages
    // Insert, or update by graphId. A row whose change key and folder are unchanged is left alone
    // (Unchanged); body fields and pendingOp are never overwritten. `generation` marks the row as
    // seen in the folder's current resync generation.
    Upsert upsertMessage(const MessageRow &message, int generation = 0);
    // The move rule: a removal reported by folder F only deletes the row while it is still in F
    // (onlyFromFolder = 0 removes it wherever it is)
    RemovedMessage removeMessage(const QString &graphId, qint64 onlyFromFolder = 0);
    std::optional<MessageRow> message(qint64 id) const;
    std::optional<MessageRow> messageByGraphId(const QString &graphId) const;
    // bodyText: the body's text for the search index (searchquery.h bodyText())
    bool setBody(qint64 message, const QString &blob, int type, qint64 size, const QString &bodyText = {});
    // Bodies stored before schema 4, newest first: their text goes into the index in the background
    QList<BodyWithoutText> bodiesWithoutText(int limit) const;
    bool setBodyText(qint64 message, const QString &bodyText);
    // A mailing list's List-Unsubscribe header (RFC 2369), taken with the body (schema 6); oneClick:
    // List-Unsubscribe-Post says "List-Unsubscribe=One-Click" (RFC 8058). Null: not known (a body
    // fetched before schema 6), empty: the message has none
    bool setUnsubscribe(qint64 message, const QString &header, bool oneClick);
    QString unsubscribe(qint64 message, bool *oneClick = nullptr) const;
    // Whether the sender asked for a read receipt (schema 7, with the body); nullopt: not known
    bool setReadReceiptRequested(qint64 message, bool requested);
    std::optional<bool> readReceiptRequested(qint64 message) const;
    // Graph's search results (immutable ids) as cache rows; ids not in the cache are left out
    QHash<QString, qint64> messageIdsByGraphIds(const QStringList &graphIds) const;
    bool setRead(qint64 message, bool read);

    // Newest first; `after` continues from the last row of the previous screen
    // focus: -1 every message, 1 Focused, 0 Other (Focused Inbox, docs/PHASE9.md)
    QList<ListRow> listPage(qint64 folder, const std::optional<PageCursor> &after, int limit, int focus = -1) const;
    // Words the user typed, all required; the last one also matches as a prefix while typing.
    // Searches subject, sender and preview; newest first; folder 0 = every folder.
    QList<ListRow> search(const QString &text, qint64 folder, int limit) const;
    // The search field (docs/PHASE8.md): words and field words through the index, the filters in
    // SQL; folders empty = the whole mailbox; onlyIds: just these rows (Graph's results).
    // ListRow::folder is set.
    QList<ListRow> search(const SearchQuery &query, const QList<qint64> &folders, int limit, const QList<qint64> &onlyIds = {}) const;
    QHash<qint64, int> unreadCounts() const;
    QList<qint64> conversation(const QString &conversationId) const;
    // Messages of these conversations outside the given folders, in one query (Ignore Conversation's
    // sweep after every sync): id, folder and whether an operation is pending
    struct ConversationMessage {
        qint64 id = 0;
        qint64 folder = 0;
        bool pending = false;
    };
    QList<ConversationMessage> conversationMessages(const QStringList &conversations, const QSet<qint64> &skipFolders) const;
    // Sweep (docs/PHASE9B.md 9b.23): the messages from this address in a folder, newest first
    QList<qint64> messagesFrom(qint64 folder, const QString &address) const;
    // Cleanup (docs/PHASE9B.md 9b.18): messages received before this time, oldest first; flagged
    // (not completed) ones stay out, as Outlook keeps items marked for follow-up
    QList<qint64> messagesBefore(qint64 folder, qint64 beforeMs) const;
    // Conversation view: conversations of a folder, newest first (keyset paging like listPage)
    QList<ConversationRow> conversationPage(qint64 folder, const std::optional<ConversationCursor> &after, int limit, int focus = -1) const;
    // The messages of one conversation in that folder, newest first
    QList<ListRow> conversationMessages(qint64 folder, const QString &key, int focus = -1) const;

    // Resync (mark and sweep): begin returns the new generation; every row seen during the fresh
    // chain gets it through upsertMessage(); sweep removes the rest and returns their blobs
    int beginResync(qint64 folder);
    QStringList sweep(qint64 folder, int generation);
    int generation(qint64 folder) const;

    // Every blob id the cache refers to (for the orphan sweep of the blob store)
    QStringList referencedBlobs() const;

    // Local changes from the operation queue
    bool setFlag(qint64 message, int flag);
    bool setCategories(qint64 message, const QStringList &categories);
    bool setFocused(qint64 message, bool focused);
    bool setMessageFolder(qint64 message, qint64 folder);
    bool setPending(qint64 message, bool pending);
    bool setFolderWellKnown(const QString &graphId, const QString &wellKnown);
    std::optional<FolderRow> folderByWellKnown(const QString &wellKnown) const;
    qint64 messageCount(qint64 folder = 0) const;

    // Bodies and attachments (the prefetcher)
    // Messages received at or after `since` without a body, newest first
    QList<QPair<qint64, QString>> messagesWithoutBody(qint64 since, int limit) const;
    bool replaceAttachments(qint64 message, const QList<AttachmentRow> &attachments);
    QList<AttachmentRow> attachments(qint64 message) const;
    bool setAttachmentBlob(qint64 attachment, const QString &blob, const QString &contentId = {});
    // The blob of a downloaded inline attachment with this Content-ID (with or without <>); empty
    // when none is cached. A signature copied from Outlook on the web names its pictures so
    QString blobOfContentId(const QString &contentId) const;
    std::optional<qint64> messageOfAttachment(qint64 attachment) const;
    // Drops bodies and inline images of messages received before `before`, except flagged ones;
    // returns the blobs to remove
    QStringList sweepBodies(qint64 before);

    // Messages being written (phase 4): saved by the composer, sent by the OutboxEngine
    bool saveOutgoing(const Outgoing &outgoing);
    std::optional<Outgoing> outgoing(const QString &uuid) const;
    std::optional<Outgoing> outgoingForDraft(const QString &draftId) const;
    // Pending or discarded ones whose next attempt is due, oldest first
    QList<Outgoing> dueOutgoing(qint64 now) const;
    QList<Outgoing> allOutgoing() const;
    bool removeOutgoing(const QString &uuid);

    // Addresses for completion: contacts, recent senders and the recipients of recent sent mail
    QList<OutgoingRecipient> knownAddresses(int recent) const;

    // Calendars (phase 5)
    qint64 upsertCalendar(const CalendarRow &calendar);
    QList<CalendarRow> calendars() const;
    std::optional<CalendarRow> calendar(qint64 id) const;
    // Events overlapping [start, end) in these calendars (all when empty), occurrences filled in
    // from their series master; masters themselves are left out
    QList<EventRow> eventsBetween(const QList<qint64> &calendars, qint64 start, qint64 end) const;
    std::optional<EventRow> event(qint64 id) const;
    std::optional<EventRow> eventByGraphId(const QString &graphId) const;
    // Occurrences of a series that were changed on their own (exceptions) in the window
    int exceptionCount(const QString &seriesMaster) const;
    // Reminders already shown: key = event graph id + start; state 1 shown, 2 dismissed
    int reminderState(const QString &key, qint64 *snoozeUntil = nullptr) const;
    bool setReminderState(const QString &key, qint64 due, int state, qint64 snoozeUntil = 0);
    bool sweepReminders(qint64 before);
    // Gives the space of deleted rows back to the file system (Settings > Offline, Free Up Space)
    bool vacuum();

    // Operation queue
    qint64 addOperation(const OperationRow &operation);
    bool updateOperationPayload(qint64 id, const QString &payload);
    std::optional<OperationRow> pendingOperation(const QString &target, const QString &kind) const;
    QList<OperationRow> operationsFor(const QString &target) const;
    // family: "event" (calendar, kinds event*), "item" (contacts and tasks, kinds item*), empty for
    // the message changes
    QList<OperationRow> dueOperations(qint64 now, int limit, const QString &family = {}) const;
    bool removeOperation(qint64 id);
    bool retryOperationLater(qint64 id, int attempts, qint64 nextAttempt, const QString &error);
    int pendingOperationCount() const;
    // The "Waiting to be sent" window (docs/PHASE8.md): every change not yet in Exchange
    QList<OperationRow> allOperations() const;
    std::optional<OperationRow> operation(qint64 id) const;
    // Retry Now: every waiting change is due at once
    bool retryAllNow();

    // Calendars, contact folders, To Do lists and their items
    qint64 upsertParent(ItemKind kind, const ParentRow &parent);
    QList<ParentRow> parents(ItemKind kind) const;
    bool setParentDeltaLink(ItemKind kind, qint64 parent, const QString &deltaLink, qint64 lastSynced, const QString &lastError);
    bool setCalendarWindow(qint64 calendar, qint64 start, qint64 end);
    bool removeParent(ItemKind kind, qint64 parent);
    Upsert upsertItem(ItemKind kind, qint64 parent, const ItemRow &item, int generation = 0);
    bool removeItem(ItemKind kind, const QString &graphId);
    // After a complete calendar round (the delta brings every series with its occurrences):
    // removes occurrences and exceptions whose series is no longer there; gives how many
    int sweepOrphanOccurrences(qint64 calendar);
    int beginParentResync(ItemKind kind, qint64 parent);
    bool sweepItems(ItemKind kind, qint64 parent, int generation);
    qint64 itemCount(ItemKind kind, qint64 parent = 0) const;
    static QStringList itemColumns(ItemKind kind);
    // Contacts and tasks for the windows (parent 0: all folders or lists)
    QList<StoredItem> items(ItemKind kind, qint64 parent = 0) const;
    std::optional<StoredItem> item(ItemKind kind, qint64 id) const;
    std::optional<StoredItem> itemByGraphId(ItemKind kind, const QString &graphId) const;
    // Open tasks whose reminder time has come
    QList<StoredItem> dueTaskReminders(qint64 now) const;

    // The FTS5 query for what the user typed (public for tests)
    static QString ftsQuery(const QString &text);

private:
    bool exec(const QString &sql);
    bool migrate(const QList<QStringList> &steps);
    QStringList blobsOfMessages(const QString &where, const QVariantList &values) const;
    bool fail(const QString &what, const QString &detail) const;

    QSqlDatabase m_db;
    QString m_connection;
    Mode m_mode = Mode::ReadWrite;
    int m_version = 0;
    mutable QString m_lastError;
};

}
