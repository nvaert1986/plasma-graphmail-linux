// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shell.h"
#include "mainwindow.h"
#include "messagelistmodel.h"
#include "profilephotos.h"
#include "mailboxset.h"
#include "avatar.h"

#include <KActionCollection>
#include <KConfigGroup>
#include <KXmlGuiWindow>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDropEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QMenu>
#include <QMimeData>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
KConfigGroup shellGroup()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Shell"));
}
}

Shell::Shell(MailboxSet *mailboxes, DaemonClient *daemon)
    : QWidget(nullptr)
    , m_stack(new QStackedWidget(this))
    , m_buttons(new QButtonGroup(this))
    , m_mailboxes(mailboxes)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowIcon(QApplication::windowIcon());

    // The navigation bar: the modules, as Outlook's on the left
    auto *bar = new QWidget(this);
    bar->setAutoFillBackground(true);
    bar->setBackgroundRole(QPalette::AlternateBase);
    auto *barLayout = new QVBoxLayout(bar);
    barLayout->setContentsMargins(4, 6, 4, 6);
    barLayout->setSpacing(2);
    // The account, as Outlook's picture at the top: its photo or initials, Change Photo
    m_account = new QToolButton(bar);
    m_account->setAutoRaise(true);
    m_account->setFocusPolicy(Qt::NoFocus);
    m_account->setIconSize(QSize(36, 36));
    m_account->setPopupMode(QToolButton::InstantPopup);
    m_account->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; }"));
    barLayout->addWidget(m_account, 0, Qt::AlignHCenter);
    barLayout->addSpacing(6);
    struct Module {
        const char *icon;
        KLocalizedString text;
        KLocalizedString tip;
    };
    const Module modules[] = {{"mail-message", ki18nc("@action:button navigation", "Mail"), ki18nc("@info:tooltip", "Mail (Ctrl+1)")},
                              {"view-calendar", ki18nc("@action:button navigation", "Calendar"), ki18nc("@info:tooltip", "Calendar (Ctrl+2)")},
                              {"view-pim-contacts", ki18nc("@action:button navigation", "Contacts"), ki18nc("@info:tooltip", "Contacts (Ctrl+3)")},
                              {"view-task", ki18nc("@action:button navigation", "Tasks"), ki18nc("@info:tooltip", "Tasks (Ctrl+4)")}};
    for (int i = 0; i < 4; ++i) {
        auto *button = new QToolButton(bar);
        button->setIcon(QIcon::fromTheme(QLatin1String(modules[i].icon)));
        button->setText(modules[i].text.toString());
        button->setToolTip(modules[i].tip.toString());
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIconSize(QSize(24, 24));
        button->setAutoRaise(true);
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setMinimumWidth(64);
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        // Right-click: the module in a window of its own, as Outlook's "Open in New Window"
        connect(button, &QWidget::customContextMenuRequested, this, [this, button, i](const QPoint &pos) {
            QMenu menu(this);
            menu.addAction(QIcon::fromTheme(QStringLiteral("window-new")), i18nc("@action:inmenu", "Open in New Window"), this, [this, i] {
                m_mail->openInNewWindow(i + 1);
            });
            menu.exec(button->mapToGlobal(pos));
        });
        m_buttons->addButton(button, i + 1);
        barLayout->addWidget(button);
        if (i == 1 || i == 3) {
            button->setAcceptDrops(true);
            button->installEventFilter(this);
        }
    }
    barLayout->addStretch();
    connect(m_buttons, &QButtonGroup::idClicked, this, [this](int module) {
        m_mail->navigate(module);
    });
    auto *line = new QFrame(this);
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(bar);
    layout->addWidget(line);
    layout->addWidget(m_stack, 1);

    m_mail = new MainWindow(mailboxes, daemon);
    m_mail->setShell(this);
    embed(1, m_mail);
    showModule(1);
    connect(m_mailboxes, &MailboxSet::photoChanged, this, &Shell::updateAccount);
    connect(m_mailboxes, &MailboxSet::changed, this, &Shell::updateAccount);
    updateAccount();

    if (const QByteArray geometry = shellGroup().readEntry("Geometry", QByteArray()); geometry.isEmpty() || !restoreGeometry(geometry)) {
        resize(1380, 820);
    }
}

Shell::~Shell() = default;

void Shell::embed(int module, QWidget *page)
{
    if (module < 1 || module > 4) {
        return;
    }
    // A window of its own until now: a widget in the shell from here on. KMainWindow gave it a
    // native window (to restore its size); kept, it would be drawn as a surface of its own over
    // the other modules (flicker, leftovers of the Calendar over Mail). Without the attribute
    // Qt drops that window when the widget moves into the shell
    page->setAttribute(Qt::WA_NativeWindow, false);
    page->setParent(m_stack, Qt::Widget);
    m_stack->addWidget(page);
    m_pages[module - 1] = page;
    connect(page, &QWidget::windowTitleChanged, this, &Shell::updateTitle);
    // Its shortcuts off until it is shown
    if (auto *window = qobject_cast<KXmlGuiWindow *>(page); window && m_stack->currentWidget() != page) {
        const QList<QAction *> actions = window->actionCollection()->actions();
        for (QAction *action : actions) {
            action->setProperty("graphmailShortcutContext", int(action->shortcutContext()));
            action->setShortcutContext(Qt::WidgetShortcut);
        }
    }
    connect(page, &QObject::destroyed, this, [this, module] {
        m_pages[module - 1] = nullptr;
    });
}

void Shell::showModule(int module)
{
    QWidget *page = module >= 1 && module <= 4 ? m_pages[module - 1] : nullptr;
    if (!page) {
        return;
    }
    m_stack->setCurrentWidget(page);
    // One module's shortcuts at a time: Ctrl+N is a new message in Mail and a new event in the
    // Calendar. Those of the hidden modules only work from their own (hidden) widgets
    for (QWidget *other : m_pages) {
        auto *window = qobject_cast<KXmlGuiWindow *>(other);
        if (!window) {
            continue;
        }
        const QList<QAction *> actions = window->actionCollection()->actions();
        for (QAction *action : actions) {
            const QVariant original = action->property("graphmailShortcutContext");
            if (!original.isValid()) {
                action->setProperty("graphmailShortcutContext", int(action->shortcutContext()));
            }
            action->setShortcutContext(other == page ? Qt::ShortcutContext(action->property("graphmailShortcutContext").toInt()) : Qt::WidgetShortcut);
        }
    }
    if (QAbstractButton *button = m_buttons->button(module)) {
        button->setChecked(true);
    }
    updateTitle();
}

int Shell::module() const
{
    for (int i = 0; i < 4; ++i) {
        if (m_pages[i] && m_pages[i] == m_stack->currentWidget()) {
            return i + 1;
        }
    }
    return 1;
}

void Shell::updateAccount()
{
    const QString key = m_mailboxes->primaryAccount() + QStringLiteral("/me");
    const std::optional<MailboxInfo> box = m_mailboxes->mailbox(key);
    if (!box) {
        m_account->hide();
        return;
    }
    m_account->show();
    m_account->setIcon(QIcon(Avatar::pixmap(m_mailboxes->photo(key), 36, box->displayName, box->address)));
    m_account->setToolTip(box->displayName.isEmpty() ? box->address : box->displayName + QStringLiteral("\n") + box->address);
    auto *menu = new QMenu(m_account);
    QAction *who = menu->addAction(box->displayName.isEmpty() ? box->address : box->displayName + QStringLiteral(" <") + box->address + QLatin1Char('>'));
    who->setEnabled(false);
    menu->addSeparator();
    menu->addAction(QIcon::fromTheme(QStringLiteral("camera-photo")), i18nc("@action:inmenu", "Change Photo..."), this, [this, key] {
        m_mail->photos()->change(this, key);
    });
    if (!m_mailboxes->photo(key).isNull()) {
        menu->addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action:inmenu", "Remove Photo"), this, [this, key] {
            m_mail->photos()->remove(this, key);
        });
    }
    delete m_account->menu();
    m_account->setMenu(menu);
}

void Shell::updateTitle()
{
    // The module's caption ("Inbox - mailbox", "Calendar") is the window's
    if (QWidget *page = m_stack->currentWidget()) {
        setWindowTitle(page->windowTitle());
    }
}

bool Shell::eventFilter(QObject *watched, QEvent *event)
{
    const int module = m_buttons->id(qobject_cast<QAbstractButton *>(watched));
    if (module != 2 && module != 4) {
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        auto *drag = static_cast<QDragMoveEvent *>(event);
        if (drag->mimeData()->hasFormat(MessageListModel::mimeType())) {
            drag->setDropAction(Qt::CopyAction);
            drag->accept();
            return true;
        }
    } else if (event->type() == QEvent::Drop) {
        auto *drop = static_cast<QDropEvent *>(event);
        // "<mailbox key>\n<id>\n<id>...": an appointment or a task from the first message
        const QStringList lines = QString::fromLatin1(drop->mimeData()->data(MessageListModel::mimeType())).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (lines.size() >= 2) {
            drop->setDropAction(Qt::CopyAction);
            drop->accept();
            if (module == 2) {
                m_mail->newAppointmentFromMail(lines.at(0), lines.at(1).toLongLong());
            } else {
                m_mail->newTaskFromMail(lines.at(0), lines.at(1).toLongLong());
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void Shell::closeEvent(QCloseEvent *event)
{
    KConfigGroup group = shellGroup();
    group.writeEntry("Geometry", saveGeometry());
    group.sync();
    // The mail module saves its layout and closes the other windows (messages, detached modules)
    m_mail->close();
    event->accept();
}

}
