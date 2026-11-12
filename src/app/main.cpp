// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "accounts.h"
#include "calendarsharingdialog.h"
#include "calendarwindow.h"
#include "composer.h"
#include "contactswindow.h"
#include "taskswindow.h"
#include "eventdialog.h"
#include "icsdialog.h"
#include "recurrence.h"
#include "daemonclient.h"
#include "mailboxset.h"
#include "mailview.h"
#include "mailboxesdialog.h"
#include "mainwindow.h"
#include "shell.h"
#include "profilephotos.h"
#include "autorepliesdialog.h"
#include "rulesdialog.h"
#include "templates.h"
#include "mailboxsettingsdialog.h"
#include "waitingwindow.h"
#include "offlinedialog.h"
#include "readingpane.h"

#include <KAboutData>
#include <KDBusService>
#include <KLocalizedString>

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

#include <memory>

namespace
{
// "<account>/<mailbox>/<message id>" from a notification's Open
void openMessage(GraphMail::MainWindow *window, const QString &spec)
{
    const QStringList parts = spec.split(QLatin1Char('/'));
    if (parts.size() == 3) {
        window->showMessage(parts.at(0) + QLatin1Char('/') + parts.at(1), parts.at(2).toLongLong());
    }
}
}

int main(int argc, char **argv)
{
    // QtWebEngine: custom schemes and shared OpenGL contexts before the application exists
    GraphMail::MailView::registerSchemes();
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    KLocalizedString::setApplicationDomain("plasma-graphmail-linux");
    // Accounts are shared by the suite; the app registration per account is this app's own
    GraphCore::Accounts::setApplication(QStringLiteral("plasma-graphmail-linux"));

    KAboutData about(QStringLiteral("plasma-graphmail-linux"),
                     i18n("Graph Mail"),
                     QStringLiteral(GRAPHMAIL_VERSION),
                     i18n("Mail, calendar and contacts for Microsoft 365 on KDE Plasma"),
                     KAboutLicense::GPL_V3,
                     i18n("Not affiliated with or endorsed by Microsoft. Microsoft 365 and Exchange Online are trademarks of Microsoft Corporation."));
    about.setOrganizationDomain("nvaert1986.github.io");
    about.setDesktopFileName(QStringLiteral("io.github.nvaert1986.graphmail"));
    KAboutData::setApplicationData(about);
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("io.github.nvaert1986.graphmail"), QIcon::fromTheme(QStringLiteral("mail-client"))));

    QCommandLineParser parser;
    about.setupCommandLine(&parser);
    parser.addOptions({
        {QStringLiteral("message"), i18n("Show this message (account/mailbox/id)"), QStringLiteral("spec")},
        {QStringLiteral("folder"), i18n("Open this folder (account/mailbox/folder id)"), QStringLiteral("path")},
        {QStringLiteral("screenshot"), i18n("Save a picture of the window to this file and quit (tests; never marks anything as read)"), QStringLiteral("file")},
        {QStringLiteral("size"), i18n("Window size for --screenshot (WIDTHxHEIGHT)"), QStringLiteral("size"), QStringLiteral("1300x800")},
        {QStringLiteral("select"), i18n("Select this message in the list for --screenshot (row number)"), QStringLiteral("row")},
        {QStringLiteral("body-pdf"), i18n("With --screenshot: also save the rendered message body as PDF (tests)"), QStringLiteral("file")},
        {QStringLiteral("save-attachments"), i18n("With --screenshot: save all attachments of the shown message into this folder first (tests)"), QStringLiteral("folder")},
        {QStringLiteral("open-window"), i18n("With --screenshot and --select: picture of the message in its own window (tests)")},
        {QStringLiteral("outgoing"), i18n("Open a message that was not sent (account/mailbox/id)"), QStringLiteral("spec")},
        {QStringLiteral("calendar"), i18n("Open the calendar")},
        {QStringLiteral("event"), i18n("Open this event (account/mailbox/id)"), QStringLiteral("spec")},
        {QStringLiteral("calendar-view"), i18n("With --calendar: day, workweek, week, month or agenda"), QStringLiteral("view")},
        {QStringLiteral("calendar-date"), i18n("With --calendar: the date to show (yyyy-MM-dd)"), QStringLiteral("date")},
        {QStringLiteral("new-event"), i18n("With --calendar and --screenshot: a new event with this title, saved (tests)"), QStringLiteral("title")},
        {QStringLiteral("event-start"), i18n("With --new-event: start (yyyy-MM-ddTHH:mm)"), QStringLiteral("time")},
        {QStringLiteral("event-end"), i18n("With --new-event: end (yyyy-MM-ddTHH:mm)"), QStringLiteral("time")},
        {QStringLiteral("event-attendees"), i18n("With --new-event: attendees (tests)"), QStringLiteral("addresses")},
        {QStringLiteral("event-teams"), i18n("With --new-event: a Teams meeting (tests)")},
        {QStringLiteral("event-weekly"), i18n("With --new-event: repeat weekly, this many times (tests)"), QStringLiteral("count")},
        {QStringLiteral("event-recurrence"), i18n("With --new-event: this repeat rule, Graph JSON (tests)"), QStringLiteral("json")},
        {QStringLiteral("event-allday"), i18n("With --new-event: an all-day event (tests)")},
        {QStringLiteral("event-calendar"), i18n("With --new-event: in this mailbox's calendar, account/mailbox (tests)"), QStringLiteral("mailbox")},
        {QStringLiteral("event-room"), i18n("With --new-event: book this room, name|address (tests)"), QStringLiteral("room")},
        {QStringLiteral("edit-event"), i18n("With --screenshot: open this event, change it and save (tests)"), QStringLiteral("spec")},
        {QStringLiteral("edit-title"), i18n("With --edit-event: the new title (tests)"), QStringLiteral("title")},
        {QStringLiteral("edit-shift"), i18n("With --edit-event: move it by this many minutes (tests)"), QStringLiteral("minutes")},
        {QStringLiteral("categorize-item"), i18n("With --tasks or --contacts and --screenshot: give a task or contact a category or take it away, spec|name|on/off (tests)"), QStringLiteral("spec")},
        {QStringLiteral("categorize-event"), i18n("With --calendar and --screenshot: give an event a category or take it away, spec|name|on/off (tests)"), QStringLiteral("spec")},
        {QStringLiteral("delete-event"), i18n("With --screenshot: delete this event as the calendar does (tests)"), QStringLiteral("spec")},
        {QStringLiteral("respond-event"), i18n("With --screenshot and --respond: answer this event from the calendar (tests)"), QStringLiteral("spec")},
        {QStringLiteral("respond"), i18n("accept, tentativelyAccept or decline (tests)"), QStringLiteral("action")},
        {QStringLiteral("meeting-respond"), i18n("With --message: answer the meeting request with this action (tests)"), QStringLiteral("action")},
        {QStringLiteral("test-series"), i18n("Tests: answer \"the series\" where it asks")},
        {QStringLiteral("test-comment"), i18n("Tests: the message of a cancellation or a response"), QStringLiteral("text")},
        {QStringLiteral("contacts"), i18n("Open the contacts")},
        {QStringLiteral("tasks"), i18n("Open the tasks")},
        {QStringLiteral("task"), i18n("Show this task (account/mailbox/id)"), QStringLiteral("spec")},
        {QStringLiteral("new-contact"), i18n("With --contacts and --screenshot: a new contact with these fields, JSON, saved (tests)"), QStringLiteral("json")},
        {QStringLiteral("contact-mailbox"), i18n("With --new-contact: in this mailbox, account/mailbox (tests)"), QStringLiteral("mailbox")},
        {QStringLiteral("edit-contact"), i18n("With --contacts and --screenshot: change this contact, see --contact-fields (tests)"), QStringLiteral("spec")},
        {QStringLiteral("contact-fields"), i18n("With --edit-contact: the fields to change, JSON (tests)"), QStringLiteral("json")},
        {QStringLiteral("delete-contact"), i18n("With --contacts and --screenshot: delete this contact (tests)"), QStringLiteral("spec")},
        {QStringLiteral("directory-search"), i18n("With --contacts and --screenshot: search the directory (tests)"), QStringLiteral("text")},
        {QStringLiteral("add-colleague"), i18n("With --calendar and --screenshot: Add a Colleague's Calendar for this address (tests)"), QStringLiteral("address")},
        {QStringLiteral("remove-colleague"), i18n("With --calendar and --screenshot: remove this colleague's calendar from Graph Mail (tests)"), QStringLiteral("address")},
        {QStringLiteral("share-calendar"), i18n("With --calendar and --screenshot: share the default calendar, address|role (tests)"), QStringLiteral("share")},
        {QStringLiteral("share-role"), i18n("With --calendar and --screenshot: change a person's permission on the default calendar, address|role (tests)"), QStringLiteral("share")},
        {QStringLiteral("share-remove"), i18n("With --calendar and --screenshot: stop sharing the default calendar with this address (tests)"), QStringLiteral("address")},
        {QStringLiteral("sharing"), i18n("With --calendar and --screenshot: show the default calendar's sharing (tests)")},
        {QStringLiteral("search"), i18n("With --screenshot: search for this text (tests)"), QStringLiteral("text")},
        {QStringLiteral("category"), i18n("With --screenshot: set|unset|create|recolor|delete a test category, action|name|colour (tests)"), QStringLiteral("spec")},
        {QStringLiteral("search-folder"), i18n("With --screenshot: create, open, change or delete a test search folder, action|name|filter (tests)"), QStringLiteral("spec")},
        {QStringLiteral("search-scope"), i18n("With --search: folder, mailbox or all (tests)"), QStringLiteral("scope")},
        {QStringLiteral("search-filter"), i18n("With --screenshot: tick search filters, comma separated (tests)"), QStringLiteral("filters")},
        {QStringLiteral("offline"), i18n("With --screenshot: open Settings > Offline; days|attachments|free to also save (tests)"), QStringLiteral("spec")},
        {QStringLiteral("auto-replies"),
         i18n("With --screenshot: open Automatic Replies; show, bar, turnoff, off|text, on|text or scheduled|start|end|text to also save (tests)"),
         QStringLiteral("spec")},
        {QStringLiteral("settings-mailbox"), i18n("With --auto-replies, --rules or --mailbox-settings: the own mailbox, account/me (tests)"), QStringLiteral("key")},
        {QStringLiteral("photo"), i18n("With --screenshot and --settings-mailbox: set this picture as the mailbox's photo, or remove it (remove) (tests)"), QStringLiteral("file")},
        {QStringLiteral("mailbox-settings"), i18n("With --screenshot: open Mailbox Settings; show, or hours|days|start|end to save working hours (tests)"), QStringLiteral("spec")},
        {QStringLiteral("rules"), i18n("With --screenshot: open Rules; show, editor|name|subject, create|name|subject, toggle|name, up|name or delete|name (tests)"), QStringLiteral("spec")},
        {QStringLiteral("waiting"), i18n("With --screenshot: open View > Waiting to Be Sent (tests)")},
        {QStringLiteral("mailboxes"), i18n("With --screenshot: open Settings > Mailboxes (tests)")},
        {QStringLiteral("check-mailbox"), i18n("With --mailboxes: check what this address gives (tests)"), QStringLiteral("address")},
        {QStringLiteral("add-mailbox"), i18n("With --mailboxes: add this mailbox after the check (tests)"), QStringLiteral("address")},
        {QStringLiteral("remove-mailbox"), i18n("With --mailboxes: remove this mailbox, without asking (tests)"), QStringLiteral("address")},
        {QStringLiteral("add-from-directory"), i18n("With --contacts and --screenshot: Add to Contacts for this address from the directory, marked as a test (tests)"), QStringLiteral("address")},
        {QStringLiteral("add-task"), i18n("With --tasks and --screenshot: add a task, list|title (tests)"), QStringLiteral("task")},
        {QStringLiteral("complete-task"), i18n("With --tasks and --screenshot: tick this task (tests)"), QStringLiteral("spec")},
        {QStringLiteral("reopen-task"), i18n("With --tasks and --screenshot: untick this task (tests)"), QStringLiteral("spec")},
        {QStringLiteral("edit-task"), i18n("With --tasks and --screenshot: change this task, see --task-fields (tests)"), QStringLiteral("spec")},
        {QStringLiteral("task-fields"), i18n("With --edit-task: the fields to change, JSON (tests)"), QStringLiteral("json")},
        {QStringLiteral("task-step"), i18n("With --tasks and --screenshot: add a step to this task, spec|text (tests)"), QStringLiteral("step")},
        {QStringLiteral("tick-step"), i18n("With --tasks and --screenshot: tick this step of this task, spec|text (tests)"), QStringLiteral("step")},
        {QStringLiteral("open-task-mail"), i18n("With --tasks and --screenshot: Open the Mail of this flagged-mail task (tests)"), QStringLiteral("spec")},
        {QStringLiteral("delete-task"), i18n("With --tasks and --screenshot: delete this task (tests)"), QStringLiteral("spec")},
        {QStringLiteral("task-list"), i18n("With --tasks and --screenshot: create, rename or delete a list, action|name|new name (tests)"), QStringLiteral("list")},
        {QStringLiteral("ics-import"), i18n("With --screenshot: import this .ics file into the default calendar (tests)"), QStringLiteral("file")},
        {QStringLiteral("compose"), i18n("With --screenshot: open a composer: new, reply, replyAll or forward (the --select row) (tests)"), QStringLiteral("kind")},
        {QStringLiteral("compose-to"), i18n("With --compose: the recipients (tests)"), QStringLiteral("addresses")},
        {QStringLiteral("compose-from-other"), i18n("With --compose: From this other address, address|name (tests)"), QStringLiteral("from")},
        {QStringLiteral("compose-subject"), i18n("With --compose: the subject (tests)"), QStringLiteral("text")},
        {QStringLiteral("compose-text"), i18n("With --compose: text to type (tests)"), QStringLiteral("text")},
        {QStringLiteral("compose-attach"), i18n("With --compose: a file to attach; may be repeated (tests)"), QStringLiteral("file")},
        {QStringLiteral("compose-send"), i18n("With --compose: send it, then quit (tests)")},
        {QStringLiteral("compose-options"), i18n("With --compose: read, delivery, sensitivity=N, defer=ISO time, comma separated (tests)"), QStringLiteral("options")},
        {QStringLiteral("compose-template-save"), i18n("With --compose: save it as a template with this name (tests)"), QStringLiteral("name")},
        {QStringLiteral("compose-template"), i18n("With --compose: fill it from the template with this name (tests)"), QStringLiteral("name")},
        {QStringLiteral("undo-send"), i18n("With --compose-send: press Undo after this many seconds (tests)"), QStringLiteral("seconds")},
        {QStringLiteral("quick-step"), i18n("With --select: run the Quick Step with this name (tests)"), QStringLiteral("name")},
        {QStringLiteral("focus-move"), i18n("With --select: move the message to Other (other) or Focused (focused), or always (always-other, always-focused) (tests)"), QStringLiteral("where")},
        {QStringLiteral("junk"), i18n("With --select: report the message as junk (junk) or not junk (notjunk) (tests)"), QStringLiteral("what")},
        {QStringLiteral("ignore-conversation"), i18n("With --select: ignore the message's conversation, or stop (off) (tests)"), QStringLiteral("on")},
        {QStringLiteral("reading-pane"), i18n("With --screenshot: right, bottom or off (tests)"), QStringLiteral("where")},
        {QStringLiteral("focus-tab"), i18n("With --folder on an Inbox: show Focused or Other (tests)"), QStringLiteral("tab")},
        {QStringLiteral("flag-when"), i18n("With --select: flag the message today, tomorrow, thisweek, nextweek, none, complete or clear (tests)"), QStringLiteral("when")},
        {QStringLiteral("compose-save"), i18n("With --compose: save it as a draft, then quit (tests)")},
        {QStringLiteral("load-images"), i18n("With --screenshot: load internet content as if \"Load Images\" was clicked (tests)")},
    });
    parser.process(app);
    about.processCommandLine(&parser);

    const bool screenshot = parser.isSet(QStringLiteral("screenshot"));
    // One instance: a second start (menu, a notification's Open) hands over and exits.
    // Screenshots (tests) always run on their own.
    std::unique_ptr<KDBusService> unique;
    if (!screenshot) {
        unique = std::make_unique<KDBusService>(KDBusService::Unique);
    }

    GraphMail::MailboxSet mailboxes;
    mailboxes.reload();
    GraphMail::DaemonClient daemon;
    // The one application window (docs/PHASE9.md) around the mail module and the others. On the
    // heap: it deletes itself when it is closed (WA_DeleteOnClose)
    auto *shell = new GraphMail::Shell(&mailboxes, &daemon);
    GraphMail::MainWindow *window = shell->mail();
    if (screenshot) {
        window->setMarkReadEnabled(false);
        window->setTestMode();
    }

    if (parser.isSet(QStringLiteral("reading-pane"))) {
        window->setReadingPane(parser.value(QStringLiteral("reading-pane")));
    }
    if (parser.isSet(QStringLiteral("folder"))) {
        window->selectFolder(parser.value(QStringLiteral("folder")));
    }
    if (parser.isSet(QStringLiteral("focus-tab"))) {
        window->setFocusTabForTests(parser.value(QStringLiteral("focus-tab")) == QLatin1String("other") ? 1 : 0);
    }
    if (parser.isSet(QStringLiteral("message"))) {
        openMessage(window, parser.value(QStringLiteral("message")));
    }
    auto openOutgoing = [window](const QString &spec) {
        const QStringList parts = spec.split(QLatin1Char('/'));
        if (parts.size() == 3) {
            window->openOutgoing(parts.at(0) + QLatin1Char('/') + parts.at(1), parts.at(2));
        }
    };
    if (parser.isSet(QStringLiteral("outgoing"))) {
        openOutgoing(parser.value(QStringLiteral("outgoing")));
    }
    auto openEvent = [window](const QString &spec) {
        const QStringList parts = spec.split(QLatin1Char('/'));
        if (parts.size() == 3) {
            window->openCalendar()->openEvent(parts.at(0) + QLatin1Char('/') + parts.at(1), parts.at(2).toLongLong());
        }
    };
    GraphMail::CalendarWindow *calendar = nullptr;
    const bool calendarTest = screenshot
        && (parser.isSet(QStringLiteral("edit-event")) || parser.isSet(QStringLiteral("delete-event")) || parser.isSet(QStringLiteral("respond-event"))
            || parser.isSet(QStringLiteral("categorize-event")));
    if (parser.isSet(QStringLiteral("calendar")) || parser.isSet(QStringLiteral("event")) || calendarTest) {
        calendar = window->openCalendar();
        const QStringList views{QStringLiteral("day"), QStringLiteral("workweek"), QStringLiteral("week"), QStringLiteral("month"), QStringLiteral("agenda")};
        if (const int view = int(views.indexOf(parser.value(QStringLiteral("calendar-view")))); view >= 0) {
            calendar->setMode(GraphMail::CalendarWindow::Mode(view));
        }
        if (parser.isSet(QStringLiteral("calendar-date"))) {
            calendar->goTo(QDate::fromString(parser.value(QStringLiteral("calendar-date")), Qt::ISODate));
        }
    }
    if (parser.isSet(QStringLiteral("event"))) {
        openEvent(parser.value(QStringLiteral("event")));
    }
    auto openTask = [window](const QString &spec) {
        const QStringList parts = spec.split(QLatin1Char('/'));
        if (parts.size() == 3) {
            window->openTasks()->showTask(parts.at(0) + QLatin1Char('/') + parts.at(1), parts.at(2).toLongLong());
        }
    };
    GraphMail::ContactsWindow *contacts = parser.isSet(QStringLiteral("contacts")) ? window->openContacts() : nullptr;
    GraphMail::TasksWindow *tasks = parser.isSet(QStringLiteral("tasks")) || parser.isSet(QStringLiteral("task")) ? window->openTasks() : nullptr;
    if (parser.isSet(QStringLiteral("task"))) {
        openTask(parser.value(QStringLiteral("task")));
    }
    if (unique) {
        QObject::connect(unique.get(), &KDBusService::activateRequested, window, [window, shell, openOutgoing, openEvent, openTask](const QStringList &arguments, const QString &) {
            QCommandLineParser again;
            again.addOption({QStringLiteral("message"), QString(), QStringLiteral("spec")});
            again.addOption({QStringLiteral("outgoing"), QString(), QStringLiteral("spec")});
            again.addOption({QStringLiteral("event"), QString(), QStringLiteral("spec")});
            again.addOption({QStringLiteral("calendar"), QString()});
            again.addOption({QStringLiteral("task"), QString(), QStringLiteral("spec")});
            again.addOption({QStringLiteral("contacts"), QString()});
            again.addOption({QStringLiteral("tasks"), QString()});
            again.parse(arguments);
            if (again.isSet(QStringLiteral("message"))) {
                openMessage(window, again.value(QStringLiteral("message")));
            }
            if (again.isSet(QStringLiteral("outgoing"))) {
                openOutgoing(again.value(QStringLiteral("outgoing")));
            }
            if (again.isSet(QStringLiteral("event"))) {
                openEvent(again.value(QStringLiteral("event")));
            } else if (again.isSet(QStringLiteral("calendar"))) {
                window->openCalendar();
            }
            if (again.isSet(QStringLiteral("task"))) {
                openTask(again.value(QStringLiteral("task")));
            } else if (again.isSet(QStringLiteral("tasks"))) {
                window->openTasks();
            }
            if (again.isSet(QStringLiteral("contacts"))) {
                window->openContacts();
            }
            shell->show();
            shell->raise();
            shell->activateWindow();
        });
    }

    if (screenshot) {
        const QStringList size = parser.value(QStringLiteral("size")).split(QLatin1Char('x'));
        shell->resize(size.value(0).toInt(), size.value(1).toInt());
        shell->show();
        if (parser.isSet(QStringLiteral("select"))) {
            window->selectRow(parser.value(QStringLiteral("select")).toInt());
        }
        if (parser.isSet(QStringLiteral("load-images"))) {
            window->loadImages();
        }
        const QString file = parser.value(QStringLiteral("screenshot"));
        const QString pdf = parser.value(QStringLiteral("body-pdf"));
        if (parser.isSet(QStringLiteral("save-attachments"))) {
            // Content that is not cached comes through the daemon: wait for it (at most 20 s)
            window->readingPane()->saveAllTo(parser.value(QStringLiteral("save-attachments")));
            QElapsedTimer waited;
            waited.start();
            while (window->readingPane()->pendingCount() > 0 && waited.elapsed() < 20000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            }
            qInfo() << "attachments still pending after save:" << window->readingPane()->pendingCount();
        }
        // The body is drawn by Chromium on its own surface, which grab() does not capture: the
        // PDF is how tests see the rendered message
        QWidget *shown = calendar ? static_cast<QWidget *>(calendar) : window;
        if (contacts) {
            shown = contacts;
        }
        if (tasks) {
            shown = tasks;
        }
        for (QWidget *w : std::initializer_list<QWidget *>{contacts, tasks}) {
            if (w) {
                w->resize(size.value(0).toInt(), size.value(1).toInt());
            }
        }
        auto spec = [](const QString &value) -> QPair<QString, qint64> {
            const QStringList parts = value.section(QLatin1Char('|'), 0, 0).split(QLatin1Char('/'));
            return parts.size() == 3 ? qMakePair(parts.at(0) + QLatin1Char('/') + parts.at(1), parts.at(2).toLongLong()) : qMakePair(QString(), qint64(0));
        };
        auto jsonOf = [&parser](const char *option) {
            return QJsonDocument::fromJson(parser.value(QLatin1String(option)).toUtf8()).object();
        };
        if (contacts && parser.isSet(QStringLiteral("new-contact"))) {
            if (auto *dialog = static_cast<QDialog *>(contacts->newContactForTests(parser.value(QStringLiteral("contact-mailbox")), jsonOf("new-contact")))) {
                shown = dialog;
                QTimer::singleShot(1500, dialog, &QDialog::accept);
            }
        }
        if (contacts && parser.isSet(QStringLiteral("edit-contact"))) {
            const auto [key, id] = spec(parser.value(QStringLiteral("edit-contact")));
            if (auto *dialog = static_cast<QDialog *>(contacts->editContactForTests(key, id, jsonOf("contact-fields")))) {
                shown = dialog;
                QTimer::singleShot(1500, dialog, &QDialog::accept);
            }
        }
        if (contacts && parser.isSet(QStringLiteral("delete-contact"))) {
            const auto [key, id] = spec(parser.value(QStringLiteral("delete-contact")));
            qWarning() << "contact delete test:" << contacts->deleteContactForTests(key, id);
        }
        if (parser.isSet(QStringLiteral("categorize-item"))) {
            const QString value = parser.value(QStringLiteral("categorize-item"));
            const QString spec = value.section(QLatin1Char('|'), 0, 0);
            const QString key = spec.section(QLatin1Char('/'), 0, 1);
            const qint64 id = spec.section(QLatin1Char('/'), 2).toLongLong();
            const QString name = value.section(QLatin1Char('|'), 1, 1);
            const bool on = value.section(QLatin1Char('|'), 2) == QLatin1String("on");
            const bool done = tasks ? tasks->categorizeForTests(key, id, name, on) : contacts && contacts->categorizeForTests(key, id, name, on);
            qWarning() << "categories test: item" << done;
        }
        if (contacts && parser.isSet(QStringLiteral("directory-search"))) {
            contacts->searchDirectoryForTests(parser.value(QStringLiteral("directory-search")));
        }
        if (contacts && parser.isSet(QStringLiteral("add-from-directory"))) {
            contacts->addFromDirectoryForTests(parser.value(QStringLiteral("add-from-directory")));
        }
        if (parser.isSet(QStringLiteral("category"))) {
            const QString spec = parser.value(QStringLiteral("category"));
            window->categoryForTests(spec.section(QLatin1Char('|'), 0, 0), spec.section(QLatin1Char('|'), 1, 1), spec.section(QLatin1Char('|'), 2));
        }
        if (parser.isSet(QStringLiteral("search-folder"))) {
            const QString spec = parser.value(QStringLiteral("search-folder"));
            window->searchFolderForTests(spec.section(QLatin1Char('|'), 0, 0), spec.section(QLatin1Char('|'), 1, 1), spec.section(QLatin1Char('|'), 2));
        }
        if (parser.isSet(QStringLiteral("search-filter"))) {
            window->filterForTests(parser.value(QStringLiteral("search-filter")).split(QLatin1Char(',')), parser.value(QStringLiteral("search-scope")));
        }
        if (parser.isSet(QStringLiteral("search"))) {
            window->searchForTests(parser.value(QStringLiteral("search")), parser.value(QStringLiteral("search-scope")));
        }
        if (parser.isSet(QStringLiteral("offline"))) {
            GraphMail::OfflineDialog *dialog = window->openOfflineSettings();
            shown = dialog;
            const QString spec = parser.value(QStringLiteral("offline"));
            if (spec.contains(QLatin1Char('|'))) {
                dialog->applyForTests(spec.section(QLatin1Char('|'), 0, 0).toInt(), spec.section(QLatin1Char('|'), 1, 1) == QLatin1String("yes"),
                                      spec.section(QLatin1Char('|'), 2, 2) == QLatin1String("free"));
            }
        }
        if (parser.isSet(QStringLiteral("auto-replies"))) {
            const QString spec = parser.value(QStringLiteral("auto-replies"));
            auto settle = [](int ms) {
                QElapsedTimer waited;
                waited.start();
                while (waited.elapsed() < ms) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                }
            };
            if (spec == QLatin1String("bar")) {
                window->showAutoRepliesBarForTests();
            } else if (spec == QLatin1String("turnoff")) {
                // The bar's Turn Off, once the settings are read
                settle(8000);
                window->turnOffAutoReplies();
                settle(8000);
            } else {
                GraphMail::AutoRepliesDialog *dialog = window->openAutoReplies(parser.value(QStringLiteral("settings-mailbox")));
                shown = dialog;
                if (spec == QLatin1String("show")) {
                    // Graph's answer comes through the daemon: wait for it (at most 10 s)
                    bool loaded = false;
                    QObject::connect(dialog, &GraphMail::AutoRepliesDialog::loadedForTests, dialog, [&loaded] {
                        loaded = true;
                    }, Qt::SingleShotConnection);
                    QElapsedTimer waited;
                    waited.start();
                    while (!loaded && waited.elapsed() < 10000) {
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                    }
                    qWarning() << "auto replies test: loaded" << loaded;
                } else {
                    const bool scheduled = spec.startsWith(QLatin1String("scheduled|"));
                    const QString kind = scheduled ? spec.section(QLatin1Char('|'), 0, 2) : spec.section(QLatin1Char('|'), 0, 0);
                    const QString text = spec.section(QLatin1Char('|'), scheduled ? 3 : 1);
                    QObject::connect(dialog, &GraphMail::AutoRepliesDialog::loadedForTests, dialog, [dialog, kind, text] {
                        dialog->saveForTests(kind, text);
                    }, Qt::SingleShotConnection);
                }
            }
        }
        if (parser.isSet(QStringLiteral("mailbox-settings"))) {
            GraphMail::MailboxSettingsDialog *dialog = window->openMailboxSettings(parser.value(QStringLiteral("settings-mailbox")));
            shown = dialog;
            bool loaded = false;
            QObject::connect(dialog, &GraphMail::MailboxSettingsDialog::loadedForTests, dialog, [&loaded] {
                loaded = true;
            }, Qt::SingleShotConnection);
            QElapsedTimer waited;
            waited.start();
            while (!loaded && waited.elapsed() < 15000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            }
            const QString spec = parser.value(QStringLiteral("mailbox-settings"));
            if (loaded && spec.startsWith(QLatin1String("hours|"))) {
                dialog->saveHoursForTests(spec.section(QLatin1Char('|'), 1, 1), spec.section(QLatin1Char('|'), 2, 2), spec.section(QLatin1Char('|'), 3, 3));
                waited.restart();
                while (waited.elapsed() < 8000) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                }
            }
        }
        if (parser.isSet(QStringLiteral("photo"))) {
            const QString file = parser.value(QStringLiteral("photo"));
            QTimer::singleShot(1500, window, [window, file, key = parser.value(QStringLiteral("settings-mailbox"))] {
                window->photos()->setForTests(key, file == QLatin1String("remove") ? QString() : file);
            });
        }
        if (parser.isSet(QStringLiteral("rules"))) {
            GraphMail::RulesDialog *dialog = window->openRules(parser.value(QStringLiteral("settings-mailbox")));
            shown = dialog;
            // Graph's answers come through the daemon: wait for the list, run the action, wait again
            auto waitLoaded = [dialog] {
                bool loaded = false;
                QObject::connect(dialog, &GraphMail::RulesDialog::loadedForTests, dialog, [&loaded] {
                    loaded = true;
                }, Qt::SingleShotConnection);
                QElapsedTimer waited;
                waited.start();
                while (!loaded && waited.elapsed() < 15000) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                }
                return loaded;
            };
            qWarning() << "rules test: loaded" << waitLoaded();
            const QString spec = parser.value(QStringLiteral("rules"));
            if (spec.startsWith(QLatin1String("editor|"))) {
                shown = dialog->editorForTests(spec.section(QLatin1Char('|'), 1, 1), spec.section(QLatin1Char('|'), 2));
            } else if (spec != QLatin1String("show")) {
                dialog->runForTests(spec);
                waitLoaded();
                // A move is two changes
                if (spec.startsWith(QLatin1String("up|"))) {
                    waitLoaded();
                }
            }
        }
        if (parser.isSet(QStringLiteral("waiting"))) {
            GraphMail::WaitingWindow *waiting = window->openWaiting();
            shown = waiting;
            qWarning() << "waiting test:" << waiting->messageCount() << "messages," << waiting->changeCount() << "changes";
        }
        if (parser.isSet(QStringLiteral("mailboxes"))) {
            GraphMail::MailboxesDialog *dialog = window->openMailboxes();
            dialog->resize(900, 600);
            shown = dialog;
            if (parser.isSet(QStringLiteral("check-mailbox"))) {
                dialog->checkForTests(parser.value(QStringLiteral("check-mailbox")), false);
            } else if (parser.isSet(QStringLiteral("add-mailbox"))) {
                dialog->checkForTests(parser.value(QStringLiteral("add-mailbox")), true);
            } else if (parser.isSet(QStringLiteral("remove-mailbox"))) {
                dialog->removeForTests(parser.value(QStringLiteral("remove-mailbox")));
            }
        }
        if (tasks) {
            const QString value = parser.value(QStringLiteral("add-task"));
            if (!value.isEmpty()) {
                qWarning() << "task add test:" << tasks->addTaskForTests(value.section(QLatin1Char('|'), 0, 0), value.section(QLatin1Char('|'), 1));
            }
            if (parser.isSet(QStringLiteral("complete-task")) || parser.isSet(QStringLiteral("reopen-task"))) {
                const bool done = parser.isSet(QStringLiteral("complete-task"));
                const auto [key, id] = spec(parser.value(done ? QStringLiteral("complete-task") : QStringLiteral("reopen-task")));
                qWarning() << "task tick test:" << tasks->completeForTests(key, id, done);
            }
            if (parser.isSet(QStringLiteral("edit-task"))) {
                const auto [key, id] = spec(parser.value(QStringLiteral("edit-task")));
                qWarning() << "task edit test:" << tasks->editTaskForTests(key, id, jsonOf("task-fields"));
            }
            if (parser.isSet(QStringLiteral("task-step"))) {
                const QString value = parser.value(QStringLiteral("task-step"));
                const auto [key, id] = spec(value);
                qWarning() << "task step test:" << tasks->addStepForTests(key, id, value.section(QLatin1Char('|'), 1));
            }
            if (parser.isSet(QStringLiteral("tick-step"))) {
                const QString value = parser.value(QStringLiteral("tick-step"));
                const auto [key, id] = spec(value);
                tasks->tickStepForTests(key, id, value.section(QLatin1Char('|'), 1));
            }
            if (parser.isSet(QStringLiteral("open-task-mail"))) {
                const auto [key, id] = spec(parser.value(QStringLiteral("open-task-mail")));
                qWarning() << "open mail test:" << tasks->openMailForTests(key, id);
            }
            if (parser.isSet(QStringLiteral("delete-task"))) {
                const auto [key, id] = spec(parser.value(QStringLiteral("delete-task")));
                qWarning() << "task delete test:" << tasks->deleteTaskForTests(key, id);
            }
            if (parser.isSet(QStringLiteral("task-list"))) {
                const QStringList parts = parser.value(QStringLiteral("task-list")).split(QLatin1Char('|'));
                qWarning() << "task list test:" << tasks->listActionForTests(parts.value(0), parts.value(1), parts.value(2));
            }
        }
        if (calendar) {
            calendar->resize(size.value(0).toInt(), size.value(1).toInt());
        }
        if (calendar && parser.isSet(QStringLiteral("event")) && calendar->lastDialog()) {
            shown = calendar->lastDialog();
        }
        if (calendar) {
            calendar->setTestAnswers(parser.isSet(QStringLiteral("test-series")), parser.value(QStringLiteral("test-comment")));
        }
        auto specOf = [&parser](const char *option) -> QPair<QString, qint64> {
            const QStringList parts = parser.value(QLatin1String(option)).split(QLatin1Char('/'));
            return parts.size() == 3 ? qMakePair(parts.at(0) + QLatin1Char('/') + parts.at(1), parts.at(2).toLongLong()) : qMakePair(QString(), qint64(0));
        };
        if (calendar && parser.isSet(QStringLiteral("add-colleague"))) {
            calendar->addColleagueForTests(parser.value(QStringLiteral("add-colleague")));
        }
        if (calendar && parser.isSet(QStringLiteral("remove-colleague"))) {
            calendar->removeColleagueForTests(parser.value(QStringLiteral("remove-colleague")));
        }
        if (calendar
            && (parser.isSet(QStringLiteral("sharing")) || parser.isSet(QStringLiteral("share-calendar")) || parser.isSet(QStringLiteral("share-role"))
                || parser.isSet(QStringLiteral("share-remove")))) {
            if (GraphMail::CalendarSharingDialog *sharing = calendar->openSharing()) {
                shown = sharing;
                const QString share = parser.value(QStringLiteral("share-calendar"));
                const QString role = parser.value(QStringLiteral("share-role"));
                if (!share.isEmpty()) {
                    sharing->addForTests(share.section(QLatin1Char('|'), 0, 0), share.section(QLatin1Char('|'), 1));
                } else if (!role.isEmpty()) {
                    sharing->setRoleForTests(role.section(QLatin1Char('|'), 0, 0), role.section(QLatin1Char('|'), 1));
                } else if (parser.isSet(QStringLiteral("share-remove"))) {
                    sharing->removeForTests(parser.value(QStringLiteral("share-remove")));
                }
            }
        }
        if (calendar && parser.isSet(QStringLiteral("edit-event"))) {
            const auto [key, id] = specOf("edit-event");
            if (auto *dialog = static_cast<GraphMail::EventDialog *>(calendar->editEventForTests(key, id))) {
                dialog->applyTestEdit(parser.value(QStringLiteral("edit-title")), parser.value(QStringLiteral("edit-shift")).toInt());
                shown = dialog;
                QTimer::singleShot(1500, dialog, [dialog] {
                    dialog->saveNow();
                });
            }
        }
        if (calendar && parser.isSet(QStringLiteral("categorize-event"))) {
            const QString value = parser.value(QStringLiteral("categorize-event"));
            const QString spec = value.section(QLatin1Char('|'), 0, 0);
            calendar->categorizeEventForTests(spec.section(QLatin1Char('/'), 0, 1), spec.section(QLatin1Char('/'), 2).toLongLong(), value.section(QLatin1Char('|'), 1, 1),
                                             value.section(QLatin1Char('|'), 2) == QLatin1String("on"));
        }
        if (calendar && parser.isSet(QStringLiteral("delete-event"))) {
            const auto [key, id] = specOf("delete-event");
            QTimer::singleShot(1000, calendar, [calendar, key, id] {
                qWarning() << "delete test:" << calendar->deleteEventForTests(key, id);
            });
        }
        if (calendar && parser.isSet(QStringLiteral("respond-event"))) {
            const auto [key, id] = specOf("respond-event");
            const QString action = parser.value(QStringLiteral("respond"));
            QTimer::singleShot(1000, calendar, [calendar, key, id, action] {
                if (action.startsWith(QLatin1String("propose|"))) {
                    qWarning() << "propose test:" << calendar->proposeForTests(key, id, action);
                    return;
                }
                qWarning() << "respond test:" << calendar->respondForTests(key, id, action);
            });
        }
        if (parser.isSet(QStringLiteral("meeting-respond"))) {
            // The bar needs the daemon's answer first
            const QString action = parser.value(QStringLiteral("meeting-respond"));
            const QString comment = parser.value(QStringLiteral("test-comment"));
            auto *poll = new QTimer(window);
            poll->setInterval(200);
            QObject::connect(poll, &QTimer::timeout, window, [window, poll, action, comment] {
                if (window->readingPane()->respondToMeetingForTests(action, comment)) {
                    qWarning() << "meeting respond test: sent";
                    poll->stop();
                }
            });
            poll->start();
        }
        if (parser.isSet(QStringLiteral("ics-import"))) {
            if (auto *ics = GraphMail::IcsDialog::open(parser.value(QStringLiteral("ics-import")), &mailboxes, &daemon, window)) {
                ics->show();
                shown = ics;
                QTimer::singleShot(1500, ics, [ics] {
                    ics->add();
                });
            } else {
                qWarning() << "ics test: the file holds no events";
            }
        }
        if (calendar && parser.isSet(QStringLiteral("new-event"))) {
            const QDateTime start = QDateTime::fromString(parser.value(QStringLiteral("event-start")), Qt::ISODate);
            const QDateTime end = QDateTime::fromString(parser.value(QStringLiteral("event-end")), Qt::ISODate);
            const QString title = parser.value(QStringLiteral("new-event"));
            if (auto *dialog = static_cast<GraphMail::EventDialog *>(calendar->newEventForTests(start, end))) {
                dialog->setTestInput(title, QStringLiteral("Test location"), parser.value(QStringLiteral("event-attendees")), 15, parser.isSet(QStringLiteral("event-teams")));
                if (parser.isSet(QStringLiteral("event-calendar"))) {
                    dialog->selectCalendarForTests(parser.value(QStringLiteral("event-calendar")));
                }
                if (parser.isSet(QStringLiteral("event-room"))) {
                    const QStringList room = parser.value(QStringLiteral("event-room")).split(QLatin1Char('|'));
                    dialog->setTestRoom(room.value(0), room.value(1));
                }
                if (parser.isSet(QStringLiteral("event-allday"))) {
                    dialog->setTestAllDay(true);
                }
                if (parser.isSet(QStringLiteral("event-recurrence"))) {
                    dialog->setTestRecurrence(QJsonDocument::fromJson(parser.value(QStringLiteral("event-recurrence")).toUtf8()).object());
                }
                if (parser.isSet(QStringLiteral("event-weekly"))) {
                    QJsonObject recurrence = GraphMail::presetRecurrence(GraphMail::RepeatPreset::Weekly, start.date());
                    QJsonObject range = recurrence.value(QLatin1String("range")).toObject();
                    range.insert(QStringLiteral("type"), QStringLiteral("numbered"));
                    range.insert(QStringLiteral("numberOfOccurrences"), parser.value(QStringLiteral("event-weekly")).toInt());
                    recurrence.insert(QStringLiteral("range"), range);
                    dialog->setTestRecurrence(recurrence);
                }
                shown = dialog;
                // Tests only save what is clearly marked as a test
                if (title.contains(QLatin1String("[TEST Graph Mail]"))) {
                    QTimer::singleShot(1500, dialog, [dialog] {
                        dialog->saveNow();
                    });
                }
            }
        }
        if (parser.isSet(QStringLiteral("compose"))) {
            const QString kind = parser.value(QStringLiteral("compose"));
            const qint64 message = kind == QLatin1String("new") ? 0 : window->readingPane()->message();
            auto *composer = static_cast<GraphMail::ComposerWindow *>(window->openComposer(kind, message));
            composer->setTestMode();
            composer->resize(900, 760);
            if (parser.isSet(QStringLiteral("compose-from-other"))) {
                const QString other = parser.value(QStringLiteral("compose-from-other"));
                composer->setOtherFromForTests(other.section(QLatin1Char('|'), 0, 0), other.section(QLatin1Char('|'), 1));
            }
            composer->setTestInput(parser.value(QStringLiteral("compose-to")),
                                   parser.value(QStringLiteral("compose-subject")),
                                   parser.value(QStringLiteral("compose-text")),
                                   parser.values(QStringLiteral("compose-attach")));
            if (parser.isSet(QStringLiteral("compose-options"))) {
                composer->setOptionsForTests(parser.value(QStringLiteral("compose-options")).split(QLatin1Char(','), Qt::SkipEmptyParts));
            }
            if (parser.isSet(QStringLiteral("compose-template"))) {
                const QList<QJsonObject> all = GraphMail::Templates::all();
                for (const QJsonObject &t : all) {
                    if (t.value(QLatin1String("name")).toString() == parser.value(QStringLiteral("compose-template"))) {
                        composer->applyTemplate(t);
                        qWarning() << "template test: applied" << t.value(QLatin1String("id")).toString();
                    }
                }
            }
            if (parser.isSet(QStringLiteral("compose-template-save"))) {
                QJsonObject t = composer->templateJson();
                t.insert(QStringLiteral("name"), parser.value(QStringLiteral("compose-template-save")));
                qWarning() << "template test: saved" << GraphMail::Templates::save(&t) << t.value(QLatin1String("id")).toString();
            }
            shown = composer;
            if (parser.isSet(QStringLiteral("compose-save"))) {
                QTimer::singleShot(1500, composer, [composer] {
                    if (composer->windowTitle().contains(QLatin1String("[TEST Graph Mail]"))) {
                        qInfo().noquote() << "Saved as a draft:" << composer->saveNow();
                    }
                });
            }
            if (parser.isSet(QStringLiteral("compose-send"))) {
                // Tests only send what is clearly marked as a test
                const bool marked = composer->windowTitle().contains(QLatin1String("[TEST Graph Mail]"));
                const int undoAfter = parser.isSet(QStringLiteral("undo-send")) ? parser.value(QStringLiteral("undo-send")).toInt() : -1;
                QTimer::singleShot(1500, composer, [composer, marked, window, undoAfter] {
                    if (!marked) {
                        qWarning() << "Not sent: the subject has no [TEST Graph Mail] mark";
                    } else if (!composer->sendNow()) {
                        qWarning() << "Not sent: no valid recipients";
                    } else {
                        qInfo() << "Handed to the daemon for sending";
                        if (undoAfter >= 0) {
                            window->undoSendForTests(composer->lastRecord(), undoAfter);
                        }
                    }
                });
            }
        }
        if (parser.isSet(QStringLiteral("quick-step"))) {
            qWarning() << "quick step test: found" << window->runQuickStepForTests(parser.value(QStringLiteral("quick-step")));
        }
        if (parser.isSet(QStringLiteral("focus-move")) || parser.isSet(QStringLiteral("junk")) || parser.isSet(QStringLiteral("ignore-conversation"))) {
            window->reading9ForTests(parser.value(QStringLiteral("focus-move")), parser.value(QStringLiteral("junk")), parser.value(QStringLiteral("ignore-conversation")));
        }
        if (parser.isSet(QStringLiteral("flag-when"))) {
            window->flagSelected(parser.value(QStringLiteral("flag-when")));
            qWarning() << "flag test:" << parser.value(QStringLiteral("flag-when"));
        }
        if (parser.isSet(QStringLiteral("open-window"))) {
            if (QWidget *message = window->openSelectedMessage()) {
                message->resize(900, 700);
                shown = message;
            }
        }
        // A saved dialog is gone by then: the window behind it instead
        const QPointer<QWidget> target(shown);
        QWidget *fallback = calendar ? static_cast<QWidget *>(calendar) : window;
        // The directory search is a round trip to Graph: give it time
        const bool slow = parser.isSet(QStringLiteral("add-from-directory")) || parser.isSet(QStringLiteral("mailboxes")) || parser.isSet(QStringLiteral("add-colleague")) || parser.isSet(QStringLiteral("search")) || parser.isSet(QStringLiteral("search-folder"))
            || parser.isSet(QStringLiteral("search-filter")) || parser.isSet(QStringLiteral("category")) || parser.isSet(QStringLiteral("remove-colleague"))
            || parser.isSet(QStringLiteral("sharing")) || parser.isSet(QStringLiteral("share-calendar")) || parser.isSet(QStringLiteral("share-role"))
            || parser.isSet(QStringLiteral("share-remove")) || (parser.isSet(QStringLiteral("compose-to")) && !parser.isSet(QStringLiteral("compose-send")))
            || parser.isSet(QStringLiteral("undo-send")) || parser.isSet(QStringLiteral("photo"));
        QTimer::singleShot(slow ? 9000 : 2500, window, [window, target, fallback, file, pdf] {
            // The whole window: a module in the shell is grabbed with its navigation bar
            (target ? target.data() : fallback)->window()->grab().save(file);
            if (pdf.isEmpty() || !window->printBodyToPdf(pdf)) {
                QCoreApplication::quit();
            }
        });
        return app.exec();
    }
    shell->show();
    return app.exec();
}
