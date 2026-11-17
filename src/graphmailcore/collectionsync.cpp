// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "collectionsync.h"
#include "blobstore.h"
#include "graphmapper.h"
#include "graphutil.h"
#include "graphmail_debug.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QPointer>

using namespace GraphCore;

namespace GraphMail
{

namespace
{
constexpr qint64 dayMs = 24LL * 3600 * 1000;

bool isGone(const GraphResponse &response)
{
    if (response.status == 410) {
        return true;
    }
    // Some services answer an expired or broken sync state with 400
    const QString code = response.graphErrorCode();
    return response.status == 400 && (code.contains(QLatin1String("SyncState"), Qt::CaseInsensitive) || code.contains(QLatin1String("resync"), Qt::CaseInsensitive));
}

bool isAccessDenied(const GraphResponse &response)
{
    // A shared mailbox without Full Access answers "Default folder Root not found" (phase 0)
    return response.status == 403
        || (response.status == 404 && QString::fromUtf8(response.body).contains(QLatin1String("Default folder"), Qt::CaseInsensitive));
}

// A list of another mailbox's calendars: Graph answers 404 ErrorItemNotFound once the share is
// gone (docs/PHASE7.md), which is a refusal, not a missing item
bool listingDenied(const GraphResponse &response, const QString &root)
{
    return isAccessDenied(response) || (root != QLatin1String("/me") && response.status == 404 && response.graphErrorCode() == QLatin1String("ErrorItemNotFound"));
}

// Kept per parent while a fresh (full) chain runs: at its end, items not seen again are removed
QString fullChainKey(ItemKind kind, qint64 parent)
{
    return QStringLiteral("fullgen:%1:%2").arg(int(kind)).arg(parent);
}
}

// --- CollectionSync -------------------------------------------------------------------------

CollectionSync::CollectionSync(SyncContext *context, QObject *parent)
    : QObject(parent)
    , m_ctx(context)
{
}

bool CollectionSync::isRunning() const
{
    return m_running;
}

void CollectionSync::run(std::function<void(const SyncResult &)> done)
{
    Q_ASSERT(!m_running);
    m_running = true;
    m_changes = 0;
    m_done = std::move(done);
    start();
}

void CollectionSync::finish(const SyncResult &result)
{
    if (!m_running) {
        return;
    }
    m_running = false;
    SyncResult final = result;
    final.changes = std::max(final.changes, m_changes);
    // Callers may delete this object from the callback
    auto done = std::move(m_done);
    done(final);
}

qint64 CollectionSync::now() const
{
    return m_ctx->now ? m_ctx->now() : QDateTime::currentMSecsSinceEpoch();
}

QString CollectionSync::isoUtc(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODate);
}

void CollectionSync::removeBlobs(const QStringList &blobs)
{
    for (const QString &blob : blobs) {
        m_ctx->blobs->remove(blob);
    }
}

void CollectionSync::finishWithEnd(ChainEnd end, const QString &detail, int changes)
{
    SyncResult result;
    result.changes = changes;
    result.error = detail;
    switch (end) {
    case ChainEnd::Network:
        result.outcome = SyncResult::Outcome::NetworkError;
        break;
    case ChainEnd::AccessDenied:
        result.outcome = SyncResult::Outcome::AccessDenied;
        break;
    case ChainEnd::Budget:
        result.outcome = SyncResult::Outcome::More;
        break;
    case ChainEnd::Delta:
        result.outcome = SyncResult::Outcome::Done;
        break;
    case ChainEnd::Gone:
    case ChainEnd::Failed:
        result.outcome = SyncResult::Outcome::Failed;
        break;
    }
    finish(result);
}

void CollectionSync::followChain(const QString &link, const HeaderList &headers, int pageBudget, const PageHandler &handler, const ChainDone &done)
{
    GraphRequest *request = m_ctx->client->start({"GET", link, {}, headers, true, nullptr});
    connect(request, &GraphRequest::finished, this, [this, request, headers, pageBudget, handler, done] {
        const GraphResponse response = request->response();
        request->deleteLater();
        if (isGone(response)) {
            done(ChainEnd::Gone, response.userMessage());
            return;
        }
        if (response.isNetworkError()) {
            done(ChainEnd::Network, response.userMessage());
            return;
        }
        if (isAccessDenied(response)) {
            done(ChainEnd::AccessDenied, response.userMessage());
            return;
        }
        if (!response.ok()) {
            done(ChainEnd::Failed, response.userMessage());
            return;
        }
        const QJsonObject page = response.json();
        const QString deltaLink = page.value(QLatin1String("@odata.deltaLink")).toString();
        const QString nextLink = page.value(QLatin1String("@odata.nextLink")).toString();
        if (deltaLink.isEmpty() && nextLink.isEmpty()) {
            done(ChainEnd::Failed, QStringLiteral("a delta page without nextLink or deltaLink"));
            return;
        }
        if (!handler(page.value(QLatin1String("value")).toArray(), deltaLink.isEmpty() ? nextLink : deltaLink, !deltaLink.isEmpty())) {
            done(ChainEnd::Failed, QStringLiteral("cache write failed: ") + m_ctx->store->lastError());
            return;
        }
        if (!deltaLink.isEmpty()) {
            done(ChainEnd::Delta, deltaLink);
        } else if (pageBudget == 1) {
            done(ChainEnd::Budget, nextLink);
        } else {
            followChain(nextLink, headers, pageBudget > 1 ? pageBudget - 1 : 0, handler, done);
        }
    });
}

// --- FolderTreeSync -------------------------------------------------------------------------

QString FolderTreeSync::key() const
{
    return QStringLiteral("tree");
}

void FolderTreeSync::start()
{
    m_result = {};
    // Always a full listing, never the saved delta link: Graph's folder delta does not report
    // changed total/unread counts (phase 2 exit gate, 2026-11-11), and those counts are what wakes
    // up the cold folders. One request for up to 250 folders.
    const QString link = m_ctx->root + QStringLiteral("/mailFolders/delta?$select=") + Mapper::folderSelect();
    m_full = true;
    m_seen.clear();
    const HeaderList headers{Prefer::header({Prefer::immutableId(), Prefer::maxPageSize(250)})};

    auto handler = [this](const QJsonArray &items, const QString &link, bool) {
        QStringList blobs;
        const bool ok = m_ctx->store->inTransaction([&] {
            for (const QJsonValue &value : items) {
                const QJsonObject item = value.toObject();
                const QString id = item.value(QLatin1String("id")).toString();
                if (id.isEmpty()) {
                    continue;
                }
                const std::optional<FolderRow> old = m_ctx->store->folderByGraphId(id);
                if (Mapper::isRemoved(item)) {
                    if (old) {
                        blobs += m_ctx->store->removeFolder(old->id);
                        m_result.foldersRemoved.append(old->id);
                        ++m_changes;
                    }
                    continue;
                }
                const FolderRow row = Mapper::folder(item);
                const qint64 folder = m_ctx->store->upsertFolder(row);
                if (folder == 0) {
                    return false;
                }
                m_seen.insert(id);
                if (!old) {
                    m_result.foldersAdded.append(folder);
                    ++m_changes;
                } else if (old->total != row.total || old->unread != row.unread || old->name != row.name) {
                    m_result.foldersChanged.append(folder);
                    ++m_changes;
                }
            }
            return m_ctx->store->setMeta(QStringLiteral("tree_delta"), link);
        });
        if (ok) {
            removeBlobs(blobs);
        }
        return ok;
    };

    followChain(link, headers, 0, handler, [this](ChainEnd end, const QString &detail) {
        if (end != ChainEnd::Delta) {
            finishWithEnd(end, detail, m_changes);
            return;
        }
        if (m_full) {
            // A complete listing: folders not in it are gone
            const QList<FolderRow> folders = m_ctx->store->folders();
            for (const FolderRow &folder : folders) {
                if (!m_seen.contains(folder.graphId)) {
                    removeBlobs(m_ctx->store->removeFolder(folder.id));
                    m_result.foldersRemoved.append(folder.id);
                }
            }
        }
        resolveWellKnown();
    });
}

void FolderTreeSync::resolveWellKnown()
{
    // v2 (2026-11-11): also the folders Graph Mail hides like Outlook (conversation history, sync
    // issues); v3 (2026-11-13): the archive folder from the mailbox setting
    if (m_ctx->store->meta(QStringLiteral("well_known")) == QLatin1String("3")) {
        finish(m_result);
        return;
    }
    // Graph's v1.0 folders carry no role; ask for each well-known name once (with immutable ids,
    // so the ids match the tree's)
    static const QStringList names{QStringLiteral("inbox"),
                                   QStringLiteral("drafts"),
                                   QStringLiteral("sentitems"),
                                   QStringLiteral("deleteditems"),
                                   QStringLiteral("junkemail"),
                                   QStringLiteral("archive"),
                                   QStringLiteral("outbox"),
                                   QStringLiteral("conversationhistory"),
                                   QStringLiteral("syncissues"),
                                   QStringLiteral("conflicts"),
                                   QStringLiteral("localfailures"),
                                   QStringLiteral("serverfailures")};
    auto pending = std::make_shared<int>(int(names.size()));
    auto complete = std::make_shared<bool>(true);
    for (const QString &name : names) {
        GraphRequest *request = m_ctx->client->start({"GET", m_ctx->root + QStringLiteral("/mailFolders/") + name + QStringLiteral("?$select=id"),
                                                      {},
                                                      {Prefer::header({Prefer::immutableId()})},
                                                      true,
                                                      nullptr});
        connect(request, &GraphRequest::finished, this, [this, request, name, pending, complete] {
            const GraphResponse response = request->response();
            request->deleteLater();
            if (response.ok()) {
                m_ctx->store->setFolderWellKnown(response.json().value(QLatin1String("id")).toString(), name);
            } else if (response.status != 404) {
                // Not every mailbox has every folder (404); anything else: try again next time
                *complete = false;
            }
            if (--*pending == 0) {
                resolveArchiveSetting(*complete);
            }
        });
    }
}

void FolderTreeSync::resolveArchiveSetting(bool complete)
{
    // The "archive" alias always names the default Archive folder; Outlook's Archive button uses
    // mailboxSettings.archiveFolder, which may name another (9b.3, tested live 2026-11-13). Without
    // access to the settings (a shared mailbox) the alias stays.
    GraphRequest *request = m_ctx->client->start({"GET", m_ctx->root + QStringLiteral("/mailboxSettings?$select=archiveFolder"), {}, {}, true, nullptr});
    connect(request, &GraphRequest::finished, this, [this, request, complete] {
        const GraphResponse response = request->response();
        request->deleteLater();
        const QString id = response.ok() ? response.json().value(QLatin1String("archiveFolder")).toString() : QString();
        if (!id.isEmpty() && m_ctx->store->folderByGraphId(id)) {
            if (const std::optional<FolderRow> old = m_ctx->store->folderByWellKnown(QStringLiteral("archive")); old && old->graphId != id) {
                m_ctx->store->setFolderWellKnown(old->graphId, QString());
            }
            m_ctx->store->setFolderWellKnown(id, QStringLiteral("archive"));
        }
        // Throttled (429) or timed out (408): ask again next time, like a server error
        if (complete && response.status < 500 && response.status != 0 && response.status != 429 && response.status != 408) {
            m_ctx->store->setMeta(QStringLiteral("well_known"), QStringLiteral("3"));
        }
        finish(m_result);
    });
}

// --- MailFolderSync -------------------------------------------------------------------------

MailFolderSync::MailFolderSync(SyncContext *context, qint64 folder, const QString &folderGraphId, QObject *parent)
    : CollectionSync(context, parent)
    , m_folder(folder)
    , m_folderGraphId(folderGraphId)
{
}

QString MailFolderSync::key() const
{
    return QStringLiteral("folder:%1").arg(m_folder);
}

qint64 MailFolderSync::folder() const
{
    return m_folder;
}

QString MailFolderSync::baseUrl() const
{
    return m_ctx->root + QStringLiteral("/mailFolders/") + m_folderGraphId + QStringLiteral("/messages/delta?$select=") + Mapper::messageSelect();
}

HeaderList MailFolderSync::headers(int pageSize) const
{
    return {Prefer::header({Prefer::immutableId(), Prefer::maxPageSize(pageSize)})};
}

MailFolderSync::PageHandler MailFolderSync::pageHandler(const std::function<void(SyncState &, const QString &)> &saveLink)
{
    return [this, saveLink](const QJsonArray &items, const QString &link, bool) {
        Store *store = m_ctx->store;
        const int generation = store->generation(m_folder);
        const bool checkPending = m_ctx->reapplyPending && store->pendingOperationCount() > 0;
        QStringList blobs;
        QStringList touched;
        QStringList unknownSparse;
        int changes = 0;
        // A draft leaving Drafts (deleted or moved in Outlook) takes Graph Mail's record of it along
        const std::optional<FolderRow> drafts = store->folderByWellKnown(QStringLiteral("drafts"));
        const bool inDrafts = drafts && drafts->id == m_folder;
        const bool ok = store->inTransaction([&] {
            for (const QJsonValue &value : items) {
                const QJsonObject item = value.toObject();
                const QString id = item.value(QLatin1String("id")).toString();
                if (id.isEmpty()) {
                    continue;
                }
                if (Mapper::isRemoved(item)) {
                    // A move into this folder still waiting (an Undo right after a move out): the
                    // removal is the earlier move's; the queued move settles where it ends up
                    if (const std::optional<OperationRow> moving = store->pendingOperation(id, QStringLiteral("move"));
                        moving && QJsonDocument::fromJson(moving->payload.toUtf8()).object().value(QLatin1String("value")).toInteger() == m_folder) {
                        continue;
                    }
                    // The move rule: only while the message is still in this folder
                    const RemovedMessage removed = store->removeMessage(id, m_folder);
                    if (removed.removed) {
                        ++changes;
                        blobs += removed.blobs;
                        if (inDrafts) {
                            store->dropDraftRecords(id);
                        }
                    }
                    continue;
                }
                MessageRow row;
                if (Mapper::isSparse(item)) {
                    const std::optional<MessageRow> cached = store->messageByGraphId(id);
                    if (!cached) {
                        // Not cached yet: fetch the whole message after this page
                        unknownSparse.append(id);
                        continue;
                    }
                    row = Mapper::mergeMessage(*cached, item, m_folder);
                } else {
                    row = Mapper::message(item, m_folder);
                }
                const Store::Upsert upsert = store->upsertMessage(row, generation);
                if (upsert == Store::Upsert::Inserted && m_incremental && !row.isRead) {
                    if (const std::optional<MessageRow> stored = store->messageByGraphId(id)) {
                        m_newUnread.append(stored->id);
                    }
                }
                switch (upsert) {
                case Store::Upsert::Failed:
                    return false;
                case Store::Upsert::Inserted:
                case Store::Upsert::Updated:
                    ++changes;
                    touched.append(id);
                    if (drafts && !inDrafts) {
                        store->dropDraftRecords(id);
                    }
                    break;
                case Store::Upsert::Unchanged:
                    break;
                }
            }
            saveLink(m_state, link);
            m_state.lastSynced = now();
            m_state.lastError.clear();
            return store->setSyncState(m_folder, m_state);
        });
        if (!ok) {
            return false;
        }
        m_changes += changes;
        removeBlobs(blobs);
        for (const QString &id : std::as_const(unknownSparse)) {
            fetchWholeMessage(id);
        }
        if (checkPending) {
            for (const QString &id : std::as_const(touched)) {
                m_ctx->reapplyPending(id);
            }
        }
        return true;
    };
}

void MailFolderSync::fetchWholeMessage(const QString &graphId)
{
    const QString url = m_ctx->root + QStringLiteral("/messages/") + graphId + QStringLiteral("?$select=") + Mapper::messageSelect();
    GraphRequest *request = m_ctx->client->start({"GET", url, {}, {Prefer::header({Prefer::immutableId()})}, true, nullptr});
    const qint64 folder = m_folder;
    connect(request, &GraphRequest::finished, this, [this, request, folder] {
        const GraphResponse response = request->response();
        request->deleteLater();
        // Gone or unreachable: the next full item or resync brings it
        if (response.ok()) {
            m_ctx->store->upsertMessage(Mapper::message(response.json(), folder), m_ctx->store->generation(folder));
        }
    });
}

void MailFolderSync::start()
{
    m_state = m_ctx->store->syncState(m_folder);
    m_incremental = m_state.initialState == Incremental;
    m_newUnread.clear();
    switch (m_state.initialState) {
    case NotStarted: {
        const qint64 since = now() - qint64(m_ctx->settings.recentWindowDays) * dayMs;
        m_state.initialState = RecentWindow;
        m_state.recentDeltaLink = baseUrl() + QStringLiteral("&$filter=receivedDateTime ge ") + isoUtc(since);
        m_ctx->store->setSyncState(m_folder, m_state);
        runRecentWindow();
        return;
    }
    case RecentWindow:
        runRecentWindow();
        return;
    case FullChain:
        // A forced resync leaves the link empty: start the chain from the beginning
        if (m_state.deltaLink.isEmpty()) {
            startFullChain();
        } else {
            runRecentIncremental();
        }
        return;
    default:
        runIncremental();
        return;
    }
}

// Phase 1 of an initial sync: the recent window, so the newest mail is listed within seconds
void MailFolderSync::runRecentWindow()
{
    followChain(m_state.recentDeltaLink,
                headers(m_ctx->settings.initialPageSize),
                0,
                pageHandler([](SyncState &state, const QString &link) {
                    state.recentDeltaLink = link;
                }),
                [this](ChainEnd end, const QString &detail) {
                    if (end == ChainEnd::Delta) {
                        startFullChain();
                    } else if (end == ChainEnd::Gone) {
                        // Start over
                        m_state = {};
                        m_ctx->store->setSyncState(m_folder, m_state);
                        finishWithEnd(ChainEnd::Budget, detail, m_changes);
                    } else {
                        finishWithEnd(end, detail, m_changes);
                    }
                });
}

// Phase 2 (and every resync): the whole folder, in turns of a few pages; rows not seen again
// before its deltaLink are removed (mark and sweep)
void MailFolderSync::startFullChain()
{
    // A resync lists everything again: none of it is new mail
    m_incremental = false;
    m_ctx->store->beginResync(m_folder);
    m_state.initialState = FullChain;
    m_state.deltaLink = baseUrl();
    m_ctx->store->setSyncState(m_folder, m_state);
    continueFullChain();
}

// While the full chain runs, new mail still comes in through the recent window's delta link
void MailFolderSync::runRecentIncremental()
{
    if (m_state.recentDeltaLink.isEmpty()) {
        continueFullChain();
        return;
    }
    followChain(m_state.recentDeltaLink,
                headers(m_ctx->settings.incrementalPageSize),
                m_ctx->settings.pageBudget,
                pageHandler([](SyncState &state, const QString &link) {
                    state.recentDeltaLink = link;
                }),
                [this](ChainEnd end, const QString &detail) {
                    if (end == ChainEnd::Gone) {
                        m_state.recentDeltaLink.clear();
                        m_ctx->store->setSyncState(m_folder, m_state);
                    } else if (end != ChainEnd::Delta && end != ChainEnd::Budget) {
                        finishWithEnd(end, detail, m_changes);
                        return;
                    }
                    continueFullChain();
                });
}

void MailFolderSync::continueFullChain()
{
    followChain(m_state.deltaLink,
                headers(m_ctx->settings.initialPageSize),
                m_ctx->settings.pageBudget,
                pageHandler([](SyncState &state, const QString &link) {
                    state.deltaLink = link;
                }),
                [this](ChainEnd end, const QString &detail) {
                    if (end == ChainEnd::Delta) {
                        removeBlobs(m_ctx->store->sweep(m_folder, m_ctx->store->generation(m_folder)));
                        m_state.initialState = Incremental;
                        m_state.recentDeltaLink.clear();
                        m_ctx->store->setSyncState(m_folder, m_state);
                        finishWithEnd(ChainEnd::Delta, {}, m_changes);
                    } else if (end == ChainEnd::Gone) {
                        startFullChain();
                    } else {
                        finishWithEnd(end, detail, m_changes);
                    }
                });
}

void MailFolderSync::runIncremental()
{
    followChain(m_state.deltaLink,
                headers(m_ctx->settings.incrementalPageSize),
                m_ctx->settings.pageBudget,
                pageHandler([](SyncState &state, const QString &link) {
                    state.deltaLink = link;
                }),
                [this](ChainEnd end, const QString &detail) {
                    if (end == ChainEnd::Gone) {
                        qCInfo(GRAPHMAIL) << "Sync state of folder" << m_folder << "expired, resyncing it";
                        m_state.recentDeltaLink.clear();
                        startFullChain();
                        return;
                    }
                    if (end != ChainEnd::Delta && end != ChainEnd::Budget) {
                        // Keep the error with the folder for the status
                        m_state.lastError = detail;
                        m_ctx->store->setSyncState(m_folder, m_state);
                        finishWithEnd(end, detail, m_changes);
                        return;
                    }
                    SyncResult result;
                    result.outcome = end == ChainEnd::Delta ? SyncResult::Outcome::Done : SyncResult::Outcome::More;
                    result.changes = m_changes;
                    result.newUnread = m_newUnread;
                    finish(result);
                });
}

// --- items under a parent (calendar, contacts, To Do lists) ---------------------------------

void CollectionSync::runItemChain(ItemKind kind,
                                  const ParentRow &parent,
                                  const QString &freshUrl,
                                  const HeaderList &headers,
                                  const ItemMapper &map,
                                  const std::function<void()> &next,
                                  bool retried)
{
    Store *store = m_ctx->store;
    const QString fullKey = fullChainKey(kind, parent.id);
    QString link = parent.deltaLink;
    if (link.isEmpty()) {
        // A fresh chain: everything not seen again before its deltaLink is removed
        const int generation = store->beginParentResync(kind, parent.id);
        store->setMeta(fullKey, QString::number(generation));
        link = freshUrl;
    }
    const QString fullGeneration = store->meta(fullKey);
    const int generation = fullGeneration.isEmpty() ? parent.generation : fullGeneration.toInt();

    auto handler = [this, store, kind, parent, generation, map](const QJsonArray &items, const QString &link, bool) {
        int changes = 0;
        const bool ok = store->inTransaction([&] {
            for (const QJsonValue &value : items) {
                const QJsonObject item = value.toObject();
                const QString id = item.value(QLatin1String("id")).toString();
                if (id.isEmpty()) {
                    continue;
                }
                if (Mapper::isRemoved(item)) {
                    if (!store->removeItem(kind, id)) {
                        return false;
                    }
                    ++changes;
                    continue;
                }
                switch (store->upsertItem(kind, parent.id, map(item), generation)) {
                case Store::Upsert::Failed:
                    return false;
                case Store::Upsert::Inserted:
                case Store::Upsert::Updated:
                    ++changes;
                    break;
                case Store::Upsert::Unchanged:
                    break;
                }
            }
            return store->setParentDeltaLink(kind, parent.id, link, now(), {});
        });
        if (ok) {
            m_changes += changes;
        }
        return ok;
    };

    followChain(link, headers, 0, handler, [=, this](ChainEnd end, const QString &detail) {
        if (end == ChainEnd::Delta) {
            if (!store->meta(fullKey).isEmpty()) {
                store->sweepItems(kind, parent.id, generation);
                store->setMeta(fullKey, {});
            }
            if (kind == ItemKind::Event) {
                m_changes += store->sweepOrphanOccurrences(parent.id);
            }
            next();
        } else if (end == ChainEnd::Gone && !retried) {
            store->setParentDeltaLink(kind, parent.id, {}, now(), {});
            ParentRow fresh = parent;
            fresh.deltaLink.clear();
            runItemChain(kind, fresh, freshUrl, headers, map, next, true);
        } else if (m_skipFailedParents && end != ChainEnd::Network) {
            // One calendar refusing (a share taken back) does not stop the others
            store->setParentDeltaLink(kind, parent.id, parent.deltaLink, now(), detail);
            next();
        } else {
            store->setParentDeltaLink(kind, parent.id, parent.deltaLink, now(), detail);
            finishWithEnd(end, detail, m_changes);
        }
    });
}

ParentItemSync::ParentItemSync(SyncContext *context, Kind kind, QObject *parent)
    : CollectionSync(context, parent)
    , m_kind(kind)
{
}

QString ParentItemSync::key() const
{
    return m_kind == Kind::Calendar ? QStringLiteral("calendar") : QStringLiteral("contacts");
}

void ParentItemSync::withParent(const std::function<void(const ParentRow &)> &next)
{
    const ItemKind kind = m_kind == Kind::Calendar ? ItemKind::Event : ItemKind::Contact;
    const QList<ParentRow> parents = m_ctx->store->parents(kind);
    if (!parents.isEmpty()) {
        next(parents.first());
        return;
    }
    if (m_kind == Kind::Contacts) {
        // The default contacts folder: /contacts/delta covers it (phase 0)
        m_ctx->store->upsertParent(kind, {0, QStringLiteral("contacts"), QStringLiteral("Contacts"), {}, {}, 0, 0, 0});
        next(m_ctx->store->parents(kind).constFirst());
        return;
    }
    // The default calendar: its id and name, once
    GraphRequest *request = m_ctx->client->start({"GET", m_ctx->root + QStringLiteral("/calendar?$select=id,name"),
                                                  {},
                                                  {Prefer::header({Prefer::immutableId()})},
                                                  true,
                                                  nullptr});
    connect(request, &GraphRequest::finished, this, [this, request, kind, next] {
        const GraphResponse response = request->response();
        request->deleteLater();
        if (!response.ok()) {
            finishWithEnd(response.isNetworkError() ? ChainEnd::Network : (listingDenied(response, m_ctx->root) ? ChainEnd::AccessDenied : ChainEnd::Failed),
                          response.userMessage(),
                          0);
            return;
        }
        const QJsonObject calendar = response.json();
        m_ctx->store->upsertParent(kind,
                                   {0, calendar.value(QLatin1String("id")).toString(), calendar.value(QLatin1String("name")).toString(), {}, {}, 0, 0, 0});
        next(m_ctx->store->parents(kind).constFirst());
    });
}

void ParentItemSync::start()
{
    if (m_kind == Kind::Contacts) {
        // The default folder (synced through /contacts) and the user's own contact folders
        m_skipFailedParents = true;
        withParent([this](const ParentRow &) {
            GraphRequest *request = m_ctx->client->start({"GET", m_ctx->root + QStringLiteral("/contactFolders?$top=100&$select=id,displayName"), {}, {Prefer::header({Prefer::immutableId()})}, true, nullptr});
            connect(request, &GraphRequest::finished, this, [this, request] {
                const GraphResponse response = request->response();
                request->deleteLater();
                Store *store = m_ctx->store;
                if (response.ok()) {
                    QSet<QString> present{QStringLiteral("contacts")};
                    const QJsonArray folders = response.json().value(QLatin1String("value")).toArray();
                    for (const QJsonValue &value : folders) {
                        const QJsonObject f = value.toObject();
                        present.insert(f.value(QLatin1String("id")).toString());
                        store->upsertParent(ItemKind::Contact, {0, f.value(QLatin1String("id")).toString(), f.value(QLatin1String("displayName")).toString(), {}, {}, 0, 0, 0});
                    }
                    const QList<ParentRow> known = store->parents(ItemKind::Contact);
                    for (const ParentRow &folder : known) {
                        if (!present.contains(folder.graphId)) {
                            store->removeParent(ItemKind::Contact, folder.id);
                            ++m_changes;
                        }
                    }
                } else if (response.isNetworkError()) {
                    finishWithEnd(ChainEnd::Network, response.userMessage(), 0);
                    return;
                }
                m_parents = store->parents(ItemKind::Contact);
                syncParent(0);
            });
        });
        return;
    }
    // Every calendar: the user's own and those colleagues shared (docs/PHASE5.md)
    m_skipFailedParents = true;
    GraphRequest *request = m_ctx->client->start({"GET",
                                                  m_ctx->root + QStringLiteral("/calendars?$top=100&$select=id,name,hexColor,canEdit,isDefaultCalendar,owner"),
                                                  {},
                                                  {Prefer::header({Prefer::immutableId()})},
                                                  true,
                                                  nullptr});
    connect(request, &GraphRequest::finished, this, [this, request] {
        const GraphResponse response = request->response();
        request->deleteLater();
        if (!response.ok()) {
            finishWithEnd(response.isNetworkError() ? ChainEnd::Network : (listingDenied(response, m_ctx->root) ? ChainEnd::AccessDenied : ChainEnd::Failed),
                          response.userMessage(),
                          0);
            return;
        }
        Store *store = m_ctx->store;
        QSet<QString> present;
        const QJsonArray calendars = response.json().value(QLatin1String("value")).toArray();
        for (const QJsonValue &value : calendars) {
            const CalendarRow row = Mapper::calendar(value.toObject(), {});
            present.insert(row.graphId);
            store->upsertCalendar(row);
        }
        const QList<ParentRow> known = store->parents(ItemKind::Event);
        for (const ParentRow &calendar : known) {
            if (!present.contains(calendar.graphId)) {
                store->removeParent(ItemKind::Event, calendar.id);
                ++m_changes;
            }
        }
        m_parents = store->parents(ItemKind::Event);
        syncParent(0);
    });
}

void ParentItemSync::syncParent(qsizetype index)
{
    if (index >= m_parents.size()) {
        finish({});
        return;
    }
    runChain(m_parents.at(index), [this, index] {
        syncParent(index + 1);
    });
}

void ParentItemSync::runChain(const ParentRow &original, const std::function<void()> &done)
{
    ParentRow parent = original;
    if (m_kind == Kind::Contacts) {
        const QString base = parent.graphId == QLatin1String("contacts") ? m_ctx->root : m_ctx->root + QStringLiteral("/contactFolders/") + parent.graphId;
        runItemChain(ItemKind::Contact, parent, base + QStringLiteral("/contacts/delta"),
                     {Prefer::header({Prefer::immutableId(), Prefer::maxPageSize(m_ctx->settings.initialPageSize)})}, &Mapper::contact, done);
        return;
    }
    // calendarView/delta is bound to its window; move the window on once its start has drifted
    const SyncSettings &s = m_ctx->settings;
    const qint64 intendedStart = now() - qint64(s.calendarDaysBack) * dayMs;
    if (parent.deltaLink.isEmpty() || parent.windowStart == 0 || intendedStart - parent.windowStart > qint64(s.calendarRewindowDays) * dayMs) {
        parent.windowStart = intendedStart;
        parent.windowEnd = now() + qint64(s.calendarDaysAhead) * dayMs;
        parent.deltaLink.clear();
        m_ctx->store->setCalendarWindow(parent.id, parent.windowStart, parent.windowEnd);
        m_ctx->store->setParentDeltaLink(ItemKind::Event, parent.id, {}, now(), {});
    }
    // The default calendar keeps the path its chain started with; the others go by their id
    const std::optional<CalendarRow> calendar = m_ctx->store->calendar(parent.id);
    const QString base = calendar && !calendar->isDefault ? m_ctx->root + QStringLiteral("/calendars/") + parent.graphId : m_ctx->root;
    const QString fresh = base + QStringLiteral("/calendarView/delta?startDateTime=") + isoUtc(parent.windowStart) + QStringLiteral("&endDateTime=")
        + isoUtc(parent.windowEnd);
    // Times in UTC: the cache stores UTC, the UI converts
    runItemChain(ItemKind::Event, parent, fresh,
                 {Prefer::header({Prefer::immutableId(), QByteArrayLiteral("outlook.timezone=\"UTC\""), Prefer::maxPageSize(m_ctx->settings.initialPageSize)})},
                 &Mapper::event, done);
}

QString TodoSync::key() const
{
    return QStringLiteral("todo");
}

void TodoSync::start()
{
    GraphRequest *request = m_ctx->client->start({"GET", QStringLiteral("/me/todo/lists?$top=100"), {}, {}, true, nullptr});
    connect(request, &GraphRequest::finished, this, [this, request] {
        const GraphResponse response = request->response();
        request->deleteLater();
        if (!response.ok()) {
            finishWithEnd(response.isNetworkError() ? ChainEnd::Network : ChainEnd::Failed, response.userMessage(), 0);
            return;
        }
        Store *store = m_ctx->store;
        QSet<QString> present;
        const QJsonArray lists = response.json().value(QLatin1String("value")).toArray();
        for (const QJsonValue &value : lists) {
            const QJsonObject list = value.toObject();
            const QString id = list.value(QLatin1String("id")).toString();
            present.insert(id);
            QString wellKnown = list.value(QLatin1String("wellknownListName")).toString();
            if (wellKnown == QLatin1String("none")) {
                wellKnown.clear();
            }
            store->upsertParent(ItemKind::Task, {0, id, list.value(QLatin1String("displayName")).toString(), wellKnown, {}, 0, 0, 0});
        }
        const QList<ParentRow> known = store->parents(ItemKind::Task);
        for (const ParentRow &list : known) {
            if (!present.contains(list.graphId)) {
                store->removeParent(ItemKind::Task, list.id);
            }
        }
        m_lists = store->parents(ItemKind::Task);
        syncList(0);
    });
}

void TodoSync::syncList(qsizetype index)
{
    if (index >= m_lists.size()) {
        finish({});
        return;
    }
    const ParentRow &list = m_lists.at(index);
    runItemChain(ItemKind::Task, list, QStringLiteral("/me/todo/lists/%1/tasks/delta").arg(list.graphId), {}, &Mapper::task, [this, index] {
        syncList(index + 1);
    });
}

}
