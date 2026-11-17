// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "collectionsync.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <memory>

namespace GraphCore
{
class TokenProvider;
}

namespace GraphMail
{

class BlobStore;
class BodyFetcher;
class OperationQueue;
class OutboxEngine;
class EventQueue;
class ItemQueue;

// ~/.local/state/graphmail/<account>/<mailbox>: cache.db and blobs/
QString cacheDirectory(const QString &account, const QString &mailbox);
// ~/.local/state/graphmail/<account>/people.json: written by the daemon, read by the composer
QString peopleFile(const QString &account);
// ~/.cache/graphmail/compose: images the composer hands over as files; the daemon deletes them once
// they are in the blob store
QString composeDirectory();

struct MailboxConfig {
    QString account;
    // "me" or the object id of a shared mailbox
    QString mailbox;
    // "/me" or "/users/<object id>"
    QString root;
    // Holds cache.db and blobs/
    QString cacheDir;
    // Graph host (tests: a local fake server)
    QString baseUrl;
    SyncSettings settings;
    int bodyWindowDays = 90;
    bool attachmentsOffline = false;
    bool prefetchOnMetered = false;
    int hotSeconds = 60;
    int warmSeconds = 300;
    int coldSeconds = 1800;
    // At most this many requests of this mailbox at once (Exchange allows four per mailbox)
    int maxConcurrent = 4;
    bool todo = true;
    // What another mailbox gives the user (docs/PHASE7.md): only these parts are synced
    bool mail = true;
    bool calendar = true;
    bool contacts = true;
};

// Syncs one mailbox (docs/PHASE2.md sections 1 and 4). Lives in its own thread: start() there
// opens the cache, creates the collections and runs the scheduler tick. Poll classes: hot (folder
// tree, Inbox, Drafts, Sent Items, Outbox, the calendar, folders the user has open), warm (changes
// in the last 7 days), cold (the rest, contacts, To Do; also woken by changed folder counts).
// Network errors pause the whole mailbox with a growing backoff. Status goes out as JSON.
class MailboxWorker : public QObject
{
    Q_OBJECT

public:
    MailboxWorker(const MailboxConfig &config, GraphCore::TokenProvider *tokens);
    ~MailboxWorker() override;

    QString cachePath() const;

public Q_SLOTS:
    void start();
    void stop();
    void syncNow();
    // Settings > Offline (docs/PHASE8.md): days of bodies to keep (-1 all, 0 none), attachments;
    // freeSpace drops what is outside the window now and shrinks the cache file
    void applyOffline(int bodyWindowDays, bool attachmentsOffline, bool prefetchOnMetered, bool freeSpace);
    // The "Waiting to be sent" window: try every waiting change now; undo and drop one
    void retryNow();
    void discardOperation(qint64 op);
    // A conflict's "Apply Mine": the user's change again, onto Microsoft 365's version now
    void applyMine(const QString &kind, qint64 row, const QString &changesJson);
    void resyncFolder(qint64 folder);
    void setFolderOpen(qint64 folder, bool open);
    void fetchBody(qint64 message);
    void fetchAttachment(qint64 attachment);
    void markRead(qint64 message, bool read, bool receipt = true);
    void setFlag(qint64 message, int flag, qint64 start = 0, qint64 due = 0);
    // Focused Inbox (docs/PHASE9.md): Move to Focused / Move to Other
    void setFocused(qint64 message, bool focused);
    // Categorize (docs/PHASE9.md): the message's categories, replaced
    void setCategories(qint64 message, const QStringList &categories);
    void move(qint64 message, qint64 folder);
    void remove(qint64 message);
    // A message being written (Outgoing JSON from the composer): stored, then brought to Exchange
    void saveOutgoing(const QString &json);
    void discardOutgoing(const QString &uuid);
    // A message read whole from Graph (recipients incl. Bcc, body), for opening a draft again;
    // answers with messageLoaded(request, json), json empty when it failed
    void loadMessage(quint64 request, qint64 message);
    // Calendar (phase 5): event JSON in Graph's shape
    void createEvent(qint64 calendar, const QString &json);
    void updateEvent(qint64 event, const QString &json);
    void deleteEvent(qint64 event, bool cancel, const QString &comment);
    void respondEvent(qint64 event, const QString &action, const QString &comment, bool sendResponse);
    // A meeting request's event, by its graph id
    void respondEventByGraphId(const QString &graphId, const QString &action, const QString &comment, bool sendResponse, qint64 proposedStart = 0, qint64 proposedEnd = 0);
    // dismiss, or snooze until `until`
    void reminderAction(qint64 event, bool dismiss, qint64 until);
    // Contacts and To Do (phase 6): kind "contact" (parent: contact folder row) or "task" (list row)
    void createItem(const QString &kind, qint64 parent, const QString &json);
    void updateItem(const QString &kind, qint64 item, const QString &json);
    void deleteItem(const QString &kind, qint64 item);
    // action: create (name), rename (list, name), delete (list)
    void todoList(const QString &action, qint64 list, const QString &name);
    // action: create, update, delete; json for create and update
    void taskStep(qint64 task, const QString &action, const QString &stepId, const QString &json);
    // A task reminder's Dismiss, or Snooze until `until`
    void taskReminderAction(qint64 task, bool dismiss, qint64 until);
    // Outlook's "Dismiss All" for the reminders of events that are over (Exchange's list, in
    // rounds: it gives at most 200 at a time)
    void dismissOverdueReminders();
    // Read-only lookups for the app, answered with queryAnswered(request, json): kind "schedule"
    // (getSchedule: addresses, start, end in ms), "rooms" (the Places API), "meetingEvent" (the
    // event of a meeting request message, by message row id)
    void query(quint64 request, const QString &kind, const QString &json);

Q_SIGNALS:
    // A change of the user's was not applied: changed elsewhere in the same fields (docs/PHASE8.md)
    void conflictFound(const QString &account, const QString &mailbox, const QString &kind, qint64 row, const QString &title, const QStringList &fields,
                       const QString &changesJson);
    void statusChanged(const QString &account, const QString &mailbox, const QString &json);
    void collectionChanged(const QString &account, const QString &mailbox, const QString &collection, int changes);
    void bodyReady(const QString &account, const QString &mailbox, qint64 message);
    void attachmentReady(const QString &account, const QString &mailbox, qint64 message, qint64 attachment, bool ok);
    // Unread mail that arrived in the Inbox during a normal (incremental) run: the newest one's
    // sender and subject, and how many arrived
    void newMail(const QString &account, const QString &mailbox, qint64 message, const QString &sender, const QString &subject, int count);
    void operationFailed(const QString &account, const QString &mailbox, const QString &message, const QString &error);
    // A message being written: saved, sent, failed or discarded (OutboxEngine::changed)
    void outgoingChanged(const QString &account, const QString &mailbox, const QString &uuid, const QString &state, const QString &error);
    // After sending from another mailbox: as, behalf or denied
    void sendRightLearned(const QString &account, const QString &fromMailbox, const QString &right);
    void messageLoaded(quint64 request, const QString &json);
    void queryAnswered(quint64 request, const QString &json);
    // A reminder of an event in the user's own calendars is due
    void reminderDue(const QString &account, const QString &mailbox, qint64 event, const QString &subject, qint64 start, const QString &location);
    void taskReminderDue(const QString &account, const QString &mailbox, qint64 task, const QString &title, qint64 due);
    // Reminders of events that are over and were never dismissed: how many (at least; Exchange
    // lists 200 at most), the oldest event's start, a few subjects (newest first)
    void overdueReminders(const QString &account, const QString &mailbox, int count, bool more, qint64 oldest, const QStringList &subjects);
    // The reminders shown now and not snoozed or dismissed (the tray lists them): JSON array of
    // {kind event|task, id, title, start, end, location, due}; sent when the list changes
    void remindersChanged(const QString &account, const QString &mailbox, const QString &json);
    // Emitted once start() is done or failed
    void started(bool ok, const QString &error);

private:
    enum PollClass { Hot = 0, Warm = 1, Cold = 2 };
    struct Entry {
        // shared: QHash values must be copyable
        std::shared_ptr<CollectionSync> sync;
        qint64 due = 0;
        PollClass pollClass = Cold;
        qint64 lastChange = 0;
        int failures = 0;
    };

    void tick();
    void addFolder(qint64 folder);
    // Answers a query with the message ids of every page of a list (search, search folder)
    void answerIds(quint64 request, const QString &firstUrl, int max);
    // One batch of the background fill; stops the timer when nothing is left
    void indexBodies();
    void addCollection(std::unique_ptr<CollectionSync> sync, PollClass pollClass);
    PollClass classFor(const QString &key) const;
    qint64 intervalMs(PollClass pollClass) const;
    void collectionDone(const QString &key, const SyncResult &result);
    void pauseForNetwork(const QString &error);
    void publishStatus(bool force = false);
    qint64 folderOf(qint64 message) const;
    void changedLocally(const QList<qint64> &folders);
    // After a calendar change: the app redraws, the calendar syncs soon
    void calendarChanged(bool syncSoon);
    void checkReminders();
    void checkTaskReminders();
    // The user's own calendars: the default one and those of the same owner it can edit
    QList<qint64> ownCalendars() const;
    // remindersChanged() when the active reminders differ from the last ones sent
    void publishReminders();
    QString m_lastReminders;
    // After a contact or To Do change: the app redraws, that collection syncs soon
    void itemsChanged(const QString &collection);
    void checkOverdueReminders(bool dismiss);
    // The own mailbox: the people the user mails most (/me/people), for address completion
    void fetchPeople();
    qint64 now() const;

    MailboxConfig m_config;
    GraphCore::TokenProvider *m_tokens;
    std::unique_ptr<Store> m_store;
    std::unique_ptr<BlobStore> m_blobs;
    GraphCore::GraphClient *m_client = nullptr;
    SyncContext m_ctx;
    OperationQueue *m_ops = nullptr;
    BodyFetcher *m_bodies = nullptr;
    OutboxEngine *m_outbox = nullptr;
    EventQueue *m_events = nullptr;
    ItemQueue *m_items = nullptr;
    qint64 m_nextReminderCheck = 0;
    // Ignore Conversation (docs/PHASE9.md): Graph has no conversation actions, so the daemon moves
    // the ignored conversations' messages (also new ones) to Deleted Items itself
    QStringList ignoredConversations() const;
    QString betaUrl() const;
    int sweepIgnored();
    qint64 m_nextOverdueCheck = 0;
    // Dismiss All is running: the next round once the queue has sent this one
    bool m_dismissingOverdue = false;
    // Ignore Conversation skipped a message with a pending operation: one more sweep is planned
    bool m_sweepAgain = false;
    int m_overdueRounds = 0;
    qint64 m_nextPeople = 0;
    QHash<QString, Entry> m_entries;
    QSet<QString> m_running;
    QSet<qint64> m_openFolders;
    QTimer m_tick;
    // Schema 4: the text of bodies stored before it goes into the search index, a batch at a time
    QTimer m_indexBodies;
    bool m_stopped = false;
    // Offline: nothing runs before this time
    qint64 m_pausedUntil = 0;
    qint64 m_backoffMs = 0;
    bool m_noAccess = false;
    // The collection that reported it: only that one is tried again (the folder tree, or the
    // calendar of a calendar-only mailbox)
    QString m_noAccessKey;
    QString m_lastError;
    qint64 m_lastSync = 0;
    qint64 m_nextMaintenance = 0;
    qint64 m_lastStatus = 0;
    bool m_statusDirty = true;
};

}
