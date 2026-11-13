// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "accounts.h"
#include "calendardata.h"
#include "calendarviews.h"
#include "graphmapper.h"
#include "recurrence.h"
#include "conversationmodel.h"
#include "foldermodel.h"
#include "mailboxset.h"
#include "mailboxworker.h"
#include "messagelistmodel.h"
#include "searchfolderdialog.h"
#include "categorylist.h"
#include "daemonclient.h"
#include "rulesdialog.h"
#include "mimeexport.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDir>
#include <QJsonArray>
#include <QTimeZone>
#include <QMimeData>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QTest>
#include <QUrl>

using namespace GraphMail;

class AppModelsTest : public QObject
{
    Q_OBJECT

private:
    QString account;
    qint64 inbox = 0;
    qint64 sent = 0;
    qint64 big = 0;

    static MessageRow message(const QString &id, qint64 folder, qint64 received, const QString &conversation, bool read = true)
    {
        MessageRow m;
        m.graphId = id;
        m.folder = folder;
        m.received = received;
        m.conversation = conversation;
        m.subject = QStringLiteral("Subject ") + id;
        m.senderName = QStringLiteral("Sender ") + id;
        m.senderAddress = id + QStringLiteral("@example.com");
        m.toList = QStringLiteral("Recipient One <one@example.com>; two@example.com");
        m.isRead = read;
        m.changeKey = QStringLiteral("ck");
        return m;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QDir(QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/graphmail")).removeRecursively();
        for (const GraphCore::Account &a : GraphCore::Accounts::all()) {
            GraphCore::Accounts::remove(a.id);
        }
        GraphCore::Account a;
        a.tenantId = QStringLiteral("t");
        a.objectId = QStringLiteral("o");
        a.userPrincipalName = QStringLiteral("me@example.com");
        account = GraphCore::Accounts::add(a).id;

        // A cache as the daemon writes it
        const QString dir = cacheDirectory(account, QStringLiteral("me"));
        QDir().mkpath(dir);
        Store store;
        QString error;
        QVERIFY(store.open(dir + QStringLiteral("/cache.db"), Store::Mode::ReadWrite, &error));
        auto folder = [&store](const QString &id, const QString &name, const QString &wellKnown, const QString &parent = {}) {
            return store.upsertFolder({0, id, parent, name, wellKnown, 0, 0});
        };
        inbox = folder(QStringLiteral("f-inbox"), QStringLiteral("Inbox"), QStringLiteral("inbox"));
        sent = folder(QStringLiteral("f-sent"), QStringLiteral("Sent Items"), QStringLiteral("sentitems"));
        folder(QStringLiteral("f-deleted"), QStringLiteral("Deleted Items"), QStringLiteral("deleteditems"));
        folder(QStringLiteral("f-zz"), QStringLiteral("Zebra projects"), {});
        folder(QStringLiteral("f-aa"), QStringLiteral("Accounting"), {});
        folder(QStringLiteral("f-child"), QStringLiteral("Child of accounting"), {}, QStringLiteral("f-aa"));
        // Outlook's system folders: hidden unless "Show All Folders"
        folder(QStringLiteral("f-ws"), QStringLiteral("Working Set"), {});
        folder(QStringLiteral("f-sync"), QStringLiteral("Problemen met synchronisatie"), QStringLiteral("syncissues"));
        folder(QStringLiteral("f-sync-child"), QStringLiteral("Conflicten"), {}, QStringLiteral("f-sync"));
        big = folder(QStringLiteral("f-big"), QStringLiteral("Big"), {});

        // Inbox: two conversations (c1 with 3 messages, one unread) and one without id
        QVERIFY(store.upsertMessage(message(QStringLiteral("a"), inbox, 1000, QStringLiteral("c1"))) == Store::Upsert::Inserted);
        store.upsertMessage(message(QStringLiteral("b"), inbox, 3000, QStringLiteral("c1"), false));
        store.upsertMessage(message(QStringLiteral("c"), inbox, 2000, QStringLiteral("c1")));
        store.upsertMessage(message(QStringLiteral("d"), inbox, 2500, QStringLiteral("c2")));
        store.upsertMessage(message(QStringLiteral("e"), inbox, 4000, {}));
        store.upsertMessage(message(QStringLiteral("s"), sent, 5000, QStringLiteral("c3")));
        // Calendars: the user's own (all-day event, a series with an occurrence) and a colleague's
        const qint64 own = store.upsertCalendar({0, QStringLiteral("CAL"), QStringLiteral("Calendar"), QStringLiteral("me@example.com"), {}, true, true});
        store.upsertCalendar({0, QStringLiteral("COL"), QStringLiteral("Colleague"), QStringLiteral("colleague@example.com"), QStringLiteral("#ff0000"), false, false});
        auto time = [](const QString &iso) {
            return QJsonObject{{QStringLiteral("dateTime"), iso}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}};
        };
        store.upsertItem(ItemKind::Event, own, Mapper::event(QJsonObject{{QStringLiteral("id"), QStringLiteral("ALLDAY")}, {QStringLiteral("subject"), QStringLiteral("Holiday")},
                                                                       {QStringLiteral("isAllDay"), true}, {QStringLiteral("start"), time(QStringLiteral("2026-11-16T00:00:00"))},
                                                                       {QStringLiteral("end"), time(QStringLiteral("2026-11-17T00:00:00"))}}));
        store.upsertItem(ItemKind::Event, own, Mapper::event(QJsonObject{{QStringLiteral("id"), QStringLiteral("M")}, {QStringLiteral("type"), QStringLiteral("seriesMaster")},
                                                                       {QStringLiteral("subject"), QStringLiteral("Stand-up")}, {QStringLiteral("start"), time(QStringLiteral("2026-01-05T08:30:00"))},
                                                                       {QStringLiteral("end"), time(QStringLiteral("2026-01-05T08:45:00"))}}));
        store.upsertItem(ItemKind::Event, own, Mapper::event(QJsonObject{{QStringLiteral("id"), QStringLiteral("O")}, {QStringLiteral("type"), QStringLiteral("occurrence")},
                                                                       {QStringLiteral("seriesMasterId"), QStringLiteral("M")}, {QStringLiteral("start"), time(QStringLiteral("2026-11-17T08:30:00"))},
                                                                       {QStringLiteral("end"), time(QStringLiteral("2026-11-17T08:45:00"))}}));

        // Big: 450 messages for paging
        for (int i = 0; i < 450; ++i) {
            store.upsertMessage(message(QStringLiteral("big%1").arg(i), big, 10000 + i, QStringLiteral("bc%1").arg(i % 300)));
        }
    }

    void folderOrderAndHiddenSystemFolders()
    {
        MailboxSet mailboxes;
        mailboxes.reload();
        QCOMPARE(mailboxes.mailboxes().size(), 1);
        FolderModel model(&mailboxes);
        const QModelIndex mailbox = model.index(0, 0);
        QCOMPARE(mailbox.data(Qt::DisplayRole).toString(), QStringLiteral("me@example.com"));
        QStringList names;
        for (int row = 0; row < model.rowCount(mailbox); ++row) {
            names.append(model.index(row, 0, mailbox).data(Qt::DisplayRole).toString());
        }
        // Well-known first in Outlook's order, then alphabetical; system folders hidden
        QCOMPARE(names, (QStringList{QStringLiteral("Inbox"), QStringLiteral("Sent Items"), QStringLiteral("Deleted Items"), QStringLiteral("Accounting"),
                                     QStringLiteral("Big"), QStringLiteral("Zebra projects")}));
        const QModelIndex accounting = model.indexForPath(account + QStringLiteral("/me/") + QString::number(model.index(3, 0, mailbox).data(FolderModel::FolderIdRole).toLongLong()));
        QCOMPARE(model.rowCount(accounting), 1);
        QCOMPARE(model.index(0, 0, mailbox).data(FolderModel::UnreadRole).toInt(), 1);

        model.setShowAllFolders(true);
        QCOMPARE(model.rowCount(model.index(0, 0)), 8);
        QVERIFY(model.indexForWellKnown(account + QStringLiteral("/me"), QStringLiteral("syncissues")).isValid());
        QVERIFY(FolderModel::isSystemFolder(QStringLiteral("RSS-kanalen"), {}));
        QVERIFY(!FolderModel::isSystemFolder(QStringLiteral("Verzonden Items"), {}));
    }

    void dropOnlyWithinTheMailbox()
    {
        MailboxSet mailboxes;
        mailboxes.reload();
        FolderModel model(&mailboxes);
        const QModelIndex target = model.indexForWellKnown(account + QStringLiteral("/me"), QStringLiteral("deleteditems"));
        QMimeData same;
        same.setData(MessageListModel::mimeType(), (account + QStringLiteral("/me\n12\n13")).toLatin1());
        QMimeData other;
        other.setData(MessageListModel::mimeType(), QByteArray("x/shared\n12"));
        QVERIFY(model.canDropMimeData(&same, Qt::MoveAction, -1, -1, target));
        QVERIFY(!model.canDropMimeData(&other, Qt::MoveAction, -1, -1, target));
        QVERIFY(!model.canDropMimeData(&same, Qt::MoveAction, -1, -1, model.index(0, 0))); // not on the mailbox node
        QSignalSpy moved(&model, &FolderModel::moveRequested);
        model.dropMimeData(&same, Qt::MoveAction, -1, -1, target);
        QCOMPARE(moved.size(), 1);
        QCOMPARE(moved.first().at(2).value<QList<qint64>>(), (QList<qint64>{12, 13}));
    }

    // Message files from the file manager: imported into the folder they are dropped on
    void dropMessageFiles()
    {
        MailboxSet mailboxes;
        mailboxes.reload();
        FolderModel model(&mailboxes);
        const QModelIndex target = model.indexForWellKnown(account + QStringLiteral("/me"), QStringLiteral("deleteditems"));
        QMimeData files;
        files.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/a.msg")), QUrl::fromLocalFile(QStringLiteral("/tmp/b.EML"))});
        QMimeData mixed;
        mixed.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/a.msg")), QUrl::fromLocalFile(QStringLiteral("/tmp/c.pdf"))});
        QMimeData remote;
        remote.setUrls({QUrl(QStringLiteral("https://example.com/a.msg"))});
        QVERIFY(model.canDropMimeData(&files, Qt::CopyAction, -1, -1, target));
        QVERIFY(!model.canDropMimeData(&mixed, Qt::CopyAction, -1, -1, target));
        QVERIFY(!model.canDropMimeData(&remote, Qt::CopyAction, -1, -1, target));
        QVERIFY(!model.canDropMimeData(&files, Qt::CopyAction, -1, -1, model.index(0, 0)));
        QSignalSpy dropped(&model, &FolderModel::filesDropped);
        QSignalSpy moved(&model, &FolderModel::moveRequested);
        model.dropMimeData(&files, Qt::CopyAction, -1, -1, target);
        QCOMPARE(dropped.size(), 1);
        QCOMPARE(moved.size(), 0);
        QCOMPARE(dropped.first().at(1).toLongLong(), target.data(FolderModel::FolderIdRole).toLongLong());
        QCOMPARE(dropped.first().at(2).toStringList(), (QStringList{QStringLiteral("/tmp/a.msg"), QStringLiteral("/tmp/b.EML")}));
    }

    void flatListPagesAndRefreshes()
    {
        MailboxSet mailboxes;
        mailboxes.reload();
        const QString key = account + QStringLiteral("/me");
        MessageListModel model;
        model.setFolder(key, mailboxes.store(key), big);
        QCOMPARE(model.rowCount(), MessageListModel::pageSize);
        QVERIFY(model.canFetchMore({}));
        model.fetchMore({});
        model.fetchMore({});
        QCOMPARE(model.rowCount(), 450);
        QVERIFY(!model.canFetchMore({}));
        // Newest first
        QCOMPARE(model.index(0).data(MessageListModel::ReceivedRole).toLongLong(), 10449);
        // A refresh keeps everything that was loaded
        model.refresh();
        QCOMPARE(model.rowCount(), 450);

        model.setFolder(key, mailboxes.store(key), sent, true);
        QCOMPARE(model.index(0).data(MessageListModel::ShowRecipientsRole).toBool(), true);
        QVERIFY(model.index(0).data(MessageListModel::ToListRole).toString().startsWith(QLatin1String("Recipient One")));
    }

    void calendarData()
    {
        MailboxSet mailboxes;
        mailboxes.reload();
        CalendarData data(&mailboxes);
        const QList<CalendarInfo> calendars = data.calendars();
        QCOMPARE(calendars.size(), 2);
        // Own calendars shown, a colleague's not until it is ticked
        QVERIFY(calendars.at(0).shown);
        QVERIFY(!calendars.at(1).shown);
        QCOMPARE(calendars.at(1).color, QColor(QStringLiteral("#ff0000")));
        QCOMPARE(data.writable().size(), 1);
        // An all-day event stays on its date, whatever the time zone
        QList<DisplayEvent> monday = data.events(QDate(2026, 11, 16), QDate(2026, 11, 16));
        QCOMPARE(monday.size(), 1);
        QVERIFY(monday.first().allDay);
        QCOMPARE(monday.first().firstDay, QDate(2026, 11, 16));
        QCOMPARE(monday.first().lastDay, QDate(2026, 11, 16));
        QVERIFY(data.events(QDate(2026, 11, 15), QDate(2026, 11, 15)).isEmpty());
        // The occurrence on Tuesday, with its master's subject, in local time
        const QList<DisplayEvent> tuesday = data.events(QDate(2026, 11, 17), QDate(2026, 11, 17));
        QCOMPARE(tuesday.size(), 1);
        QCOMPARE(tuesday.first().subject, QStringLiteral("Stand-up"));
        QVERIFY(tuesday.first().recurring());
        QCOMPARE(tuesday.first().start, QDateTime(QDate(2026, 11, 17), QTime(8, 30), QTimeZone::UTC).toLocalTime());
        QCOMPARE(eventTimeText(monday.first()), QStringLiteral("All day"));
    }

    // A colleague who does not share their calendar: getSchedule's blocks (docs/PHASE7.md)
    void freeBusyCalendar()
    {
        MailboxSet mailboxes;
        mailboxes.reload();
        CalendarData data(&mailboxes);
        data.addFreeBusy(QStringLiteral("acc"), QStringLiteral("colleague@example.com"), QStringLiteral("A Colleague"));
        const QList<CalendarInfo> calendars = data.calendars();
        const auto person = std::find_if(calendars.cbegin(), calendars.cend(), [](const CalendarInfo &c) {
            return CalendarData::isFreeBusy(c.mailboxKey);
        });
        QVERIFY(person != calendars.cend());
        QVERIFY(person->shown);
        QVERIFY(!person->canEdit);
        const QDate day(2026, 11, 18);
        QCOMPARE(data.freeBusyMissing(day, day).value(QStringLiteral("acc")), QStringList{QStringLiteral("colleague@example.com")});
        const QJsonObject schedule = QJsonDocument::fromJson(R"({"scheduleId":"Colleague@example.com","availabilityView":"0220",
            "scheduleItems":[{"status":"busy","subject":"Planning","location":"Room 1","start":{"dateTime":"2026-11-18T09:00:00.0000000","timeZone":"UTC"},
                              "end":{"dateTime":"2026-11-18T10:00:00.0000000","timeZone":"UTC"}},
                             {"status":"tentative","start":{"dateTime":"2026-11-18T13:00:00.0000000","timeZone":"UTC"},
                              "end":{"dateTime":"2026-11-18T13:30:00.0000000","timeZone":"UTC"}},
                             {"status":"free","start":{"dateTime":"2026-11-18T15:00:00.0000000","timeZone":"UTC"},
                              "end":{"dateTime":"2026-11-18T16:00:00.0000000","timeZone":"UTC"}}]})")
                                         .object();
        data.setFreeBusy(schedule, day, day);
        QVERIFY(data.freeBusyMissing(day, day).isEmpty());
        QList<DisplayEvent> blocks;
        const QList<DisplayEvent> all = data.events(day, day);
        std::copy_if(all.cbegin(), all.cend(), std::back_inserter(blocks), [](const DisplayEvent &e) {
            return CalendarData::isFreeBusy(e.mailboxKey);
        });
        // Free time is not drawn; a block without a subject says what it is
        QCOMPARE(blocks.size(), 2);
        QCOMPARE(blocks.at(0).subject, QStringLiteral("Planning"));
        QCOMPARE(blocks.at(0).location, QStringLiteral("Room 1"));
        QCOMPARE(blocks.at(0).start, QDateTime(day, QTime(9, 0), QTimeZone::UTC).toLocalTime());
        QCOMPARE(blocks.at(1).subject, QStringLiteral("Tentative"));
        QCOMPARE(blocks.at(1).showAs, 1);
        data.removeFreeBusy(QStringLiteral("colleague@example.com"));
        const QList<CalendarInfo> after = data.calendars();
        QVERIFY(std::none_of(after.cbegin(), after.cend(), [](const CalendarInfo &c) {
            return CalendarData::isFreeBusy(c.mailboxKey);
        }));
    }

    void recurrenceTexts()
    {
        const QDate monday(2026, 11, 16);
        QJsonObject weekly = presetRecurrence(RepeatPreset::Weekly, monday);
        QCOMPARE(weekly.value(QLatin1String("pattern")).toObject().value(QLatin1String("daysOfWeek")).toArray().first().toString(), QStringLiteral("monday"));
        QCOMPARE(weekly.value(QLatin1String("range")).toObject().value(QLatin1String("startDate")).toString(), QStringLiteral("2026-11-16"));
        QVERIFY(!weekly.value(QLatin1String("range")).toObject().value(QLatin1String("recurrenceTimeZone")).toString().isEmpty());
        QCOMPARE(describeRecurrence(weekly), QStringLiteral("Every week on Monday"));
        QJsonObject range = weekly.value(QLatin1String("range")).toObject();
        range.insert(QStringLiteral("type"), QStringLiteral("numbered"));
        range.insert(QStringLiteral("numberOfOccurrences"), 3);
        weekly.insert(QStringLiteral("range"), range);
        QCOMPARE(describeRecurrence(weekly), QStringLiteral("Every week on Monday, 3 times"));
        QCOMPARE(describeRecurrence(presetRecurrence(RepeatPreset::Monthly, monday)), QStringLiteral("Every month on day 16"));
        QCOMPARE(describeRecurrence(presetRecurrence(RepeatPreset::Yearly, monday)), QStringLiteral("Every year on 16 November"));
        const QJsonObject relative{{QStringLiteral("pattern"),
                                    QJsonObject{{QStringLiteral("type"), QStringLiteral("relativeMonthly")}, {QStringLiteral("interval"), 2},
                                                {QStringLiteral("index"), QStringLiteral("second")}, {QStringLiteral("daysOfWeek"), QJsonArray{QStringLiteral("tuesday")}}}},
                                   {QStringLiteral("range"), QJsonObject{{QStringLiteral("type"), QStringLiteral("endDate")}, {QStringLiteral("endDate"), QStringLiteral("2027-06-30")}}}};
        QCOMPARE(describeRecurrence(relative), QStringLiteral("Every 2 months on the second Tuesday, until 30 June 2027"));
    }

    // Search results from several mailboxes (docs/PHASE8.md): each row knows its own
    void searchResultsKeepTheirMailbox()
    {
        int calls = 0;
        ListRow a;
        a.id = 1;
        a.subject = QStringLiteral("A");
        ListRow b;
        b.id = 1;
        b.subject = QStringLiteral("B");
        MessageListModel model;
        model.setResults([&]() {
            ++calls;
            return QList<SearchHit>{{QStringLiteral("acc/me"), a, QStringLiteral("Inbox")}, {QStringLiteral("acc/shared"), b, QStringLiteral("Postvak IN")}};
        });
        QVERIFY(model.isSearch());
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.index(1).data(MessageListModel::MailboxKeyRole).toString(), QStringLiteral("acc/shared"));
        QCOMPARE(model.index(1).data(MessageListModel::FolderNameRole).toString(), QStringLiteral("Postvak IN"));
        QCOMPARE(model.mailboxKeyOf(0), QStringLiteral("acc/me"));
        // A drag takes only the rows of one mailbox
        std::unique_ptr<QMimeData> drag(model.mimeData({model.index(0), model.index(1)}));
        QCOMPARE(QString::fromLatin1(drag->data(MessageListModel::mimeType())), QStringLiteral("acc/me\n1"));
        // refresh() asks again (a read state changed)
        model.refresh();
        QCOMPARE(calls, 2);
        model.clear();
        QVERIFY(!model.isSearch());
    }

    // The rules of a search folder as Exchange's filter (accepted live, phase 8 probe)
    void searchFolderFilter()
    {
        QCOMPARE(SearchFolderDialog::buildFilter(QStringLiteral("O'Brien"), {}, false, false, false, false, {}),
                 QStringLiteral("(contains(from/emailAddress/address,'O''Brien') or contains(from/emailAddress/name,'O''Brien'))"));
        QCOMPARE(SearchFolderDialog::buildFilter({}, QStringLiteral("invoice"), true, true, true, true, {}),
                 QStringLiteral("contains(subject,'invoice') and isRead eq false and flag/flagStatus eq 'flagged' and hasAttachments eq true and importance eq 'high'"));
        const QString since = SearchFolderDialog::buildFilter({}, {}, false, false, false, false, QDate(2026, 10, 1));
        QVERIFY(since.startsWith(QLatin1String("receivedDateTime ge 2026-09-30T")) || since.startsWith(QLatin1String("receivedDateTime ge 2026-10-01T")));
        QVERIFY(since.endsWith(QLatin1Char('Z')));
        QVERIFY(SearchFolderDialog::buildFilter({}, {}, false, false, false, false, {}).isEmpty());
    }

    // Inbox rules (docs/PHASE9.md): the editor writes what it shows and keeps what a rule made in
    // Outlook has beyond that (other conditions, exceptions, more categories)
    void ruleEditorKeepsWhatItDoesNotShow()
    {
        MailboxSet mailboxes;
        mailboxes.reload();
        FolderModel folders(&mailboxes);
        DaemonClient daemon;
        CategoryList categories(&daemon);
        const QString key = account + QStringLiteral("/me");
        const QJsonObject outlook = QJsonDocument::fromJson(R"({
            "id": "R1", "displayName": "Invoices", "sequence": 2, "isEnabled": true, "hasError": false, "isReadOnly": false,
            "conditions": {"fromAddresses": [{"emailAddress": {"name": "Shop", "address": "shop@example.com"}}],
                           "subjectContains": ["invoice", "bill"], "sentCcMe": true},
            "actions": {"moveToFolder": "f-aa", "assignCategories": ["Red category", "Blue category"], "stopProcessingRules": true},
            "exceptions": {"bodyContains": ["reminder"]}})")
                                       .object();
        RuleEditor editor(&mailboxes, &folders, &categories, key, outlook);
        const QJsonObject out = editor.rule();
        QCOMPARE(out.value(QLatin1String("displayName")).toString(), QStringLiteral("Invoices"));
        QVERIFY(out.value(QLatin1String("isEnabled")).toBool());
        QVERIFY(!out.contains(QLatin1String("id")) && !out.contains(QLatin1String("hasError")));
        const QJsonObject c = out.value(QLatin1String("conditions")).toObject();
        QCOMPARE(c.value(QLatin1String("subjectContains")).toArray(), QJsonArray({QStringLiteral("invoice"), QStringLiteral("bill")}));
        QCOMPARE(c.value(QLatin1String("fromAddresses")).toArray().at(0).toObject().value(QLatin1String("emailAddress")).toObject().value(QLatin1String("address")).toString(),
                 QStringLiteral("shop@example.com"));
        QVERIFY(c.value(QLatin1String("sentCcMe")).toBool());
        const QJsonObject a = out.value(QLatin1String("actions")).toObject();
        QCOMPARE(a.value(QLatin1String("moveToFolder")).toString(), QStringLiteral("f-aa"));
        QCOMPARE(a.value(QLatin1String("assignCategories")).toArray(), QJsonArray({QStringLiteral("Red category"), QStringLiteral("Blue category")}));
        QVERIFY(a.value(QLatin1String("stopProcessingRules")).toBool());
        QCOMPARE(out.value(QLatin1String("exceptions")).toObject(), outlook.value(QLatin1String("exceptions")).toObject());

        // In a sentence, with the folder's name from the cache
        const QString text = RulesDialog::summary(outlook, &mailboxes, key);
        QVERIFY2(text.contains(QStringLiteral("shop@example.com")) && text.contains(QStringLiteral("invoice; bill")) && text.contains(QStringLiteral("Accounting")),
                 qPrintable(text));

        // A new rule from the test path: subject, mark as read, importance high
        RuleEditor fresh(&mailboxes, &folders, &categories, key, {});
        fresh.fillForTests(QStringLiteral("Test"), QStringLiteral("one; two"));
        const QJsonObject made = fresh.rule();
        QCOMPARE(made.value(QLatin1String("conditions")).toObject(), QJsonObject({{QStringLiteral("subjectContains"), QJsonArray({QStringLiteral("one"), QStringLiteral("two")})}}));
        QCOMPARE(made.value(QLatin1String("actions")).toObject(),
                 QJsonObject({{QStringLiteral("markAsRead"), true}, {QStringLiteral("markImportance"), QStringLiteral("high")}}));
        QVERIFY(!made.contains(QLatin1String("exceptions")));
    }

    // Export (docs/PHASE9B.md): a message rebuilt from the cache and the mbox format
    void exportFormats()
    {
        MailboxSet mailboxes;
        mailboxes.reload();
        Store *store = mailboxes.store(account + QStringLiteral("/me"));
        QVERIFY(store);
        const QList<ListRow> rows = store->listPage(inbox, std::nullopt, 1);
        QVERIFY(!rows.isEmpty());
        const QByteArray mime = MimeExport::rebuild(store, mailboxes.blobs(account + QStringLiteral("/me")), rows.constFirst().id);
        QVERIFY(mime.contains("\r\nSubject: "));
        QVERIFY(mime.contains("\r\nMIME-Version: 1.0\r\n"));
        QVERIFY(mime.contains("X-GraphMail-Rebuilt:"));
        QVERIFY(mime.startsWith("From: "));
        // Names that are not ASCII are encoded words
        QCOMPARE(MimeExport::encodeWord(QStringLiteral("Café")), QByteArray("=?UTF-8?B?") + QByteArray("Café").toBase64() + "?=");
        QCOMPARE(MimeExport::encodeWord(QStringLiteral("Plain")), QByteArray("Plain"));
        // mboxrd: a "From " line and its quoted forms get one more ">", CRLF becomes LF
        const QByteArray entry = MimeExport::mboxEntry("Subject: x\r\n\r\nFrom here\r\n>From there\r\nok\r\n", QStringLiteral("a@example.com"),
                                                       QDateTime(QDate(2026, 11, 13), QTime(9, 5, 7), QTimeZone::UTC));
        QCOMPARE(entry, QByteArray("From a@example.com Fri Nov 13 09:05:07 2026\nSubject: x\n\n>From here\n>>From there\nok\n\n"));
    }

    void conversations()
    {
        MailboxSet mailboxes;
        mailboxes.reload();
        const QString key = account + QStringLiteral("/me");
        ConversationModel model;
        model.setFolder(key, mailboxes.store(key), inbox);
        // e (alone, 4000), c1 (newest 3000), c2 (2500)
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.index(0, 0).data(MessageListModel::SubjectRole).toString(), QStringLiteral("Subject e"));
        const QModelIndex c1 = model.index(1, 0);
        QCOMPARE(c1.data(MessageListModel::ConversationCountRole).toInt(), 3);
        QCOMPARE(c1.data(MessageListModel::SubjectRole).toString(), QStringLiteral("Subject b")); // its newest
        QCOMPARE(c1.data(MessageListModel::IsReadRole).toBool(), false); // b is unread
        QVERIFY(model.hasChildren(c1));
        QVERIFY(!model.hasChildren(model.index(0, 0)));
        QCOMPARE(model.rowCount(c1), 3);
        QCOMPARE(model.messagesOf(c1).size(), 3);
        QCOMPARE(model.messagesOf(model.index(0, c1.column(), c1)).size(), 1);
        QCOMPARE(model.parent(model.index(2, 0, c1)), c1);

        // Paging over conversations: 300 in Big
        model.setFolder(key, mailboxes.store(key), big);
        QCOMPARE(model.rowCount(), ConversationModel::pageSize);
        model.fetchMore({});
        QCOMPARE(model.rowCount(), 300);
        QVERIFY(!model.canFetchMore({}));
    }
};

QTEST_MAIN(AppModelsTest)

#include "appmodelstest.moc"
