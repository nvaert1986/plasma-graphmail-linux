// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "blobstore.h"
#include "collectionsync.h"
#include "fakegraphserver.h"
#include "graphmapper.h"
#include "graphclient.h"
#include "throttle.h"
#include "tokenprovider.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTimeZone>

using namespace GraphMail;

namespace
{
class StaticTokens : public GraphCore::TokenProvider
{
public:
    QString accessToken(QString *, bool) override
    {
        return QStringLiteral("token");
    }
    QString cachedAccessToken() override
    {
        return QStringLiteral("token");
    }
};

QJsonObject msg(const QString &id, const QString &changeKey, const QString &received = QStringLiteral("2026-11-01T10:00:00Z"), bool read = false)
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("changeKey"), changeKey},
            {QStringLiteral("subject"), QString(QStringLiteral("Subject ") + id)},
            {QStringLiteral("receivedDateTime"), received},
            {QStringLiteral("isRead"), read},
            {QStringLiteral("bodyPreview"), QString(QStringLiteral("Preview of ") + id)},
            {QStringLiteral("from"),
             QJsonObject{{QStringLiteral("emailAddress"),
                          QJsonObject{{QStringLiteral("name"), QStringLiteral("Example Support")}, {QStringLiteral("address"), QStringLiteral("support@example.com")}}}}},
            {QStringLiteral("flag"), QJsonObject{{QStringLiteral("flagStatus"), QStringLiteral("notFlagged")}}}};
}

QJsonObject removed(const QString &id)
{
    return {{QStringLiteral("id"), id}, {QStringLiteral("@removed"), QJsonObject{{QStringLiteral("reason"), QStringLiteral("deleted")}}}};
}

FakeReply page(const QJsonArray &items, const QString &nextLink, const QString &deltaLink)
{
    QJsonObject body{{QStringLiteral("value"), items}};
    if (!nextLink.isEmpty()) {
        body.insert(QStringLiteral("@odata.nextLink"), nextLink);
    }
    if (!deltaLink.isEmpty()) {
        body.insert(QStringLiteral("@odata.deltaLink"), deltaLink);
    }
    return jsonReply(200, QJsonDocument(body).toJson(QJsonDocument::Compact));
}
}

class CollectionSyncTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir dir;
    std::unique_ptr<FakeGraphServer> server;
    StaticTokens tokens;
    std::unique_ptr<GraphCore::GraphClient> client;
    std::unique_ptr<Store> store;
    std::unique_ptr<BlobStore> blobs;
    SyncContext ctx;
    int counter = 0;

    QString url(const QString &path) const
    {
        return server->baseUrl() + path;
    }

    SyncResult runSync(CollectionSync &sync)
    {
        SyncResult result;
        QEventLoop loop;
        bool finished = false;
        sync.run([&](const SyncResult &r) {
            result = r;
            finished = true;
            loop.quit();
        });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        if (!finished) {
            loop.exec();
        }
        if (!finished) {
            qWarning() << "sync did not finish";
            result.outcome = SyncResult::Outcome::Failed;
            result.error = QStringLiteral("timeout");
        }
        return result;
    }

    QStringList graphIdsIn(qint64 folder)
    {
        QStringList ids;
        const QList<ListRow> rows = store->listPage(folder, std::nullopt, 1000);
        for (const ListRow &row : rows) {
            ids.append(store->message(row.id)->graphId);
        }
        ids.sort();
        return ids;
    }

private Q_SLOTS:
    void init()
    {
        GraphCore::Throttle::resetForTests();
        server = std::make_unique<FakeGraphServer>();
        client = std::make_unique<GraphCore::GraphClient>(&tokens);
        client->setBaseUrl(server->baseUrl());
        GraphCore::RetryPolicy policy;
        policy.maxAttempts = 2;
        policy.backoffBaseMs = 10;
        policy.maxBackoffMs = 20;
        client->setRetryPolicy(policy);
        store = std::make_unique<Store>();
        QString error;
        QVERIFY(store->open(dir.path() + QStringLiteral("/sync-%1.db").arg(++counter), Store::Mode::ReadWrite, &error));
        blobs = std::make_unique<BlobStore>(dir.path() + QStringLiteral("/blobs-%1").arg(counter));
        ctx = {};
        ctx.store = store.get();
        ctx.blobs = blobs.get();
        ctx.client = client.get();
        ctx.root = QStringLiteral("/me");
        ctx.now = [] {
            return QDateTime(QDate(2026, 11, 11), QTime(12, 0), QTimeZone::UTC).toMSecsSinceEpoch();
        };
    }

    void cleanup()
    {
        store.reset();
    }

    void mailFolderLifecycle()
    {
        const qint64 folder = store->upsertFolder({0, QStringLiteral("F1"), {}, QStringLiteral("Inbox"), {}, 0, 0});
        ctx.settings.pageBudget = 1;
        int fullStarts = 0;
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            const QString target = QString::fromUtf8(r.target);
            if (target.contains(QLatin1String("/me/mailFolders/F1/messages/delta"))) {
                if (target.contains(QLatin1String("filter"))) {
                    // The recent window: two pages
                    return page({msg(QStringLiteral("m1"), QStringLiteral("k1")), msg(QStringLiteral("m2"), QStringLiteral("k2"))}, url(QStringLiteral("/recent2")), {});
                }
                if (++fullStarts == 1) {
                    return page({msg(QStringLiteral("m1"), QStringLiteral("k1")), msg(QStringLiteral("m2"), QStringLiteral("k2")),
                                 msg(QStringLiteral("m3"), QStringLiteral("k3")), msg(QStringLiteral("m4"), QStringLiteral("k4"), QStringLiteral("2024-01-01T00:00:00Z"))},
                                url(QStringLiteral("/full2")), {});
                }
                // The resync after the expired state: only m1 and m2 still exist
                return page({msg(QStringLiteral("m1"), QStringLiteral("k1b"), QStringLiteral("2026-11-01T10:00:00Z"), true), msg(QStringLiteral("m2"), QStringLiteral("k2"))},
                            {}, url(QStringLiteral("/fullDelta3")));
            }
            if (target == QLatin1String("/v1.0/recent2")) {
                return page({msg(QStringLiteral("m3"), QStringLiteral("k3"))}, {}, url(QStringLiteral("/recentDelta1")));
            }
            if (target == QLatin1String("/v1.0/recentDelta1")) {
                // New mail while the full chain is still running
                return page({msg(QStringLiteral("m6"), QStringLiteral("k6"))}, {}, url(QStringLiteral("/recentDelta2")));
            }
            if (target == QLatin1String("/v1.0/full2")) {
                return page({msg(QStringLiteral("m5"), QStringLiteral("k5"))}, {}, url(QStringLiteral("/fullDelta1")));
            }
            if (target == QLatin1String("/v1.0/fullDelta1")) {
                return page({removed(QStringLiteral("m4")), msg(QStringLiteral("m1"), QStringLiteral("k1b"), QStringLiteral("2026-11-01T10:00:00Z"), true)}, {},
                            url(QStringLiteral("/fullDelta2")));
            }
            if (target == QLatin1String("/v1.0/fullDelta2")) {
                return jsonReply(410, R"({"error":{"code":"SyncStateNotFound","message":"expired"}})");
            }
            return jsonReply(404, R"({"error":{"code":"unexpected"}})");
        };

        MailFolderSync sync(&ctx, folder, QStringLiteral("F1"));
        // Run 1: the whole recent window, then one page (budget) of the full chain
        SyncResult result = runSync(sync);
        QCOMPARE(int(result.outcome), int(SyncResult::Outcome::More));
        QCOMPARE(graphIdsIn(folder), (QStringList{QStringLiteral("m1"), QStringLiteral("m2"), QStringLiteral("m3"), QStringLiteral("m4")}));
        QCOMPARE(store->syncState(folder).initialState, int(MailFolderSync::FullChain));
        // The recent window asked for a filtered first page at the big page size
        const FakeRequest &first = server->requests.first();
        QVERIFY(first.target.contains("filter=receivedDateTime"));
        QVERIFY(first.headers.value("prefer").contains("odata.maxpagesize=512"));
        QVERIFY(first.headers.value("prefer").contains("ImmutableId"));
        // The sender and the preview made it into the list row
        const ListRow row = store->listPage(folder, std::nullopt, 1).first();
        QCOMPARE(row.senderName, QStringLiteral("Example Support"));
        QVERIFY(row.preview.startsWith(QLatin1String("Preview of")));

        // Run 2: new mail through the recent link, then the rest of the full chain and its sweep
        result = runSync(sync);
        QCOMPARE(int(result.outcome), int(SyncResult::Outcome::Done));
        QCOMPARE(graphIdsIn(folder),
                 (QStringList{QStringLiteral("m1"), QStringLiteral("m2"), QStringLiteral("m3"), QStringLiteral("m4"), QStringLiteral("m5"), QStringLiteral("m6")}));
        QCOMPARE(store->syncState(folder).initialState, int(MailFolderSync::Incremental));
        QVERIFY(store->syncState(folder).recentDeltaLink.isEmpty());

        // Run 3: incremental: m4 removed, m1 read
        result = runSync(sync);
        QCOMPARE(int(result.outcome), int(SyncResult::Outcome::Done));
        QCOMPARE(result.changes, 2);
        QVERIFY(!graphIdsIn(folder).contains(QStringLiteral("m4")));
        QVERIFY(store->messageByGraphId(QStringLiteral("m1"))->isRead);
        QVERIFY(server->requests.constLast().headers.value("prefer").contains("odata.maxpagesize=100"));

        // Run 4: the sync state expired: a full resync removes what no longer exists
        result = runSync(sync);
        QCOMPARE(int(result.outcome), int(SyncResult::Outcome::Done));
        QCOMPARE(graphIdsIn(folder), (QStringList{QStringLiteral("m1"), QStringLiteral("m2")}));
        QCOMPARE(fullStarts, 2);
        QVERIFY(store->syncState(folder).deltaLink.endsWith(QLatin1String("/fullDelta3")));
    }

    void sparseDeltaItems()
    {
        // Graph sends only the changed properties for a read change: {id, parentFolderId, isRead}
        const qint64 folder = store->upsertFolder({0, QStringLiteral("F1"), {}, QStringLiteral("Inbox"), {}, 0, 0});
        auto sparse = [](const QString &id) {
            return QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("parentFolderId"), QStringLiteral("F1")}, {QStringLiteral("isRead"), true}};
        };
        bool wholeFetched = false;
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            const QString target = QString::fromUtf8(r.target);
            if (target.contains(QLatin1String("/me/mailFolders/F1/messages/delta"))) {
                return page({msg(QStringLiteral("m1"), QStringLiteral("k1"))}, {}, url(target.contains(QLatin1String("filter")) ? QStringLiteral("/recentDelta") : QStringLiteral("/fullDelta1")));
            }
            if (target == QLatin1String("/v1.0/fullDelta1")) {
                // m1 is cached; m9 is not
                return page({sparse(QStringLiteral("m1")), sparse(QStringLiteral("m9"))}, {}, url(QStringLiteral("/fullDelta2")));
            }
            if (target.startsWith(QLatin1String("/v1.0/me/messages/m9?"))) {
                wholeFetched = true;
                return jsonReply(200, QJsonDocument(msg(QStringLiteral("m9"), QStringLiteral("k9"))).toJson(QJsonDocument::Compact));
            }
            return page({}, {}, url(QStringLiteral("/fullDelta2")));
        };
        MailFolderSync sync(&ctx, folder, QStringLiteral("F1"));
        for (int run = 0; run < 3 && store->syncState(folder).initialState != MailFolderSync::Incremental; ++run) {
            QCOMPARE(int(runSync(sync).outcome) == int(SyncResult::Outcome::Failed), false);
        }
        QCOMPARE(store->syncState(folder).initialState, int(MailFolderSync::Incremental));
        QCOMPARE(int(runSync(sync).outcome), int(SyncResult::Outcome::Done));

        // m1: read now, everything else kept (it used to become a row without subject or date)
        const MessageRow m1 = *store->messageByGraphId(QStringLiteral("m1"));
        QVERIFY(m1.isRead);
        QCOMPARE(m1.subject, QStringLiteral("Subject m1"));
        QCOMPARE(m1.senderName, QStringLiteral("Example Support"));
        QVERIFY(m1.received > 0);
        // m9: fetched whole instead of stored from the sparse item
        QTRY_VERIFY(wholeFetched);
        QTRY_VERIFY(store->messageByGraphId(QStringLiteral("m9")).has_value());
        QCOMPARE(store->messageByGraphId(QStringLiteral("m9"))->subject, QStringLiteral("Subject m9"));

        // A second sparse change of the same message is written too (no change key to compare)
        server->handler = [&](const FakeRequest &) -> FakeReply {
            QJsonObject unread = sparse(QStringLiteral("m1"));
            unread.insert(QStringLiteral("isRead"), false);
            return page({unread}, {}, url(QStringLiteral("/fullDelta3")));
        };
        QCOMPARE(int(runSync(sync).outcome), int(SyncResult::Outcome::Done));
        QVERIFY(!store->messageByGraphId(QStringLiteral("m1"))->isRead);
        QCOMPARE(store->messageByGraphId(QStringLiteral("m1"))->subject, QStringLiteral("Subject m1"));
    }

    void draftLeavingDraftsDropsItsRecord()
    {
        // A draft deleted in Outlook: moved to Deleted Items, or gone from Drafts for good. Graph
        // Mail's saved record of it goes along; one still being sent stays
        const qint64 drafts = store->upsertFolder({0, QStringLiteral("FD"), {}, QStringLiteral("Drafts"), QStringLiteral("drafts"), 0, 0});
        const qint64 deleted = store->upsertFolder({0, QStringLiteral("FX"), {}, QStringLiteral("Deleted Items"), QStringLiteral("deleteditems"), 0, 0});
        const auto record = [this](const QString &uuid, const QString &draft, bool send) {
            Outgoing o;
            o.uuid = uuid;
            o.draftId = draft;
            o.state = send ? Outgoing::State::Pending : Outgoing::State::Saved;
            o.send = send;
            QVERIFY(store->saveOutgoing(o));
        };
        record(QStringLiteral("r1"), QStringLiteral("d1"), false);
        record(QStringLiteral("r2"), QStringLiteral("d2"), false);
        record(QStringLiteral("r3"), QStringLiteral("d3"), true);
        record(QStringLiteral("r4"), QStringLiteral("d4"), false);
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            const QString target = QString::fromUtf8(r.target);
            if (target.contains(QLatin1String("/mailFolders/FD/messages/delta"))) {
                // d4 is still a draft (updated); d2 and d3 left Drafts
                return page({msg(QStringLiteral("d4"), QStringLiteral("k4")), removed(QStringLiteral("d2")), removed(QStringLiteral("d3"))}, {}, url(QStringLiteral("/draftsDone")));
            }
            if (target.contains(QLatin1String("/mailFolders/FX/messages/delta"))) {
                return page({msg(QStringLiteral("d1"), QStringLiteral("k1"))}, {}, url(QStringLiteral("/deletedDone")));
            }
            return page({}, {}, url(QStringLiteral("/done")));
        };
        // d2/d3 are cached in Drafts first, so the removal is theirs to report
        QVERIFY(store->upsertMessage(Mapper::message(msg(QStringLiteral("d2"), QStringLiteral("k2")), drafts)) == Store::Upsert::Inserted);
        QVERIFY(store->upsertMessage(Mapper::message(msg(QStringLiteral("d3"), QStringLiteral("k3")), drafts)) == Store::Upsert::Inserted);
        MailFolderSync deletedSync(&ctx, deleted, QStringLiteral("FX"));
        QCOMPARE(int(runSync(deletedSync).outcome) == int(SyncResult::Outcome::Failed), false);
        MailFolderSync draftsSync(&ctx, drafts, QStringLiteral("FD"));
        QCOMPARE(int(runSync(draftsSync).outcome) == int(SyncResult::Outcome::Failed), false);
        QVERIFY(store->messageByGraphId(QStringLiteral("d1")).has_value());
        QVERIFY(!store->outgoing(QStringLiteral("r1")));
        QVERIFY(!store->outgoing(QStringLiteral("r2")));
        QVERIFY(store->outgoing(QStringLiteral("r3")));
        QVERIFY(store->outgoing(QStringLiteral("r4")));
    }

    void removalDuringAMoveBackIsSkipped()
    {
        // Clean Up (or Delete) moved m1 and m2 out of the folder, Undo moved them back at once. The
        // folder's delta still reports both as removed (the first move); m1's move back is queued,
        // so its row stays; m2 has no move waiting and goes
        const qint64 folder = store->upsertFolder({0, QStringLiteral("F1"), {}, QStringLiteral("Sent Items"), QStringLiteral("sentitems"), 0, 0});
        const qint64 deleted = store->upsertFolder({0, QStringLiteral("FX"), {}, QStringLiteral("Deleted Items"), QStringLiteral("deleteditems"), 0, 0});
        QVERIFY(store->upsertMessage(Mapper::message(msg(QStringLiteral("m1"), QStringLiteral("k1")), folder)) == Store::Upsert::Inserted);
        QVERIFY(store->upsertMessage(Mapper::message(msg(QStringLiteral("m2"), QStringLiteral("k2")), folder)) == Store::Upsert::Inserted);
        const qint64 row = store->messageByGraphId(QStringLiteral("m1"))->id;
        OperationRow back;
        back.kind = QStringLiteral("move");
        back.target = QStringLiteral("m1");
        back.payload = QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("value"), folder}, {QStringLiteral("previous"), deleted},
                                                                   {QStringLiteral("destination"), QStringLiteral("F1")}}).toJson(QJsonDocument::Compact));
        QVERIFY(store->addOperation(back) != 0);
        // An incremental round, as after the move out
        SyncState state;
        state.deltaLink = url(QStringLiteral("/sentIncremental"));
        state.initialState = MailFolderSync::Incremental;
        QVERIFY(store->setSyncState(folder, state));
        server->handler = [&](const FakeRequest &) -> FakeReply {
            return page({removed(QStringLiteral("m1")), removed(QStringLiteral("m2"))}, {}, url(QStringLiteral("/sentDone")));
        };
        MailFolderSync sync(&ctx, folder, QStringLiteral("F1"));
        QCOMPARE(int(runSync(sync).outcome) == int(SyncResult::Outcome::Failed), false);
        const std::optional<MessageRow> m1 = store->messageByGraphId(QStringLiteral("m1"));
        QVERIFY(m1.has_value());
        // The same row: the list keeps it, no new row after the next sync
        QCOMPARE(m1->id, row);
        QVERIFY(!store->messageByGraphId(QStringLiteral("m2")).has_value());
    }

    void folderTree()
    {
        int listings = 0;
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            const QString target = QString::fromUtf8(r.target);
            auto folder = [](const QString &id, const QString &name, int total, int unread, const QString &parent = QStringLiteral("ROOT")) {
                return QJsonObject{{QStringLiteral("id"), id},
                                   {QStringLiteral("displayName"), name},
                                   {QStringLiteral("parentFolderId"), parent},
                                   {QStringLiteral("totalItemCount"), total},
                                   {QStringLiteral("unreadItemCount"), unread}};
            };
            // Every run is a full listing (the delta link would not report count changes)
            if (target.startsWith(QLatin1String("/v1.0/me/mailFolders/delta"))) {
                switch (++listings) {
                case 1:
                    return page({folder(QStringLiteral("A"), QStringLiteral("Inbox"), 10, 2), folder(QStringLiteral("B"), QStringLiteral("Projects"), 5, 0),
                                 folder(QStringLiteral("C"), QStringLiteral("Old"), 1, 0, QStringLiteral("B"))},
                                {}, url(QStringLiteral("/tree1")));
                case 2:
                    return page({folder(QStringLiteral("A"), QStringLiteral("Inbox"), 10, 2), folder(QStringLiteral("B"), QStringLiteral("Projects"), 6, 1)}, {},
                                url(QStringLiteral("/tree2")));
                default:
                    return page({folder(QStringLiteral("A"), QStringLiteral("Inbox"), 11, 3)}, {}, url(QStringLiteral("/tree3")));
                }
            }
            if (target.startsWith(QLatin1String("/v1.0/me/mailFolders/inbox"))) {
                return jsonReply(200, R"({"id":"A"})");
            }
            // The alias names the default Archive; the mailbox setting another folder, which wins
            if (target.startsWith(QLatin1String("/v1.0/me/mailFolders/archive"))) {
                return jsonReply(200, R"({"id":"C"})");
            }
            if (target.startsWith(QLatin1String("/v1.0/me/mailboxSettings"))) {
                return jsonReply(200, R"({"archiveFolder":"B"})");
            }
            return jsonReply(404, R"({"error":{"code":"ErrorFolderNotFound"}})");
        };
        FolderTreeSync sync(&ctx);
        SyncResult result = runSync(sync);
        QCOMPARE(int(result.outcome), int(SyncResult::Outcome::Done));
        QCOMPARE(result.foldersAdded.size(), 3);
        QCOMPARE(store->folders().size(), 3);
        QCOMPARE(store->folderByWellKnown(QStringLiteral("inbox"))->graphId, QStringLiteral("A"));
        QCOMPARE(store->folderByGraphId(QStringLiteral("C"))->parentGraphId, QStringLiteral("B"));
        QCOMPARE(store->folderByWellKnown(QStringLiteral("archive"))->graphId, QStringLiteral("B"));
        QCOMPARE(store->folderByGraphId(QStringLiteral("C"))->wellKnown, QString());
        QCOMPARE(store->meta(QStringLiteral("well_known")), QStringLiteral("3"));

        // A count change (the case Graph's folder delta does not report) and a removal
        result = runSync(sync);
        const qint64 b = store->folderByGraphId(QStringLiteral("B"))->id;
        QCOMPARE(result.foldersChanged, QList<qint64>{b});
        QCOMPARE(store->folder(b)->unread, 1);
        QCOMPARE(result.foldersRemoved.size(), 1);
        QVERIFY(!store->folderByGraphId(QStringLiteral("C")).has_value());

        // B is gone from the listing
        result = runSync(sync);
        QCOMPARE(int(result.outcome), int(SyncResult::Outcome::Done));
        QVERIFY(!store->folderByGraphId(QStringLiteral("B")).has_value());
        QCOMPARE(store->folderByGraphId(QStringLiteral("A"))->unread, 3);
        // Well-known names were resolved only once
        int lookups = 0;
        for (const FakeRequest &r : std::as_const(server->requests)) {
            lookups += r.target.contains("/mailFolders/inbox") ? 1 : 0;
        }
        QCOMPARE(lookups, 1);
    }

    void calendarWindow()
    {
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            const QString target = QString::fromUtf8(r.target);
            auto event = [](const QString &id, const QString &start) {
                return QJsonObject{{QStringLiteral("id"), id},
                                   {QStringLiteral("changeKey"), QString(QStringLiteral("ck-") + id)},
                                   {QStringLiteral("subject"), QString(QStringLiteral("Meeting ") + id)},
                                   {QStringLiteral("start"), QJsonObject{{QStringLiteral("dateTime"), start}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}}},
                                   {QStringLiteral("end"), QJsonObject{{QStringLiteral("dateTime"), start}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}}},
                                   {QStringLiteral("showAs"), QStringLiteral("busy")},
                                   {QStringLiteral("type"), QStringLiteral("occurrence")}};
            };
            if (target.startsWith(QLatin1String("/v1.0/me/calendars?"))) {
                // The own calendar, one a colleague shared, and one whose share was taken back
                return jsonReply(200,
                                 R"({"value":[{"id":"CAL","name":"Calendar","isDefaultCalendar":true,"canEdit":true,"hexColor":"#058039","owner":{"address":"me@example.com"}},)"
                                 R"({"id":"SH","name":"Colleague","canEdit":false,"hexColor":"","owner":{"address":"colleague@example.com"}},)"
                                 R"({"id":"GONE","name":"Revoked","canEdit":false,"owner":{"address":"other@example.com"}}]})");
            }
            if (target.startsWith(QLatin1String("/v1.0/me/calendars/SH/calendarView/delta"))) {
                return page({event(QStringLiteral("s1"), QStringLiteral("2026-11-11T10:00:00.0000000"))}, {}, url(QStringLiteral("/sh1")));
            }
            if (target.startsWith(QLatin1String("/v1.0/me/calendars/GONE/calendarView/delta"))) {
                return jsonReply(403, R"({"error":{"code":"ErrorAccessDenied","message":"Access is denied."}})");
            }
            if (target == QLatin1String("/v1.0/sh1")) {
                return page({}, {}, url(QStringLiteral("/sh1")));
            }
            if (target.startsWith(QLatin1String("/v1.0/me/calendarView/delta"))) {
                return page({event(QStringLiteral("e1"), QStringLiteral("2026-11-11T14:00:00.0000000")),
                             event(QStringLiteral("e2"), QStringLiteral("2026-11-12T09:30:00.0000000"))},
                            {}, url(QStringLiteral("/cal1")));
            }
            if (target == QLatin1String("/v1.0/cal1")) {
                return page({removed(QStringLiteral("e1"))}, {}, url(QStringLiteral("/cal2")));
            }
            return jsonReply(404, "{}");
        };
        ParentItemSync sync(&ctx, ParentItemSync::Kind::Calendar);
        SyncResult result = runSync(sync);
        // The refused calendar does not stop the others
        QCOMPARE(int(result.outcome), int(SyncResult::Outcome::Done));
        QCOMPARE(store->itemCount(ItemKind::Event), 3);
        const QList<CalendarRow> calendars = store->calendars();
        QCOMPARE(calendars.size(), 3);
        QCOMPARE(calendars.first().graphId, QStringLiteral("CAL"));
        QVERIFY(calendars.first().isDefault);
        QCOMPARE(calendars.first().color, QStringLiteral("#058039"));
        QCOMPARE(calendars.at(1).owner, QStringLiteral("colleague@example.com"));
        QVERIFY(!calendars.at(1).canEdit);
        QVERIFY(calendars.at(1).color.isEmpty());
        const ParentRow calendar = store->parents(ItemKind::Event).first();
        QCOMPARE(calendar.graphId, QStringLiteral("CAL"));
        // The window: 90 days back, 365 ahead of "now"
        QCOMPARE(calendar.windowEnd - calendar.windowStart, qint64(455) * 24 * 3600 * 1000);
        const FakeRequest &view = server->requests.at(1);
        QVERIFY(view.target.contains("startDateTime=2026-08-13T12:00:00Z"));
        QVERIFY(view.headers.value("prefer").contains("outlook.timezone=\"UTC\""));

        result = runSync(sync);
        QCOMPARE(store->itemCount(ItemKind::Event), 2);
        QCOMPARE(result.changes, 1);
    }

    // Quick drags in the calendar (0.14.5): while changes of an event wait in the queue, a sync
    // brings Exchange's version so far; the cache keeps the waiting changes on top of it
    void calendarSyncKeepsQueuedChanges()
    {
        auto at = std::make_shared<QString>(QStringLiteral("2026-11-11T14:00:00.0000000"));
        server->handler = [&, at](const FakeRequest &r) -> FakeReply {
            const QString target = QString::fromUtf8(r.target);
            const QJsonObject event{{QStringLiteral("id"), QStringLiteral("e1")},
                                    {QStringLiteral("subject"), QStringLiteral("Planning")},
                                    {QStringLiteral("start"), QJsonObject{{QStringLiteral("dateTime"), *at}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}}},
                                    {QStringLiteral("end"), QJsonObject{{QStringLiteral("dateTime"), *at}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}}},
                                    {QStringLiteral("type"), QStringLiteral("singleInstance")}};
            if (target.startsWith(QLatin1String("/v1.0/me/calendars?"))) {
                return jsonReply(200, R"({"value":[{"id":"CAL","name":"Calendar","isDefaultCalendar":true,"canEdit":true,"owner":{"address":"me@example.com"}}]})");
            }
            if (target.startsWith(QLatin1String("/v1.0/me/calendarView/delta")) || target == QLatin1String("/v1.0/cal1")) {
                return page({event}, {}, url(QStringLiteral("/cal1")));
            }
            return jsonReply(404, "{}");
        };
        ParentItemSync sync(&ctx, ParentItemSync::Kind::Calendar);
        QCOMPARE(int(runSync(sync).outcome), int(SyncResult::Outcome::Done));
        const auto startOf = [this] {
            return QDateTime::fromMSecsSinceEpoch(store->eventByGraphId(QStringLiteral("e1"))->start, QTimeZone::UTC).time();
        };
        QCOMPARE(startOf(), QTime(14, 0));
        // Moved here twice; neither change has reached Exchange yet
        const auto moved = [](const QString &iso) {
            const QJsonObject time{{QStringLiteral("dateTime"), iso}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}};
            return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("changes"), QJsonObject{{QStringLiteral("start"), time}, {QStringLiteral("end"), time}}}}).toJson(QJsonDocument::Compact));
        };
        OperationRow first;
        first.kind = QStringLiteral("eventUpdate");
        first.target = QStringLiteral("e1");
        first.payload = moved(QStringLiteral("2026-11-11T15:00:00"));
        const qint64 firstId = store->addOperation(first);
        OperationRow second = first;
        second.payload = moved(QStringLiteral("2026-11-11T16:00:00"));
        store->addOperation(second);
        runSync(sync);
        QCOMPARE(startOf(), QTime(16, 0));
        // The first has arrived: still the last one on top
        store->removeOperation(firstId);
        *at = QStringLiteral("2026-11-11T15:00:00.0000000");
        runSync(sync);
        QCOMPARE(startOf(), QTime(16, 0));
        // Nothing waiting any more: Exchange's version as it is
        for (const OperationRow &op : store->allOperations()) {
            store->removeOperation(op.id);
        }
        *at = QStringLiteral("2026-11-11T17:00:00.0000000");
        runSync(sync);
        QCOMPARE(startOf(), QTime(17, 0));
        // Deleted here, the delete still on its way: the sync does not bring it back
        store->removeItem(ItemKind::Event, QStringLiteral("e1"));
        OperationRow deleting;
        deleting.kind = QStringLiteral("eventDelete");
        deleting.target = QStringLiteral("e1");
        deleting.payload = QStringLiteral("{}");
        store->addOperation(deleting);
        runSync(sync);
        QVERIFY(!store->eventByGraphId(QStringLiteral("e1")));
    }

    void occurrencesTakeTheirMastersDetails()
    {
        const qint64 calendar = store->upsertCalendar({0, QStringLiteral("CAL"), QStringLiteral("Calendar"), QStringLiteral("me@example.com"), {}, true, true});
        auto time = [](const QString &iso) {
            return QJsonObject{{QStringLiteral("dateTime"), iso}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}};
        };
        // calendarView/delta sends an occurrence with only its times; the master has the rest
        const QJsonObject master{{QStringLiteral("id"), QStringLiteral("M")},
                                 {QStringLiteral("changeKey"), QStringLiteral("k")},
                                 {QStringLiteral("type"), QStringLiteral("seriesMaster")},
                                 {QStringLiteral("subject"), QStringLiteral("Weekly")},
                                 {QStringLiteral("location"), QJsonObject{{QStringLiteral("displayName"), QStringLiteral("Room 1")}}},
                                 {QStringLiteral("start"), time(QStringLiteral("2026-01-05T09:00:00"))},
                                 {QStringLiteral("end"), time(QStringLiteral("2026-01-05T10:00:00"))},
                                 {QStringLiteral("isReminderOn"), true}};
        const QJsonObject occurrence{{QStringLiteral("id"), QStringLiteral("O")},
                                     {QStringLiteral("type"), QStringLiteral("occurrence")},
                                     {QStringLiteral("seriesMasterId"), QStringLiteral("M")},
                                     {QStringLiteral("start"), time(QStringLiteral("2026-11-16T09:00:00"))},
                                     {QStringLiteral("end"), time(QStringLiteral("2026-11-16T10:00:00"))}};
        QVERIFY(store->upsertItem(ItemKind::Event, calendar, Mapper::event(master)) == Store::Upsert::Inserted);
        QVERIFY(store->upsertItem(ItemKind::Event, calendar, Mapper::event(occurrence)) == Store::Upsert::Inserted);
        const qint64 day = QDateTime(QDate(2026, 11, 16), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
        const QList<EventRow> events = store->eventsBetween({}, day, day + 24 * 3600 * 1000);
        // The master itself is not shown, its occurrence is, with the master's details
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().graphId, QStringLiteral("O"));
        QCOMPARE(events.first().subject, QStringLiteral("Weekly"));
        QCOMPARE(events.first().location, QStringLiteral("Room 1"));
        QVERIFY(events.first().json.contains(QLatin1String("isReminderOn")));
        QCOMPARE(QDateTime::fromMSecsSinceEpoch(events.first().start, QTimeZone::UTC).time(), QTime(9, 0));
        // An event ending exactly at the start of the range is not in it
        QVERIFY(store->eventsBetween({}, day + 10 * 3600 * 1000, day + 11 * 3600 * 1000).isEmpty());

        // The series removed (the delta names only the series): its occurrence goes with it
        QVERIFY(store->removeItem(ItemKind::Event, QStringLiteral("M")));
        QVERIFY(!store->eventByGraphId(QStringLiteral("O")));
        // One left behind earlier (a series removed before that): swept after a complete round
        QJsonObject orphan = occurrence;
        orphan.insert(QStringLiteral("id"), QStringLiteral("O2"));
        orphan.insert(QStringLiteral("seriesMasterId"), QStringLiteral("GONE"));
        QVERIFY(store->upsertItem(ItemKind::Event, calendar, Mapper::event(orphan)) == Store::Upsert::Inserted);
        QVERIFY(store->upsertItem(ItemKind::Event, calendar, Mapper::event(master)) == Store::Upsert::Inserted);
        QVERIFY(store->upsertItem(ItemKind::Event, calendar, Mapper::event(occurrence)) == Store::Upsert::Inserted);
        QCOMPARE(store->sweepOrphanOccurrences(calendar), 1);
        QVERIFY(!store->eventByGraphId(QStringLiteral("O2")));
        QVERIFY(store->eventByGraphId(QStringLiteral("O")));
    }

    void todoLists()
    {
        server->handler = [&](const FakeRequest &r) -> FakeReply {
            const QString target = QString::fromUtf8(r.target);
            if (target.startsWith(QLatin1String("/v1.0/me/todo/lists?"))) {
                return jsonReply(200, R"({"value":[{"id":"L1","displayName":"Tasks","wellknownListName":"defaultList"},
                                                   {"id":"L2","displayName":"Flagged Emails","wellknownListName":"flaggedEmails"}]})");
            }
            if (target.startsWith(QLatin1String("/v1.0/me/todo/lists/L1/tasks/delta"))) {
                return page({QJsonObject{{QStringLiteral("id"), QStringLiteral("t1")},
                                         {QStringLiteral("title"), QStringLiteral("Write the design")},
                                         {QStringLiteral("status"), QStringLiteral("completed")},
                                         {QStringLiteral("lastModifiedDateTime"), QStringLiteral("2026-11-11T08:00:00Z")}}},
                            {}, url(QStringLiteral("/todo1")));
            }
            if (target.startsWith(QLatin1String("/v1.0/me/todo/lists/L2/tasks/delta"))) {
                return page({}, {}, url(QStringLiteral("/todo2")));
            }
            return jsonReply(404, "{}");
        };
        TodoSync sync(&ctx);
        const SyncResult result = runSync(sync);
        QCOMPARE(int(result.outcome), int(SyncResult::Outcome::Done));
        const QList<ParentRow> lists = store->parents(ItemKind::Task);
        QCOMPARE(lists.size(), 2);
        QCOMPARE(lists.first().wellKnown, QStringLiteral("defaultList"));
        QCOMPARE(store->itemCount(ItemKind::Task), 1);
    }

    void sharedMailboxWithoutAccess()
    {
        ctx.root = QStringLiteral("/users/abc");
        const qint64 folder = store->upsertFolder({0, QStringLiteral("F1"), {}, QStringLiteral("Inbox"), {}, 0, 0});
        server->handler = [](const FakeRequest &) {
            return jsonReply(404, R"({"error":{"code":"ErrorItemNotFound","message":"The specified object was not found in the store., Default folder Root not found."}})");
        };
        MailFolderSync sync(&ctx, folder, QStringLiteral("F1"));
        QCOMPARE(int(runSync(sync).outcome), int(SyncResult::Outcome::AccessDenied));
        QVERIFY(server->requests.first().target.startsWith("/v1.0/users/abc/"));
    }

    // A colleague's calendar mailbox whose share was taken away: the listing answers 404
    // ErrorItemNotFound (docs/PHASE7.md), a refusal; the own mailbox keeps treating it as an error
    void calendarShareTakenAway()
    {
        server->handler = [](const FakeRequest &) {
            return jsonReply(404, R"({"error":{"code":"ErrorItemNotFound","message":"The specified object was not found in the store."}})");
        };
        ctx.root = QStringLiteral("/users/abc");
        store->upsertParent(ItemKind::Event, {0, QStringLiteral("C1"), QStringLiteral("Calendar"), {}, {}, 0, 0, 0});
        ParentItemSync shared(&ctx, ParentItemSync::Kind::Calendar);
        QCOMPARE(int(runSync(shared).outcome), int(SyncResult::Outcome::AccessDenied));
        ctx.root = QStringLiteral("/me");
        ParentItemSync own(&ctx, ParentItemSync::Kind::Calendar);
        QCOMPARE(int(runSync(own).outcome), int(SyncResult::Outcome::Failed));
    }

    void networkFailure()
    {
        const qint64 folder = store->upsertFolder({0, QStringLiteral("F1"), {}, QStringLiteral("Inbox"), {}, 0, 0});
        server->handler = [](const FakeRequest &) {
            FakeReply reply;
            reply.drop = true;
            return reply;
        };
        MailFolderSync sync(&ctx, folder, QStringLiteral("F1"));
        QCOMPARE(int(runSync(sync).outcome), int(SyncResult::Outcome::NetworkError));
        // Nothing is lost: the next run continues with the recent window
        QCOMPARE(store->syncState(folder).initialState, int(MailFolderSync::RecentWindow));
    }
};

QTEST_GUILESS_MAIN(CollectionSyncTest)

#include "collectionsynctest.moc"
