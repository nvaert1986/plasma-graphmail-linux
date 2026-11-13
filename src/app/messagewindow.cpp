// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "messagewindow.h"
#include "printing.h"
#include "composer.h"
#include "daemonclient.h"
#include "foldermodel.h"
#include "mailboxset.h"
#include "mailview.h"
#include "movedialog.h"
#include "readingpane.h"
#include "store.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KStandardAction>

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
        m_daemon->markRead(account(), mailbox(), m_message, true);
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
    // File > Print, as from the main window (docs/PHASE9B.md)
    KStandardAction::print(
        this,
        [this] {
            const std::optional<MailboxInfo> box = m_mailboxes->mailbox(m_mailboxKey);
            Printing::printMessage(this, m_mailboxes->store(m_mailboxKey), m_mailboxes->blobs(m_mailboxKey), m_message, false,
                                   box ? (box->displayName.isEmpty() ? box->address : box->displayName) : QString());
        },
        actionCollection());
    KActionCollection *actions = actionCollection();

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
                   {"forward", "forward", "mail-forward", QKeySequence(Qt::CTRL | Qt::Key_F)}};
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

    // Escape closes the message, as in Outlook
    QAction *closeWindow = KStandardAction::close(this, &QWidget::close, actions);
    actions->setDefaultShortcuts(closeWindow, {QKeySequence(Qt::Key_Escape), QKeySequence::Close});
}

void MessageWindow::moveTo(qint64 folder)
{
    const Store *store = m_mailboxes->store(m_mailboxKey);
    const std::optional<MessageRow> row = store ? store->message(m_message) : std::nullopt;
    if (row && row->folder != folder) {
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
