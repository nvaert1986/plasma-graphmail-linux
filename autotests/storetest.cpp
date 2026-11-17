// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "searchquery.h"
#include "store.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

using namespace GraphMail;

namespace
{
MessageRow makeMessage(const QString &graphId, qint64 folder, qint64 received, const QString &subject = QStringLiteral("Subject"))
{
    MessageRow m;
    m.graphId = graphId;
    m.folder = folder;
    m.received = received;
    m.sent = received - 1000;
    m.subject = subject;
    m.senderName = QStringLiteral("Sender Name");
    m.senderAddress = QStringLiteral("sender@example.com");
    m.preview = QStringLiteral("A preview line");
    m.changeKey = QStringLiteral("ck-") + graphId;
    m.conversation = QStringLiteral("conv-1");
    return m;
}
}

class StoreTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir dir;
    Store store;
    QString path;
    qint64 inbox = 0;
    qint64 archive = 0;
    int counter = 0;

    QString freshPath()
    {
        return dir.path() + QStringLiteral("/cache-%1.db").arg(++counter);
    }

private Q_SLOTS:
    void init()
    {
        QString error;
        path = freshPath();
        QVERIFY2(store.open(path, Store::Mode::ReadWrite, &error), qPrintable(error));
        inbox = store.upsertFolder({0, QStringLiteral("f-inbox"), {}, QStringLiteral("Inbox"), QStringLiteral("inbox"), 0, 0});
        archive = store.upsertFolder({0, QStringLiteral("f-archive"), {}, QStringLiteral("Archive"), {}, 0, 0});
        QVERIFY(inbox > 0 && archive > 0);
    }

    void cleanup()
    {
        store.close();
    }

    void freshDatabaseHasCurrentSchema()
    {
        QCOMPARE(store.version(), Store::schemaVersion);
        QVERIFY(store.setMeta(QStringLiteral("account"), QStringLiteral("abc")));
        QCOMPARE(store.meta(QStringLiteral("account")), QStringLiteral("abc"));
        QVERIFY(store.setMeta(QStringLiteral("account"), QStringLiteral("def")));
        QCOMPARE(store.meta(QStringLiteral("account")), QStringLiteral("def"));
    }

    void migratesAnOlderDatabase()
    {
        const QString older = freshPath();
        QString error;
        {
            Store v1;
            QVERIFY(v1.open(older, Store::Mode::ReadWrite, &error));
            QVERIFY(v1.upsertFolder({0, QStringLiteral("f"), {}, QStringLiteral("F"), {}, 0, 0}) > 0);
        }
        // A later program version adds a step; the data survives
        QList<QStringList> steps = Store::migrations();
        steps.append({QStringLiteral("ALTER TABLE folders ADD COLUMN color TEXT"), QStringLiteral("UPDATE folders SET color = 'red'")});
        Store v2;
        QVERIFY2(v2.open(older, Store::Mode::ReadWrite, &error, &steps), qPrintable(error));
        QCOMPARE(v2.version(), Store::schemaVersion + 1);
        QCOMPARE(v2.folders().size(), 1);
        v2.close();

        // The older program refuses the newer database instead of damaging it
        Store old;
        QVERIFY(!old.open(older, Store::Mode::ReadWrite, &error));
        QVERIFY(error.contains(QLatin1String("newer version")));
        // A read-only reader needs exactly its own version
        QVERIFY(!old.open(older, Store::Mode::ReadOnly, &error));
    }

    void failedMigrationRollsBack()
    {
        const QString older = freshPath();
        QList<QStringList> steps = Store::migrations();
        steps.append({QStringLiteral("CREATE TABLE half(x)"), QStringLiteral("THIS IS NOT SQL")});
        Store broken;
        QString error;
        QVERIFY(!broken.open(older, Store::Mode::ReadWrite, &error, &steps));
        QVERIFY(error.contains(QStringLiteral("migration to schema %1").arg(Store::schemaVersion + 1)));
        // The current version stays intact, without the half-created table
        Store ok;
        QVERIFY(ok.open(older, Store::Mode::ReadWrite, &error));
        QCOMPARE(ok.version(), Store::schemaVersion);
    }

    void setReadMovesTheUnreadCount()
    {
        Store store;
        QString error;
        QVERIFY(store.open(freshPath(), Store::Mode::ReadWrite, &error));
        const qint64 folder = store.upsertFolder({0, QStringLiteral("F"), {}, QStringLiteral("Inbox"), QStringLiteral("inbox"), 10, 3});
        MessageRow m;
        m.graphId = QStringLiteral("m");
        m.folder = folder;
        m.changeKey = QStringLiteral("k");
        QVERIFY(store.upsertMessage(m) == Store::Upsert::Inserted);
        const qint64 id = store.messageByGraphId(QStringLiteral("m"))->id;
        QVERIFY(store.setRead(id, true));
        QCOMPARE(store.folder(folder)->unread, 2);
        // Setting what it already is changes nothing
        QVERIFY(store.setRead(id, true));
        QCOMPARE(store.folder(folder)->unread, 2);
        QVERIFY(store.setRead(id, false));
        QCOMPARE(store.folder(folder)->unread, 3);
    }

    void phase2CacheGetsTheOutgoingTable()
    {
        // A cache written by 0.3.0 (schema 1) opens with the outgoing table added, data kept
        const QString path = freshPath();
        const QList<QStringList> v1{Store::migrations().first()};
        QString error;
        {
            Store old;
            QVERIFY(old.open(path, Store::Mode::ReadWrite, &error, &v1));
            QCOMPARE(old.version(), 1);
            QVERIFY(old.upsertFolder({0, QStringLiteral("F"), {}, QStringLiteral("Inbox"), QStringLiteral("inbox"), 0, 0}) > 0);
        }
        Store store;
        QVERIFY(store.open(path, Store::Mode::ReadWrite, &error));
        QCOMPARE(store.version(), Store::schemaVersion);
        QCOMPARE(store.folders().size(), 1);
        Outgoing o;
        o.uuid = QStringLiteral("x");
        o.draftId = QStringLiteral("d");
        QVERIFY(store.saveOutgoing(o));
        QCOMPARE(store.outgoingForDraft(QStringLiteral("d"))->uuid, QStringLiteral("x"));
        QCOMPARE(store.dueOutgoing(0).size(), 1);
        QVERIFY(store.removeOutgoing(QStringLiteral("x")));
        QVERIFY(store.allOutgoing().isEmpty());
    }

    void draftLeavingDraftsDropsItsRecords()
    {
        Store store;
        QString error;
        QVERIFY(store.open(freshPath(), Store::Mode::ReadWrite, &error));
        const qint64 drafts = store.upsertFolder({0, QStringLiteral("f-drafts"), {}, QStringLiteral("Drafts"), QStringLiteral("drafts"), 0, 0});
        const qint64 deleted = store.upsertFolder({0, QStringLiteral("f-deleted"), {}, QStringLiteral("Deleted Items"), QStringLiteral("deleteditems"), 0, 0});
        const auto record = [&store](const QString &uuid, const QString &draft, Outgoing::State state, bool send) {
            Outgoing o;
            o.uuid = uuid;
            o.draftId = draft;
            o.state = state;
            o.send = send;
            QVERIFY(store.saveOutgoing(o));
        };
        // d1 stays in Drafts; d2 was deleted in Outlook (now in Deleted Items)
        QVERIFY(store.upsertMessage(makeMessage(QStringLiteral("d1"), drafts, 1000)) == Store::Upsert::Inserted);
        QVERIFY(store.upsertMessage(makeMessage(QStringLiteral("d2"), deleted, 1000)) == Store::Upsert::Inserted);
        record(QStringLiteral("kept-in-drafts"), QStringLiteral("d1"), Outgoing::State::Saved, false);
        record(QStringLiteral("saved"), QStringLiteral("d2"), Outgoing::State::Saved, false);
        record(QStringLiteral("failed-save"), QStringLiteral("d2"), Outgoing::State::Failed, false);
        // Being written or sent: kept, the engine finishes or reports them
        record(QStringLiteral("pending"), QStringLiteral("d2"), Outgoing::State::Pending, false);
        record(QStringLiteral("failed-send"), QStringLiteral("d2"), Outgoing::State::Failed, true);
        QCOMPARE(store.dropStaleDraftRecords(drafts), 2);
        QStringList left;
        for (const Outgoing &o : store.allOutgoing()) {
            left.append(o.uuid);
        }
        left.sort();
        QCOMPARE(left, QStringList({QStringLiteral("failed-send"), QStringLiteral("kept-in-drafts"), QStringLiteral("pending")}));
        // A draft gone for good (not in the cache at all) is dropped by its id
        record(QStringLiteral("gone"), QStringLiteral("d3"), Outgoing::State::Saved, false);
        QCOMPARE(store.dropDraftRecords(QStringLiteral("d3")), 1);
        QCOMPARE(store.dropDraftRecords(QString()), 0);
        QCOMPARE(store.allOutgoing().size(), 3);
    }

    void readOnlyReaderSeesTheWriter()
    {
        QVERIFY(store.upsertMessage(makeMessage(QStringLiteral("m1"), inbox, 1000)) == Store::Upsert::Inserted);
        Store reader;
        QString error;
        QVERIFY2(reader.open(path, Store::Mode::ReadOnly, &error), qPrintable(error));
        QCOMPARE(reader.listPage(inbox, std::nullopt, 10).size(), 1);
        // Writes from the reader fail
        QVERIFY(!reader.setMeta(QStringLiteral("x"), QStringLiteral("y")));
        // New writes show up for the reader
        QVERIFY(store.upsertMessage(makeMessage(QStringLiteral("m2"), inbox, 2000)) == Store::Upsert::Inserted);
        QCOMPARE(reader.listPage(inbox, std::nullopt, 10).size(), 2);
    }

    void upsertInsertUpdateUnchanged()
    {
        MessageRow m = makeMessage(QStringLiteral("m1"), inbox, 1000);
        m.categories = {QStringLiteral("Red category"), QStringLiteral("Blue")};
        QCOMPARE(store.upsertMessage(m), Store::Upsert::Inserted);
        QCOMPARE(store.upsertMessage(m), Store::Upsert::Unchanged);

        const qint64 id = store.messageByGraphId(QStringLiteral("m1"))->id;
        QVERIFY(store.setBody(id, QStringLiteral("0123456789abcdef0123456789abcdef"), 1, 4321));

        m.changeKey = QStringLiteral("ck-new");
        m.isRead = true;
        m.subject = QStringLiteral("Changed subject");
        QCOMPARE(store.upsertMessage(m), Store::Upsert::Updated);
        const MessageRow stored = store.message(id).value();
        QCOMPARE(stored.subject, QStringLiteral("Changed subject"));
        QVERIFY(stored.isRead);
        QCOMPARE(stored.categories, m.categories);
        // An update from Graph never drops the cached body
        QCOMPARE(stored.bodyBlob, QStringLiteral("0123456789abcdef0123456789abcdef"));
        QCOMPARE(stored.bodySize, 4321);
    }

    void moveRule()
    {
        QCOMPARE(store.upsertMessage(makeMessage(QStringLiteral("m1"), inbox, 1000)), Store::Upsert::Inserted);
        // The archive's chain reports the moved message first (same immutable id, same change key)
        QCOMPARE(store.upsertMessage(makeMessage(QStringLiteral("m1"), archive, 1000)), Store::Upsert::Updated);
        QCOMPARE(store.messageByGraphId(QStringLiteral("m1"))->folder, archive);
        // Then the inbox's chain reports it removed: it must stay
        QVERIFY(!store.removeMessage(QStringLiteral("m1"), inbox).removed);
        QVERIFY(store.messageByGraphId(QStringLiteral("m1")).has_value());
        // A removal from the folder it is in deletes it
        QVERIFY(store.removeMessage(QStringLiteral("m1"), archive).removed);
        QVERIFY(!store.messageByGraphId(QStringLiteral("m1")).has_value());
        QVERIFY(!store.removeMessage(QStringLiteral("unknown")).removed);
    }

    void removalReturnsBlobs()
    {
        store.upsertMessage(makeMessage(QStringLiteral("m1"), inbox, 1000));
        const qint64 id = store.messageByGraphId(QStringLiteral("m1"))->id;
        store.setBody(id, QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 1, 10);
        {
            // An attachment row as the prefetcher will add it (step 2.5), written through a second
            // connection to the same file
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("attachments-test"));
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery insert(db);
            insert.prepare(QStringLiteral("INSERT INTO attachments(message, name, blob) VALUES (?, 'a.pdf', 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb')"));
            insert.addBindValue(id);
            QVERIFY(insert.exec());
        }
        QSqlDatabase::removeDatabase(QStringLiteral("attachments-test"));
        QStringList referenced = store.referencedBlobs();
        referenced.sort();
        QCOMPARE(referenced, (QStringList{QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")}));

        RemovedMessage removed = store.removeMessage(QStringLiteral("m1"));
        QVERIFY(removed.removed);
        removed.blobs.sort();
        QCOMPARE(removed.blobs, referenced);
        // The attachment row went with the message
        QVERIFY(store.referencedBlobs().isEmpty());
    }

    void keysetPagingWithTies()
    {
        // 25 messages, several with the same received time
        for (int i = 0; i < 25; ++i) {
            store.upsertMessage(makeMessage(QStringLiteral("m%1").arg(i), inbox, 1000 + (i / 3) * 10));
        }
        QList<qint64> seen;
        std::optional<PageCursor> cursor;
        for (;;) {
            const QList<ListRow> page = store.listPage(inbox, cursor, 7);
            if (page.isEmpty()) {
                break;
            }
            for (const ListRow &row : page) {
                QVERIFY(!seen.contains(row.id));
                if (!seen.isEmpty()) {
                    const MessageRow previous = store.message(seen.last()).value();
                    QVERIFY(previous.received > row.received || (previous.received == row.received && previous.id > row.id));
                }
                seen.append(row.id);
            }
            cursor = PageCursor{page.last().received, page.last().id};
        }
        QCOMPARE(seen.size(), 25);
        QVERIFY(store.listPage(archive, std::nullopt, 10).isEmpty());
    }

    // Focused Inbox (docs/PHASE9.md): the tabs page only their messages, flat and by conversation
    void focusedAndOther()
    {
        for (int i = 0; i < 6; ++i) {
            MessageRow m = makeMessage(QStringLiteral("f%1").arg(i), inbox, 1000 + i);
            m.conversation = QStringLiteral("c%1").arg(i % 2);
            m.focused = i < 4;
            store.upsertMessage(m);
        }
        QCOMPARE(store.listPage(inbox, std::nullopt, 50).size(), 6);
        QCOMPARE(store.listPage(inbox, std::nullopt, 50, 1).size(), 4);
        const QList<ListRow> other = store.listPage(inbox, std::nullopt, 50, 0);
        QCOMPARE(other.size(), 2);
        QCOMPARE(store.message(other.first().id)->graphId, QStringLiteral("f5"));
        // Paging keeps the filter
        const QList<ListRow> second = store.listPage(inbox, PageCursor{other.first().received, other.first().id}, 50, 0);
        QCOMPARE(second.size(), 1);
        // Conversations: c1 has f1, f3 (Focused) and f5 (Other)
        const QList<ConversationRow> focusedConversations = store.conversationPage(inbox, std::nullopt, 50, 1);
        QCOMPARE(focusedConversations.size(), 2);
        const QList<ConversationRow> otherConversations = store.conversationPage(inbox, std::nullopt, 50, 0);
        QCOMPARE(otherConversations.size(), 2);
        QCOMPARE(store.conversationMessages(inbox, QStringLiteral("c1"), 1).size(), 2);
        QCOMPARE(store.conversationMessages(inbox, QStringLiteral("c1"), -1).size(), 3);
        // Moved to Other here; Graph's next update from an old delta link does not carry the field
        // and must not move it back
        const qint64 f0 = store.messageByGraphId(QStringLiteral("f0"))->id;
        QVERIFY(store.setFocused(f0, false));
        MessageRow update = makeMessage(QStringLiteral("f0"), inbox, 1000);
        update.conversation = QStringLiteral("c0");
        update.changeKey = QStringLiteral("new");
        update.focused = true;
        update.focusKnown = false;
        QCOMPARE(store.upsertMessage(update), Store::Upsert::Updated);
        QVERIFY(!store.message(f0)->focused);
        // One that carries it wins, also with the same change key (the Inbox's fresh chain)
        update.focusKnown = true;
        QCOMPARE(store.upsertMessage(update), Store::Upsert::Updated);
        QVERIFY(store.message(f0)->focused);
        QCOMPARE(store.upsertMessage(update), Store::Upsert::Unchanged);
    }

    void searchFindsWordsSendersAndPrefixes()
    {
        MessageRow a = makeMessage(QStringLiteral("a"), inbox, 3000, QStringLiteral("Acme ticket closure request"));
        a.senderName = QStringLiteral("Example Support");
        MessageRow b = makeMessage(QStringLiteral("b"), archive, 2000, QStringLiteral("Lunch at the café"));
        b.preview = QStringLiteral("Shall we meet at twelve?");
        MessageRow c = makeMessage(QStringLiteral("c"), inbox, 1000, QStringLiteral("Quarterly report \"draft\""));
        for (const MessageRow &m : {a, b, c}) {
            QCOMPARE(store.upsertMessage(m), Store::Upsert::Inserted);
        }
        auto subjects = [this](const QString &text, qint64 folder = 0) {
            QStringList result;
            const QList<ListRow> rows = store.search(text, folder, 50);
            for (const ListRow &row : rows) {
                result.append(row.subject);
            }
            return result;
        };
        QCOMPARE(subjects(QStringLiteral("ticket")), QStringList{a.subject});
        QCOMPARE(subjects(QStringLiteral("acm")), QStringList{a.subject}); // prefix while typing
        QCOMPARE(subjects(QStringLiteral("acm ")), QStringList{}); // a finished word is not a prefix
        QCOMPARE(subjects(QStringLiteral("support closure")), QStringList{a.subject}); // sender + subject, both required
        QCOMPARE(subjects(QStringLiteral("cafe")), QStringList{b.subject}); // diacritics ignored
        QCOMPARE(subjects(QStringLiteral("twelve")), QStringList{b.subject}); // preview
        QCOMPARE(subjects(QStringLiteral("subject"), archive).size(), 0);
        QCOMPARE(subjects(QStringLiteral("nowhere")).size(), 0);
        // One letter while typing is a prefix of words in every preview ("A preview line")
        QCOMPARE(subjects(QStringLiteral("a")).size(), 3);
        // Newest first
        QCOMPARE(subjects(QStringLiteral("sender")), (QStringList{a.subject, b.subject, c.subject}));
        // FTS5 syntax typed by the user is only text, never an error
        for (const QString &tricky : {QStringLiteral("\"draft"), QStringLiteral("NOT report"), QStringLiteral("report AND"), QStringLiteral("-"),
                                      QStringLiteral("subject:x"), QStringLiteral("(report"), QStringLiteral("*"), QStringLiteral("NEAR(a b)")}) {
            store.search(tricky, 0, 10);
            QVERIFY2(store.lastError().isEmpty() || !store.lastError().startsWith(QLatin1String("search")), qPrintable(tricky + store.lastError()));
        }
        QCOMPARE(subjects(QStringLiteral("\"draft")), QStringList{c.subject});
    }

    void searchFollowsUpdatesAndDeletes()
    {
        store.upsertMessage(makeMessage(QStringLiteral("m1"), inbox, 1000, QStringLiteral("Original words")));
        QCOMPARE(store.search(QStringLiteral("original"), 0, 10).size(), 1);
        MessageRow changed = makeMessage(QStringLiteral("m1"), inbox, 1000, QStringLiteral("Replacement text"));
        changed.changeKey = QStringLiteral("ck-2");
        QCOMPARE(store.upsertMessage(changed), Store::Upsert::Updated);
        QCOMPARE(store.search(QStringLiteral("original"), 0, 10).size(), 0);
        QCOMPARE(store.search(QStringLiteral("replacement"), 0, 10).size(), 1);
        store.removeMessage(QStringLiteral("m1"));
        QCOMPARE(store.search(QStringLiteral("replacement"), 0, 10).size(), 0);
    }

    // Phase 8: one syntax for local and online search
    void searchQuerySyntax()
    {
        SearchQuery q = SearchQuery::parse(QStringLiteral("from:jan subject:\"quarterly report\" invoice hasattachments:yes isread:no received>=2026-11-01"));
        QCOMPARE(q.words, QStringList{QStringLiteral("invoice")});
        QCOMPARE(q.fields.value(QStringLiteral("from")), QStringList{QStringLiteral("jan")});
        QCOMPARE(q.fields.value(QStringLiteral("subject")), QStringList{QStringLiteral("quarterly report")});
        QCOMPARE(q.attachments, std::optional<bool>(true));
        QCOMPARE(q.unread, std::optional<bool>(true));
        QCOMPARE(q.receivedFrom, QDateTime(QDate(2026, 11, 1), QTime(0, 0)).toMSecsSinceEpoch());
        QVERIFY(!q.lastIsPrefix);
        QCOMPARE(q.kql(), QStringLiteral("invoice AND from:jan AND subject:\"quarterly report\" AND hasattachments:true AND received>=2026-11-01"));
        QVERIFY(q.fts().contains(QStringLiteral("{sender_name sender_address} : \"jan\"")));
        // Typing: the last word is a prefix; unknown keys and URLs are plain words
        q = SearchQuery::parse(QStringLiteral("re:status http://x.example invo"));
        QCOMPARE(q.words, (QStringList{QStringLiteral("re:status"), QStringLiteral("http://x.example"), QStringLiteral("invo")}));
        QVERIFY(q.lastIsPrefix);
        QVERIFY(q.fts().endsWith(QStringLiteral("\"invo\"*")));
        // Only filters: no MATCH at all
        q = SearchQuery::parse(QStringLiteral("isflagged:yes"));
        QVERIFY(q.fts().isEmpty());
        QVERIFY(!q.isEmpty());
        QVERIFY(SearchQuery::parse(QStringLiteral("  - ")).isEmpty());
        // category: and attachment: (Graph: category:, attachment:)
        q = SearchQuery::parse(QStringLiteral("category:\"Red category\" attachment:report"));
        QCOMPARE(q.categories, QStringList{QStringLiteral("Red category")});
        QCOMPARE(q.attachmentNames, QStringList{QStringLiteral("report")});
        QCOMPARE(q.kql(), QStringLiteral("category:\"Red category\" AND attachment:report"));
        // A yes/no keyword without a value means yes ("hasattachment:" as typed)
        QCOMPARE(SearchQuery::parse(QStringLiteral("proxmox hasattachment:")).attachments, std::optional<bool>(true));
        QCOMPARE(SearchQuery::parse(QStringLiteral("proxmox hasattachment:")).words, QStringList{QStringLiteral("proxmox")});
    }

    void bodyTextFromHtml()
    {
        const QByteArray html = "<html><head><style>p{color:red}</style><title>t</title></head><body><!-- x --><p>Caf&eacute; &amp; "
                                "&#8364;5&nbsp;<b>bold</b></p><script>var a=1;</script>&lt;tag&gt;</body></html>";
        QCOMPARE(bodyText(html, true), QStringLiteral("Café & \u20AC5 bold <tag>"));
        // Named entities are decoded once: "&amp;lt;" stays the text "&lt;"
        QCOMPARE(bodyText("&euml;&amp;lt;&unknown;", true), QStringLiteral("ë&lt;&unknown;"));
        QCOMPARE(bodyText("  plain\n\ntext  ", false), QStringLiteral("plain text"));
        QCOMPARE(bodyText(QByteArray(100, 'a'), false, 10).size(), 10);
    }

    // Body text and recipients are searchable while the body is offline; filters and folders
    void searchBodiesRecipientsAndFilters()
    {
        MessageRow a = makeMessage(QStringLiteral("a"), inbox, 3000, QStringLiteral("Meeting notes"));
        a.toList = QStringLiteral("Jan Jansen <jan@example.com>");
        a.isRead = false;
        a.hasAttachments = true;
        MessageRow b = makeMessage(QStringLiteral("b"), archive, 2000, QStringLiteral("Other"));
        b.isRead = true;
        b.categories = QStringList{QStringLiteral("Blue")};
        for (const MessageRow &m : {a, b}) {
            QCOMPARE(store.upsertMessage(m), Store::Upsert::Inserted);
        }
        const qint64 idA = store.messageByGraphId(QStringLiteral("a"))->id;
        const qint64 idB = store.messageByGraphId(QStringLiteral("b"))->id;
        QVERIFY(store.setBody(idB, QStringLiteral("blob-b"), 1, 100, QStringLiteral("the firewall migration plan")));
        auto ids = [this](const QString &text, const QList<qint64> &folders = {}) {
            QList<qint64> result;
            const QList<ListRow> rows = store.search(SearchQuery::parse(text), folders, 50);
            for (const ListRow &row : rows) {
                result.append(row.id);
            }
            return result;
        };
        QCOMPARE(ids(QStringLiteral("migration ")), QList<qint64>{idB});
        QCOMPARE(ids(QStringLiteral("body:firewall ")), QList<qint64>{idB});
        QCOMPARE(ids(QStringLiteral("subject:firewall ")), QList<qint64>{});
        QCOMPARE(ids(QStringLiteral("to:jansen ")), QList<qint64>{idA});
        QCOMPARE(ids(QStringLiteral("isread:no")), QList<qint64>{idA});
        QCOMPARE(ids(QStringLiteral("hasattachments:yes")), QList<qint64>{idA});
        QCOMPARE(ids(QStringLiteral("category:Blue")), QList<qint64>{idB});
        QCOMPARE(ids(QStringLiteral("category:Bl")), QList<qint64>{}); // a whole category name, not part of one
        QCOMPARE(ids(QStringLiteral("sender "), {archive}), QList<qint64>{idB});
        QCOMPARE(store.search(SearchQuery::parse(QStringLiteral("sender ")), {}, 50).first().folder, inbox);
        // Graph's results: only these rows, still filtered
        QCOMPARE(store.search(SearchQuery::parse(QStringLiteral("isread:no")), {}, 50, {idA, idB}).size(), 1);
        QCOMPARE(store.messageIdsByGraphIds({QStringLiteral("a"), QStringLiteral("nope")}).value(QStringLiteral("a")), idA);
        // The body leaves the window: its text leaves the index
        store.sweepBodies(5000);
        QCOMPARE(ids(QStringLiteral("migration ")), QList<qint64>{});
    }

    void bodiesStoredBeforeSchema4GetTheirText()
    {
        store.upsertMessage(makeMessage(QStringLiteral("old"), inbox, 1000));
        const qint64 id = store.messageByGraphId(QStringLiteral("old"))->id;
        {
            // A body as schema 3 stored it, written through a second connection
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("body-text-test"));
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("UPDATE messages SET body_blob = 'blob-old', body_type = 2, body_text = NULL WHERE id = %1").arg(id)));
        }
        QSqlDatabase::removeDatabase(QStringLiteral("body-text-test"));
        QCOMPARE(store.bodiesWithoutText(10).size(), 1);
        QVERIFY(store.setBodyText(id, QStringLiteral("archived quotation")));
        QVERIFY(store.bodiesWithoutText(10).isEmpty());
        QCOMPARE(store.search(SearchQuery::parse(QStringLiteral("quotation ")), {}, 10).size(), 1);
    }

    void ftsQueryEscaping()
    {
        QCOMPARE(Store::ftsQuery(QStringLiteral("hello wor")), QStringLiteral("\"hello\" \"wor\"*"));
        QCOMPARE(Store::ftsQuery(QStringLiteral("hello ")), QStringLiteral("\"hello\""));
        QCOMPARE(Store::ftsQuery(QStringLiteral("say \"hi\"")), QStringLiteral("\"say\" \"\"\"hi\"\"\"*"));
        QCOMPARE(Store::ftsQuery(QStringLiteral(" - & ")), QString());
        QCOMPARE(Store::ftsQuery(QString()), QString());
    }

    void unreadCountsAndConversations()
    {
        MessageRow read = makeMessage(QStringLiteral("r"), inbox, 1000);
        read.isRead = true;
        store.upsertMessage(read);
        store.upsertMessage(makeMessage(QStringLiteral("u1"), inbox, 2000));
        MessageRow other = makeMessage(QStringLiteral("u2"), archive, 3000);
        other.conversation = QStringLiteral("conv-2");
        store.upsertMessage(other);
        QHash<qint64, int> counts = store.unreadCounts();
        QCOMPARE(counts.value(inbox), 1);
        QCOMPARE(counts.value(archive), 1);
        QVERIFY(store.setRead(store.messageByGraphId(QStringLiteral("u1"))->id, true));
        QCOMPARE(store.unreadCounts().value(inbox), 0);

        QCOMPARE(store.conversation(QStringLiteral("conv-1")).size(), 2);
        QCOMPARE(store.conversation(QStringLiteral("conv-2")).size(), 1);
    }

    void resyncSweepsWhatWasNotSeen()
    {
        for (int i = 0; i < 5; ++i) {
            store.upsertMessage(makeMessage(QStringLiteral("m%1").arg(i), inbox, 1000 + i));
        }
        store.upsertMessage(makeMessage(QStringLiteral("other"), archive, 1));
        const int generation = store.beginResync(inbox);
        QCOMPARE(generation, 1);
        QCOMPARE(store.generation(inbox), 1);
        // The fresh chain still has m0, m2 (unchanged) and m4 (changed), and a new m5
        store.upsertMessage(makeMessage(QStringLiteral("m0"), inbox, 1000), generation);
        store.upsertMessage(makeMessage(QStringLiteral("m2"), inbox, 1002), generation);
        MessageRow changed = makeMessage(QStringLiteral("m4"), inbox, 1004);
        changed.changeKey = QStringLiteral("ck-new");
        store.upsertMessage(changed, generation);
        store.upsertMessage(makeMessage(QStringLiteral("m5"), inbox, 1005), generation);
        store.sweep(inbox, generation);

        QStringList left;
        const QList<ListRow> rows = store.listPage(inbox, std::nullopt, 50);
        for (const ListRow &row : rows) {
            left.append(store.message(row.id)->graphId);
        }
        left.sort();
        QCOMPARE(left, (QStringList{QStringLiteral("m0"), QStringLiteral("m2"), QStringLiteral("m4"), QStringLiteral("m5")}));
        // Other folders are untouched
        QVERIFY(store.messageByGraphId(QStringLiteral("other")).has_value());
    }

    void foldersAndSyncState()
    {
        // Update by graph id keeps the row and a known well-known name
        const qint64 again = store.upsertFolder({0, QStringLiteral("f-inbox"), {}, QStringLiteral("Postvak IN"), {}, 12, 3});
        QCOMPARE(again, inbox);
        const FolderRow row = store.folder(inbox).value();
        QCOMPARE(row.name, QStringLiteral("Postvak IN"));
        QCOMPARE(row.wellKnown, QStringLiteral("inbox"));
        QCOMPARE(row.total, 12);
        QCOMPARE(store.folderByGraphId(QStringLiteral("f-archive"))->id, archive);
        QCOMPARE(store.folders().size(), 2);

        SyncState state{QStringLiteral("https://delta/link"), QStringLiteral("https://recent"), 2, 123456, {}};
        QVERIFY(store.setSyncState(inbox, state));
        const SyncState read = store.syncState(inbox);
        QCOMPARE(read.deltaLink, state.deltaLink);
        QCOMPARE(read.recentDeltaLink, state.recentDeltaLink);
        QCOMPARE(read.initialState, 2);
        QCOMPARE(read.lastSynced, 123456);

        store.upsertMessage(makeMessage(QStringLiteral("m1"), inbox, 1));
        store.setBody(store.messageByGraphId(QStringLiteral("m1"))->id, QStringLiteral("cccccccccccccccccccccccccccccccc"), 1, 1);
        QCOMPARE(store.removeFolder(inbox), QStringList{QStringLiteral("cccccccccccccccccccccccccccccccc")});
        QVERIFY(!store.messageByGraphId(QStringLiteral("m1")).has_value()); // cascaded
        QCOMPARE(store.folders().size(), 1);
    }

    void transactionsRollBack()
    {
        const bool committed = store.inTransaction([this] {
            store.upsertMessage(makeMessage(QStringLiteral("t1"), inbox, 1));
            return false; // something failed halfway through a delta page
        });
        QVERIFY(!committed);
        QVERIFY(!store.messageByGraphId(QStringLiteral("t1")).has_value());
        QVERIFY(store.inTransaction([this] {
            return store.upsertMessage(makeMessage(QStringLiteral("t2"), inbox, 1)) == Store::Upsert::Inserted;
        }));
        QVERIFY(store.messageByGraphId(QStringLiteral("t2")).has_value());
    }
};

QTEST_GUILESS_MAIN(StoreTest)

#include "storetest.moc"
