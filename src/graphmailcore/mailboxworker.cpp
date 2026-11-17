// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mailboxworker.h"

#include <graphcore/people.h>
#include "messageimport.h"
#include "blobstore.h"
#include "bodyfetcher.h"
#include "connectivity.h"
#include "graphmail_debug.h"
#include "graphmapper.h"
#include "mailboxaccess.h"
#include "graphrequest.h"
#include "graphutil.h"
#include "operationqueue.h"
#include "eventqueue.h"
#include "itemqueue.h"
#include "outboxengine.h"
#include "reminders.h"
#include "searchquery.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTimeZone>
#include <QUrl>

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>

namespace GraphMail
{

namespace
{
constexpr int tickMs = 1000;
constexpr qint64 warmWindowMs = 7LL * 24 * 3600 * 1000;
constexpr qint64 firstBackoffMs = 15 * 1000;
constexpr qint64 maxBackoffMs = 5 * 60 * 1000;
constexpr qint64 noAccessRetryMs = 3600 * 1000;
constexpr qint64 maintenanceIntervalMs = 24LL * 3600 * 1000;
constexpr qint64 statusIntervalMs = 2000;
constexpr qint64 peopleIntervalMs = 24LL * 3600 * 1000;

qint64 localMidnight(const QDate &date)
{
    return date.isValid() ? QDateTime(date, QTime(0, 0)).toMSecsSinceEpoch() : 0;
}
bool isHotFolder(const QString &wellKnown)
{
    return wellKnown == QLatin1String("inbox") || wellKnown == QLatin1String("drafts") || wellKnown == QLatin1String("sentitems")
        || wellKnown == QLatin1String("outbox");
}
}

QString cacheDirectory(const QString &account, const QString &mailbox)
{
    // Like Graph Sync's ~/.local/state/graphsync
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/graphmail/") + account + QLatin1Char('/') + mailbox;
}

QString peopleFile(const QString &account)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/graphmail/") + account + QStringLiteral("/people.json");
}

QString composeDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/graphmail/compose");
}

MailboxWorker::MailboxWorker(const MailboxConfig &config, GraphCore::TokenProvider *tokens)
    : m_config(config)
    , m_tokens(tokens)
    // A child, so moveToThread() takes it along: the worker is created in the main thread and
    // starts the timer in its own
    , m_tick(this)
    , m_indexBodies(this)
{
}

MailboxWorker::~MailboxWorker()
{
    stop();
}

QString MailboxWorker::cachePath() const
{
    return m_config.cacheDir + QStringLiteral("/cache.db");
}

qint64 MailboxWorker::now() const
{
    return QDateTime::currentMSecsSinceEpoch();
}

void MailboxWorker::start()
{
    QDir().mkpath(m_config.cacheDir);
    m_store = std::make_unique<Store>();
    QString error;
    if (!m_store->open(cachePath(), Store::Mode::ReadWrite, &error)) {
        qCWarning(GRAPHMAIL).noquote() << "Cannot open the cache of" << m_config.mailbox << ":" << error;
        m_lastError = error;
        Q_EMIT started(false, error);
        return;
    }
    m_store->setMeta(QStringLiteral("account"), m_config.account);
    m_store->setMeta(QStringLiteral("mailbox"), m_config.mailbox);
    m_store->setMeta(QStringLiteral("root"), m_config.root);
    m_blobs = std::make_unique<BlobStore>(m_config.cacheDir + QStringLiteral("/blobs"));

    m_client = new GraphCore::GraphClient(m_tokens, this);
    if (!m_config.baseUrl.isEmpty()) {
        m_client->setBaseUrl(m_config.baseUrl);
    }
    m_client->setThrottleScope(m_config.account);

    m_ctx.store = m_store.get();
    m_ctx.blobs = m_blobs.get();
    m_ctx.client = m_client;
    m_ctx.root = m_config.root;
    m_ctx.settings = m_config.settings;

    m_ops = new OperationQueue(&m_ctx, this);
    m_ctx.reapplyPending = [this](const QString &graphId) {
        m_ops->reapply(graphId);
    };
    connect(m_ops, &OperationQueue::failed, this, [this](const QString &graphId, const QString &kind, const QString &error) {
        Q_EMIT operationFailed(m_config.account, m_config.mailbox, graphId, error);
        // The old state is back in the cache
        if (const std::optional<MessageRow> row = m_store->messageByGraphId(graphId)) {
            changedLocally({row->folder});
            // A refused delete or move because the message is no longer where the cache has it
            // (ErrorCannotDeleteObject: already in Recoverable Items): list the folder again. Not
            // for a missing permission, where the cache is right and a resync is only traffic
            static const QStringList gone{QStringLiteral("ErrorCannotDeleteObject"), QStringLiteral("ErrorItemNotFound"), QStringLiteral("ErrorMoveCopyFailed")};
            const bool stale = std::any_of(gone.cbegin(), gone.cend(), [&error](const QString &code) {
                return error.contains(code);
            });
            if (stale && (kind == QLatin1String("delete") || kind == QLatin1String("move"))) {
                resyncFolder(row->folder);
            }
        }
        m_statusDirty = true;
    });
    connect(m_ops, &OperationQueue::completed, this, [this] {
        m_statusDirty = true;
    });
    connect(m_ops, &OperationQueue::networkError, this, [this] {
        pauseForNetwork(QStringLiteral("operation could not reach Microsoft 365"));
    });

    m_outbox = new OutboxEngine(&m_ctx, this);
    connect(m_outbox, &OutboxEngine::changed, this, [this](const QString &uuid, const QString &state, const QString &error) {
        m_statusDirty = true;
        // A refused Send As goes out as "sendAsDenied:<address>": the app says it in a sentence
        QString reason = error;
        if (state == QLatin1String("failed") && error.contains(QLatin1String("ErrorSendAsDenied"))) {
            const std::optional<Outgoing> o = m_store ? m_store->outgoing(uuid) : std::nullopt;
            reason = QStringLiteral("sendAsDenied:") + (o ? o->fromAddress : QString());
        }
        Q_EMIT outgoingChanged(m_config.account, m_config.mailbox, uuid, state, reason);
        // The next one (or a newer save of this one) right away
        QTimer::singleShot(0, this, &MailboxWorker::tick);
    });
    connect(m_outbox, &OutboxEngine::sendRightLearned, this, [this](const QString &fromMailbox, const QString &right) {
        Q_EMIT sendRightLearned(m_config.account, fromMailbox, right);
    });
    connect(m_outbox, &OutboxEngine::networkError, this, [this] {
        pauseForNetwork(QStringLiteral("sending could not reach Microsoft 365"));
    });

    m_events = new EventQueue(&m_ctx, this);
    connect(m_events, &EventQueue::completed, this, [this](const QString &kind) {
        m_statusDirty = true;
        if (kind == QLatin1String("eventDismiss")) {
            // Dismiss All: once this round is sent, the next one
            if (m_dismissingOverdue && m_events->inFlight() == 0 && m_store->dueOperations(std::numeric_limits<qint64>::max(), 1, QStringLiteral("event")).isEmpty()) {
                checkOverdueReminders(true);
            }
            return;
        }
        calendarChanged(true);
    });
    connect(m_events, &EventQueue::failed, this, [this](const QString &graphId, const QString &kind, const QString &error) {
        if (kind == QLatin1String("eventDismiss")) {
            // One reminder that cannot be dismissed must not stop Dismiss All
            if (m_dismissingOverdue && m_events->inFlight() == 0 && m_store->dueOperations(std::numeric_limits<qint64>::max(), 1, QStringLiteral("event")).isEmpty()) {
                checkOverdueReminders(true);
            }
            return;
        }
        Q_EMIT operationFailed(m_config.account, m_config.mailbox, graphId, error);
        calendarChanged(false);
    });
    connect(m_events, &EventQueue::networkError, this, [this] {
        pauseForNetwork(QStringLiteral("calendar change could not reach Microsoft 365"));
    });

    m_items = new ItemQueue(&m_ctx, this);
    auto relayConflict = [this](const QString &kind, qint64 row, const QString &title, const QStringList &fields, const QJsonObject &changes) {
        m_statusDirty = true;
        Q_EMIT collectionChanged(m_config.account, m_config.mailbox, kind == QLatin1String("event") ? QStringLiteral("calendar") : QStringLiteral("todo"), 1);
        Q_EMIT conflictFound(m_config.account, m_config.mailbox, kind, row, title, fields, QString::fromUtf8(QJsonDocument(changes).toJson(QJsonDocument::Compact)));
    };
    connect(m_items, &ItemQueue::conflict, this, relayConflict);
    connect(m_outbox, &OutboxEngine::draftConflict, this, [this](const QString &, const QString &subject) {
        Q_EMIT conflictFound(m_config.account, m_config.mailbox, QStringLiteral("draft"), 0, subject, {}, {});
    });
    connect(m_events, &EventQueue::conflict, this, relayConflict);
    connect(m_items, &ItemQueue::completed, this, [this](const QString &kind) {
        m_statusDirty = true;
        itemsChanged(kind.startsWith(QLatin1String("itemList")) || kind == QLatin1String("itemStep") ? QStringLiteral("todo") : QString());
    });
    connect(m_items, &ItemQueue::failed, this, [this](const QString &graphId, const QString &, const QString &error) {
        Q_EMIT operationFailed(m_config.account, m_config.mailbox, graphId, error);
        itemsChanged({});
    });
    connect(m_items, &ItemQueue::networkError, this, [this] {
        pauseForNetwork(QStringLiteral("a contact or task change could not reach Microsoft 365"));
    });

    m_bodies = new BodyFetcher(&m_ctx, this);
    m_bodies->setWindowDays(m_config.bodyWindowDays);
    m_bodies->setAttachmentsOffline(m_config.attachmentsOffline);
    m_bodies->setPrefetchOnMetered(m_config.prefetchOnMetered);
    connect(m_bodies, &BodyFetcher::bodyReady, this, [this](qint64 message) {
        Q_EMIT bodyReady(m_config.account, m_config.mailbox, message);
    });
    connect(m_bodies, &BodyFetcher::attachmentReady, this, [this](qint64 message, qint64 attachment, bool ok) {
        Q_EMIT attachmentReady(m_config.account, m_config.mailbox, message, attachment, ok);
    });
    connect(m_bodies, &BodyFetcher::networkError, this, [this] {
        pauseForNetwork(QStringLiteral("body fetch could not reach Microsoft 365"));
    });

    if (m_config.mail) {
        addCollection(std::make_unique<FolderTreeSync>(&m_ctx), Hot);
    }
    if (m_config.calendar) {
        addCollection(std::make_unique<ParentItemSync>(&m_ctx, ParentItemSync::Kind::Calendar), Hot);
    }
    if (m_config.contacts) {
        addCollection(std::make_unique<ParentItemSync>(&m_ctx, ParentItemSync::Kind::Contacts), Cold);
    }
    if (m_config.todo) {
        addCollection(std::make_unique<TodoSync>(&m_ctx), Cold);
    }
    if (m_config.mail) {
        const QList<FolderRow> folders = m_store->folders();
        for (const FolderRow &folder : folders) {
            addFolder(folder.id);
        }
    }
    // Everything is due now: an initial sync, or catching up after the daemon was off
    for (Entry &entry : m_entries) {
        entry.due = 0;
    }
    m_nextMaintenance = now() + 5 * 60 * 1000; // not in the middle of the first catch-up

    m_tick.setInterval(tickMs);
    connect(&m_tick, &QTimer::timeout, this, &MailboxWorker::tick);
    m_tick.start();
    m_indexBodies.setInterval(2000);
    connect(&m_indexBodies, &QTimer::timeout, this, &MailboxWorker::indexBodies);
    m_indexBodies.start();
    Q_EMIT started(true, {});
    tick();
}

void MailboxWorker::applyOffline(int bodyWindowDays, bool attachmentsOffline, bool prefetchOnMetered, bool freeSpace)
{
    if (!m_bodies || !m_store) {
        return;
    }
    const bool smaller = bodyWindowDays >= 0 && (m_config.bodyWindowDays < 0 || bodyWindowDays < m_config.bodyWindowDays);
    m_config.bodyWindowDays = bodyWindowDays;
    m_config.attachmentsOffline = attachmentsOffline;
    m_config.prefetchOnMetered = prefetchOnMetered;
    m_bodies->setWindowDays(bodyWindowDays);
    m_bodies->setAttachmentsOffline(attachmentsOffline);
    m_bodies->setPrefetchOnMetered(prefetchOnMetered);
    if (smaller || freeSpace) {
        m_bodies->sweep();
    }
    if (freeSpace) {
        m_store->vacuum();
    }
    m_statusDirty = true;
    tick();
}

void MailboxWorker::applyMine(const QString &kind, qint64 row, const QString &changesJson)
{
    const QJsonObject changes = QJsonDocument::fromJson(changesJson.toUtf8()).object();
    if (kind == QLatin1String("event")) {
        m_events->update(row, changes);
    } else {
        m_items->update(kind == QLatin1String("contact") ? ItemKind::Contact : ItemKind::Task, row, changes);
    }
    m_statusDirty = true;
    tick();
}

void MailboxWorker::retryNow()
{
    if (!m_store) {
        return;
    }
    m_store->retryAllNow();
    // An offline pause ends too: the user asks for an attempt now
    m_pausedUntil = 0;
    m_backoffMs = 0;
    m_statusDirty = true;
    tick();
}

void MailboxWorker::discardOperation(qint64 id)
{
    const std::optional<OperationRow> op = m_store ? m_store->operation(id) : std::nullopt;
    if (!op) {
        return;
    }
    bool done = false;
    if (op->kind.startsWith(QLatin1String("event"))) {
        done = m_events->discard(*op);
    } else if (op->kind.startsWith(QLatin1String("item"))) {
        done = m_items->discard(*op);
    } else {
        done = m_ops->discard(*op);
    }
    qCInfo(GRAPHMAIL) << "Discarded the waiting change" << op->kind << id << done;
    m_statusDirty = true;
    Q_EMIT collectionChanged(m_config.account, m_config.mailbox, QStringLiteral("operations"), 1);
}

void MailboxWorker::indexBodies()
{
    if (!m_store || !m_blobs) {
        return;
    }
    // Small batches: the daemon stays responsive and the cache is not locked for long
    const QList<BodyWithoutText> bodies = m_store->bodiesWithoutText(200);
    if (bodies.isEmpty()) {
        m_indexBodies.stop();
        return;
    }
    m_store->inTransaction([&] {
        for (const BodyWithoutText &b : bodies) {
            QString error;
            const std::optional<QByteArray> content = m_blobs->get(b.blob, &error);
            // A missing blob gets an empty text: it is not tried again
            m_store->setBodyText(b.id, content ? bodyText(*content, b.type == 1) : QString(QLatin1String("")));
        }
        return true;
    });
}

void MailboxWorker::stop()
{
    if (m_stopped) {
        return;
    }
    m_stopped = true;
    m_tick.stop();
    m_indexBodies.stop();
    // Collections first (they hold requests and use the store), then the store in this thread
    m_entries.clear();
    m_running.clear();
    delete m_bodies;
    m_bodies = nullptr;
    delete m_outbox;
    m_outbox = nullptr;
    delete m_events;
    m_events = nullptr;
    delete m_items;
    m_items = nullptr;
    delete m_ops;
    m_ops = nullptr;
    delete m_client;
    m_client = nullptr;
    m_store.reset();
}

void MailboxWorker::addCollection(std::unique_ptr<CollectionSync> sync, PollClass pollClass)
{
    const QString key = sync->key();
    Entry entry;
    entry.sync = std::move(sync);
    entry.pollClass = pollClass;
    m_entries.insert(key, std::move(entry));
}

void MailboxWorker::addFolder(qint64 folder)
{
    const std::optional<FolderRow> row = m_store->folder(folder);
    if (!row || m_entries.contains(QStringLiteral("folder:%1").arg(folder))) {
        return;
    }
    auto sync = std::make_unique<MailFolderSync>(&m_ctx, folder, row->graphId);
    const QString key = sync->key();
    addCollection(std::move(sync), classFor(key));
}

MailboxWorker::PollClass MailboxWorker::classFor(const QString &key) const
{
    if (key == QLatin1String("tree") || key == QLatin1String("calendar")) {
        return Hot;
    }
    if (!key.startsWith(QLatin1String("folder:"))) {
        return Cold;
    }
    const qint64 folder = key.mid(7).toLongLong();
    if (m_openFolders.contains(folder)) {
        return Hot;
    }
    const std::optional<FolderRow> row = m_store->folder(folder);
    if (row && isHotFolder(row->wellKnown)) {
        return Hot;
    }
    const auto it = m_entries.constFind(key);
    if (it != m_entries.cend() && it->lastChange > 0 && now() - it->lastChange < warmWindowMs) {
        return Warm;
    }
    return Cold;
}

qint64 MailboxWorker::intervalMs(PollClass pollClass) const
{
    switch (pollClass) {
    case Hot:
        return qint64(m_config.hotSeconds) * 1000;
    case Warm:
        return qint64(m_config.warmSeconds) * 1000;
    case Cold:
        break;
    }
    return qint64(m_config.coldSeconds) * 1000;
}

void MailboxWorker::tick()
{
    if (m_stopped) {
        return;
    }
    const qint64 t = now();
    if (t < m_pausedUntil) {
        publishStatus();
        return;
    }
    if (m_pausedUntil != 0) {
        if (!GraphCore::Connectivity::systemReportsOnline()) {
            m_pausedUntil = t + firstBackoffMs;
            return;
        }
        m_pausedUntil = 0;
    }

    auto freeSlots = [this] {
        return m_config.maxConcurrent - int(m_running.size()) - m_ops->inFlight() - m_events->inFlight() - m_items->inFlight() - m_bodies->inFlight() - (m_outbox->busy() ? 1 : 0);
    };
    int free = freeSlots();
    // The user's changes and mail being sent first: they are what the user waits for
    if (free > 0 && !m_noAccess) {
        m_ops->process(std::min(free, 2));
        free = freeSlots();
    }
    if (free > 0 && !m_noAccess) {
        m_events->process(std::min(free, 2));
        free = freeSlots();
    }
    if (free > 0 && !m_noAccess) {
        m_items->process(std::min(free, 2));
        free = freeSlots();
    }
    if (free > 0 && !m_noAccess) {
        m_outbox->process();
        free = freeSlots();
    }

    // The folder tree first on a fresh cache: there are no folders to sync before it. A mailbox
    // without mail (calendar only, docs/PHASE7.md) has no tree to wait for.
    const bool treeKnown = !m_config.mail || !m_store->meta(QStringLiteral("tree_delta")).isEmpty();
    QList<QString> due;
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (m_running.contains(it.key()) || it->due > t) {
            continue;
        }
        if (m_noAccess && it.key() != m_noAccessKey) {
            continue;
        }
        if (!treeKnown && it.key() != QLatin1String("tree")) {
            continue;
        }
        due.append(it.key());
    }
    std::sort(due.begin(), due.end(), [this](const QString &a, const QString &b) {
        const Entry &ea = m_entries[a];
        const Entry &eb = m_entries[b];
        return ea.pollClass != eb.pollClass ? ea.pollClass < eb.pollClass : ea.due < eb.due;
    });
    for (const QString &key : std::as_const(due)) {
        if (free <= 0) {
            break;
        }
        --free;
        m_running.insert(key);
        CollectionSync *sync = m_entries[key].sync.get();
        sync->run([this, key](const SyncResult &result) {
            collectionDone(key, result);
        });
    }
    // Schema 5 (Focused Inbox): the Inbox's delta link asks without inferenceClassification; one
    // fresh chain brings it for every message there and for new mail from then on
    if (m_config.mail && treeKnown && m_store->meta(QStringLiteral("focusSynced")).isEmpty()) {
        m_store->setMeta(QStringLiteral("focusSynced"), QStringLiteral("1"));
        // Only an Inbox already past its first chain (an older cache) needs it: a fresh chain
        // asks for inferenceClassification already
        if (const std::optional<FolderRow> inbox = m_store->folderByWellKnown(QStringLiteral("inbox"));
            inbox && m_store->syncState(inbox->id).initialState == MailFolderSync::Incremental) {
            resyncFolder(inbox->id);
            return;
        }
    }
    // Spare room: bodies
    if (free > 0 && !m_noAccess) {
        m_bodies->process(std::min(free, 2));
    }

    if (t >= m_nextMaintenance && m_running.isEmpty()) {
        m_nextMaintenance = t + maintenanceIntervalMs;
        m_bodies->sweep();
        m_store->sweepReminders(t - 30LL * 24 * 3600 * 1000);
        // Drafts deleted or moved in Outlook leave no record of Graph Mail behind
        if (const std::optional<FolderRow> drafts = m_store->folderByWellKnown(QStringLiteral("drafts"))) {
            if (const int dropped = m_store->dropStaleDraftRecords(drafts->id)) {
                qCInfo(GRAPHMAIL) << "Dropped" << dropped << "records of drafts that left Drafts";
            }
        }
    }
    // Reminders: the own mailbox, every half minute (also while offline: they come from the cache)
    if (m_config.mailbox == QLatin1String("me") && t >= m_nextReminderCheck) {
        m_nextReminderCheck = t + 30 * 1000;
        checkReminders();
        checkTaskReminders();
        publishReminders();
    }
    // Overdue reminders (events that are over): from Exchange's list, once a day and after a start
    if (m_config.mailbox == QLatin1String("me") && treeKnown && t >= m_nextOverdueCheck && m_pausedUntil == 0) {
        m_nextOverdueCheck = t + 24LL * 3600 * 1000;
        checkOverdueReminders(false);
    }
    if (m_config.mailbox == QLatin1String("me") && t >= m_nextPeople && treeKnown) {
        m_nextPeople = t + peopleIntervalMs;
        fetchPeople();
    }
    // A long chain (the recent window of a big folder) runs for minutes before it finishes: keep
    // the counts in the status moving meanwhile (publishStatus() limits this to every 2 s)
    if (!m_running.isEmpty() || m_bodies->inFlight() > 0) {
        m_statusDirty = true;
    }
    publishStatus();
}

void MailboxWorker::collectionDone(const QString &key, const SyncResult &result)
{
    m_running.remove(key);
    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        return; // removed while running (its folder was deleted)
    }
    Entry &entry = *it;
    const qint64 t = now();
    m_statusDirty = true;
    if (result.changes > 0) {
        entry.lastChange = t;
        Q_EMIT collectionChanged(m_config.account, m_config.mailbox, key, result.changes);
        if (key.startsWith(QLatin1String("folder:"))) {
            sweepIgnored();
        }
    }
    if (!result.newUnread.isEmpty() && key.startsWith(QLatin1String("folder:"))) {
        const std::optional<FolderRow> folder = m_store->folder(key.mid(7).toLongLong());
        if (folder && folder->wellKnown == QLatin1String("inbox")) {
            // The newest of them for the notification text
            std::optional<MessageRow> newest;
            for (qint64 id : result.newUnread) {
                const std::optional<MessageRow> m = m_store->message(id);
                if (m && !m->isRead && (!newest || m->received > newest->received)) {
                    newest = m;
                }
            }
            if (newest) {
                Q_EMIT newMail(m_config.account,
                               m_config.mailbox,
                               newest->id,
                               newest->senderName.isEmpty() ? newest->senderAddress : newest->senderName,
                               newest->subject,
                               int(result.newUnread.size()));
            }
        }
    }

    switch (result.outcome) {
    case SyncResult::Outcome::Done:
        entry.failures = 0;
        m_backoffMs = 0;
        m_noAccess = false;
        m_lastSync = t;
        entry.pollClass = classFor(key);
        entry.due = t + intervalMs(entry.pollClass);
        break;
    case SyncResult::Outcome::More:
        entry.failures = 0;
        m_backoffMs = 0;
        entry.due = t; // continue at the next free slot
        break;
    case SyncResult::Outcome::NetworkError:
        entry.due = t;
        pauseForNetwork(result.error);
        break;
    case SyncResult::Outcome::AccessDenied:
        m_noAccess = true;
        m_noAccessKey = key;
        m_lastError = QStringLiteral("no access to this mailbox (ask the owner or IT for Full Access): ") + result.error;
        entry.due = t + noAccessRetryMs;
        break;
    case SyncResult::Outcome::Failed:
        ++entry.failures;
        m_lastError = key + QStringLiteral(": ") + result.error;
        qCWarning(GRAPHMAIL).noquote() << m_config.mailbox << m_lastError;
        entry.due = t + std::min<qint64>(qint64(entry.failures) * entry.failures * 60 * 1000, 3600 * 1000);
        break;
    }

    if (key == QLatin1String("tree")) {
        for (qint64 folder : result.foldersRemoved) {
            const QString folderKey = QStringLiteral("folder:%1").arg(folder);
            m_running.remove(folderKey);
            m_entries.remove(folderKey);
        }
        for (qint64 folder : result.foldersAdded) {
            addFolder(folder);
        }
        // A changed count wakes a cold folder up
        for (qint64 folder : result.foldersChanged) {
            const auto changed = m_entries.find(QStringLiteral("folder:%1").arg(folder));
            if (changed != m_entries.end()) {
                changed->due = std::min(changed->due, t);
            }
        }
    }
    QTimer::singleShot(0, this, &MailboxWorker::tick);
}

void MailboxWorker::pauseForNetwork(const QString &error)
{
    // Several collections fail in the same round: double the wait once per round, not per failure
    if (m_pausedUntil > now()) {
        return;
    }
    m_backoffMs = m_backoffMs == 0 ? firstBackoffMs : std::min(m_backoffMs * 2, maxBackoffMs);
    m_pausedUntil = now() + m_backoffMs;
    m_lastError = QStringLiteral("offline: ") + error;
    m_statusDirty = true;
    qCInfo(GRAPHMAIL).noquote() << m_config.mailbox << "offline, retrying in" << m_backoffMs / 1000 << "s:" << error;
}

void MailboxWorker::publishStatus(bool force)
{
    const qint64 t = now();
    if (!force && (!m_statusDirty || t - m_lastStatus < statusIntervalMs)) {
        return;
    }
    m_statusDirty = false;
    m_lastStatus = t;

    int folders = 0;
    int initial = 0;
    const QList<FolderRow> rows = m_store->folders();
    for (const FolderRow &row : rows) {
        ++folders;
        if (m_store->syncState(row.id).initialState != MailFolderSync::Incremental) {
            ++initial;
        }
    }
    QString state = QStringLiteral("idle");
    if (m_noAccess) {
        state = QStringLiteral("noAccess");
    } else if (m_pausedUntil > t) {
        state = QStringLiteral("offline");
    } else if (initial > 0 || (m_config.mail ? m_store->meta(QStringLiteral("tree_delta")).isEmpty() : m_lastSync == 0)) {
        state = QStringLiteral("initialSync");
    } else if (!m_running.isEmpty()) {
        state = QStringLiteral("syncing");
    }
    int inboxUnread = 0;
    if (m_config.mail) {
        if (const std::optional<FolderRow> inbox = m_store->folderByWellKnown(QStringLiteral("inbox"))) {
            inboxUnread = inbox->unread;
        }
    }
    int outgoingWaiting = 0;
    int outgoingFailed = 0;
    const QList<Outgoing> outgoing = m_store->allOutgoing();
    for (const Outgoing &o : outgoing) {
        outgoingWaiting += o.send && o.state == Outgoing::State::Pending ? 1 : 0;
        outgoingFailed += o.state == Outgoing::State::Failed ? 1 : 0;
    }
    const QJsonObject status{{QStringLiteral("account"), m_config.account},
                             {QStringLiteral("mailbox"), m_config.mailbox},
                             {QStringLiteral("state"), state},
                             {QStringLiteral("folders"), folders},
                             {QStringLiteral("foldersInInitialSync"), initial},
                             {QStringLiteral("messages"), m_store->messageCount()},
                             {QStringLiteral("events"), m_store->itemCount(ItemKind::Event)},
                             {QStringLiteral("contacts"), m_store->itemCount(ItemKind::Contact)},
                             {QStringLiteral("tasks"), m_store->itemCount(ItemKind::Task)},
                             {QStringLiteral("pendingOperations"), m_store->pendingOperationCount()},
                             {QStringLiteral("outgoing"), outgoingWaiting},
                             {QStringLiteral("outgoingFailed"), outgoingFailed},
                             {QStringLiteral("running"), int(m_running.size())},
                             {QStringLiteral("lastSync"), m_lastSync},
                             {QStringLiteral("lastError"), m_lastError},
                             {QStringLiteral("cache"), cachePath()},
                             // The tray's badge
                             {QStringLiteral("inboxUnread"), inboxUnread}};
    Q_EMIT statusChanged(m_config.account, m_config.mailbox, QString::fromUtf8(QJsonDocument(status).toJson(QJsonDocument::Compact)));
}

void MailboxWorker::syncNow()
{
    for (Entry &entry : m_entries) {
        entry.due = 0;
    }
    m_pausedUntil = 0;
    m_backoffMs = 0;
    tick();
}

void MailboxWorker::resyncFolder(qint64 folder)
{
    if (!m_store || !m_store->folder(folder)) {
        return;
    }
    // Forget the delta link: the next run starts a full chain with mark and sweep
    SyncState state = m_store->syncState(folder);
    if (state.initialState == MailFolderSync::Incremental) {
        // FullChain with an empty link: MailFolderSync starts a fresh chain (mark and sweep)
        state.initialState = MailFolderSync::FullChain;
        state.recentDeltaLink.clear();
        state.deltaLink.clear();
        m_store->setSyncState(folder, state);
    }
    const auto it = m_entries.find(QStringLiteral("folder:%1").arg(folder));
    if (it != m_entries.end()) {
        it->due = 0;
    }
    tick();
}

void MailboxWorker::setFolderOpen(qint64 folder, bool open)
{
    if (open) {
        m_openFolders.insert(folder);
    } else {
        m_openFolders.remove(folder);
    }
    const auto it = m_entries.find(QStringLiteral("folder:%1").arg(folder));
    if (it != m_entries.end()) {
        it->pollClass = classFor(it.key());
        if (open) {
            it->due = 0;
            tick();
        }
    }
}

void MailboxWorker::fetchBody(qint64 message)
{
    if (m_bodies) {
        m_bodies->fetchNow(message);
        tick();
    }
}

void MailboxWorker::fetchAttachment(qint64 attachment)
{
    if (m_bodies) {
        m_bodies->fetchAttachmentNow(attachment);
    }
}

qint64 MailboxWorker::folderOf(qint64 message) const
{
    const std::optional<MessageRow> row = m_store ? m_store->message(message) : std::nullopt;
    return row ? row->folder : 0;
}

void MailboxWorker::changedLocally(const QList<qint64> &folders)
{
    // The cache changed at once: the app shows it now, not after the folder's next sync
    for (qint64 folder : folders) {
        if (folder != 0) {
            Q_EMIT collectionChanged(m_config.account, m_config.mailbox, QStringLiteral("folder:%1").arg(folder), 1);
        }
    }
}

void MailboxWorker::markRead(qint64 message, bool read, bool receipt)
{
    if (m_ops && m_ops->markRead(message, read, receipt)) {
        m_statusDirty = true;
        changedLocally({folderOf(message)});
        tick();
    }
}

void MailboxWorker::setCategories(qint64 message, const QStringList &categories)
{
    if (m_ops && m_ops->setCategories(message, categories)) {
        changedLocally({folderOf(message)});
        tick();
    }
}

void MailboxWorker::setFlag(qint64 message, int flag, qint64 start, qint64 due)
{
    if (m_ops && m_ops->setFlag(message, flag, start, due)) {
        changedLocally({folderOf(message)});
        tick();
    }
}

void MailboxWorker::setFocused(qint64 message, bool focused)
{
    if (m_ops && m_ops->setFocused(message, focused)) {
        changedLocally({folderOf(message)});
        tick();
    }
}

QString MailboxWorker::betaUrl() const
{
    // Graph's beta endpoint for actions v1.0 does not have (markAsJunk, reportMessage, ...)
    QString beta = m_client->baseUrl();
    beta.replace(QLatin1String("/v1.0"), QLatin1String("/beta"));
    return beta;
}

QStringList MailboxWorker::ignoredConversations() const
{
    QStringList ids;
    const QJsonArray list = QJsonDocument::fromJson(m_store->meta(QStringLiteral("ignoredConversations")).toUtf8()).array();
    for (const QJsonValue &v : list) {
        ids.append(v.toString());
    }
    return ids;
}

int MailboxWorker::sweepIgnored()
{
    const QStringList ignored = m_store ? ignoredConversations() : QStringList();
    if (ignored.isEmpty() || !m_ops) {
        return 0;
    }
    // Without a known Deleted Items a remove would delete for good: no sweep until the folder
    // tree is mapped (a fresh cache, a mailbox without one)
    if (!m_store->folderByWellKnown(QStringLiteral("deleteditems"))) {
        return 0;
    }
    // Sent mail, drafts and what is already deleted stay where they are, as in Outlook, also in
    // subfolders of those folders
    QSet<QString> keepGraphIds;
    for (const char *wellKnown : {"deleteditems", "sentitems", "drafts", "outbox"}) {
        if (const std::optional<FolderRow> f = m_store->folderByWellKnown(QLatin1String(wellKnown))) {
            keepGraphIds.insert(f->graphId);
        }
    }
    const QList<FolderRow> folders = m_store->folders();
    for (bool grew = true; grew;) {
        grew = false;
        for (const FolderRow &f : folders) {
            if (!keepGraphIds.contains(f.graphId) && keepGraphIds.contains(f.parentGraphId)) {
                keepGraphIds.insert(f.graphId);
                grew = true;
            }
        }
    }
    QSet<qint64> keep;
    for (const FolderRow &f : folders) {
        if (keepGraphIds.contains(f.graphId)) {
            keep.insert(f.id);
        }
    }
    int moved = 0;
    bool skipped = false;
    // One query for all ignored conversations: this runs after every sync with changes
    for (const Store::ConversationMessage &m : m_store->conversationMessages(ignored, keep)) {
        if (m.pending) {
            // Being read or changed right now (the message the user ignored it from): next round
            skipped = true;
        } else if (m_ops->remove(m.id)) {
            changedLocally({m.folder});
            ++moved;
        }
    }
    if (skipped && !m_sweepAgain) {
        m_sweepAgain = true;
        QTimer::singleShot(15 * 1000, this, [this] {
            m_sweepAgain = false;
            if (!m_stopped) {
                sweepIgnored();
                tick();
            }
        });
    }
    return moved;
}

void MailboxWorker::move(qint64 message, qint64 folder)
{
    const qint64 from = folderOf(message);
    if (m_ops && m_ops->move(message, folder)) {
        changedLocally({from, folder});
        tick();
    }
}

void MailboxWorker::remove(qint64 message)
{
    const qint64 from = folderOf(message);
    if (m_ops && m_ops->remove(message)) {
        changedLocally({from});
        tick();
    }
}


void MailboxWorker::saveOutgoing(const QString &json)
{
    if (!m_store) {
        return;
    }
    Outgoing incoming = Outgoing::fromJson(QJsonDocument::fromJson(json.toUtf8()).object());
    if (incoming.uuid.isEmpty()) {
        return;
    }
    // Files the composer attached: into the blob store, so the message no longer depends on them
    for (OutgoingAttachment &a : incoming.attachments) {
        if (a.path.isEmpty()) {
            continue;
        }
        QFile file(a.path);
        QString error;
        if (!file.open(QIODevice::ReadOnly)) {
            qCWarning(GRAPHMAIL).noquote() << "Cannot read the attachment" << a.path << file.errorString();
            continue;
        }
        const QByteArray data = file.readAll();
        a.blob = m_blobs->put(data, &error);
        a.size = data.size();
        if (QFileInfo(a.path).absolutePath().startsWith(composeDirectory())) {
            file.remove();
        }
        a.path.clear();
    }
    const std::optional<Outgoing> stored = m_store->outgoing(incoming.uuid);
    Outgoing record = stored ? Outgoing::merged(*stored, incoming) : incoming;
    // Without content and not on the draft (a file that could not be read): nothing to send
    record.attachments.removeIf([](const OutgoingAttachment &a) {
        return a.blob.isEmpty() && a.graphId.isEmpty();
    });
    if (stored) {
        // Content no longer referenced (attachments removed before they were uploaded)
        for (const OutgoingAttachment &old : stored->attachments) {
            const bool used = std::any_of(record.attachments.cbegin(), record.attachments.cend(), [&old](const OutgoingAttachment &a) {
                return a.blob == old.blob;
            });
            if (!used && !old.blob.isEmpty()) {
                m_blobs->remove(old.blob);
            }
        }
    }
    record.state = Outgoing::State::Pending;
    record.attempts = 0;
    record.nextAttempt = 0;
    record.lastError.clear();
    record.updated = now();
    m_store->saveOutgoing(record);
    m_statusDirty = true;
    // Sending does not wait for an offline pause to run out when the system says it is online
    if (record.send && m_pausedUntil != 0 && GraphCore::Connectivity::systemReportsOnline()) {
        m_pausedUntil = 0;
    }
    tick();
}

void MailboxWorker::discardOutgoing(const QString &uuid)
{
    if (!m_store) {
        return;
    }
    std::optional<Outgoing> stored = m_store->outgoing(uuid);
    if (!stored) {
        return;
    }
    stored->state = Outgoing::State::Discard;
    stored->send = false;
    stored->nextAttempt = 0;
    m_store->saveOutgoing(*stored);
    tick();
}

void MailboxWorker::loadMessage(quint64 request, qint64 message)
{
    const std::optional<MessageRow> row = m_store ? m_store->message(message) : std::nullopt;
    if (!row || !m_client) {
        Q_EMIT messageLoaded(request, {});
        return;
    }
    const QString path = m_ctx.root + QStringLiteral("/messages/") + row->graphId
        + QStringLiteral("?$select=subject,body,toRecipients,ccRecipients,bccRecipients,from,importance,internetMessageId,isDraft&$expand=attachments($select=id,name,contentType,size,isInline)");
    GraphCore::GraphRequest *r = m_client->start({"GET", path, {}, {GraphCore::Prefer::header({GraphCore::Prefer::immutableId()})}, true, nullptr});
    connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request] {
        const GraphCore::GraphResponse response = r->response();
        r->deleteLater();
        Q_EMIT messageLoaded(request, response.ok() ? QString::fromUtf8(response.body) : QString());
    });
}

void MailboxWorker::fetchPeople()
{
    // The suite's people search (GraphCore::People): everyone the user works with, for completion
    const GraphCore::People::Request people = GraphCore::People::relevant({}, 500);
    GraphCore::GraphRequest *r = m_client->start({"GET", people.path, {}, people.headers, true, nullptr});
    const QString file = peopleFile(m_config.account);
    connect(r, &GraphCore::GraphRequest::finished, this, [r, file] {
        const GraphCore::GraphResponse response = r->response();
        r->deleteLater();
        if (!response.ok()) {
            // Not important enough to retry soon (People.Read may also be missing)
            qCInfo(GRAPHMAIL).noquote() << "People for address completion not fetched:" << response.describe();
            return;
        }
        QJsonArray people;
        const QList<GraphCore::People::Person> found = GraphCore::People::parseRelevant(response.json());
        for (const GraphCore::People::Person &person : found) {
            for (const QString &address : person.emails) {
                people.append(QJsonObject{{QStringLiteral("name"), person.name}, {QStringLiteral("address"), address}});
            }
        }
        QSaveFile out(file);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(QJsonDocument(people).toJson(QJsonDocument::Compact));
            out.commit();
        }
    });
}


void MailboxWorker::calendarChanged(bool syncSoon)
{
    Q_EMIT collectionChanged(m_config.account, m_config.mailbox, QStringLiteral("calendar"), 1);
    auto it = m_entries.find(QStringLiteral("calendar"));
    if (syncSoon && it != m_entries.end()) {
        // Exchange may have made occurrences, a series or a sent invitation of it
        it->due = std::min(it->due, now() + 3000);
    }
}

void MailboxWorker::createEvent(qint64 calendar, const QString &json)
{
    if (m_events && m_events->create(calendar, QJsonDocument::fromJson(json.toUtf8()).object())) {
        calendarChanged(false);
        tick();
    }
}

void MailboxWorker::updateEvent(qint64 event, const QString &json)
{
    if (m_events && m_events->update(event, QJsonDocument::fromJson(json.toUtf8()).object())) {
        calendarChanged(false);
        tick();
    }
}

void MailboxWorker::deleteEvent(qint64 event, bool cancel, const QString &comment)
{
    if (m_events && m_events->remove(event, cancel, comment)) {
        calendarChanged(false);
        tick();
    }
}

void MailboxWorker::respondEvent(qint64 event, const QString &action, const QString &comment, bool sendResponse)
{
    if (m_events && m_events->respond(event, action, comment, sendResponse)) {
        calendarChanged(false);
        tick();
    }
}

void MailboxWorker::respondEventByGraphId(const QString &graphId, const QString &action, const QString &comment, bool sendResponse, qint64 proposedStart, qint64 proposedEnd)
{
    if (m_events && m_events->respondByGraphId(graphId, {}, action, comment, sendResponse, proposedStart, proposedEnd)) {
        calendarChanged(false);
        tick();
    }
}

void MailboxWorker::reminderAction(qint64 event, bool dismiss, qint64 until)
{
    const std::optional<EventRow> e = m_store ? m_store->event(event) : std::nullopt;
    if (!e || !m_events) {
        return;
    }
    const QString key = e->graphId + QLatin1Char(':') + QString::number(e->start);
    if (dismiss) {
        m_store->setReminderState(key, e->start, 2);
        m_events->dismissReminder(event);
    } else {
        m_store->setReminderState(key, e->start, 1, until);
        m_events->snoozeReminder(event, until);
    }
    publishReminders();
    tick();
}

QList<qint64> MailboxWorker::ownCalendars() const
{
    // The user's own calendars: those owned by the default calendar's owner
    QString own;
    QList<qint64> calendars;
    const QList<CalendarRow> all = m_store->calendars();
    for (const CalendarRow &c : all) {
        if (c.isDefault) {
            own = c.owner;
        }
    }
    for (const CalendarRow &c : all) {
        if (c.isDefault || (!own.isEmpty() && c.owner.compare(own, Qt::CaseInsensitive) == 0 && c.canEdit)) {
            calendars.append(c.id);
        }
    }
    return calendars;
}

void MailboxWorker::publishReminders()
{
    if (!m_store || m_config.mailbox != QLatin1String("me")) {
        return;
    }
    // What checkReminders() and checkTaskReminders() showed and nobody snoozed or dismissed since
    QJsonArray active;
    const qint64 t = now();
    const QList<qint64> calendars = ownCalendars();
    if (!calendars.isEmpty()) {
        const QList<EventRow> events = m_store->eventsBetween(calendars, t, t + 15LL * 24 * 3600 * 1000);
        for (const EventRow &e : events) {
            const QJsonObject json = QJsonDocument::fromJson(e.json.toUtf8()).object();
            if (!json.value(QLatin1String("isReminderOn")).toBool() || json.value(QLatin1String("isCancelled")).toBool() || e.response == 4 || e.end <= t) {
                continue;
            }
            qint64 snoozeUntil = 0;
            const int state = m_store->reminderState(e.graphId + QLatin1Char(':') + QString::number(e.start), &snoozeUntil);
            if (state != 1 || (snoozeUntil != 0 && snoozeUntil > t)) {
                continue;
            }
            active.append(QJsonObject{{QStringLiteral("kind"), QStringLiteral("event")},
                                      {QStringLiteral("id"), e.id},
                                      {QStringLiteral("title"), e.subject},
                                      {QStringLiteral("start"), e.start},
                                      {QStringLiteral("end"), e.end},
                                      {QStringLiteral("location"), e.location}});
        }
    }
    const QList<StoredItem> tasks = m_store->dueTaskReminders(t);
    for (const StoredItem &task : tasks) {
        const QJsonObject json = QJsonDocument::fromJson(task.json.toUtf8()).object();
        if (!json.value(QLatin1String("isReminderOn")).toBool()) {
            continue;
        }
        const QString key = QStringLiteral("task:") + task.graphId + QLatin1Char(':') + json.value(QLatin1String("reminderDateTime")).toObject().value(QLatin1String("dateTime")).toString();
        if (m_store->reminderState(key) != 1) {
            continue;
        }
        active.append(QJsonObject{{QStringLiteral("kind"), QStringLiteral("task")},
                                  {QStringLiteral("id"), task.id},
                                  {QStringLiteral("title"), json.value(QLatin1String("title")).toString()},
                                  {QStringLiteral("due"), localMidnight(Mapper::dueDate(json.value(QLatin1String("dueDateTime")).toObject()))}});
    }
    const QString text = QString::fromUtf8(QJsonDocument(active).toJson(QJsonDocument::Compact));
    if (text != m_lastReminders) {
        m_lastReminders = text;
        Q_EMIT remindersChanged(m_config.account, m_config.mailbox, text);
    }
}

void MailboxWorker::checkReminders()
{
    if (!m_store) {
        return;
    }
    const QList<qint64> calendars = ownCalendars();
    if (calendars.isEmpty()) {
        return;
    }
    const qint64 t = now();
    // Reminders can be set up to two weeks ahead
    const QList<EventRow> events = m_store->eventsBetween(calendars, t, t + 15LL * 24 * 3600 * 1000);
    for (const EventRow &e : events) {
        const QJsonObject json = QJsonDocument::fromJson(e.json.toUtf8()).object();
        if (!json.value(QLatin1String("isReminderOn")).toBool() || json.value(QLatin1String("isCancelled")).toBool() || e.response == 4) {
            continue;
        }
        const qint64 due = e.start - qint64(json.value(QLatin1String("reminderMinutesBeforeStart")).toInt()) * 60 * 1000;
        if (due > t || e.end <= t) {
            continue;
        }
        const QString key = e.graphId + QLatin1Char(':') + QString::number(e.start);
        qint64 snoozeUntil = 0;
        const int state = m_store->reminderState(key, &snoozeUntil);
        // Dismissed, or shown and not snoozed, or snoozed till later
        if (state == 2 || (state == 1 && (snoozeUntil == 0 || snoozeUntil > t))) {
            continue;
        }
        m_store->setReminderState(key, e.start, 1);
        Q_EMIT reminderDue(m_config.account, m_config.mailbox, e.id, e.subject, e.start, e.location);
    }
}

void MailboxWorker::query(quint64 request, const QString &kind, const QString &json)
{
    const QJsonObject args = QJsonDocument::fromJson(json.toUtf8()).object();
    GraphCore::GraphRequestSpec spec;
    spec.headers = {GraphCore::Prefer::header({GraphCore::Prefer::immutableId(), QByteArrayLiteral("outlook.timezone=\"UTC\"")})};
    auto utc = [](qint64 ms) {
        return QJsonObject{{QStringLiteral("dateTime"), QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))},
                           {QStringLiteral("timeZone"), QStringLiteral("UTC")}};
    };
    if (kind == QLatin1String("schedule")) {
        spec.verb = "POST";
        spec.pathOrUrl = m_ctx.root + QStringLiteral("/calendar/getSchedule");
        spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
        spec.body = QJsonDocument(QJsonObject{{QStringLiteral("schedules"), args.value(QLatin1String("addresses"))},
                                              {QStringLiteral("startTime"), utc(args.value(QLatin1String("start")).toInteger())},
                                              {QStringLiteral("endTime"), utc(args.value(QLatin1String("end")).toInteger())},
                                              {QStringLiteral("availabilityViewInterval"), args.value(QLatin1String("interval")).toInt(30)}})
                        .toJson(QJsonDocument::Compact);
    } else if (kind == QLatin1String("masterCategories")) {
        // The mailbox's categories (names and colours), for the search filters
        spec.pathOrUrl = m_ctx.root + QStringLiteral("/outlook/masterCategories");
    } else if (kind == QLatin1String("categoryCreate") || kind == QLatin1String("categoryUpdate") || kind == QLatin1String("categoryDelete")) {
        // Managing the master categories (docs/PHASE9.md): new, colour, delete. Graph cannot
        // rename one (displayName is fixed once made). Answered as {"ok", "problem", "detail"}.
        const QString base = m_ctx.root + QStringLiteral("/outlook/masterCategories");
        if (kind == QLatin1String("categoryCreate")) {
            spec.verb = "POST";
            spec.pathOrUrl = base;
            spec.body = QJsonDocument(QJsonObject{{QStringLiteral("displayName"), args.value(QLatin1String("name"))}, {QStringLiteral("color"), args.value(QLatin1String("color"))}})
                            .toJson(QJsonDocument::Compact);
        } else if (kind == QLatin1String("categoryUpdate")) {
            spec.verb = "PATCH";
            spec.pathOrUrl = base + QLatin1Char('/') + args.value(QLatin1String("id")).toString();
            spec.body = QJsonDocument(QJsonObject{{QStringLiteral("color"), args.value(QLatin1String("color"))}}).toJson(QJsonDocument::Compact);
        } else {
            spec.verb = "DELETE";
            spec.pathOrUrl = base + QLatin1Char('/') + args.value(QLatin1String("id")).toString();
        }
        if (!spec.body.isEmpty()) {
            spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
        }
        spec.maxAttempts = 2;
        GraphCore::GraphRequest *r = m_client->start(spec);
        connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            const QJsonObject answer{{QStringLiteral("ok"), response.ok()},
                                     {QStringLiteral("problem"), accessProblem(response)},
                                     {QStringLiteral("detail"), response.ok() ? QString() : response.userMessage()}};
            Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
        });
        return;
    } else if (kind.startsWith(QLatin1String("messageRule"))) {
        // Inbox rules (docs/PHASE9.md): list, create, change, delete. Own mailbox only (another
        // mailbox's rules: 403). Answered as {"ok", "problem", "detail", "body"}
        const QString base = m_ctx.root + QStringLiteral("/mailFolders/inbox/messageRules");
        const QString id = args.value(QLatin1String("id")).toString();
        if (kind == QLatin1String("messageRules")) {
            spec.pathOrUrl = base;
        } else if (kind == QLatin1String("messageRuleCreate")) {
            spec.verb = "POST";
            spec.pathOrUrl = base;
        } else if (kind == QLatin1String("messageRuleUpdate")) {
            spec.verb = "PATCH";
            spec.pathOrUrl = base + QLatin1Char('/') + id;
        } else if (kind == QLatin1String("messageRuleDelete")) {
            spec.verb = "DELETE";
            spec.pathOrUrl = base + QLatin1Char('/') + id;
        } else {
            Q_EMIT queryAnswered(request, {});
            return;
        }
        if (spec.verb == "POST" || spec.verb == "PATCH") {
            spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
            spec.body = QJsonDocument(args.value(QLatin1String("rule")).toObject()).toJson(QJsonDocument::Compact);
        }
        spec.maxAttempts = 2;
        GraphCore::GraphRequest *r = m_client->start(spec);
        connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            const QJsonObject answer{{QStringLiteral("ok"), response.ok()},
                                     {QStringLiteral("problem"), accessProblem(response)},
                                     {QStringLiteral("detail"), response.ok() ? QString() : response.userMessage()},
                                     {QStringLiteral("body"), response.json()}};
            Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
        });
        return;
    } else if (kind == QLatin1String("mailboxSettings") || kind == QLatin1String("mailboxSettingsUpdate")) {
        // Automatic replies, working hours, time zone, formats (docs/PHASE9.md). Own mailbox only:
        // Graph refuses another mailbox's settings (403). Answered as {"ok", "problem", "detail",
        // "body"}, so a dialog can say why it could not read or save them
        spec.pathOrUrl = m_ctx.root + QStringLiteral("/mailboxSettings");
        if (kind == QLatin1String("mailboxSettingsUpdate")) {
            spec.verb = "PATCH";
            spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
            spec.body = QJsonDocument(args.value(QLatin1String("settings")).toObject()).toJson(QJsonDocument::Compact);
        }
        spec.maxAttempts = 2;
        GraphCore::GraphRequest *r = m_client->start(spec);
        connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            const QJsonObject answer{{QStringLiteral("ok"), response.ok()},
                                     {QStringLiteral("problem"), accessProblem(response)},
                                     {QStringLiteral("detail"), response.ok() ? QString() : response.userMessage()},
                                     {QStringLiteral("body"), response.json()}};
            Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
        });
        return;
    } else if (kind == QLatin1String("ignoreConversation")) {
        // Ignore Conversation on or off for the message's conversation; answered {"ok", "moved"}
        const std::optional<MessageRow> m = m_store ? m_store->message(args.value(QLatin1String("message")).toInteger()) : std::nullopt;
        QStringList ignored = m_store ? ignoredConversations() : QStringList();
        int moved = 0;
        if (m && !m->conversation.isEmpty()) {
            ignored.removeAll(m->conversation);
            if (args.value(QLatin1String("on")).toBool()) {
                ignored.append(m->conversation);
            }
            m_store->setMeta(QStringLiteral("ignoredConversations"), QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(ignored)).toJson(QJsonDocument::Compact)));
            moved = sweepIgnored();
            tick();
        }
        Q_EMIT queryAnswered(request,
                             QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("ok"), bool(m && !m->conversation.isEmpty())}, {QStringLiteral("moved"), moved}})
                                                   .toJson(QJsonDocument::Compact)));
        return;
    } else if (kind == QLatin1String("ignoredConversations")) {
        Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(m_store ? ignoredConversations() : QStringList())).toJson(QJsonDocument::Compact)));
        return;
    } else if (kind == QLatin1String("focusOverride") || kind == QLatin1String("junk") || kind == QLatin1String("phish") || kind == QLatin1String("copy")) {
        // Always Move to Focused/Other (an override for the sender), and Report Junk / Not Junk
        // (beta markAsJunk/markAsNotJunk: Exchange moves the message and adds the sender to the
        // blocked or safe senders). Answered {"ok", "problem", "detail"}
        spec.verb = "POST";
        spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
        if (kind == QLatin1String("focusOverride")) {
            spec.pathOrUrl = m_ctx.root + QStringLiteral("/inferenceClassification/overrides");
            spec.body = QJsonDocument(QJsonObject{{QStringLiteral("classifyAs"), args.value(QLatin1String("focused")).toBool() ? QStringLiteral("focused") : QStringLiteral("other")},
                                                  {QStringLiteral("senderEmailAddress"),
                                                   QJsonObject{{QStringLiteral("name"), args.value(QLatin1String("name"))}, {QStringLiteral("address"), args.value(QLatin1String("address"))}}}})
                            .toJson(QJsonDocument::Compact);
        } else {
            const std::optional<MessageRow> m = m_store ? m_store->message(args.value(QLatin1String("message")).toInteger()) : std::nullopt;
            if (!m) {
                Q_EMIT queryAnswered(request, {});
                return;
            }
            if (kind == QLatin1String("junk")) {
                const bool junk = args.value(QLatin1String("junk")).toBool();
                spec.pathOrUrl = betaUrl() + m_ctx.root + QStringLiteral("/messages/") + m->graphId + (junk ? QStringLiteral("/markAsJunk") : QStringLiteral("/markAsNotJunk"));
                spec.body = QJsonDocument(junk ? QJsonObject{{QStringLiteral("moveToJunk"), true}} : QJsonObject{{QStringLiteral("moveToInbox"), true}}).toJson(QJsonDocument::Compact);
            } else if (kind == QLatin1String("phish")) {
                // Report Phishing (beta reportMessage, as Outlook on the web): Microsoft gets the
                // report and Exchange moves the message to Deleted Items
                spec.pathOrUrl = betaUrl() + m_ctx.root + QStringLiteral("/messages/") + m->graphId + QStringLiteral("/reportMessage");
                spec.body = QJsonDocument(QJsonObject{{QStringLiteral("ReportAction"), QStringLiteral("phish")}, {QStringLiteral("IsMessageMoveRequested"), true}})
                                .toJson(QJsonDocument::Compact);
            } else if (kind == QLatin1String("copy")) {
                // Copy to a folder (v1.0): the copy arrives with that folder's next sync
                const std::optional<FolderRow> target = m_store->folder(args.value(QLatin1String("folder")).toInteger());
                if (!target) {
                    Q_EMIT queryAnswered(request, {});
                    return;
                }
                spec.pathOrUrl = m_ctx.root + QStringLiteral("/messages/") + m->graphId + QStringLiteral("/copy");
                spec.body = QJsonDocument(QJsonObject{{QStringLiteral("destinationId"), target->graphId}}).toJson(QJsonDocument::Compact);
            }
        }
        spec.maxAttempts = 2;
        GraphCore::GraphRequest *r = m_client->start(spec);
        connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request, kind] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            const QJsonObject answer{{QStringLiteral("ok"), response.ok()},
                                     {QStringLiteral("problem"), accessProblem(response)},
                                     {QStringLiteral("detail"), response.ok() ? QString() : response.userMessage()}};
            Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
            // The message moved (or was copied) in Exchange: the folders show it at once
            if (response.ok() && kind != QLatin1String("focusOverride")) {
                syncNow();
            }
        });
        return;
    } else if (kind == QLatin1String("undoSend")) {
        // Undo send (docs/PHASE9.md): a message still held is a draft again; answered "true"
        // when it was in time. One that went out cannot be taken back
        std::optional<Outgoing> stored = m_store ? m_store->outgoing(args.value(QLatin1String("uuid")).toString()) : std::nullopt;
        const bool inTime = stored && stored->send && stored->state == Outgoing::State::Pending && stored->sendAfter > now();
        if (inTime) {
            stored->send = false;
            stored->sendAfter = 0;
            stored->nextAttempt = 0;
            m_store->saveOutgoing(*stored);
            tick();
        }
        Q_EMIT queryAnswered(request, inTime ? QStringLiteral("true") : QStringLiteral("false"));
        return;
    } else if (kind == QLatin1String("mailTips")) {
        // MailTips for the composer: automatic replies, mailbox full, outside the organisation,
        // group sizes, restrictions, the largest message a recipient takes
        spec.verb = "POST";
        spec.pathOrUrl = m_ctx.root + QStringLiteral("/getMailTips");
        spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
        spec.body = QJsonDocument(QJsonObject{{QStringLiteral("EmailAddresses"), args.value(QLatin1String("addresses"))},
                                              {QStringLiteral("MailTipsOptions"),
                                               QStringLiteral("automaticReplies, mailboxFullStatus, maxMessageSize, externalMemberCount, totalMemberCount, "
                                                              "recipientScope, deliveryRestriction, moderationStatus")}})
                        .toJson(QJsonDocument::Compact);
    } else if (kind == QLatin1String("messageMime")) {
        // Export (docs/PHASE9B.md): the message as Exchange keeps it (MIME), written into "file";
        // answered {"ok", "problem", "detail", "bytes"}
        const std::optional<MessageRow> m = m_store ? m_store->message(args.value(QLatin1String("message")).toInteger()) : std::nullopt;
        auto file = std::make_shared<QFile>(args.value(QLatin1String("file")).toString());
        if (!m || !file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            Q_EMIT queryAnswered(request, QStringLiteral("{\"ok\":false,\"problem\":\"error\"}"));
            return;
        }
        spec.pathOrUrl = m_ctx.root + QStringLiteral("/messages/") + m->graphId + QStringLiteral("/$value");
        spec.sink = file.get();
        spec.maxAttempts = 2;
        GraphCore::GraphRequest *r = m_client->start(spec);
        connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request, file] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            const qint64 bytes = file->size();
            file->close();
            if (!response.ok()) {
                file->remove();
            }
            const QJsonObject answer{{QStringLiteral("ok"), response.ok()},
                                     {QStringLiteral("problem"), accessProblem(response)},
                                     {QStringLiteral("detail"), response.ok() ? QString() : response.userMessage()},
                                     {QStringLiteral("bytes"), bytes}};
            Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
        });
        return;
    } else if (kind == QLatin1String("importMessage")) {
        // PST and .msg import (docs/PHASE9B.md): one message into the folder (graph id), from the
        // request file the app wrote; answered {"ok", "problem", "detail", "id"}
        auto *job = new MessageImport(m_client, m_ctx.root, args.value(QLatin1String("folder")).toString(), args.value(QLatin1String("file")).toString(), this);
        job->setSkipDuplicates(args.value(QLatin1String("skipDuplicates")).toBool());
        connect(job, &MessageImport::finished, this, [this, request](const QJsonObject &answer) {
            Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
        });
        job->start();
        return;
    } else if (kind == QLatin1String("folderCreate")) {
        // A mail folder under a parent (graph id or well-known name, msgfolderroot for the top); for an
        // import an existing one of that name is used. Answered {"ok", "problem", "detail", "id"}
        const QString parent = m_ctx.root + QStringLiteral("/mailFolders/") + args.value(QLatin1String("parent")).toString() + QStringLiteral("/childFolders");
        const QString name = args.value(QLatin1String("name")).toString();
        spec.verb = "POST";
        spec.pathOrUrl = parent;
        spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
        spec.body = QJsonDocument(QJsonObject{{QStringLiteral("displayName"), name}}).toJson(QJsonDocument::Compact);
        spec.maxAttempts = 2;
        GraphCore::GraphRequest *r = m_client->start(spec);
        // "unique": New Folder in the folder pane, where an existing name is an error, not a match
        const bool unique = args.value(QLatin1String("unique")).toBool();
        connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request, parent, name, unique] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            const auto reply = [this, request](const GraphCore::GraphResponse &res, const QString &id) {
                const QJsonObject answer{{QStringLiteral("ok"), !id.isEmpty()},
                                         {QStringLiteral("problem"), id.isEmpty() ? accessProblem(res) : QString()},
                                         {QStringLiteral("detail"), id.isEmpty() ? res.userMessage() : QString()},
                                         {QStringLiteral("id"), id}};
                Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
            };
            if (response.status != 409 || unique) {
                reply(response, response.ok() ? response.json().value(QLatin1String("id")).toString() : QString());
                if (response.ok() && unique) {
                    syncNow();
                }
                return;
            }
            // ErrorFolderExists: the folder of that name
            GraphCore::GraphRequestSpec find;
            QString quoted = name;
            quoted.replace(QLatin1Char('\''), QStringLiteral("''"));
            find.pathOrUrl = parent + QStringLiteral("?$select=id&$filter=")
                + QString::fromLatin1(QUrl::toPercentEncoding(QStringLiteral("displayName eq '%1'").arg(quoted)));
            find.headers = {GraphCore::Prefer::header({GraphCore::Prefer::immutableId()})};
            GraphCore::GraphRequest *f = m_client->start(find);
            connect(f, &GraphCore::GraphRequest::finished, this, [f, reply] {
                const GraphCore::GraphResponse found = f->response();
                f->deleteLater();
                const QJsonArray value = found.json().value(QLatin1String("value")).toArray();
                reply(found, found.ok() && !value.isEmpty() ? value.first().toObject().value(QLatin1String("id")).toString() : QString());
            });
        });
        return;
    } else if (kind == QLatin1String("folderChange")) {
        // Rename, move, delete or mark all read in a mail folder (Outlook's folder pane). "action":
        // markAllRead, rename (name),
        // move (target: a folder id, 0 for the top of the mailbox) or delete: moved to Deleted Items,
        // or gone for good when it already is inside Deleted Items. Answered {"ok", "problem", "detail"}
        const std::optional<FolderRow> folder = m_store ? m_store->folder(args.value(QLatin1String("folder")).toInteger()) : std::nullopt;
        const QString action = args.value(QLatin1String("action")).toString();
        // Outlook's own folders (Inbox, Sent Items, ...) cannot be renamed, moved or deleted
        if (!folder || (action != QLatin1String("markAllRead") && !folder->wellKnown.isEmpty())) {
            Q_EMIT queryAnswered(request, {});
            return;
        }
        const QString path = m_ctx.root + QStringLiteral("/mailFolders/") + folder->graphId;
        spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
        if (action == QLatin1String("markAllRead")) {
            // One request for the whole folder (beta); no read receipts go out, as in Outlook
            spec.verb = "POST";
            spec.pathOrUrl = betaUrl() + path + QStringLiteral("/updateAllMessagesReadState");
            spec.body = QJsonDocument(QJsonObject{{QStringLiteral("isRead"), true}, {QStringLiteral("suppressReadReceipts"), true}}).toJson(QJsonDocument::Compact);
        } else if (action == QLatin1String("rename")) {
            spec.verb = "PATCH";
            spec.pathOrUrl = path;
            spec.body = QJsonDocument(QJsonObject{{QStringLiteral("displayName"), args.value(QLatin1String("name")).toString()}}).toJson(QJsonDocument::Compact);
        } else if (action == QLatin1String("move")) {
            const qint64 targetId = args.value(QLatin1String("target")).toInteger();
            const std::optional<FolderRow> target = targetId != 0 ? m_store->folder(targetId) : std::nullopt;
            if (targetId != 0 && !target) {
                Q_EMIT queryAnswered(request, {});
                return;
            }
            spec.verb = "POST";
            spec.pathOrUrl = path + QStringLiteral("/move");
            spec.body = QJsonDocument(QJsonObject{{QStringLiteral("destinationId"), target ? target->graphId : QStringLiteral("msgfolderroot")}}).toJson(QJsonDocument::Compact);
        } else if (action == QLatin1String("delete")) {
            // Inside Deleted Items (at any depth): permanently, as Outlook does there
            bool inDeleted = false;
            QString parent = folder->parentGraphId;
            for (int depth = 0; depth < 64 && !parent.isEmpty() && !inDeleted; ++depth) {
                const std::optional<FolderRow> up = m_store->folderByGraphId(parent);
                if (!up) {
                    break;
                }
                inDeleted = up->wellKnown == QLatin1String("deleteditems");
                parent = up->parentGraphId;
            }
            // Graph's DELETE skips Deleted Items (the folder lands in Recoverable Items at once,
            // probed 2026-11-16); Outlook moves it to Deleted Items, so a move does
            spec.verb = "POST";
            spec.pathOrUrl = path + (inDeleted ? QStringLiteral("/permanentDelete") : QStringLiteral("/move"));
            spec.body = inDeleted ? QByteArrayLiteral("{}")
                                  : QJsonDocument(QJsonObject{{QStringLiteral("destinationId"), QStringLiteral("deleteditems")}}).toJson(QJsonDocument::Compact);
        } else {
            Q_EMIT queryAnswered(request, {});
            return;
        }
        spec.maxAttempts = 2;
        GraphCore::GraphRequest *r = m_client->start(spec);
        connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            const QJsonObject answer{{QStringLiteral("ok"), response.ok()},
                                     {QStringLiteral("problem"), response.ok() ? QString() : accessProblem(response)},
                                     {QStringLiteral("detail"), response.ok() ? QString() : response.userMessage()}};
            Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
            // The folder tree's delta brings the change into the cache
            if (response.ok()) {
                syncNow();
            }
        });
        return;
    } else if (kind == QLatin1String("archiveFolderSet")) {
        // The mailbox's archive folder (docs/PHASE9B.md, 9b.3): mailboxSettings.archiveFolder. Graph
        // does not list it as writable; when Exchange takes it the cache's well-known mapping
        // follows at once. Answered {"ok", "problem", "detail"}
        const QString id = args.value(QLatin1String("folder")).toString();
        spec.verb = "PATCH";
        spec.pathOrUrl = m_ctx.root + QStringLiteral("/mailboxSettings");
        spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
        spec.body = QJsonDocument(QJsonObject{{QStringLiteral("archiveFolder"), id}}).toJson(QJsonDocument::Compact);
        spec.maxAttempts = 2;
        GraphCore::GraphRequest *r = m_client->start(spec);
        connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request, id] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            // Exchange may answer OK yet keep its own: only a returned archiveFolder counts
            const bool taken = response.ok() && response.json().value(QLatin1String("archiveFolder")).toString() == id;
            if (taken && m_store) {
                if (const std::optional<FolderRow> old = m_store->folderByWellKnown(QStringLiteral("archive")); old && old->graphId != id) {
                    m_store->setFolderWellKnown(old->graphId, QString());
                }
                m_store->setFolderWellKnown(id, QStringLiteral("archive"));
                Q_EMIT collectionChanged(m_config.account, m_config.mailbox, QStringLiteral("tree"), 1);
            }
            const QJsonObject answer{{QStringLiteral("ok"), taken},
                                     {QStringLiteral("problem"), taken ? QString() : accessProblem(response)},
                                     {QStringLiteral("detail"), taken ? QString() : response.ok() ? QStringLiteral("not taken") : response.userMessage()}};
            Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
        });
        return;
    } else if (kind == QLatin1String("importItem")) {
        // PST import of a contact, event or To Do task (docs/PHASE9B.md) into a container (graph
        // id; empty: the default contacts or calendar). Answered {"ok", "problem", "detail", "id"}
        const QString what = args.value(QLatin1String("kind")).toString();
        const QString container = args.value(QLatin1String("container")).toString();
        if (what == QLatin1String("contact")) {
            spec.pathOrUrl = m_ctx.root + (container.isEmpty() ? QStringLiteral("/contacts") : QStringLiteral("/contactFolders/") + container + QStringLiteral("/contacts"));
        } else if (what == QLatin1String("event")) {
            spec.pathOrUrl = m_ctx.root + (container.isEmpty() ? QStringLiteral("/events") : QStringLiteral("/calendars/") + container + QStringLiteral("/events"));
        } else if (what == QLatin1String("task") && !container.isEmpty()) {
            spec.pathOrUrl = m_ctx.root + QStringLiteral("/todo/lists/") + container + QStringLiteral("/tasks");
        } else {
            Q_EMIT queryAnswered(request, QStringLiteral("{\"ok\":false,\"problem\":\"error\"}"));
            return;
        }
        spec.verb = "POST";
        spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
        spec.body = QJsonDocument(args.value(QLatin1String("item")).toObject()).toJson(QJsonDocument::Compact);
        spec.maxAttempts = 2;
        GraphCore::GraphRequest *r = m_client->start(spec);
        connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            const QJsonObject answer{{QStringLiteral("ok"), response.ok()},
                                     {QStringLiteral("problem"), accessProblem(response)},
                                     {QStringLiteral("detail"), response.ok() ? QString() : response.userMessage()},
                                     {QStringLiteral("id"), response.json().value(QLatin1String("id")).toString()}};
            Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
        });
        return;
    } else if (kind == QLatin1String("containerCreate") || kind == QLatin1String("defaultTaskList")) {
        // A contact folder, calendar or To Do list for an import (an existing one of that name
        // is used), or the mailbox's default To Do list. Answered {"ok", "problem", "detail", "id"}
        const QString what = args.value(QLatin1String("kind")).toString();
        const QString name = args.value(QLatin1String("name")).toString();
        QString collection;
        QString nameField = QStringLiteral("displayName");
        if (kind == QLatin1String("defaultTaskList") || what == QLatin1String("task")) {
            collection = m_ctx.root + QStringLiteral("/todo/lists");
        } else if (what == QLatin1String("contact")) {
            collection = m_ctx.root + QStringLiteral("/contactFolders");
        } else if (what == QLatin1String("event")) {
            collection = m_ctx.root + QStringLiteral("/calendars");
            nameField = QStringLiteral("name");
        } else {
            Q_EMIT queryAnswered(request, QStringLiteral("{\"ok\":false,\"problem\":\"error\"}"));
            return;
        }
        const bool findDefault = kind == QLatin1String("defaultTaskList");
        const auto reply = [this, request](const GraphCore::GraphResponse &res, const QString &id) {
            const QJsonObject answer{{QStringLiteral("ok"), !id.isEmpty()},
                                     {QStringLiteral("problem"), id.isEmpty() ? accessProblem(res) : QString()},
                                     {QStringLiteral("detail"), id.isEmpty() ? (res.ok() ? QStringLiteral("not found") : res.userMessage()) : QString()},
                                     {QStringLiteral("id"), id}};
            Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
        };
        // Looks the collection through for the default list or the one of that name
        const auto find = [this, collection, nameField, name, findDefault, reply] {
            GraphCore::GraphRequestSpec list;
            list.pathOrUrl = collection + QStringLiteral("?$top=200");
            list.headers = {GraphCore::Prefer::header({GraphCore::Prefer::immutableId()})};
            GraphCore::GraphRequest *f = m_client->start(list);
            connect(f, &GraphCore::GraphRequest::finished, this, [f, nameField, name, findDefault, reply] {
                const GraphCore::GraphResponse found = f->response();
                f->deleteLater();
                const QJsonArray value = found.json().value(QLatin1String("value")).toArray();
                for (const QJsonValue &v : value) {
                    const QJsonObject o = v.toObject();
                    const bool match = findDefault ? o.value(QLatin1String("wellknownListName")).toString() == QLatin1String("defaultList")
                                                   : o.value(nameField).toString().compare(name, Qt::CaseInsensitive) == 0;
                    if (match) {
                        reply(found, o.value(QLatin1String("id")).toString());
                        return;
                    }
                }
                reply(found, QString());
            });
        };
        if (findDefault) {
            find();
            return;
        }
        spec.verb = "POST";
        spec.pathOrUrl = collection;
        spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
        spec.body = QJsonDocument(QJsonObject{{nameField, name}}).toJson(QJsonDocument::Compact);
        spec.maxAttempts = 2;
        GraphCore::GraphRequest *r = m_client->start(spec);
        connect(r, &GraphCore::GraphRequest::finished, this, [r, find, reply] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            // ErrorFolderExists and the like: the one that is there
            if (response.status == 409 || (response.status == 400 && response.describe().contains(QLatin1String("Exist"), Qt::CaseInsensitive))) {
                find();
                return;
            }
            reply(response, response.ok() ? response.json().value(QLatin1String("id")).toString() : QString());
        });
        return;
    } else if (kind == QLatin1String("mailboxPhoto")) {
        // The mailbox's own photo (docs/PHASE9.md), as base64; empty when it has none (404)
        spec.pathOrUrl = m_ctx.root + QStringLiteral("/photos/96x96/$value");
        GraphCore::GraphRequest *r = m_client->start(spec);
        connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            Q_EMIT queryAnswered(request, response.ok() ? QString::fromLatin1(response.body.toBase64()) : QString());
        });
        return;
    } else if (kind == QLatin1String("mailboxPhotoSet") || kind == QLatin1String("mailboxPhotoRemove")) {
        // Change Photo / Remove Photo: a JPEG (base64 "image") or none. Answered {"ok", "problem",
        // "detail", "status"}: a missing permission (403) is told apart
        spec.pathOrUrl = m_ctx.root + QStringLiteral("/photo/$value");
        if (kind == QLatin1String("mailboxPhotoSet")) {
            spec.verb = "PUT";
            spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("image/jpeg")});
            spec.body = QByteArray::fromBase64(args.value(QLatin1String("image")).toString().toLatin1());
        } else {
            spec.verb = "DELETE";
        }
        spec.maxAttempts = 2;
        GraphCore::GraphRequest *r = m_client->start(spec);
        connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            const QJsonObject answer{{QStringLiteral("ok"), response.ok()},
                                     {QStringLiteral("problem"), accessProblem(response)},
                                     {QStringLiteral("detail"), response.ok() ? QString() : response.userMessage()},
                                     {QStringLiteral("status"), response.status}};
            Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
        });
        return;
    } else if (kind == QLatin1String("supportedTimeZones")) {
        // Settings > Mailbox: the zones Exchange takes, by their Windows names
        spec.pathOrUrl = m_ctx.root + QStringLiteral("/outlook/supportedTimeZones(TimeZoneStandard=microsoft.graph.timeZoneStandard'Windows')");
    } else if (kind == QLatin1String("supportedLanguages")) {
        spec.pathOrUrl = m_ctx.root + QStringLiteral("/outlook/supportedLanguages");
    } else if (kind == QLatin1String("rooms")) {
        spec.pathOrUrl = QStringLiteral("/places/microsoft.graph.room?$top=200");
    } else if (kind == QLatin1String("directory")) {
        // The organisation's people (read only), as Outlook's People search: the suite's directory
        // search (GraphCore::People) with the fields of a contact card
        const GraphCore::People::Request directory = GraphCore::People::directory(args.value(QLatin1String("text")).toString(), 40,
                                                                                  {QStringLiteral("id"), QStringLiteral("displayName"), QStringLiteral("givenName"), QStringLiteral("surname"),
                                                                                   QStringLiteral("mail"), QStringLiteral("userPrincipalName"), QStringLiteral("jobTitle"), QStringLiteral("department"),
                                                                                   QStringLiteral("officeLocation"), QStringLiteral("companyName"), QStringLiteral("businessPhones"), QStringLiteral("mobilePhone")});
        spec.pathOrUrl = directory.path;
        spec.headers.append(directory.headers);
    } else if (kind == QLatin1String("photo")) {
        // A contact's photo (contact: graph id) or a colleague's (user: id); answered as base64
        const QString contact = args.value(QLatin1String("contact")).toString();
        spec.pathOrUrl = GraphCore::People::photoPath(args.value(QLatin1String("user")).toString(), 96);
        if (!contact.isEmpty()) {
            spec.pathOrUrl = m_ctx.root + QStringLiteral("/contacts/") + contact + QStringLiteral("/photo/$value");
        }
        GraphCore::GraphRequest *r = m_client->start(spec);
        connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            Q_EMIT queryAnswered(request, response.ok() ? QString::fromLatin1(response.body.toBase64()) : QString());
        });
        return;
    } else if (kind == QLatin1String("taskSteps")) {
        const std::optional<StoredItem> task = m_store ? m_store->item(ItemKind::Task, args.value(QLatin1String("task")).toInteger()) : std::nullopt;
        const QList<ParentRow> lists = m_store ? m_store->parents(ItemKind::Task) : QList<ParentRow>();
        QString list;
        for (const ParentRow &l : lists) {
            if (task && l.id == task->parent) {
                list = l.graphId;
            }
        }
        if (!task || list.isEmpty()) {
            Q_EMIT queryAnswered(request, {});
            return;
        }
        spec.pathOrUrl = QStringLiteral("/me/todo/lists/") + list + QStringLiteral("/tasks/") + task->graphId + QStringLiteral("/checklistItems");
    } else if (kind == QLatin1String("meetingEvent")) {
        const std::optional<MessageRow> row = m_store ? m_store->message(args.value(QLatin1String("message")).toInteger()) : std::nullopt;
        if (!row) {
            Q_EMIT queryAnswered(request, {});
            return;
        }
        spec.pathOrUrl = m_ctx.root + QStringLiteral("/messages/") + row->graphId
            + QStringLiteral("?$select=id&$expand=microsoft.graph.eventMessage/event($select=id,subject,start,end,isAllDay,location,responseStatus,isCancelled,isOrganizer,seriesMasterId,type)");
    } else if (kind == QLatin1String("search")) {
        // Graph's $search (docs/PHASE8.md): every folder or one, bodies included, KQL. Answered
        // as {"ok", "ids": immutable ids, "more"}; the app shows the cached rows of these ids.
        QString kql = args.value(QLatin1String("kql")).toString();
        kql.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        const std::optional<FolderRow> folder = m_store && args.contains(QLatin1String("folder")) ? m_store->folder(args.value(QLatin1String("folder")).toInteger()) : std::nullopt;
        const QString base = folder ? QString(m_ctx.root + QStringLiteral("/mailFolders/") + folder->graphId) : m_ctx.root;
        const QString first = base + QStringLiteral("/messages?$top=250&$select=id&$search=")
            + QString::fromLatin1(QUrl::toPercentEncoding(QLatin1Char('"') + kql + QLatin1Char('"')));
        const int max = args.value(QLatin1String("max")).toInt(1000);
        answerIds(request, first, max);
        return;
    } else if (kind == QLatin1String("searchFolderMessages")) {
        // What an Outlook search folder holds now (Exchange evaluates it), as immutable ids
        spec.pathOrUrl.clear();
        answerIds(request,
                  m_ctx.root + QStringLiteral("/mailFolders/") + args.value(QLatin1String("id")).toString() + QStringLiteral("/messages?$top=250&$select=id"),
                  args.value(QLatin1String("max")).toInt(1000));
        return;
    } else if (kind.startsWith(QLatin1String("searchFolder"))) {
        // Outlook's search folders (mailSearchFolder under Finder): list, create, change, delete
        const QString base = m_ctx.root + QStringLiteral("/mailFolders/searchfolders/childFolders");
        const QString id = args.value(QLatin1String("id")).toString();
        if (kind == QLatin1String("searchFolders")) {
            spec.pathOrUrl = base + QStringLiteral("?$top=100");
        } else if (kind == QLatin1String("searchFolderCreate")) {
            const std::optional<FolderRow> folder = m_store && args.value(QLatin1String("folder")).toInteger() != 0
                ? m_store->folder(args.value(QLatin1String("folder")).toInteger())
                : std::nullopt;
            spec.verb = "POST";
            spec.pathOrUrl = base;
            spec.body = QJsonDocument(QJsonObject{{QStringLiteral("@odata.type"), QStringLiteral("microsoft.graph.mailSearchFolder")},
                                                  {QStringLiteral("displayName"), args.value(QLatin1String("name"))},
                                                  {QStringLiteral("includeNestedFolders"), true},
                                                  {QStringLiteral("sourceFolderIds"),
                                                   QJsonArray{folder ? folder->graphId : QStringLiteral("msgfolderroot")}},
                                                  {QStringLiteral("filterQuery"), args.value(QLatin1String("filter"))}})
                            .toJson(QJsonDocument::Compact);
        } else if (kind == QLatin1String("searchFolderUpdate")) {
            spec.verb = "PATCH";
            spec.pathOrUrl = m_ctx.root + QStringLiteral("/mailFolders/") + id;
            spec.body = QJsonDocument(QJsonObject{{QStringLiteral("@odata.type"), QStringLiteral("microsoft.graph.mailSearchFolder")},
                                                  {QStringLiteral("displayName"), args.value(QLatin1String("name"))},
                                                  {QStringLiteral("filterQuery"), args.value(QLatin1String("filter"))}})
                            .toJson(QJsonDocument::Compact);
        } else if (kind == QLatin1String("searchFolderDelete")) {
            spec.verb = "DELETE";
            spec.pathOrUrl = m_ctx.root + QStringLiteral("/mailFolders/") + id;
        } else {
            Q_EMIT queryAnswered(request, {});
            return;
        }
        if (!spec.body.isEmpty()) {
            spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
        }
        // A search folder just made is still being set up: a delete then fails once
        spec.maxAttempts = 3;
        GraphCore::GraphRequest *r = m_client->start(spec);
        connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            const QJsonObject answer{{QStringLiteral("ok"), response.ok()},
                                     {QStringLiteral("problem"), accessProblem(response)},
                                     {QStringLiteral("detail"), response.ok() ? QString() : response.userMessage()},
                                     {QStringLiteral("body"), response.json()}};
            Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
        });
        return;
    } else if (kind == QLatin1String("calendarRemove")) {
        // A colleague's calendar shared with the user taken off their calendar list (Outlook's
        // "Remove calendar"); the owner's calendar itself stays. Not the user's own calendars.
        const std::optional<CalendarRow> calendar = m_store ? m_store->calendar(args.value(QLatin1String("calendar")).toInteger()) : std::nullopt;
        if (!calendar || calendar->isDefault || calendar->canEdit) {
            Q_EMIT queryAnswered(request, QStringLiteral("{\"ok\":false,\"problem\":\"error\"}"));
            return;
        }
        spec.verb = "DELETE";
        spec.pathOrUrl = m_ctx.root + QStringLiteral("/calendars/") + calendar->graphId;
        spec.maxAttempts = 2;
        const qint64 id = calendar->id;
        GraphCore::GraphRequest *r = m_client->start(spec);
        connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request, id] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            if (response.ok() || response.status == 404) {
                m_store->removeParent(ItemKind::Event, id);
                Q_EMIT collectionChanged(m_config.account, m_config.mailbox, QStringLiteral("calendar"), 1);
            }
            const QJsonObject answer{{QStringLiteral("ok"), response.ok() || response.status == 404},
                                     {QStringLiteral("problem"), response.status == 404 ? QString() : accessProblem(response)},
                                     {QStringLiteral("detail"), response.ok() ? QString() : response.userMessage()}};
            Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
        });
        return;
    } else if (kind.startsWith(QLatin1String("calendarPermission"))) {
        // Who may see one of the user's calendars (docs/PHASE7.md): list, add, change, remove.
        // Answered as {"ok", "problem" (accessProblem's code), "body"}: the dialog needs the reason.
        // Graph documents the primary calendar's path (/calendar/calendarPermissions); creating a
        // permission through /calendars/{id} answered 500 in the phase 7 live test
        const QString base = args.value(QLatin1String("isDefault")).toBool()
            ? QString(m_ctx.root + QStringLiteral("/calendar/calendarPermissions"))
            : QString(m_ctx.root + QStringLiteral("/calendars/") + args.value(QLatin1String("calendar")).toString() + QStringLiteral("/calendarPermissions"));
        const QString permission = args.value(QLatin1String("permission")).toString();
        if (kind == QLatin1String("calendarPermissions")) {
            spec.pathOrUrl = base;
        } else if (kind == QLatin1String("calendarPermissionAdd")) {
            spec.verb = "POST";
            spec.pathOrUrl = base;
            spec.body = QJsonDocument(QJsonObject{{QStringLiteral("emailAddress"),
                                                   QJsonObject{{QStringLiteral("name"), args.value(QLatin1String("name"))}, {QStringLiteral("address"), args.value(QLatin1String("address"))}}},
                                                  {QStringLiteral("isInsideOrganization"), true},
                                                  {QStringLiteral("isRemovable"), true},
                                                  {QStringLiteral("role"), args.value(QLatin1String("role"))}})
                            .toJson(QJsonDocument::Compact);
        } else if (kind == QLatin1String("calendarPermissionUpdate")) {
            spec.verb = "PATCH";
            spec.pathOrUrl = base + QLatin1Char('/') + permission;
            spec.body = QJsonDocument(QJsonObject{{QStringLiteral("role"), args.value(QLatin1String("role"))}}).toJson(QJsonDocument::Compact);
        } else if (kind == QLatin1String("calendarPermissionRemove")) {
            spec.verb = "DELETE";
            spec.pathOrUrl = base + QLatin1Char('/') + permission;
        } else {
            Q_EMIT queryAnswered(request, {});
            return;
        }
        if (!spec.body.isEmpty()) {
            spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
        }
        // The user waits in the sharing dialog: one retry, then the answer
        spec.maxAttempts = 2;
        GraphCore::GraphRequest *r = m_client->start(spec);
        connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            const QJsonObject answer{{QStringLiteral("ok"), response.ok()},
                                     {QStringLiteral("problem"), accessProblem(response)},
                                     {QStringLiteral("detail"), response.ok() ? QString() : response.userMessage()},
                                     {QStringLiteral("body"), response.json()}};
            Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
        });
        return;
    } else {
        Q_EMIT queryAnswered(request, {});
        return;
    }
    GraphCore::GraphRequest *r = m_client->start(spec);
    connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request] {
        const GraphCore::GraphResponse response = r->response();
        r->deleteLater();
        Q_EMIT queryAnswered(request, response.ok() ? QString::fromUtf8(response.body) : QString());
    });
}


void MailboxWorker::answerIds(quint64 request, const QString &first, int max)
{
    // Every page up to max, then the ids as immutable ids: $search ignores that preference
    auto ids = std::make_shared<QJsonArray>();
    auto page = std::make_shared<std::function<void(const QString &)>>();
    *page = [this, request, ids, page, max](const QString &url) {
        GraphCore::GraphRequestSpec next;
        next.pathOrUrl = url;
        next.headers = {GraphCore::Prefer::header({GraphCore::Prefer::immutableId()})};
        GraphCore::GraphRequest *r = m_client->start(next);
        connect(r, &GraphCore::GraphRequest::finished, this, [this, r, request, ids, page, max] {
            const GraphCore::GraphResponse response = r->response();
            r->deleteLater();
            if (!response.ok()) {
                const QJsonObject answer{{QStringLiteral("ok"), false}, {QStringLiteral("problem"), accessProblem(response)}, {QStringLiteral("ids"), *ids}};
                Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
                return;
            }
            const QJsonObject json = response.json();
            const QJsonArray value = json.value(QLatin1String("value")).toArray();
            for (const QJsonValue &v : value) {
                ids->append(v.toObject().value(QLatin1String("id")).toString());
            }
            const QString nextLink = json.value(QLatin1String("@odata.nextLink")).toString();
            if (!nextLink.isEmpty() && ids->size() < max) {
                (*page)(nextLink);
                return;
            }
            const bool more = !nextLink.isEmpty();
            // Ids in the REST form ("AAMk...") need translating; immutable ones are kept
            QJsonArray rest;
            QJsonArray immutable;
            for (const QJsonValue &v : std::as_const(*ids)) {
                (v.toString().startsWith(QLatin1String("AAMk")) ? rest : immutable).append(v);
            }
            if (rest.isEmpty()) {
                const QJsonObject answer{{QStringLiteral("ok"), true}, {QStringLiteral("ids"), immutable}, {QStringLiteral("more"), more}};
                Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
                return;
            }
            // $search ignores the immutable id preference (phase 8 probe): translate to the
            // immutable ids the cache keys on (at most 1000 per call; $search gives 275)
            GraphCore::GraphRequestSpec translate;
            translate.verb = "POST";
            translate.pathOrUrl = m_ctx.root + QStringLiteral("/translateExchangeIds");
            translate.headers = {{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}};
            translate.body = QJsonDocument(QJsonObject{{QStringLiteral("inputIds"), rest},
                                                       {QStringLiteral("sourceIdType"), QStringLiteral("restId")},
                                                       {QStringLiteral("targetIdType"), QStringLiteral("restImmutableEntryId")}})
                                 .toJson(QJsonDocument::Compact);
            GraphCore::GraphRequest *t = m_client->start(translate);
            connect(t, &GraphCore::GraphRequest::finished, this, [this, t, request, more, immutable]() mutable {
                const GraphCore::GraphResponse translated = t->response();
                t->deleteLater();
                const QJsonArray results = translated.json().value(QLatin1String("value")).toArray();
                for (const QJsonValue &v : results) {
                    immutable.append(v.toObject().value(QLatin1String("targetId")).toString());
                }
                const QJsonObject answer{{QStringLiteral("ok"), translated.ok()},
                                         {QStringLiteral("problem"), accessProblem(translated)},
                                         {QStringLiteral("ids"), immutable},
                                         {QStringLiteral("more"), more}};
                Q_EMIT queryAnswered(request, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
            });
        });
    };
    (*page)(first);
}

void MailboxWorker::dismissOverdueReminders()
{
    m_dismissingOverdue = true;
    m_overdueRounds = 0;
    checkOverdueReminders(true);
}

void MailboxWorker::checkOverdueReminders(bool dismiss)
{
    if (!m_client || !m_events) {
        return;
    }
    const qint64 t = now();
    auto iso = [](qint64 ms) {
        return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ"));
    };
    // The window does not limit Exchange's answer much: it lists every reminder due and not dismissed
    const QString path = m_ctx.root + QStringLiteral("/reminderView(startDateTime='") + iso(t - 60 * 1000) + QStringLiteral("',endDateTime='") + iso(t) + QStringLiteral("')");
    GraphCore::GraphRequest *r = m_client->start({"GET", path, {}, {}, true, nullptr});
    connect(r, &GraphCore::GraphRequest::finished, this, [this, r, t, dismiss] {
        const GraphCore::GraphResponse response = r->response();
        r->deleteLater();
        if (!response.ok()) {
            m_dismissingOverdue = false;
            qCInfo(GRAPHMAIL).noquote() << "Overdue reminders not read:" << response.describe();
            return;
        }
        const QJsonArray view = response.json().value(QLatin1String("value")).toArray();
        const QList<OverdueReminder> overdue = Reminders::overdue(view, t);
        if (dismiss) {
            // At most 20 rounds of 200: a backlog larger than that is a reason to look, not to loop
            if (overdue.isEmpty() || ++m_overdueRounds > 20) {
                m_dismissingOverdue = false;
                Q_EMIT overdueReminders(m_config.account, m_config.mailbox, 0, false, 0, {});
                return;
            }
            for (const OverdueReminder &o : overdue) {
                m_events->dismissReminderByGraphId(o.eventId);
            }
            tick();
            return;
        }
        if (overdue.isEmpty()) {
            return;
        }
        qint64 oldest = std::numeric_limits<qint64>::max();
        for (const OverdueReminder &o : overdue) {
            oldest = std::min(oldest, o.start);
        }
        QList<OverdueReminder> newest = overdue;
        std::sort(newest.begin(), newest.end(), [](const OverdueReminder &a, const OverdueReminder &b) {
            return a.start > b.start;
        });
        QStringList subjects;
        for (int i = 0; i < newest.size() && i < 3; ++i) {
            subjects.append(newest.at(i).subject);
        }
        qCInfo(GRAPHMAIL) << "Overdue reminders:" << overdue.size() << (view.size() >= Reminders::viewLimit ? "or more" : "");
        Q_EMIT overdueReminders(m_config.account, m_config.mailbox, int(overdue.size()), view.size() >= Reminders::viewLimit, oldest, subjects);
    });
}


void MailboxWorker::itemsChanged(const QString &collection)
{
    // Which collection: the queue says todo for lists and steps; otherwise both are redrawn
    const QStringList keys = collection.isEmpty() ? QStringList{QStringLiteral("contacts"), QStringLiteral("todo")} : QStringList{collection};
    for (const QString &key : keys) {
        Q_EMIT collectionChanged(m_config.account, m_config.mailbox, key, 1);
        auto it = m_entries.find(key);
        if (it != m_entries.end()) {
            it->due = std::min(it->due, now() + 3000);
        }
    }
}

void MailboxWorker::createItem(const QString &kind, qint64 parent, const QString &json)
{
    const ItemKind k = kind == QLatin1String("contact") ? ItemKind::Contact : ItemKind::Task;
    if (m_items && m_items->create(k, parent, QJsonDocument::fromJson(json.toUtf8()).object())) {
        Q_EMIT collectionChanged(m_config.account, m_config.mailbox, k == ItemKind::Contact ? QStringLiteral("contacts") : QStringLiteral("todo"), 1);
        tick();
    }
}

void MailboxWorker::updateItem(const QString &kind, qint64 item, const QString &json)
{
    const ItemKind k = kind == QLatin1String("contact") ? ItemKind::Contact : ItemKind::Task;
    if (m_items && m_items->update(k, item, QJsonDocument::fromJson(json.toUtf8()).object())) {
        Q_EMIT collectionChanged(m_config.account, m_config.mailbox, k == ItemKind::Contact ? QStringLiteral("contacts") : QStringLiteral("todo"), 1);
        tick();
    }
}

void MailboxWorker::deleteItem(const QString &kind, qint64 item)
{
    const ItemKind k = kind == QLatin1String("contact") ? ItemKind::Contact : ItemKind::Task;
    if (m_items && m_items->remove(k, item)) {
        Q_EMIT collectionChanged(m_config.account, m_config.mailbox, k == ItemKind::Contact ? QStringLiteral("contacts") : QStringLiteral("todo"), 1);
        tick();
    }
}

void MailboxWorker::todoList(const QString &action, qint64 list, const QString &name)
{
    if (!m_items) {
        return;
    }
    const bool ok = action == QLatin1String("create") ? m_items->createList(name)
        : action == QLatin1String("rename")           ? m_items->renameList(list, name)
                                                      : m_items->deleteList(list);
    if (ok) {
        Q_EMIT collectionChanged(m_config.account, m_config.mailbox, QStringLiteral("todo"), 1);
        tick();
    }
}

void MailboxWorker::taskStep(qint64 task, const QString &action, const QString &stepId, const QString &json)
{
    if (m_items && m_items->step(task, action, stepId, QJsonDocument::fromJson(json.toUtf8()).object())) {
        tick();
    }
}

void MailboxWorker::taskReminderAction(qint64 task, bool dismiss, qint64 until)
{
    const std::optional<StoredItem> t = m_store ? m_store->item(ItemKind::Task, task) : std::nullopt;
    if (!t || !m_items) {
        return;
    }
    const QJsonObject json = QJsonDocument::fromJson(t->json.toUtf8()).object();
    const QString key = QStringLiteral("task:") + t->graphId + QLatin1Char(':') + json.value(QLatin1String("reminderDateTime")).toObject().value(QLatin1String("dateTime")).toString();
    if (dismiss) {
        m_store->setReminderState(key, now(), 2);
        // To Do ignores isReminderOn false while a reminder time is set (HTTP 200, still on)
        m_items->update(ItemKind::Task, task, {{QStringLiteral("isReminderOn"), false}, {QStringLiteral("reminderDateTime"), QJsonValue::Null}});
    } else {
        // To Do keeps one reminder time: snoozing moves it
        m_store->setReminderState(key, now(), 2);
        m_items->update(ItemKind::Task,
                        task,
                        {{QStringLiteral("isReminderOn"), true},
                         {QStringLiteral("reminderDateTime"),
                          QJsonObject{{QStringLiteral("dateTime"), QDateTime::fromMSecsSinceEpoch(until, QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))},
                                      {QStringLiteral("timeZone"), QStringLiteral("UTC")}}}});
    }
    Q_EMIT collectionChanged(m_config.account, m_config.mailbox, QStringLiteral("todo"), 1);
    publishReminders();
    tick();
}

void MailboxWorker::checkTaskReminders()
{
    if (!m_store) {
        return;
    }
    const QList<StoredItem> due = m_store->dueTaskReminders(now());
    for (const StoredItem &t : due) {
        const QJsonObject json = QJsonDocument::fromJson(t.json.toUtf8()).object();
        if (!json.value(QLatin1String("isReminderOn")).toBool()) {
            continue;
        }
        const QString key = QStringLiteral("task:") + t.graphId + QLatin1Char(':') + json.value(QLatin1String("reminderDateTime")).toObject().value(QLatin1String("dateTime")).toString();
        if (m_store->reminderState(key) != 0) {
            continue;
        }
        m_store->setReminderState(key, now(), 1);
        Q_EMIT taskReminderDue(m_config.account, m_config.mailbox, t.id, json.value(QLatin1String("title")).toString(),
                               localMidnight(Mapper::dueDate(json.value(QLatin1String("dueDateTime")).toObject())));
    }
}

}
