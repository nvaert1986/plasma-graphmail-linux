// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "accounts.h"
#include "calendarsharingdialog.h"
#include "calendaroptions.h"
#include "cleanup.h"
#include "messagedrag.h"
#include "calendarwindow.h"
#include "composer.h"
#include "contactswindow.h"
#include "taskswindow.h"
#include "eventdialog.h"
#include "icsdialog.h"
#include "recurrence.h"
#include "daemonclient.h"
#include "mailboxset.h"
#include "mailto.h"
#include "mailview.h"
#include "mailboxesdialog.h"
#include "mainwindow.h"
#include "updatesdialog.h"
#include "signaturedialog.h"
#include "shell.h"
#include "profilephotos.h"
#include "autorepliesdialog.h"
#include "rulesdialog.h"
#include "templates.h"
#include "mailboxsettingsdialog.h"
#include "waitingwindow.h"
#include "offlinedialog.h"
#include "readingpane.h"

#include <clocale>
#include <KAboutData>
#include <KActionCollection>
#include <KDBusService>
#include <KLocalizedString>
#include <KNotifyConfigWidget>

#include <QApplication>
#include <QDateEdit>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QStandardPaths>
#include <QSettings>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QDir>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QPointer>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QTimer>

#include <execinfo.h>
#include <memory>
#include <unistd.h>

namespace
{
// Debugging: GRAPHMAIL_TRACE_WARNINGS="<text>" writes a backtrace to stderr after every warning
// containing that text, to find the code behind a warning such as QTextCursor::setPosition's
QtMessageHandler previousHandler = nullptr;

void traceWarnings(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    previousHandler(type, context, message);
    static const QString pattern = qEnvironmentVariable("GRAPHMAIL_TRACE_WARNINGS");
    if (type == QtWarningMsg && message.contains(pattern)) {
        void *frames[64];
        const int count = backtrace(frames, 64);
        static const char header[] = "=== backtrace of the warning above ===\n";
        (void)!write(STDERR_FILENO, header, sizeof(header) - 1);
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
    }
}

// Tests: a window opened after the screenshot target was chosen (Forward as vCard)
QPointer<QWidget> &lateShown()
{
    static QPointer<QWidget> widget;
    return widget;
}

// The language chosen under Help > Switch Application Language (plasma-label-creator's fix). KXmlGui
// applies it once the application exists, but Plasma's platform theme already loads the frameworks'
// translations while the application is made: apply it before.
void applyChosenLanguage()
{
    const QSettings overrides(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/klanguageoverridesrc"), QSettings::IniFormat);
    const QByteArray chosen = overrides.value(QStringLiteral("Language/plasma-graphmail-linux")).toByteArray();
    if (chosen.isEmpty()) {
        return;
    }
    const QByteArray desktop = qgetenv("LANGUAGE");
    qputenv("LANGUAGE", desktop.isEmpty() ? chosen : chosen + ':' + desktop);

    // gettext ignores LANGUAGE when the locale for messages is C or POSIX (a desktop with LANG=C,
    // as the user's laptop): then messages take another installed locale, the chosen language's
    // own if there is one. Dates and numbers keep the desktop's formats.
    if (!qEnvironmentVariableIsEmpty("LC_ALL")) {
        return;
    }
    const char *current = std::setlocale(LC_MESSAGES, "");
    const QByteArray messages = current ? QByteArray(current) : QByteArray("C");
    if (messages != "C" && messages != "POSIX" && !messages.startsWith("C.")) {
        return;
    }
    const QByteArray language = chosen.split(':').constFirst();
    const QByteArray own = language.contains('_') ? language : language + '_' + language.toUpper();
    for (const QByteArray &candidate : QByteArrayList{own + ".UTF-8", "en_US.UTF-8", "en_GB.UTF-8"}) {
        if (std::setlocale(LC_MESSAGES, candidate.constData())) {
            qputenv("LC_MESSAGES", candidate);
            return;
        }
    }
}

// "<account>/<mailbox>/<message id>" from a notification's Open
void openMessage(GraphMail::MainWindow *window, const QString &spec)
{
    const QStringList parts = spec.split(QLatin1Char('/'));
    if (parts.size() == 3) {
        window->showMessage(parts.at(0) + QLatin1Char('/') + parts.at(1), parts.at(2).toLongLong());
    }
}

// Positional arguments: mailto: links (Graph Mail as the desktop's mail program), file:// URLs and
// paths (.msg, .eml, .pst from the file manager), paths relative to where that start was made
void openArguments(GraphMail::MainWindow *window, const QStringList &arguments, const QString &workingDirectory)
{
    QStringList files;
    for (const QString &argument : arguments) {
        if (GraphMail::Mailto::isMailto(argument)) {
            window->openMailto(argument);
        } else if (argument.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
            files.append(QUrl(argument).toLocalFile());
        } else {
            files.append(QDir(workingDirectory).absoluteFilePath(argument));
        }
    }
    window->openFiles(files);
}
}

int main(int argc, char **argv)
{
    // QtWebEngine: custom schemes and shared OpenGL contexts before the application exists
    GraphMail::MailView::registerSchemes();
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    if (!qEnvironmentVariableIsEmpty("GRAPHMAIL_TRACE_WARNINGS")) {
        previousHandler = qInstallMessageHandler(traceWarnings);
    }
    applyChosenLanguage();
    QApplication app(argc, argv);
    KLocalizedString::setApplicationDomain("plasma-graphmail-linux");
    // Started from the build folder, before installing: its translations are in build/locale,
    // next to build/bin (else the language dialog offers only US English)
    if (const QString built = QCoreApplication::applicationDirPath() + QStringLiteral("/../locale"); QFileInfo::exists(built)) {
        KLocalizedString::addDomainLocaleDir("plasma-graphmail-linux", built);
    }
    // Accounts are shared by the suite; the app registration per account is this app's own
    GraphCore::Accounts::setApplication(QStringLiteral("plasma-graphmail-linux"));

    KAboutData about(QStringLiteral("plasma-graphmail-linux"),
                     i18n("Graph Mail"),
                     QStringLiteral(GRAPHMAIL_VERSION),
                     i18n("Mail, calendar and contacts for Microsoft 365 on KDE Plasma"),
                     KAboutLicense::GPL_V3,
                     i18n("Not affiliated with or endorsed by Microsoft. Microsoft 365 and Exchange Online are trademarks of Microsoft Corporation."));
    about.setOrganizationDomain("nvaert1986.github.io");
    // Our own issue tracker: KDE's default bug address would send Report Bug to KDE and makes KDE's
    // Help menu offer "Donate" (to KDE)
    about.setBugAddress("https://github.com/nvaert1986/plasma-graphmail-linux/issues");
    about.setDesktopFileName(QStringLiteral("io.github.nvaert1986.graphmail"));
    KAboutData::setApplicationData(about);
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("io.github.nvaert1986.graphmail"), QIcon::fromTheme(QStringLiteral("mail-client"))));

    QCommandLineParser parser;
    about.setupCommandLine(&parser);
    parser.addPositionalArgument(QStringLiteral("files"),
                                 i18n("Outlook messages (.msg), mail files (.eml) to show, Outlook data files (.pst) to import, or mailto: links to write to"),
                                 QStringLiteral("[files or links...]"));
    parser.addOptions({
        {QStringLiteral("message"), i18n("Show this message (account/mailbox/id)"), QStringLiteral("spec")},
        {QStringLiteral("folder"), i18n("Open this folder (account/mailbox/folder id)"), QStringLiteral("path")},
        {QStringLiteral("screenshot"), i18n("Save a picture of the window to this file and quit (tests; never marks anything as read)"), QStringLiteral("file")},
        {QStringLiteral("size"), i18n("Window size for --screenshot (WIDTHxHEIGHT)"), QStringLiteral("size"), QStringLiteral("1300x800")},
        {QStringLiteral("select"), i18n("Select this message in the list for --screenshot (row number)"), QStringLiteral("row")},
        {QStringLiteral("message-details"), i18n("With --screenshot: show Message Details of the selected message once its headers are in (tests)")},
        {QStringLiteral("reply-with-meeting"), i18n("With --screenshot: show Reply with Meeting for the selected message (tests)")},
        {QStringLiteral("mail-to"), i18n("With --screenshot: the selected message as if dropped on calendar or tasks (tests)"), QStringLiteral("module")},
        {QStringLiteral("configure-notifications"), i18n("With --screenshot: show Configure Notifications (tests)")},
        {QStringLiteral("calendar-options"), i18n("With --screenshot: show Calendar Options (tests)")},
        {QStringLiteral("cleanup-dialog"), i18n("With --screenshot: show Clean Up Old Mail (tests)")},
        {QStringLiteral("forward-contact"), i18n("With --contacts, --directory-search and --screenshot: Forward as vCard for the first person found (tests)")},
        {QStringLiteral("undo-move"), i18n("With --screenshot: move this message to a folder, then Undo, account/mailbox/message|folder; logged (tests)"), QStringLiteral("spec")},
        {QStringLiteral("dark-palette"), i18n("Use a dark palette and log whether the shown message got the dark style (tests)")},
        {QStringLiteral("preview-attachment"), i18n("With --message and --screenshot: preview this attachment (row id) (tests)"), QStringLiteral("id")},
        {QStringLiteral("sweep"), i18n("With --screenshot: sweep without the dialog, account/mailbox/message|scope|folder (scope 0 all, 2 all but the newest; folder 0 deletes) (tests)"), QStringLiteral("spec")},
        {QStringLiteral("calendar-slot"), i18n("With --calendar and --screenshot: select this time, start|end (yyyy-MM-ddTHH:mm) (tests)"), QStringLiteral("times")},
        {QStringLiteral("calendar-pick"), i18n("With --calendar: drag in the date picker from one day number to another (\"10-13\"), logged (tests)"), QStringLiteral("days")},
        {QStringLiteral("calendar-mouse"), i18n("With --calendar and --screenshot: mouse steps on the week grid, logged (tests)"), QStringLiteral("steps")},
        {QStringLiteral("export-calendar"), i18n("With --calendar and --screenshot: export the default calendar into this .ics file, logged (tests)"), QStringLiteral("file")},
        {QStringLiteral("cleanup-test"), i18n("With --screenshot: give this folder its own cleanup rule (move to Archive, older than 1 day), run, Undo, remove the rule, account/mailbox/folder; logged (tests)"), QStringLiteral("spec")},
        {QStringLiteral("preview-close"), i18n("With --preview-attachment: close the preview after a moment (tests)")},
        {QStringLiteral("delete-test"), i18n("With --folder, --select and --screenshot: Delete the selected message, log what is selected next, Undo (tests)")},
        {QStringLiteral("new-task-dialog"), i18n("With --tasks and --screenshot: show New Task (\"show\"), or save one with this [TEST Graph Mail] title (tests)"), QStringLiteral("spec")},
        {QStringLiteral("drag-out"), i18n("With --screenshot and --select: the files a drag of the selected message gives, logged (tests)")},
        {QStringLiteral("body-pdf"), i18n("With --screenshot: also save the rendered message body as PDF (tests)"), QStringLiteral("file")},
        {QStringLiteral("save-attachments"), i18n("With --screenshot: save all attachments of the shown message into this folder first (tests)"), QStringLiteral("folder")},
        {QStringLiteral("open-window"), i18n("With --screenshot and --select: picture of the message in its own window (tests)")},
        {QStringLiteral("outgoing"), i18n("Open a message that was not sent (account/mailbox/id)"), QStringLiteral("spec")},
        {QStringLiteral("calendar"), i18n("Open the calendar")},
        {QStringLiteral("event"), i18n("Open this event (account/mailbox/id)"), QStringLiteral("spec")},
        {QStringLiteral("compose-link"), i18n("Write a new message with this synced OneDrive or SharePoint file as a link (repeatable; Graph Sync)"), QStringLiteral("path")},
        {QStringLiteral("compose-cloud"), i18n("With --compose: attach these synced files as OneDrive links, comma-separated; their state is logged (tests)"), QStringLiteral("paths")},
        {QStringLiteral("calendar-view"), i18n("With --calendar: day, workweek, week, month or agenda"), QStringLiteral("view")},
        {QStringLiteral("calendar-date"), i18n("With --calendar: the date to show (yyyy-MM-dd)"), QStringLiteral("date")},
        {QStringLiteral("new-event"), i18n("With --calendar and --screenshot: a new event with this title, saved (tests)"), QStringLiteral("title")},
        {QStringLiteral("event-start"), i18n("With --new-event: start (yyyy-MM-ddTHH:mm)"), QStringLiteral("time")},
        {QStringLiteral("event-end"), i18n("With --new-event: end (yyyy-MM-ddTHH:mm)"), QStringLiteral("time")},
        {QStringLiteral("event-attendees"), i18n("With --new-event: attendees (tests)"), QStringLiteral("addresses")},
        {QStringLiteral("event-teams"), i18n("With --new-event: a Teams meeting (tests)")},
        {QStringLiteral("event-weekly"), i18n("With --new-event: repeat weekly, this many times (tests)"), QStringLiteral("count")},
        {QStringLiteral("event-recurrence"), i18n("With --new-event: this repeat rule, Graph JSON (tests)"), QStringLiteral("json")},
        {QStringLiteral("event-zones"), i18n("With --new-event: the start and end time zones, start|end IANA ids; the times are in those zones (tests)"), QStringLiteral("zones")},
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
        {QStringLiteral("signatures"), i18n("With --screenshot: open the signatures window (never saved) (tests)")},
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
        {QStringLiteral("updates"), i18n("With --screenshot: open Settings > Updates; \"check\" also presses Check Now; the status is logged (tests)"), QStringLiteral("action")},
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
        {QStringLiteral("compose-drop"), i18n("With --compose: drop these files (comma separated) on the text and log the result (tests)"), QStringLiteral("files")},
        {QStringLiteral("compose-html"), i18n("With --compose: insert the HTML of this file as a paste and log its pictures (tests)"), QStringLiteral("file")},
        {QStringLiteral("compose-keys"), i18n("With --compose: play these keys on the text and log every undo (tests)"), QStringLiteral("keys")},
        {QStringLiteral("compose-type"), i18n("With --compose: type this into the text and log the links in it (tests)"), QStringLiteral("text")},
        {QStringLiteral("compose-narrow"), i18n("With --compose: a narrow composer window (tests)")},
        {QStringLiteral("compose-check-names"), i18n("With --compose: run Check Names on the recipients, logged (tests)")},
        {QStringLiteral("compose-from-other"), i18n("With --compose: From this other address, address|name (tests)"), QStringLiteral("from")},
        {QStringLiteral("compose-subject"), i18n("With --compose: the subject (tests)"), QStringLiteral("text")},
        {QStringLiteral("compose-text"), i18n("With --compose: text to type (tests)"), QStringLiteral("text")},
        {QStringLiteral("compose-attach"), i18n("With --compose: a file to attach; may be repeated (tests)"), QStringLiteral("file")},
        {QStringLiteral("compose-send"), i18n("With --compose: send it, then quit (tests)")},
        {QStringLiteral("compose-press-send"), i18n("With --compose: press Send as the user does (OneDrive links shared first); only a [TEST Graph Mail] subject (tests)")},
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
        {QStringLiteral("export-file"), i18n("With --screenshot: save the selected message (--select) or the whole folder (--export-folder) into this file (tests)"), QStringLiteral("file")},
        {QStringLiteral("export-folder"), i18n("With --export-file: the folder opened with --folder, as mbox (tests)")},
        {QStringLiteral("export-pst"), i18n("With --screenshot and --folder: export that folder with its subfolders (or with --export-pst-mailbox the whole mailbox) to this Outlook data file (tests)"), QStringLiteral("file")},
        {QStringLiteral("export-pst-mailbox"), i18n("With --export-pst: the whole mailbox of --folder (tests)")},
        {QStringLiteral("export-pst-items"), i18n("With --export-pst: only the contacts, calendars and tasks of the mailbox of --folder (tests)")},
        {QStringLiteral("import-pst"), i18n("With --screenshot and --folder: import this Outlook data file into the mailbox of --folder, in a new folder named after the file (tests)"), QStringLiteral("file")},
        {QStringLiteral("import-pst-merge"), i18n("With --import-pst: into the mailbox's own folders (tests)")},
        {QStringLiteral("archive-folder"), i18n("With --screenshot and --folder: make this folder id the mailbox's archive folder (tests)"), QStringLiteral("id")},
        {QStringLiteral("show-file"), i18n("With --screenshot: show this .msg or .eml in the message viewer, or this .pst in the import dialog, and take the picture of that (tests)"), QStringLiteral("file")},
        {QStringLiteral("import-messages"), i18n("With --screenshot and --folder: import these .msg or .eml files (comma separated) into that folder (tests)"), QStringLiteral("files")},
        {QStringLiteral("print-pdf"), i18n("With --screenshot: print the message shown, or the calendar, contacts or tasks opened, into this PDF (tests)"), QStringLiteral("file")},
        {QStringLiteral("flag-when"), i18n("With --select: flag the message today, tomorrow, thisweek, nextweek, none, complete or clear (tests)"), QStringLiteral("when")},
        {QStringLiteral("compose-save"), i18n("With --compose: save it as a draft, then quit (tests)")},
        {QStringLiteral("load-images"), i18n("With --screenshot: load internet content as if \"Load Images\" was clicked (tests)")},
        {QStringLiteral("click-link"), i18n("With --screenshot: click the shown message's first link with this text; links are only logged (tests)"), QStringLiteral("text")},
    });
    parser.process(app);
    if (parser.isSet(QStringLiteral("dark-palette"))) {
        // Tests: a dark desktop theme without changing the user's
        QPalette dark(QColor(0x31, 0x36, 0x3b), QColor(0x23, 0x26, 0x29));
        dark.setColor(QPalette::Base, QColor(0x1b, 0x1e, 0x20));
        dark.setColor(QPalette::Text, QColor(0xfc, 0xfc, 0xfc));
        dark.setColor(QPalette::WindowText, QColor(0xfc, 0xfc, 0xfc));
        QApplication::setPalette(dark);
    }
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
    if (parser.isSet(QStringLiteral("compose-link")) && !parser.isSet(QStringLiteral("screenshot"))) {
        window->composeWithCloudLinks(parser.values(QStringLiteral("compose-link")));
    }
    if (unique) {
        QObject::connect(unique.get(), &KDBusService::activateRequested, window, [window, shell, openOutgoing, openEvent, openTask](const QStringList &arguments, const QString &workingDirectory) {
            QCommandLineParser again;
            again.addOption({QStringLiteral("message"), QString(), QStringLiteral("spec")});
            again.addOption({QStringLiteral("outgoing"), QString(), QStringLiteral("spec")});
            again.addOption({QStringLiteral("event"), QString(), QStringLiteral("spec")});
            again.addOption({QStringLiteral("calendar"), QString()});
            again.addOption({QStringLiteral("task"), QString(), QStringLiteral("spec")});
            again.addOption({QStringLiteral("contacts"), QString()});
            again.addOption({QStringLiteral("tasks"), QString()});
            again.addOption({QStringLiteral("compose-link"), QString(), QStringLiteral("path")});
            again.parse(arguments);
            if (again.isSet(QStringLiteral("compose-link"))) {
                window->composeWithCloudLinks(again.values(QStringLiteral("compose-link")));
            }
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
            openArguments(window, again.positionalArguments(), workingDirectory);
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
        if (parser.isSet(QStringLiteral("click-link"))) {
            GraphMail::MailView::setLinkTestMode(true);
            window->readingPane()->clickLinkForTests(parser.value(QStringLiteral("click-link")));
            QElapsedTimer waited;
            waited.start();
            while (waited.elapsed() < 8000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            }
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
        if (parser.isSet(QStringLiteral("signatures"))) {
            // Shown, never accepted: the screenshot only
            auto *dialog = new GraphMail::SignatureDialog(window->mailboxes(), window);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->show();
            shown = dialog;
        }
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
            if (parser.isSet(QStringLiteral("forward-contact"))) {
                // The directory answers later: forward once the first person is selected
                QTimer::singleShot(4000, contacts, [contacts] {
                    // No autosave into the mailbox, no close question at the end
                    if (auto *composer = qobject_cast<GraphMail::ComposerWindow *>(contacts->forwardAsVCard())) {
                        composer->setTestMode();
                        lateShown() = composer;
                    }
                });
            }
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
        if (parser.isSet(QStringLiteral("updates"))) {
            GraphMail::UpdatesDialog *dialog = window->openUpdates();
            shown = dialog;
            if (parser.value(QStringLiteral("updates")) == QLatin1String("check")) {
                dialog->checkNowForTests();
                QElapsedTimer waited;
                waited.start();
                while (waited.elapsed() < 5000) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                }
            }
            qWarning().noquote() << "updates test:" << dialog->statusForTests().replace(QLatin1Char('\n'), QLatin1String(" | "));
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
                    const auto loaded = std::make_shared<bool>(false);
                    QObject::connect(dialog, &GraphMail::AutoRepliesDialog::loadedForTests, dialog, [loaded] {
                        *loaded = true;
                    }, Qt::SingleShotConnection);
                    QElapsedTimer waited;
                    waited.start();
                    while (!*loaded && waited.elapsed() < 10000) {
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                    }
                    qWarning() << "auto replies test: loaded" << *loaded;
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
            const auto loaded = std::make_shared<bool>(false);
            QObject::connect(dialog, &GraphMail::MailboxSettingsDialog::loadedForTests, dialog, [loaded] {
                *loaded = true;
            }, Qt::SingleShotConnection);
            QElapsedTimer waited;
            waited.start();
            while (!*loaded && waited.elapsed() < 15000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            }
            const QString spec = parser.value(QStringLiteral("mailbox-settings"));
            if (*loaded && spec.startsWith(QLatin1String("hours|"))) {
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
        if (parser.isSet(QStringLiteral("configure-notifications"))) {
            if (QWidget *dialog = KNotifyConfigWidget::configure(window)) {
                shown = dialog->window();
            }
        }
        if (parser.isSet(QStringLiteral("drag-out"))) {
            // As a file manager asks: the text/uri-list bytes of the drag
            GraphMail::MessageDragData data(window->mailboxKey(), {window->readingPane()->message()});
            const QByteArray list = data.data(QStringLiteral("text/uri-list"));
            for (const QByteArray &line : list.split('\n')) {
                const QString path = QUrl(QString::fromUtf8(line.trimmed())).toLocalFile();
                if (!path.isEmpty()) {
                    QFile file(path);
                    const QByteArray head = file.open(QIODevice::ReadOnly) ? file.read(4096) : QByteArray();
                    qWarning("drag-out: %s, %lld bytes, original %d", qPrintable(QFileInfo(path).fileName()), qlonglong(file.size()), int(!head.contains("X-GraphMail-Rebuilt")));
                }
            }
            if (list.isEmpty()) {
                qWarning("drag-out: no files");
            }
        }
        if (calendar && parser.isSet(QStringLiteral("calendar-slot"))) {
            const QString slot = parser.value(QStringLiteral("calendar-slot"));
            calendar->selectSlotForTests(QDateTime::fromString(slot.section(QLatin1Char('|'), 0, 0), Qt::ISODate),
                                         QDateTime::fromString(slot.section(QLatin1Char('|'), 1, 1), Qt::ISODate));
        }
        if (calendar && parser.isSet(QStringLiteral("calendar-pick"))) {
            const QString days = parser.value(QStringLiteral("calendar-pick"));
            QTimer::singleShot(500, calendar, [calendar, days] {
                qWarning().noquote() << "pick test:" << calendar->pickerDragForTests(days.section(QLatin1Char('-'), 0, 0).toInt(), days.section(QLatin1Char('-'), 1, 1).toInt());
            });
        }
        if (calendar && parser.isSet(QStringLiteral("calendar-mouse"))) {
            calendar->mouseForTests(parser.value(QStringLiteral("calendar-mouse")));
            // The dialog it opened is the picture
            const QWidgetList all = QApplication::topLevelWidgets();
            for (QWidget *w : all) {
                if (auto *dialog = qobject_cast<GraphMail::EventDialog *>(w); dialog && dialog->isVisible()) {
                    lateShown() = dialog;
                }
            }
        }
        if (calendar && parser.isSet(QStringLiteral("export-calendar"))) {
            const QString file = calendar->exportCalendar(parser.value(QStringLiteral("export-calendar")));
            QFile written(file);
            const QByteArray ics = written.open(QIODevice::ReadOnly) ? written.readAll() : QByteArray();
            qWarning() << "export test:" << !file.isEmpty() << ics.count("BEGIN:VEVENT") << "events";
        }
        if (parser.isSet(QStringLiteral("cleanup-test"))) {
            const QString spec = parser.value(QStringLiteral("cleanup-test"));
            const QString key = spec.section(QLatin1Char('/'), 0, 1);
            const qint64 folder = spec.section(QLatin1Char('/'), 2).section(QLatin1Char('|'), 0, 0).toLongLong();
            // "|pst <folder>": export to an Outlook data file in that folder instead of moving
            const QString pstFolder = spec.section(QLatin1Char('|'), 1).section(QLatin1Char(' '), 1);
            const GraphMail::Cleanup::Settings saved = GraphMail::Cleanup::settings();
            if (!pstFolder.isEmpty()) {
                GraphMail::Cleanup::Settings changed = saved;
                changed.pstFolder = pstFolder;
                GraphMail::Cleanup::setSettings(changed);
            }
            GraphMail::Store *store = window->mailboxes()->store(key);
            const std::optional<GraphMail::FolderRow> row = store ? store->folder(folder) : std::nullopt;
            if (row) {
                const auto count = [store, folder] {
                    return store->messagesBefore(folder, QDateTime::currentMSecsSinceEpoch()).size();
                };
                const auto wait = [](int ms) {
                    QElapsedTimer waited;
                    waited.start();
                    while (waited.elapsed() < ms) {
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                    }
                };
                GraphMail::Cleanup::setFolderRule(key, row->graphId,
                                                  {GraphMail::Cleanup::FolderRule::Mode::Own,
                                                   pstFolder.isEmpty() ? GraphMail::Cleanup::Action::Archive : GraphMail::Cleanup::Action::ExportPst, 1});
                qWarning() << "cleanup test: before" << count() << "planned" << GraphMail::Cleanup::plan(window->mailboxes(), QDateTime::currentMSecsSinceEpoch()).size();
                window->runCleanupForTests();
                wait(pstFolder.isEmpty() ? 8000 : 30000);
                qWarning() << "cleanup test: after the run" << count();
                window->undoForTests();
                wait(8000);
                qWarning() << "cleanup test: after Undo" << count();
                GraphMail::Cleanup::setFolderRule(key, row->graphId, {});
            }
            GraphMail::Cleanup::setSettings(saved);
        }
        if (parser.isSet(QStringLiteral("delete-test"))) {
            const auto wait = [](int ms) {
                QElapsedTimer waited;
                waited.start();
                while (waited.elapsed() < ms) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                }
            };
            wait(2000);
            const qint64 before = window->readingPane()->message();
            if (QAction *remove = window->actionCollection()->action(QStringLiteral("delete"))) {
                remove->trigger();
            }
            wait(6000);
            qWarning() << "delete test: deleted" << before << "now selected" << window->readingPane()->message();
            window->undoForTests();
            wait(6000);
        }
        if (parser.isSet(QStringLiteral("sweep"))) {
            // Never scope 1 here: that makes a rule, a standing change in the mailbox
            const QString spec = parser.value(QStringLiteral("sweep"));
            const QString key = spec.section(QLatin1Char('/'), 0, 1);
            const qint64 message = spec.section(QLatin1Char('/'), 2).section(QLatin1Char('|'), 0, 0).toLongLong();
            const int scope = spec.section(QLatin1Char('|'), 1, 1).toInt();
            if (scope != 1) {
                window->sweepMessages(key, message, scope, spec.section(QLatin1Char('|'), 2, 2).toLongLong());
                QElapsedTimer waited;
                waited.start();
                while (waited.elapsed() < 8000) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                }
                qWarning() << "sweep test: done";
            }
        }
        if (parser.isSet(QStringLiteral("undo-move"))) {
            const QString spec = parser.value(QStringLiteral("undo-move"));
            const QString key = spec.section(QLatin1Char('/'), 0, 1);
            const qint64 message = spec.section(QLatin1Char('/'), 2).section(QLatin1Char('|'), 0, 0).toLongLong();
            const qint64 folder = spec.section(QLatin1Char('|'), 1).toLongLong();
            const auto folderNow = [window, key, message] {
                const GraphMail::Store *store = window->mailboxes()->store(key);
                const std::optional<GraphMail::MessageRow> row = store ? store->message(message) : std::nullopt;
                return row ? row->folder : 0;
            };
            const auto wait = [](int ms) {
                QElapsedTimer waited;
                waited.start();
                while (waited.elapsed() < ms) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                }
            };
            qWarning() << "undo test: before" << folderNow();
            window->rememberForUndo(key, {message}, false);
            window->daemon()->move(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), message, folder);
            wait(8000);
            qWarning() << "undo test: moved" << folderNow();
            if (QAction *undo = window->actionCollection()->action(QStringLiteral("edit_undo"))) {
                undo->trigger();
            }
            wait(8000);
            qWarning() << "undo test: undone" << folderNow();
        }
        if (parser.isSet(QStringLiteral("dark-palette"))) {
            QTimer::singleShot(1500, window, [window] {
                window->readingPane()->bodyHtmlForTests([](const QString &html) {
                    qWarning() << "dark test: styled" << html.contains(QLatin1String("hue-rotate(180deg)"));
                });
            });
        }
        if (parser.isSet(QStringLiteral("preview-attachment"))) {
            const qint64 attachment = parser.value(QStringLiteral("preview-attachment")).toLongLong();
            QTimer::singleShot(2000, window, [window, attachment] {
                window->readingPane()->previewAttachment(attachment);
            });
            if (parser.isSet(QStringLiteral("preview-close"))) {
                // Closed while LibreOffice still converts (crashed before 2026-11-16)
                QTimer::singleShot(4000, window, [window] {
                    if (QWidget *preview = window->readingPane()->lastPreview()) {
                        preview->close();
                        qWarning() << "preview test: closed early";
                    }
                });
            }
            // Okular or LibreOffice need a moment: the picture is taken late
            QTimer::singleShot(8000, window, [window] {
                lateShown() = window->readingPane()->lastPreview();
            });
        }
        if (parser.isSet(QStringLiteral("cleanup-dialog"))) {
            auto *dialog = new GraphMail::Cleanup::Dialog(window);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->show();
            shown = dialog;
        }
        if (parser.isSet(QStringLiteral("calendar-options"))) {
            auto *dialog = new GraphMail::CalendarOptions::Dialog(window);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->show();
            shown = dialog;
        }
        if (parser.isSet(QStringLiteral("mail-to"))) {
            if (parser.value(QStringLiteral("mail-to")) == QLatin1String("calendar")) {
                if (QWidget *dialog = window->newAppointmentFromMail(window->mailboxKey(), window->readingPane()->message())) {
                    shown = dialog;
                }
            } else {
                window->newTaskFromMail(window->mailboxKey(), window->readingPane()->message());
                QElapsedTimer waited;
                waited.start();
                while (waited.elapsed() < 8000) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                }
            }
        }
        if (parser.isSet(QStringLiteral("reply-with-meeting"))) {
            if (QWidget *dialog = window->replyWithMeeting(window->mailboxKey(), window->readingPane()->message())) {
                shown = dialog;
            }
        }
        if (parser.isSet(QStringLiteral("message-details"))) {
            if (QWidget *dialog = window->openMessageDetails()) {
                shown = dialog;
                auto *text = dialog->findChild<QPlainTextEdit *>();
                QElapsedTimer waited;
                waited.start();
                while (text && text->toPlainText().size() < 20 && waited.elapsed() < 20000) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                }
                qWarning() << "details test:" << (text ? text->toPlainText().count(QLatin1Char('\n')) : -1) << "header lines";
            }
        }
        if (parser.isSet(QStringLiteral("rules"))) {
            GraphMail::RulesDialog *dialog = window->openRules(parser.value(QStringLiteral("settings-mailbox")));
            shown = dialog;
            // Graph's answers come through the daemon: wait for the list, run the action, wait again
            auto waitLoaded = [dialog] {
                const auto loaded = std::make_shared<bool>(false);
                QObject::connect(dialog, &GraphMail::RulesDialog::loadedForTests, dialog, [loaded] {
                    *loaded = true;
                }, Qt::SingleShotConnection);
                QElapsedTimer waited;
                waited.start();
                while (!*loaded && waited.elapsed() < 15000) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                }
                return *loaded;
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
        if (tasks && parser.isSet(QStringLiteral("new-task-dialog"))) {
            // "show", or a [TEST Graph Mail] title to save with a due date (tomorrow) and Important
            const QString spec = parser.value(QStringLiteral("new-task-dialog"));
            if (QDialog *dialog = tasks->newTaskDialog()) {
                lateShown() = dialog;
                if (spec.contains(QLatin1String("[TEST Graph Mail]"))) {
                    // Into the administrator test account's list, never the user's own
                    if (auto *list = dialog->findChild<QComboBox *>()) {
                        for (int i = 0; i < list->count(); ++i) {
                            if (list->itemText(i).contains(QLatin1String("administrator@"))) {
                                list->setCurrentIndex(i);
                            }
                        }
                        if (!list->currentText().contains(QLatin1String("administrator@"))) {
                            qWarning() << "task dialog test: no administrator list";
                            dialog->reject();
                        }
                    }
                    dialog->findChild<QLineEdit *>()->setText(spec);
                    const QList<QCheckBox *> boxes = dialog->findChildren<QCheckBox *>();
                    for (QCheckBox *box : boxes) {
                        box->setChecked(box->text().contains(QLatin1String("Due")) || box->text().contains(QLatin1String("Important")));
                    }
                    if (auto *due = dialog->findChild<QDateEdit *>(); due && !qobject_cast<QDateTimeEdit *>(due->parent())) {
                        due->setDate(QDate::currentDate().addDays(1));
                    }
                    QTimer::singleShot(1500, dialog, &QDialog::accept);
                    qWarning() << "task dialog test: saving" << spec;
                }
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
                if (parser.isSet(QStringLiteral("event-zones"))) {
                    const QString zones = parser.value(QStringLiteral("event-zones"));
                    dialog->setTestZones(zones.section(QLatin1Char('|'), 0, 0).toUtf8(), zones.section(QLatin1Char('|'), 1, 1).toUtf8());
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
        // Tests: a mailto: link given with --screenshot shows the composer it opens
        for (const QString &argument : parser.positionalArguments()) {
            if (GraphMail::Mailto::isMailto(argument)) {
                if (auto *composer = qobject_cast<GraphMail::ComposerWindow *>(window->openMailto(argument))) {
                    composer->setTestMode();
                    composer->resize(parser.isSet(QStringLiteral("compose-narrow")) ? QSize(620, 500) : QSize(900, 760));
                    shown = composer;
                }
            }
        }
        if (parser.isSet(QStringLiteral("compose"))) {
            const QString kind = parser.value(QStringLiteral("compose"));
            const qint64 message = kind == QLatin1String("new") ? 0 : window->readingPane()->message();
            auto *composer = static_cast<GraphMail::ComposerWindow *>(window->openComposer(kind, message));
            composer->setTestMode();
            composer->resize(parser.isSet(QStringLiteral("compose-narrow")) ? QSize(620, 500) : QSize(900, 760));
            if (parser.isSet(QStringLiteral("compose-cloud"))) {
                composer->attachCloudFiles(parser.value(QStringLiteral("compose-cloud")).split(QLatin1Char(','), Qt::SkipEmptyParts));
            }
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
            if (parser.isSet(QStringLiteral("compose-type"))) {
                QTimer::singleShot(1500, composer, [composer, text = parser.value(QStringLiteral("compose-type"))] {
                    qWarning() << "type test:" << composer->typeBodyForTests(text);
                });
            }
            if (parser.isSet(QStringLiteral("compose-html"))) {
                QFile html(parser.value(QStringLiteral("compose-html")));
                if (html.open(QIODevice::ReadOnly)) {
                    composer->insertHtmlForTests(QString::fromUtf8(html.readAll()));
                }
                QTimer::singleShot(7000, composer, [composer] {
                    qWarning().noquote() << "html test:" << composer->imagesForTests().join(QStringLiteral(" ;; "));
                });
            }
            if (parser.isSet(QStringLiteral("compose-keys"))) {
                QTimer::singleShot(1500, composer, [composer, keys = parser.value(QStringLiteral("compose-keys"))] {
                    qWarning().noquote() << "keys test:" << composer->keysForTests(keys).join(QStringLiteral(" ;; "));
                });
            }
            if (parser.isSet(QStringLiteral("compose-drop"))) {
                QTimer::singleShot(1500, composer, [composer, files = parser.value(QStringLiteral("compose-drop"))] {
                    qWarning() << "drop test:" << composer->dropFilesForTests(files.split(QLatin1Char(',')));
                });
            }
            if (parser.isSet(QStringLiteral("compose-cloud"))) {
                // Links come from Graph Sync (an upload may take a while): wait, then log
                QElapsedTimer waited;
                waited.start();
                while (waited.elapsed() < 60000) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                    const QStringList states = composer->cloudStateForTests();
                    // --compose-drop hands its files over after 1.5 s: judge only after that
                    if (waited.elapsed() > 3000 && !states.isEmpty() && std::none_of(states.cbegin(), states.cend(), [](const QString &s) {
                            return s.endsWith(QLatin1String(" uploading"));
                        })) {
                        break;
                    }
                }
                qWarning().noquote() << "cloud test: state" << composer->cloudStateForTests().join(QStringLiteral("; "));
            }
            if (parser.isSet(QStringLiteral("compose-check-names"))) {
                composer->typeRecipientsForTests(parser.value(QStringLiteral("compose-to")));
                QTimer::singleShot(1500, composer, [composer] {
                    composer->checkNames([composer](bool resolved) {
                        qWarning() << "check names test:" << resolved << composer->recipientsForTests();
                    });
                });
            }
            if (parser.isSet(QStringLiteral("compose-save"))) {
                QTimer::singleShot(1500, composer, [composer] {
                    if (composer->windowTitle().contains(QLatin1String("[TEST Graph Mail]"))) {
                        qInfo().noquote() << "Saved as a draft:" << composer->saveNow();
                    }
                });
            }
            if (parser.isSet(QStringLiteral("compose-press-send"))) {
                // The whole Send path (uploads, giving the recipients access, then sending)
                QTimer::singleShot(2000, composer, [composer] {
                    if (!composer->windowTitle().contains(QLatin1String("[TEST Graph Mail]"))) {
                        qWarning() << "Not sent: the subject has no [TEST Graph Mail] mark";
                    } else if (QAction *send = composer->actionCollection()->action(QStringLiteral("send"))) {
                        qWarning() << "press send test: pressed";
                        send->trigger();
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
        if (parser.isSet(QStringLiteral("export-file"))) {
            const auto finished = std::make_shared<int>(-1);
            auto done = [finished](bool ok) {
                *finished = ok ? 1 : 0;
            };
            const QString file = parser.value(QStringLiteral("export-file"));
            if (parser.isSet(QStringLiteral("export-folder"))) {
                const QString path = parser.value(QStringLiteral("folder"));
                window->exportFolder(path.section(QLatin1Char('/'), 0, 1), path.section(QLatin1Char('/'), 2).toLongLong(), file, done);
            } else {
                window->exportMessages(window->selectedMessageIds(), file, done);
            }
            // Each message is asked of the daemon first: wait (at most 60 s)
            QElapsedTimer waited;
            waited.start();
            while (*finished < 0 && waited.elapsed() < 60000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            }
            qWarning() << "export test: done" << *finished;
        }
        if (parser.isSet(QStringLiteral("export-pst")) || parser.isSet(QStringLiteral("import-pst")) || parser.isSet(QStringLiteral("import-messages"))
            || parser.isSet(QStringLiteral("archive-folder"))) {
            const auto finished = std::make_shared<int>(-1);
            auto done = [finished](bool ok) {
                *finished = ok ? 1 : 0;
            };
            const QString path = parser.value(QStringLiteral("folder"));
            const QString key = path.section(QLatin1Char('/'), 0, 1);
            if (parser.isSet(QStringLiteral("export-pst"))) {
                const bool items = parser.isSet(QStringLiteral("export-pst-items"));
                const QList<qint64> folders = parser.isSet(QStringLiteral("export-pst-mailbox")) || items ? QList<qint64>() : QList<qint64>{path.section(QLatin1Char('/'), 2).toLongLong()};
                window->exportPst(key, folders, parser.value(QStringLiteral("export-pst")), done, items);
            } else if (parser.isSet(QStringLiteral("archive-folder"))) {
                window->chooseArchiveFolder(key, parser.value(QStringLiteral("archive-folder")).toLongLong(), done);
            } else if (parser.isSet(QStringLiteral("import-messages"))) {
                window->importMessageFiles(key, path.section(QLatin1Char('/'), 2).toLongLong(), parser.value(QStringLiteral("import-messages")).split(QLatin1Char(',')), done);
            } else {
                window->importPst(key, parser.value(QStringLiteral("import-pst")), parser.isSet(QStringLiteral("import-pst-merge")), false, done);
            }
            // Messages go through the daemon one at a time: wait (at most 10 minutes)
            QElapsedTimer waited;
            waited.start();
            while (*finished < 0 && waited.elapsed() < 600000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            }
            qWarning() << "pst test: done" << *finished;
        }
        if (parser.isSet(QStringLiteral("print-pdf"))) {
            const QString file = parser.value(QStringLiteral("print-pdf"));
            if (calendar) {
                calendar->printView(file);
                qWarning() << "print test: calendar" << QFileInfo::exists(file);
            } else if (contacts) {
                contacts->printList(file);
                qWarning() << "print test: contacts" << QFileInfo::exists(file);
            } else if (tasks) {
                tasks->printList(file);
                qWarning() << "print test: tasks" << QFileInfo::exists(file);
            } else {
                // The message's page loads first: wait for it (at most 20 s)
                const auto finished = std::make_shared<int>(-1);
                window->printMessage(file, [finished](bool ok) {
                    *finished = ok ? 1 : 0;
                });
                QElapsedTimer waited;
                waited.start();
                while (*finished < 0 && waited.elapsed() < 20000) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                }
                qWarning() << "print test: message" << *finished;
            }
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
        if (parser.isSet(QStringLiteral("show-file"))) {
            if (QWidget *w = window->showFileForTests(parser.value(QStringLiteral("show-file")))) {
                shown = w;
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
            || parser.isSet(QStringLiteral("undo-send")) || parser.isSet(QStringLiteral("photo")) || parser.isSet(QStringLiteral("forward-contact"))
            || parser.isSet(QStringLiteral("preview-attachment")) || parser.isSet(QStringLiteral("compose-html"));
        QTimer::singleShot(slow ? 9000 : 2500, window, [window, target, fallback, file, pdf] {
            // The whole window: a module in the shell is grabbed with its navigation bar
            QWidget *picture = lateShown() ? lateShown().data() : (target ? target.data() : fallback);
            picture->window()->grab().save(file);
            if (pdf.isEmpty() || !window->printBodyToPdf(pdf)) {
                QCoreApplication::quit();
            }
        });
        return app.exec();
    }
    shell->show();
    if (const QStringList arguments = parser.positionalArguments(); !arguments.isEmpty()) {
        // After the window is up, as when the file manager hands them to a running copy
        QTimer::singleShot(0, window, [window, arguments] {
            openArguments(window, arguments, QDir::currentPath());
        });
    }
    return app.exec();
}
