// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "graphclient.h"
#include "store.h"

#include <QJsonArray>
#include <QObject>
#include <QSet>

#include <functional>

namespace GraphMail
{

using GraphCore::GraphClient;
using GraphCore::GraphRequest;
using GraphCore::GraphResponse;
using GraphCore::HeaderList;

class BlobStore;

struct SyncSettings {
    // The quick first chain of an initial folder sync lists this many days (docs/PHASE2.md section 4)
    int recentWindowDays = 90;
    // Graph caps message pages at 512 (phase 0); incremental pages are small anyway
    int initialPageSize = 512;
    int incrementalPageSize = 100;
    // Pages per run before the scheduler gives other collections a turn
    int pageBudget = 8;
    int calendarDaysBack = 90;
    int calendarDaysAhead = 365;
    // The calendar window moves on when its start is this many days older than intended
    int calendarRewindowDays = 7;
};

// What one mailbox's collections share. Everything lives in the mailbox worker's thread.
struct SyncContext {
    Store *store = nullptr;
    BlobStore *blobs = nullptr;
    GraphClient *client = nullptr;
    // "/me" or "/users/<object id>"
    QString root;
    SyncSettings settings;
    // Re-applies queued local changes to a message after Graph updated it (OperationQueue)
    std::function<void(const QString &graphId)> reapplyPending;
    std::function<qint64()> now;
};

struct SyncResult {
    enum class Outcome { Done, More, NetworkError, AccessDenied, Failed };
    Outcome outcome = Outcome::Done;
    int changes = 0;
    QString error;
    // Folder tree only: folders whose total or unread count changed, new ones, removed ones
    QList<qint64> foldersChanged;
    QList<qint64> foldersAdded;
    QList<qint64> foldersRemoved;
    // Mail folders: unread messages that arrived in an incremental run (never during an initial
    // sync or a resync), for new-mail notifications
    QList<qint64> newUnread;
};

// One collection: a delta chain (or a few) kept in the cache. run() never blocks; done is called
// once. Subclasses: FolderTreeSync, MailFolderSync, ParentItemSync (calendar, contacts), TodoSync.
class CollectionSync : public QObject
{
    Q_OBJECT

public:
    explicit CollectionSync(SyncContext *context, QObject *parent = nullptr);

    virtual QString key() const = 0;
    void run(std::function<void(const SyncResult &)> done);
    bool isRunning() const;

protected:
    enum class ChainEnd { Delta, Budget, Gone, Network, AccessDenied, Failed };
    // Stores one page (items, then the link to continue with) in one transaction; false aborts
    using PageHandler = std::function<bool(const QJsonArray &items, const QString &link, bool isDeltaLink)>;
    using ChainDone = std::function<void(ChainEnd end, const QString &detail)>;

    virtual void start() = 0;
    void finish(const SyncResult &result);
    // Follows a delta chain from `link`; pageBudget 0 = until the deltaLink
    void followChain(const QString &link, const HeaderList &headers, int pageBudget, const PageHandler &handler, const ChainDone &done);
    // Maps a ChainEnd that is not Delta/Budget/Gone to a result and finishes
    void finishWithEnd(ChainEnd end, const QString &detail, int changes);
    void removeBlobs(const QStringList &blobs);
    // One parent's items (a calendar window, the contacts, a To Do list): the parent's delta link,
    // or a fresh chain with mark and sweep when it has none or it expired. next() runs after the
    // deltaLink; errors finish the sync.
    using ItemMapper = std::function<ItemRow(const QJsonObject &)>;
    void runItemChain(ItemKind kind,
                      const ParentRow &parent,
                      const QString &freshUrl,
                      const HeaderList &headers,
                      const ItemMapper &map,
                      const std::function<void()> &next,
                      bool retried = false);
    qint64 now() const;
    static QString isoUtc(qint64 ms);

    SyncContext *m_ctx;
    int m_changes = 0;
    // Item chains of several parents (calendars): a refusing one is skipped
    bool m_skipFailedParents = false;

private:
    std::function<void(const SyncResult &)> m_done;
    bool m_running = false;
};

// The folder hierarchy: a full listing every run (one request), which adds, renames and removes
// folders and reports changed counts; those wake up the folders of the cold poll class. Graph's
// folder delta link would be cheaper but does not report count changes. Resolves well-known
// folders once.
class FolderTreeSync : public CollectionSync
{
    Q_OBJECT
public:
    using CollectionSync::CollectionSync;
    QString key() const override;

private:
    void start() override;
    void resolveWellKnown();

    SyncResult m_result;
    QSet<QString> m_seen;
    bool m_full = false;
};

// The messages of one mail folder: recent window first, then the full chain, then incremental
class MailFolderSync : public CollectionSync
{
    Q_OBJECT
public:
    MailFolderSync(SyncContext *context, qint64 folder, const QString &folderGraphId, QObject *parent = nullptr);
    QString key() const override;
    qint64 folder() const;

    // Initial states stored in folders.initial_state
    enum State { NotStarted = 0, RecentWindow = 1, FullChain = 2, Incremental = 3 };

private:
    void start() override;
    void runRecentWindow();
    void startFullChain();
    void runRecentIncremental();
    void continueFullChain();
    void runIncremental();
    PageHandler pageHandler(const std::function<void(SyncState &, const QString &)> &saveLink);
    void fetchWholeMessage(const QString &graphId);
    QString baseUrl() const;
    HeaderList headers(int pageSize) const;

    qint64 m_folder;
    QString m_folderGraphId;
    SyncState m_state;
    // True while following the incremental delta link: only then are inserts "new mail"
    bool m_incremental = false;
    QList<qint64> m_newUnread;
};

// Calendar events (calendarView window) and contacts: items under one parent row
class ParentItemSync : public CollectionSync
{
    Q_OBJECT
public:
    enum class Kind { Calendar, Contacts };
    ParentItemSync(SyncContext *context, Kind kind, QObject *parent = nullptr);
    QString key() const override;

private:
    void start() override;
    void withParent(const std::function<void(const ParentRow &)> &next);
    void syncParent(qsizetype index);
    void runChain(const ParentRow &parent, const std::function<void()> &done);

    Kind m_kind;
    QList<ParentRow> m_parents;
};

// Microsoft To Do: the lists, then every list's tasks/delta chain (own mailbox only)
class TodoSync : public CollectionSync
{
    Q_OBJECT
public:
    using CollectionSync::CollectionSync;
    QString key() const override;

private:
    void start() override;
    void syncList(qsizetype index);

    QList<ParentRow> m_lists;
};

}
