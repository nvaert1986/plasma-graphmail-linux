// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "accounts.h"
#include "calendardata.h"
#include "calendarexport.h"
#include "cleanup.h"
#include "conditionalformat.h"
#include "calendaroptions.h"
#include "calendarviews.h"
#include "graphmapper.h"
#include "mailto.h"
#include "messagedetails.h"
#include "messagedrag.h"
#include "readreceipts.h"
#include "recipientedit.h"
#include "replyoptions.h"
#include "sweepdialog.h"
#include "unsubscribe.h"
#include "vcard.h"
#include "recurrence.h"
#include "conversationmodel.h"
#include "foldermodel.h"
#include "mailboxset.h"
#include "maileditor.h"
#include "mailboxworker.h"
#include "messagelistmodel.h"
#include "searchfolderdialog.h"
#include "categorylist.h"
#include "daemonclient.h"
#include "rulesdialog.h"
#include "mimeexport.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <KCalendarCore/ICalFormat>
#include <KCalendarCore/MemoryCalendar>

#include <QDir>
#include <QJsonArray>
#include <QTimeZone>
#include <QMimeData>
#include <QSignalSpy>
#include <KActionCollection>
#include <KFontAction>
#include <KPIMTextEdit/RichTextComposer>
#include <KPIMTextEdit/RichTextComposerActions>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QLineEdit>
#include <QMainWindow>
#include <QToolBar>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QTest>
#include <QTextDocument>

#include <KPIMTextEdit/RichTextComposer>

#include "signaturedialog.h"
#include "spelllanguage.h"
#include "fontfamilybox.h"

#include <Sonnet/Speller>
#include <QComboBox>
#include "signatures.h"
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
    void signatureFontsSurviveInsertion()
    {
        // A signature with one piece at 12 pt and one without its own size, as the signature editor
        // shows them (the default font of a new message, 11 pt): saved, then inserted into a message
        // whose default font differs, both keep the size the editor showed (0.14.2)
        KPIMTextEdit::RichTextComposer editor;
        QFont base;
        base.setFamilies({QStringLiteral("Aptos"), QStringLiteral("Calibri"), QStringLiteral("Carlito")});
        base.setPointSize(11);
        editor.document()->setDefaultFont(base);
        editor.activateRichText();
        editor.setHtml(QStringLiteral("<p><span style=\"font-size:12pt\">Twelve</span> Eleven</p>"));
        GraphMail::Signature signature;
        GraphMail::SignatureDialog::store(&editor, &signature);
        QTextDocument message;
        QFont other;
        other.setPointSize(9);
        message.setDefaultFont(other);
        QTextCursor cursor(&message);
        cursor.insertHtml(signature.html);
        const auto sizeAt = [&message](const QString &word) {
            QTextCursor c = message.find(word);
            return c.charFormat().fontPointSize();
        };
        QCOMPARE(sizeAt(QStringLiteral("Twelve")), 12.0);
        QCOMPARE(sizeAt(QStringLiteral("Eleven")), 11.0);
        // Opened and saved again: still one wrapper, not one per save
        GraphMail::SignatureDialog::load(&editor, signature);
        GraphMail::SignatureDialog::store(&editor, &signature);
        GraphMail::SignatureDialog::load(&editor, signature);
        GraphMail::SignatureDialog::store(&editor, &signature);
        QCOMPARE(signature.html.count(QLatin1String("<div style=\"font-family:")), 1);
        QVERIFY(signature.html.contains(QLatin1String("font-size:12pt")));
    }

    // 0.14.4: typing a font name in the font box with the whole text selected. Before, the first
    // letter was applied as a font and put the focus back in the text, so the second letter
    // replaced the text. Now the box keeps the focus and Enter applies the font
    void fontBoxKeepsTheText()
    {
        using GraphMail::FontFamilyBox;
        // A family that is no start of another one: typed whole, the box completes nothing more
        QString family;
        const QStringList families = QFontDatabase::families();
        for (const QString &f : families) {
            if (f.size() >= 4 && !f.contains(QLatin1Char('[')) && std::count_if(families.cbegin(), families.cend(), [&f](const QString &other) {
                    return other.startsWith(f, Qt::CaseInsensitive);
                }) == 1) {
                family = f;
                break;
            }
        }
        if (family.isEmpty()) {
            QSKIP("no suitable font installed");
        }
        QCOMPARE(FontFamilyBox::installedFamily(QStringLiteral("  ") + family.toUpper()), family);
        QVERIFY(FontFamilyBox::installedFamily(QStringLiteral("No Such Font Family")).isEmpty());

        QMainWindow window;
        auto *editor = new KPIMTextEdit::RichTextComposer(&window);
        window.setCentralWidget(editor);
        editor->activateRichText();
        KActionCollection actions(&window);
        editor->composerActions()->createActions(&actions);
        auto *fontAction = qobject_cast<KFontAction *>(actions.action(QStringLiteral("format_font_family")));
        QVERIFY(fontAction);
        new FontFamilyBox(fontAction, editor);
        window.addToolBar(QStringLiteral("format"))->addAction(fontAction);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        auto *box = window.findChild<QFontComboBox *>();
        QVERIFY(box && box->lineEdit());

        const QString body = QStringLiteral("The body of the mail, all of it selected.");
        editor->setPlainText(body);
        editor->setFocus();
        editor->selectAll();
        box->lineEdit()->setFocus();
        QTRY_VERIFY(box->lineEdit()->hasFocus());
        box->lineEdit()->selectAll();
        for (const QChar c : family) {
            QTest::keyClick(box->lineEdit(), c.toLatin1());
            QVERIFY(box->hasFocus());
        }
        QCOMPARE(editor->toPlainText(), body);
        QTest::keyClick(box->lineEdit(), Qt::Key_Return);
        QCOMPARE(editor->toPlainText(), body);
        QTRY_VERIFY(editor->hasFocus());
        QTextCursor cursor(editor->document());
        cursor.setPosition(5);
        QCOMPARE(cursor.charFormat().fontFamilies().toStringList().value(0), family);

        // A name that is no font changes nothing, and leaving the box shows the font at the cursor
        editor->setFocus();
        editor->selectAll();
        box->lineEdit()->setFocus();
        QTRY_VERIFY(box->lineEdit()->hasFocus());
        box->lineEdit()->selectAll();
        QTest::keyClicks(box->lineEdit(), QStringLiteral("zzqx"));
        QTest::keyClick(box->lineEdit(), Qt::Key_Return);
        editor->setFocus();
        QTRY_VERIFY(editor->hasFocus());
        QCOMPARE(editor->toPlainText(), body);
        QCOMPARE(cursor.charFormat().fontFamilies().toStringList().value(0), family);
        QCOMPARE(box->currentText(), family);
    }

    // 0.14.4: the composer's spelling language. Which dictionary a guessed language gets, and (when
    // Dutch and English dictionaries are installed) Automatic following the text, a choice by hand
    // staying and being remembered for the mailbox
    void spellLanguageFollowsTheText()
    {
        using GraphMail::SpellLanguage;
        const QStringList installed{QStringLiteral("en_AU"), QStringLiteral("en_GB"), QStringLiteral("en_US"), QStringLiteral("nl"), QStringLiteral("de_AT"), QStringLiteral("de_DE")};
        QCOMPARE(SpellLanguage::pickDictionary(QStringLiteral("nl"), installed, {}, {}), QStringLiteral("nl"));
        QCOMPARE(SpellLanguage::pickDictionary(QStringLiteral("en"), installed, QStringLiteral("en_GB"), QStringLiteral("nl_NL")), QStringLiteral("en_GB"));
        QCOMPARE(SpellLanguage::pickDictionary(QStringLiteral("en"), installed, QStringLiteral("nl"), QStringLiteral("en_AU")), QStringLiteral("en_AU"));
        QCOMPARE(SpellLanguage::pickDictionary(QStringLiteral("en"), installed, {}, QStringLiteral("C")), QStringLiteral("en_US"));
        QCOMPARE(SpellLanguage::pickDictionary(QStringLiteral("de"), installed, {}, {}), QStringLiteral("de_DE"));
        QCOMPARE(SpellLanguage::pickDictionary(QStringLiteral("nl"), {QStringLiteral("nl_BE"), QStringLiteral("nl_NL")}, {}, QStringLiteral("nl-BE")), QStringLiteral("nl_BE"));
        QCOMPARE(SpellLanguage::pickDictionary(QStringLiteral("fr"), installed, {}, {}), QString());
        QCOMPARE(SpellLanguage::pickDictionary(QString(), installed, {}, {}), QString());

        const QStringList dictionaries = Sonnet::Speller().availableDictionaries().values();
        const auto has = [&dictionaries](const QString &language) {
            return std::any_of(dictionaries.cbegin(), dictionaries.cend(), [&language](const QString &d) {
                return d == language || d.startsWith(language + QLatin1Char('_'));
            });
        };
        if (!has(QStringLiteral("nl")) || !has(QStringLiteral("en"))) {
            QSKIP("Needs Dutch and English spelling dictionaries");
        }
        QWidget window;
        KPIMTextEdit::RichTextComposer editor(&window);
        editor.setSpellCheckingSupport(true);
        SpellLanguage spell(&editor, &window);
        const QString mailbox = QStringLiteral("test/spell");
        editor.setPlainText(QStringLiteral("Hoi Jan, de bestanden staan op de gedeelde schijf. Laat maar weten of het werkt, dan pas ik het morgen ook toe op de andere locatie."));
        spell.setMailbox(mailbox);
        QVERIFY2(spell.currentDictionary().startsWith(QLatin1String("nl")), qPrintable(spell.currentDictionary()));
        QCOMPARE(spell.comboBox()->currentIndex(), 0);
        editor.setPlainText(QStringLiteral("Hi John, the files are on the shared drive. Let me know whether it works, then I will apply it to the other location tomorrow as well."));
        spell.setMailbox(mailbox);
        QVERIFY2(spell.currentDictionary().startsWith(QLatin1String("en")), qPrintable(spell.currentDictionary()));
        // By hand: English stays over Dutch text, and comes back for this mailbox
        const int english = spell.comboBox()->findData(spell.currentDictionary());
        QVERIFY(english > 0);
        Q_EMIT spell.comboBox()->activated(english);
        editor.setPlainText(QStringLiteral("Hoi Jan, de bestanden staan op de gedeelde schijf. Laat maar weten of het werkt, dan pas ik het morgen ook toe op de andere locatie."));
        spell.setMailbox(mailbox);
        QVERIFY(spell.currentDictionary().startsWith(QLatin1String("en")));
        QCOMPARE(spell.comboBox()->currentIndex(), english);
        // Back to Automatic
        Q_EMIT spell.comboBox()->activated(0);
        QVERIFY(spell.currentDictionary().startsWith(QLatin1String("nl")));
    }

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
        // A name with a comma (Exchange's "Last, First") is quoted, quotes inside escaped
        QCOMPARE(MimeExport::displayName(QStringLiteral("Doe, John")), QByteArray("\"Doe, John\""));
        QCOMPARE(MimeExport::displayName(QStringLiteral("Ann \"Annie\" Doe")), QByteArray("\"Ann \\\"Annie\\\" Doe\""));
        QCOMPARE(MimeExport::displayName(QStringLiteral("Plain Name")), QByteArray("Plain Name"));
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

    // mailto: links (RFC 6068): addresses in the path and in to/cc/bcc, decoded text, line
    // breaks, a literal plus, attach= only for local files
    void mailtoLinks()
    {
        using namespace GraphMail;
        QVERIFY(Mailto::isMailto(QStringLiteral("MAILTO:a@example.com")));
        QVERIFY(!Mailto::isMailto(QStringLiteral("/home/example/a.eml")));
        Mailto::Link l = Mailto::parse(QUrl(QStringLiteral(
            "mailto:erin@example.com,desk%40example.com?cc=a@example.com&BCC=b@example.com,c@example.com&subject=Ren%C3%A9%20%E2%82%AC%2012&body=Line%201%0D%0ALine+2")));
        QCOMPARE(l.to, (QStringList{QStringLiteral("erin@example.com"), QStringLiteral("desk@example.com")}));
        QCOMPARE(l.cc, QStringList{QStringLiteral("a@example.com")});
        QCOMPARE(l.bcc, (QStringList{QStringLiteral("b@example.com"), QStringLiteral("c@example.com")}));
        QCOMPARE(l.subject, QStringLiteral(u"René € 12"));
        QCOMPARE(l.body, QStringLiteral("Line 1\nLine+2"));
        l = Mailto::parse(QUrl(QStringLiteral("mailto:?to=x@example.com&attach=/tmp/a%20b.pdf&attachment=file:///tmp/c.txt&attach=https://example.com/d")));
        QCOMPARE(l.to, QStringList{QStringLiteral("x@example.com")});
        QCOMPARE(l.attachments, (QStringList{QStringLiteral("/tmp/a b.pdf"), QStringLiteral("/tmp/c.txt")}));
        QVERIFY(Mailto::parse(QUrl(QStringLiteral("https://example.com"))).to.isEmpty());
    }

    // Message Details shows the header block: up to the first empty line, CRLF or LF, folded
    // lines kept as they are
    void headerBlock()
    {
        using GraphMail::MessageDetailsDialog;
        QCOMPARE(MessageDetailsDialog::headerBlock("Received: from a.example.com\r\n\tby b.example.com\r\nSubject: Example\r\n\r\nBody\r\n\r\nMore"),
                 QStringLiteral("Received: from a.example.com\n\tby b.example.com\nSubject: Example"));
        QCOMPARE(MessageDetailsDialog::headerBlock("Subject: LF only\n\nBody"), QStringLiteral("Subject: LF only"));
        QCOMPARE(MessageDetailsDialog::headerBlock("Subject: no body"), QStringLiteral("Subject: no body"));
    }

    // Reply option "prefix each line": "> " before every line, one more ">" on quoted lines
    void prefixLines()
    {
        QCOMPARE(GraphMail::ReplyOptions::prefixLines(QStringLiteral("One\n\n> Quoted\nTwo")), QStringLiteral("> One\n> \n>> Quoted\n> Two"));
    }

    // Calendar options: the three ways Outlook counts the first week of the year
    void weekNumbers()
    {
        using namespace GraphMail::CalendarOptions;
        // Monday and four days is ISO 8601, which QDate knows
        for (QDate day(2020, 1, 1); day.year() < 2031; day = day.addDays(1)) {
            QCOMPARE(weekNumber(day, Qt::Monday, FirstWeek::FirstFourDays), day.weekNumber());
        }
        // Sunday and 1 January (the United States): 28 December 2025 starts week 1 of 2026
        QCOMPARE(weekStart(QDate(2025, 12, 31), Qt::Sunday), QDate(2025, 12, 28));
        QCOMPARE(weekNumber(QDate(2025, 12, 31), Qt::Sunday, FirstWeek::JanuaryFirst), 1);
        QCOMPARE(weekNumber(QDate(2026, 1, 4), Qt::Sunday, FirstWeek::JanuaryFirst), 2);
        QCOMPARE(weekNumber(QDate(2026, 11, 16), Qt::Sunday, FirstWeek::JanuaryFirst), 47);
        // Monday and the first full week: 2026 starts on a Thursday, so 5 January is week 1
        QCOMPARE(weekNumber(QDate(2026, 1, 4), Qt::Monday, FirstWeek::FirstFullWeek), 52);
        QCOMPARE(weekNumber(QDate(2026, 1, 5), Qt::Monday, FirstWeek::FirstFullWeek), 1);
    }

    // List-Unsubscribe: one click over https first, then a mail, then the web page
    void unsubscribeWays()
    {
        using namespace GraphMail::Unsubscribe;
        const QString both = QStringLiteral("<mailto:leave@lists.example.com?subject=unsubscribe>, <https://news.example.com/u/a1b2\n c3>");
        Way way = choose(both, true);
        QCOMPARE(way.kind, Kind::OneClick);
        QCOMPARE(way.url, QUrl(QStringLiteral("https://news.example.com/u/a1b2c3")));
        way = choose(both, false);
        QCOMPARE(way.kind, Kind::Mail);
        QCOMPARE(way.url.path(), QStringLiteral("leave@lists.example.com"));
        way = choose(QStringLiteral("<https://news.example.com/u/1>"), false);
        QCOMPARE(way.kind, Kind::Browser);
        // One click needs https; plain http only opens in the browser
        way = choose(QStringLiteral("<http://news.example.com/u/1>"), true);
        QCOMPARE(way.kind, Kind::Browser);
        QCOMPARE(choose(QStringLiteral("<javascript:alert(1)>, <ftp://example.com/x>"), true).kind, Kind::None);
        QCOMPARE(choose(QString(), true).kind, Kind::None);
    }

    // The one-click request (RFC 8058): a POST with the fixed body and no cookies
    void unsubscribePost()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray request;
        connect(&server, &QTcpServer::newConnection, this, [&server, &request] {
            QTcpSocket *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket, &request] {
                request += socket->readAll();
                if (request.endsWith("List-Unsubscribe=One-Click")) {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                }
            });
        });
        bool finished = false;
        bool ok = false;
        GraphMail::Unsubscribe::post(QUrl(QStringLiteral("http://127.0.0.1:%1/u/1").arg(server.serverPort())), this, [&](bool success, const QString &) {
            finished = true;
            ok = success;
        });
        QTRY_VERIFY(finished);
        QVERIFY(ok);
        QVERIFY(request.startsWith("POST /u/1 "));
        QVERIFY(request.contains("application/x-www-form-urlencoded"));
        QVERIFY(!request.toLower().contains("cookie:"));
    }

    // Forward as vCard: escaping, the fields of a Graph contact, folding inside UTF-8
    void vCard()
    {
        const QJsonObject contact = QJsonDocument::fromJson(R"({
            "displayName": "Renée de Vries", "givenName": "Renée", "surname": "de Vries",
            "companyName": "Acme; Inc", "department": "Sales, East", "jobTitle": "Buyer",
            "emailAddresses": [{"address": "renee@example.com"}, {"address": "r.devries@example.org"}],
            "businessPhones": ["+31 10 000 0000"], "mobilePhone": "+31 6 0000 0000",
            "businessAddress": {"street": "Example Street 1", "city": "Utrecht", "postalCode": "1234 AB", "countryOrRegion": "Netherlands"},
            "birthday": "1980-02-29T11:59:00Z", "personalNotes": "Line one\nLine two"
        })").object();
        const QByteArray card = GraphMail::VCard::fromContact(contact);
        QVERIFY(card.startsWith("BEGIN:VCARD\r\nVERSION:3.0\r\n"));
        QVERIFY(card.endsWith("END:VCARD\r\n"));
        QVERIFY(card.contains("FN:Ren\xc3\xa9" "e de Vries\r\n"));
        QVERIFY(card.contains("N:de Vries;Ren\xc3\xa9" "e;;;\r\n"));
        QVERIFY(card.contains("ORG:Acme\\; Inc;Sales\\, East\r\n"));
        QVERIFY(card.contains("EMAIL;TYPE=INTERNET,PREF:renee@example.com\r\n"));
        QVERIFY(card.contains("EMAIL;TYPE=INTERNET:r.devries@example.org\r\n"));
        QVERIFY(card.contains("TEL;TYPE=CELL,VOICE:+31 6 0000 0000\r\n"));
        QVERIFY(card.contains("ADR;TYPE=WORK:;;Example Street 1;Utrecht;;1234 AB;Netherlands\r\n"));
        QVERIFY(card.contains("BDAY:1980-02-29\r\n"));
        QVERIFY(card.contains("NOTE:Line one\\nLine two\r\n"));
        // A long line folds at 75 octets and never inside the two bytes of an "é"
        const QByteArray folded = GraphMail::VCard::fromContact(QJsonObject{{QStringLiteral("displayName"), QString(QStringLiteral("é")).repeated(60)}});
        for (const QByteArray &line : folded.split('\n')) {
            QVERIFY(line.size() <= 76);
            QVERIFY(QString::fromUtf8(line).indexOf(QChar::ReplacementCharacter) < 0);
        }
        QVERIFY(folded.contains("\r\n \xc3\xa9"));
    }

    // Check Names: every typed word must be in the entry, accents and case ignored
    void checkNamesMatching()
    {
        const QStringList entries{QStringLiteral("Renée de Vries <renee@example.com>"), QStringLiteral("René Jansen <rene@example.org>"),
                                  QStringLiteral("Acme Sales <sales@example.com>")};
        QCOMPARE(GraphMail::AddressBook::matches(entries, QStringLiteral("renee")), QStringList{entries.at(0)});
        QCOMPARE(GraphMail::AddressBook::matches(entries, QStringLiteral("RENE")).size(), 2);
        QCOMPARE(GraphMail::AddressBook::matches(entries, QStringLiteral("rene jansen")), QStringList{entries.at(1)});
        QCOMPARE(GraphMail::AddressBook::matches(entries, QStringLiteral("sales@")), QStringList{entries.at(2)});
        QVERIFY(GraphMail::AddressBook::matches(entries, QStringLiteral("nobody")).isEmpty());
        QVERIFY(GraphMail::AddressBook::matches(entries, QStringLiteral("  ")).isEmpty());
        // The parts as typed: a comma inside a name before the address does not split
        QCOMPARE(GraphMail::splitRecipients(QStringLiteral("Doe, Jane <jane@example.com>; rene")).size(), 2);
    }

    // Sweep's rule for future messages: from the address, deleted or moved, nothing after it
    void sweepRule()
    {
        QJsonObject rule = GraphMail::SweepDialog::rule(QStringLiteral("news@example.com"), QString(), 4);
        QCOMPARE(rule.value(QLatin1String("sequence")).toInt(), 4);
        QVERIFY(rule.value(QLatin1String("isEnabled")).toBool());
        QCOMPARE(rule.value(QLatin1String("conditions")).toObject().value(QLatin1String("fromAddresses")).toArray().first().toObject().value(QLatin1String("emailAddress")).toObject().value(QLatin1String("address")).toString(),
                 QStringLiteral("news@example.com"));
        QVERIFY(rule.value(QLatin1String("actions")).toObject().value(QLatin1String("delete")).toBool());
        QVERIFY(rule.value(QLatin1String("actions")).toObject().value(QLatin1String("stopProcessingRules")).toBool());
        rule = GraphMail::SweepDialog::rule(QStringLiteral("news@example.com"), QStringLiteral("AAMkFolder"), 1);
        QCOMPARE(rule.value(QLatin1String("actions")).toObject().value(QLatin1String("moveToFolder")).toString(), QStringLiteral("AAMkFolder"));
        QVERIFY(!rule.value(QLatin1String("actions")).toObject().contains(QLatin1String("delete")));
    }

    // Export Calendar: an .ics that KCalendarCore reads back with the same events
    void calendarExport()
    {
        GraphMail::DisplayEvent meeting;
        meeting.graphId = QStringLiteral("AAMkMeeting");
        meeting.subject = QStringLiteral("Acme planning; Q3, draft");
        meeting.location = QStringLiteral("Room 1");
        meeting.start = QDateTime(QDate(2026, 3, 2), QTime(9, 30));
        meeting.end = QDateTime(QDate(2026, 3, 2), QTime(10, 0));
        meeting.firstDay = meeting.lastDay = QDate(2026, 3, 2);
        meeting.json = QJsonObject{{QStringLiteral("iCalUId"), QStringLiteral("uid-1@example.com")}};
        GraphMail::DisplayEvent holiday;
        holiday.graphId = QStringLiteral("AAMkDay");
        holiday.subject = QStringLiteral("Day off");
        holiday.allDay = true;
        holiday.showAs = 0;
        holiday.firstDay = QDate(2026, 3, 5);
        holiday.lastDay = QDate(2026, 3, 6);
        // Two occurrences of one series share Exchange's iCalendar id
        GraphMail::DisplayEvent first = meeting;
        first.type = 1;
        first.firstDay = first.lastDay = QDate(2026, 3, 9);
        GraphMail::DisplayEvent second = first;
        second.firstDay = second.lastDay = QDate(2026, 3, 16);
        const QByteArray ics = GraphMail::CalendarExport::toIcs({meeting, holiday, first, second}, QStringLiteral("Calendar"));
        auto calendar = KCalendarCore::MemoryCalendar::Ptr::create(QTimeZone::systemTimeZone());
        QVERIFY(KCalendarCore::ICalFormat().fromString(calendar, QString::fromUtf8(ics)));
        QCOMPARE(calendar->events().size(), 4);
        const KCalendarCore::Event::Ptr read = calendar->event(QStringLiteral("uid-1@example.com"));
        QVERIFY(read);
        QCOMPARE(read->summary(), meeting.subject);
        QCOMPARE(read->location(), QStringLiteral("Room 1"));
        QCOMPARE(read->dtStart().toLocalTime(), meeting.start);
        const KCalendarCore::Event::Ptr day = calendar->event(QStringLiteral("AAMkDay"));
        QVERIFY(day && day->allDay());
        QCOMPARE(day->dtEnd().date(), QDate(2026, 3, 6));
        QCOMPARE(day->transparency(), KCalendarCore::Event::Transparent);
        QVERIFY(calendar->event(QStringLiteral("uid-1@example.com-2026-03-16")));
    }

    // Conditional formatting: sender name or address, or subject; off rules and empty text never
    void conditionalFormatting()
    {
        using namespace GraphMail::ConditionalFormat;
        Rule boss{QStringLiteral("Boss"), QStringLiteral("sender"), QStringLiteral("jane@example.com"), QColor(Qt::red), true};
        Rule urgent{QStringLiteral("Urgent"), QStringLiteral("subject"), QStringLiteral("urgent"), QColor(Qt::blue), true};
        Rule off{QStringLiteral("Off"), QStringLiteral("sender"), QStringLiteral("acme"), QColor(Qt::green), false};
        const QList<Rule> list{off, boss, urgent};
        QCOMPARE(colorFor(list, QStringLiteral("Jane Doe"), QStringLiteral("JANE@example.com"), QStringLiteral("URGENT: invoice")), QColor(Qt::red));
        QCOMPARE(colorFor(list, QStringLiteral("Acme Sales"), QStringLiteral("sales@acme.example"), QStringLiteral("Urgent offer")), QColor(Qt::blue));
        QVERIFY(!colorFor(list, QStringLiteral("Acme Sales"), QStringLiteral("sales@acme.example"), QStringLiteral("Offer")));
        QVERIFY(!(Rule{QString(), QStringLiteral("subject"), QStringLiteral("  "), QColor(Qt::red), true}.matches({}, {}, QStringLiteral("anything"))));
    }

    // Read receipts: Exchange sends when marked read, so every way of marking decides first
    void readReceiptDecisions()
    {
        using namespace GraphMail::ReadReceipts;
        QCOMPARE(decide(Mode::Always, true, false), Decision::Send);
        QCOMPARE(decide(Mode::Always, std::nullopt, true), Decision::Send);
        QCOMPARE(decide(Mode::Never, true, false), Decision::DontSend);
        QCOMPARE(decide(Mode::Ask, true, false), Decision::Ask);
        // Mark as Read by hand in Ask mode: no receipt without a yes
        QCOMPARE(decide(Mode::Ask, true, true), Decision::DontSend);
        QCOMPARE(decide(Mode::Ask, false, false), Decision::DontSend);
        // Not known yet: the reading pane marks it read once the headers are there
        QCOMPARE(decide(Mode::Ask, std::nullopt, false), Decision::Wait);
    }

    // Clean Up Old Mail: off by default; a folder's own setting counts anyway; some folders never
    // Pasted HTML (Outlook on the web's signature) rewritten into what Qt's import keeps
    void pastedHtmlForQt()
    {
        using GraphMail::MailEditor;
        // A cell's CSS width as the attribute Qt reads
        QVERIFY(MailEditor::htmlForQt(QStringLiteral("<td style=\"vertical-align:top; width:75px; height:auto\">")).contains(QLatin1String("width=\"75\"")));
        // Its side padding counts in (Qt's width holds it); a table keeps no width (a browser widens it)
        QVERIFY(MailEditor::htmlForQt(QStringLiteral("<td style=\"padding-right:7px; vertical-align:top; width:18.8334px\">")).contains(QLatin1String("width=\"26\"")));
        QVERIFY(!MailEditor::htmlForQt(QStringLiteral("<table style=\"width:37.6667px; border-collapse:collapse\">")).contains(QLatin1String("width=")));
        // A unitless line height: the font size times it, in pixels (12pt = 16px, x 1.333)
        const QString p = MailEditor::htmlForQt(QStringLiteral("<p style=\"line-height:1.333; margin:0.1pt; font-size:12pt\">x</p>"));
        QVERIFY2(p.contains(QLatin1String("line-height:21px")), qPrintable(p));
        // "initial" (and one without a font size) is left to Qt's own spacing
        QVERIFY(!MailEditor::htmlForQt(QStringLiteral("<p style=\"line-height:initial; color:red\">x</p>")).contains(QLatin1String("line-height")));
        // A paragraph with only a line break: one empty line, as in a browser
        QCOMPARE(MailEditor::htmlForQt(QStringLiteral("<p style=\"margin:0\"><span>  <br></span></p>")), QStringLiteral("<p style=\"margin:0\"><span>&nbsp;</span></p>"));
        // Everything else stays as it was
        QCOMPARE(MailEditor::htmlForQt(QStringLiteral("<b>Hello</b> <a href=\"mailto:a@example.com\">a</a>")), QStringLiteral("<b>Hello</b> <a href=\"mailto:a@example.com\">a</a>"));
    }

    void cleanupRules()
    {
        using namespace GraphMail::Cleanup;
        Settings off;
        QVERIFY(!off.enabled);
        QCOMPARE(off.days, 365);
        FolderRule inherit;
        QVERIFY(!effective(off, inherit));
        FolderRule own{FolderRule::Mode::Own, Action::Delete, 30};
        QVERIFY(effective(off, own));
        QCOMPARE(effective(off, own)->days, 30);
        Settings on = off;
        on.enabled = true;
        on.action = Action::ExportPst;
        QCOMPARE(effective(on, inherit)->action, Action::ExportPst);
        QVERIFY(!effective(on, FolderRule{FolderRule::Mode::Off, Action::Archive, 365}));
        QVERIFY(excluded(QStringLiteral("deleteditems")) && excluded(QStringLiteral("archive")) && excluded(QStringLiteral("drafts")));
        QVERIFY(!excluded(QStringLiteral("inbox")) && !excluded(QString()));
    }

    // Dragging messages: a small envelope (the folder pane stays visible), a count for several
    void dragPixmap()
    {
        const QPixmap one = GraphMail::MessageListView::dragPixmap(1, 1.0);
        QCOMPARE(one.size(), QSize(32, 32));
        const QPixmap three = GraphMail::MessageListView::dragPixmap(3, 2.0);
        QCOMPARE(three.deviceIndependentSize().toSize(), QSize(41, 41));
        // The badge (beside its number) has the accent colour
        const QImage image = three.toImage();
        QCOMPARE(QColor(image.pixel(image.width() - 32, 18)).rgb(), QApplication::palette().color(QPalette::Highlight).rgb());
        if (const QByteArray dir = qgetenv("GRAPHMAIL_SAVE_DRAG"); !dir.isEmpty()) {
            one.save(QString::fromLocal8Bit(dir) + QStringLiteral("/one.png"));
            three.save(QString::fromLocal8Bit(dir) + QStringLiteral("/three.png"));
        }
    }
};

QTEST_MAIN(AppModelsTest)

#include "appmodelstest.moc"
