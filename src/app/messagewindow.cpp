// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "messagewindow.h"
#include "mainwindow.h"
#include "readreceipts.h"
#include "printing.h"
#include "composer.h"
#include "daemonclient.h"
#include "foldermodel.h"
#include "mailboxset.h"
#include "mailview.h"
#include "messagedetails.h"
#include "mimeexport.h"
#include "movedialog.h"
#include "readingpane.h"
#include "store.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KMessageBox>
#include <KStandardAction>
#include <KToggleAction>

#include <QAction>
#include <QIcon>

namespace GraphMail
{

MessageWindow::MessageWindow(MailboxSet *mailboxes,
                             DaemonClient *daemon,
                             FolderModel *folders,
                             const QString &mailboxKey,
                             qint64 message,
                             bool markRead,
                             QWidget *parent)
    : KXmlGuiWindow(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
    , m_folders(folders)
    , m_mailboxKey(mailboxKey)
    , m_message(message)
    , m_reading(new ReadingPane(this))
{
    // Its own window, not a child dialog of the main window
    setWindowFlag(Qt::Window);
    setAttribute(Qt::WA_DeleteOnClose);
    MailView::prepareWindow(this);
    m_reading->setContext(m_daemon, m_mailboxes);
    setCentralWidget(m_reading);

    connect(m_reading, &ReadingPane::bodyNeeded, this, [this](qint64 id) {
        m_daemon->fetchBody(account(), mailbox(), id);
    });
    connect(m_reading, &ReadingPane::mailtoRequested, this, &MessageWindow::mailtoRequested);
    // Print... in the message's right-click menu: the window's own Print
    connect(m_reading, &ReadingPane::printRequested, this, [this] {
        if (QAction *print = actionCollection()->action(KStandardAction::name(KStandardAction::Print))) {
            print->trigger();
        }
    });
    connect(m_reading, &ReadingPane::attachmentNeeded, this, [this](qint64 attachment) {
        m_daemon->fetchAttachment(account(), mailbox(), attachment);
    });
    connect(m_daemon, &DaemonClient::bodyReady, this, [this](const QString &acc, const QString &mbx, qint64 id) {
        if (acc + QLatin1Char('/') + mbx == m_mailboxKey && id == m_message) {
            m_reading->reload();
        }
    });
    connect(m_daemon, &DaemonClient::attachmentReady, this, [this](const QString &acc, const QString &mbx, qint64 id, qint64 attachment, bool ok) {
        if (acc + QLatin1Char('/') + mbx == m_mailboxKey) {
            m_reading->attachmentReady(id, attachment, ok);
        }
    });
    connect(m_daemon, &DaemonClient::collectionChanged, this, [this](const QString &acc, const QString &mbx, const QString &, int) {
        if (acc + QLatin1Char('/') + mbx == m_mailboxKey) {
            refresh();
        }
    });

    setupActions();
    setupGUI(QSize(900, 800), ToolBar | Keys | Save | Create, QStringLiteral("graphmailmessageui.rc"));
    // Separate from the main window's size and toolbar settings
    setAutoSaveSettings(QStringLiteral("MessageWindow"));

    m_reading->showMessage(m_mailboxes->store(m_mailboxKey), m_mailboxes->blobs(m_mailboxKey), m_message);
    refresh();
    // Opening a message is reading it (Outlook does the same)
    const Store *store = m_mailboxes->store(m_mailboxKey);
    if (const std::optional<MessageRow> row = store ? store->message(m_message) : std::nullopt; markRead && row && !row->isRead) {
        // A read receipt the user is asked about waits for the reading pane's answer
        const ReadReceipts::Decision decision = ReadReceipts::decide(ReadReceipts::mode(), store->readReceiptRequested(m_message), false);
        if (decision == ReadReceipts::Decision::Wait) {
            m_reading->markReadWhenKnown();
        } else if (decision != ReadReceipts::Decision::Ask) {
            m_daemon->markRead(account(), mailbox(), m_message, true, decision == ReadReceipts::Decision::Send);
        }
    }
}

MessageWindow::~MessageWindow() = default;

qint64 MessageWindow::message() const
{
    return m_message;
}

QString MessageWindow::mailboxKey() const
{
    return m_mailboxKey;
}

QString MessageWindow::account() const
{
    return m_mailboxKey.section(QLatin1Char('/'), 0, 0);
}

QString MessageWindow::mailbox() const
{
    return m_mailboxKey.section(QLatin1Char('/'), 1);
}

void MessageWindow::setupActions()
{
    // View > Zoom and the dark message background (docs/PHASE9B.md 9b.25)
    KStandardAction::zoomIn(this, [this] {
        m_reading->zoom(1);
    }, actionCollection());
    KStandardAction::zoomOut(this, [this] {
        m_reading->zoom(-1);
    }, actionCollection());
    KStandardAction::actualSize(this, [this] {
        m_reading->zoom(0);
    }, actionCollection());
    auto *dark = new KToggleAction(QIcon::fromTheme(QStringLiteral("color-mode-invert-text")), i18nc("@action", "Dark Background for Messages"), this);
    actionCollection()->addAction(QStringLiteral("dark_messages"), dark);
    dark->setChecked(MailView::darkMessages());
    dark->setVisible(MailView::darkTheme());
    connect(dark, &KToggleAction::toggled, this, [this](bool on) {
        MailView::setDarkMessages(on);
        m_reading->reload();
    });
    // File > Print, as from the main window (docs/PHASE9B.md)
    KStandardAction::print(
        this,
        [this] {
            const std::optional<MailboxInfo> box = m_mailboxes->mailbox(m_mailboxKey);
            Printing::printMessage(this, m_mailboxes->store(m_mailboxKey), m_mailboxes->blobs(m_mailboxKey), m_message, m_reading->remoteAllowed(),
                                   box ? (box->displayName.isEmpty() ? box->address : box->displayName) : QString());
        },
        actionCollection());
    KActionCollection *actions = actionCollection();

    // File > Save As (.eml or .msg), Save as PDF and Message Details, as in the main window
    KStandardAction::saveAs(
        this,
        [this] {
            const Store *store = m_mailboxes->store(m_mailboxKey);
            const std::optional<MessageRow> row = store ? store->message(m_message) : std::nullopt;
            const QString file = MimeExport::askFile(this, row ? row->subject : QString(), false);
            if (file.isEmpty()) {
                return;
            }
            auto *exporter = new MimeExporter(m_mailboxes, m_daemon, m_mailboxKey, {m_message}, file, false, this);
            connect(exporter, &MimeExporter::finished, this, [this](bool ok, int, int, const QString &path) {
                if (!ok) {
                    KMessageBox::error(this, i18nc("@info:status %1 a file", "Could not save %1.", path));
                }
            });
            exporter->start();
        },
        actions);
    QAction *savePdf = actions->addAction(QStringLiteral("save_pdf"));
    savePdf->setText(i18nc("@action", "Save as PDF..."));
    savePdf->setIcon(QIcon::fromTheme(QStringLiteral("application-pdf")));
    connect(savePdf, &QAction::triggered, this, [this] {
        const Store *store = m_mailboxes->store(m_mailboxKey);
        const std::optional<MessageRow> row = store ? store->message(m_message) : std::nullopt;
        const QString file = MimeExport::askPdfFile(this, row ? row->subject : QString());
        if (file.isEmpty()) {
            return;
        }
        const std::optional<MailboxInfo> box = m_mailboxes->mailbox(m_mailboxKey);
        Printing::printMessage(this, m_mailboxes->store(m_mailboxKey), m_mailboxes->blobs(m_mailboxKey), m_message, m_reading->remoteAllowed(),
                               box ? (box->displayName.isEmpty() ? box->address : box->displayName) : QString(), file, [this, file](bool ok) {
                                   if (!ok) {
                                       KMessageBox::error(this, i18nc("@info:status %1 a file", "Could not save %1.", file));
                                   }
                               });
    });
    QAction *details = actions->addAction(QStringLiteral("message_details"));
    details->setText(i18nc("@action", "Message Details..."));
    details->setIcon(QIcon::fromTheme(QStringLiteral("document-properties")));
    connect(details, &QAction::triggered, this, [this] {
        MessageDetailsDialog::open(this, m_mailboxes, m_daemon, m_mailboxKey, m_message);
    });

    QAction *markUnread = actions->addAction(QStringLiteral("mark_unread"));
    markUnread->setText(i18nc("@action", "Mark as Unread"));
    markUnread->setIcon(QIcon::fromTheme(QStringLiteral("mail-mark-unread")));
    actions->setDefaultShortcut(markUnread, QKeySequence(Qt::CTRL | Qt::Key_U));
    connect(markUnread, &QAction::triggered, this, [this] {
        m_daemon->markRead(account(), mailbox(), m_message, false);
    });

    QAction *flag = actions->addAction(QStringLiteral("flag"));
    flag->setText(i18nc("@action follow up on a message", "Flag"));
    flag->setIcon(QIcon::fromTheme(QStringLiteral("flag-red")));
    actions->setDefaultShortcut(flag, Qt::Key_Insert);
    connect(flag, &QAction::triggered, this, [this] {
        const Store *store = m_mailboxes->store(m_mailboxKey);
        const std::optional<MessageRow> row = store ? store->message(m_message) : std::nullopt;
        m_daemon->setFlag(account(), mailbox(), m_message, row && row->flag == 1 ? 0 : 1);
    });

    QAction *remove = actions->addAction(QStringLiteral("delete"));
    remove->setText(i18nc("@action", "Delete"));
    remove->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete")));
    actions->setDefaultShortcut(remove, Qt::Key_Delete);
    connect(remove, &QAction::triggered, this, [this] {
        if (MainWindow *main = MainWindow::instance()) {
            main->rememberForUndo(m_mailboxKey, {m_message}, true);
        }
        m_daemon->remove(account(), mailbox(), m_message);
        close();
    });

    QAction *move = actions->addAction(QStringLiteral("move"));
    move->setText(i18nc("@action", "Move to Folder..."));
    move->setIconText(i18nc("@action short toolbar text", "Move"));
    move->setIcon(QIcon::fromTheme(QStringLiteral("go-jump")));
    actions->setDefaultShortcut(move, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    connect(move, &QAction::triggered, this, [this] {
        MoveDialog dialog(m_folders, m_mailboxKey, this);
        if (dialog.exec() == QDialog::Accepted && dialog.folder() != 0) {
            moveTo(dialog.folder());
        }
    });

    QAction *archive = actions->addAction(QStringLiteral("archive"));
    archive->setText(i18nc("@action move to the Archive folder", "Archive"));
    archive->setIcon(QIcon::fromTheme(QStringLiteral("archive-insert")));
    actions->setDefaultShortcut(archive, Qt::Key_Backspace);
    connect(archive, &QAction::triggered, this, [this] {
        const QModelIndex index = m_folders->indexForWellKnown(m_mailboxKey, QStringLiteral("archive"));
        if (index.isValid()) {
            moveTo(index.data(FolderModel::FolderIdRole).toLongLong());
        }
    });

    const struct {
        const char *name;
        const char *kind;
        const char *icon;
        QKeySequence shortcut;
    } answers[] = {{"reply", "reply", "mail-reply-sender", QKeySequence(Qt::CTRL | Qt::Key_R)},
                   {"reply_all", "replyAll", "mail-reply-all", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R)},
                   {"forward", "forward", "mail-forward", QKeySequence(Qt::CTRL | Qt::Key_F)},
                   {"forward_as_attachment", "forwardAsAttachment", "mail-forward", QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_F)},
                   {"edit_as_new", "editAsNew", "mail-message-new", QKeySequence()}};
    for (const auto &answer : answers) {
        QAction *action = actions->addAction(QLatin1String(answer.name));
        action->setIcon(QIcon::fromTheme(QLatin1String(answer.icon)));
        actions->setDefaultShortcut(action, answer.shortcut);
        const QString kind = QLatin1String(answer.kind);
        connect(action, &QAction::triggered, this, [this, kind] {
            ComposeRequest request;
            request.mailboxKey = m_mailboxKey;
            request.kind = kind;
            request.message = m_message;
            (new ComposerWindow(m_mailboxes, m_daemon, request))->show();
        });
    }
    actions->action(QStringLiteral("reply"))->setText(i18nc("@action", "Reply"));
    actions->action(QStringLiteral("reply_all"))->setText(i18nc("@action", "Reply All"));
    actions->action(QStringLiteral("forward"))->setText(i18nc("@action", "Forward"));
    actions->action(QStringLiteral("forward_as_attachment"))->setText(i18nc("@action", "Forward as Attachment"));
    actions->action(QStringLiteral("edit_as_new"))->setText(i18nc("@action Outlook's Resend", "Edit as New Message"));
    QAction *meeting = actions->addAction(QStringLiteral("reply_with_meeting"));
    meeting->setText(i18nc("@action", "Reply with Meeting"));
    meeting->setIcon(QIcon::fromTheme(QStringLiteral("appointment-new")));
    actions->setDefaultShortcut(meeting, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_R));
    connect(meeting, &QAction::triggered, this, [this] {
        Q_EMIT replyWithMeetingRequested(m_mailboxKey, m_message);
    });

    // Escape closes the message, as in Outlook
    QAction *closeWindow = KStandardAction::close(this, &QWidget::close, actions);
    actions->setDefaultShortcuts(closeWindow, {QKeySequence(Qt::Key_Escape), QKeySequence::Close});
}

void MessageWindow::moveTo(qint64 folder)
{
    const Store *store = m_mailboxes->store(m_mailboxKey);
    const std::optional<MessageRow> row = store ? store->message(m_message) : std::nullopt;
    if (row && row->folder != folder) {
        if (MainWindow *main = MainWindow::instance()) {
            main->rememberForUndo(m_mailboxKey, {m_message}, false);
        }
        m_daemon->move(account(), mailbox(), m_message, folder);
    }
    close();
}

void MessageWindow::refresh()
{
    const Store *store = m_mailboxes->store(m_mailboxKey);
    const std::optional<MessageRow> row = store ? store->message(m_message) : std::nullopt;
    if (!row) {
        // Deleted or moved away elsewhere (Outlook on the phone, another window)
        close();
        return;
    }
    setWindowTitle(row->subject.isEmpty() ? i18nc("@info a message without a subject", "(no subject)") : row->subject);
    if (QAction *archive = actionCollection()->action(QStringLiteral("archive"))) {
        const QModelIndex index = m_folders->indexForWellKnown(m_mailboxKey, QStringLiteral("archive"));
        archive->setEnabled(index.isValid() && index.data(FolderModel::FolderIdRole).toLongLong() != row->folder);
    }
    m_reading->reload();
}

}
