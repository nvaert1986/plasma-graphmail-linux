// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "waitingwindow.h"
#include "daemonclient.h"
#include "mailboxset.h"
#include "store.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
constexpr int keyRole = Qt::UserRole;
constexpr int idRole = Qt::UserRole + 1;

QString when(qint64 ms)
{
    return ms > 0 ? QLocale().toString(QDateTime::fromMSecsSinceEpoch(ms), QLocale::ShortFormat) : QString();
}

QTreeWidget *list(const QStringList &headers, QWidget *parent)
{
    auto *tree = new QTreeWidget(parent);
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(false);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setStretchLastSection(false);
    for (int column = 1; column < headers.size(); ++column) {
        tree->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    return tree;
}
}

QString WaitingWindow::describeKind(const QString &kind)
{
    static const QHash<QString, KLocalizedString> texts{
        {QStringLiteral("read"), ki18nc("@item a waiting change", "Mark as read or unread")},
        {QStringLiteral("flag"), ki18nc("@item a waiting change", "Flag")},
        {QStringLiteral("categories"), ki18nc("@item a waiting change", "Categories")},
        {QStringLiteral("move"), ki18nc("@item a waiting change", "Move")},
        {QStringLiteral("delete"), ki18nc("@item a waiting change", "Delete")},
        {QStringLiteral("eventCreate"), ki18nc("@item a waiting change", "New event")},
        {QStringLiteral("eventUpdate"), ki18nc("@item a waiting change", "Change event")},
        {QStringLiteral("eventDelete"), ki18nc("@item a waiting change", "Delete event")},
        {QStringLiteral("eventCancel"), ki18nc("@item a waiting change", "Cancel meeting")},
        {QStringLiteral("eventRespond"), ki18nc("@item a waiting change", "Answer invitation")},
        {QStringLiteral("eventDismiss"), ki18nc("@item a waiting change", "Dismiss reminder")},
        {QStringLiteral("eventSnooze"), ki18nc("@item a waiting change", "Snooze reminder")},
        {QStringLiteral("itemCreate"), ki18nc("@item a waiting change", "New contact or task")},
        {QStringLiteral("itemUpdate"), ki18nc("@item a waiting change", "Change contact or task")},
        {QStringLiteral("itemDelete"), ki18nc("@item a waiting change", "Delete contact or task")},
        {QStringLiteral("itemListCreate"), ki18nc("@item a waiting change", "New task list")},
        {QStringLiteral("itemListUpdate"), ki18nc("@item a waiting change", "Rename task list")},
        {QStringLiteral("itemListDelete"), ki18nc("@item a waiting change", "Delete task list")},
        {QStringLiteral("itemStep"), ki18nc("@item a waiting change", "Task step")},
    };
    const auto it = texts.constFind(kind);
    return it == texts.cend() ? kind : it->toString();
}

WaitingWindow::WaitingWindow(MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent)
    : QDialog(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
    , m_state(new QLabel(this))
    , m_messages(list({i18nc("@title:column", "Message"), i18nc("@title:column", "To"), i18nc("@title:column", "State"), i18nc("@title:column", "Mailbox")}, this))
    , m_changes(list({i18nc("@title:column", "Change"), i18nc("@title:column", "Item"), i18nc("@title:column", "Tries"), i18nc("@title:column", "Next try"),
                      i18nc("@title:column", "Mailbox")},
                     this))
    , m_open(new QPushButton(QIcon::fromTheme(QStringLiteral("document-edit")), i18nc("@action:button", "Open"), this))
    , m_send(new QPushButton(QIcon::fromTheme(QStringLiteral("mail-send")), i18nc("@action:button", "Send Again"), this))
    , m_delete(new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action:button", "Delete"), this))
    , m_retry(new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), i18nc("@action:button", "Retry Now"), this))
    , m_discard(new QPushButton(QIcon::fromTheme(QStringLiteral("edit-undo")), i18nc("@action:button", "Discard"), this))
{
    setWindowTitle(i18nc("@title:window", "Waiting to Be Sent"));
    resize(860, 560);
    m_state->setWordWrap(true);
    m_discard->setToolTip(i18nc("@info:tooltip", "Undo the change here; Microsoft 365 keeps what it has"));
    auto *messageButtons = new QHBoxLayout;
    messageButtons->addStretch();
    messageButtons->addWidget(m_open);
    messageButtons->addWidget(m_send);
    messageButtons->addWidget(m_delete);
    auto *changeButtons = new QHBoxLayout;
    changeButtons->addStretch();
    changeButtons->addWidget(m_retry);
    changeButtons->addWidget(m_discard);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_state);
    layout->addWidget(new QLabel(i18nc("@title:group", "Messages"), this));
    layout->addWidget(m_messages, 1);
    layout->addLayout(messageButtons);
    layout->addWidget(new QLabel(i18nc("@title:group", "Changes"), this));
    layout->addWidget(m_changes, 1);
    layout->addLayout(changeButtons);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_open, &QPushButton::clicked, this, [this] {
        if (const QTreeWidgetItem *item = m_messages->currentItem()) {
            Q_EMIT openOutgoing(item->data(0, keyRole).toString(), item->data(0, idRole).toString());
        }
    });
    connect(m_send, &QPushButton::clicked, this, &WaitingWindow::sendAgain);
    connect(m_delete, &QPushButton::clicked, this, &WaitingWindow::deleteMessage);
    connect(m_retry, &QPushButton::clicked, this, &WaitingWindow::retry);
    connect(m_discard, &QPushButton::clicked, this, &WaitingWindow::discard);
    connect(m_messages, &QTreeWidget::itemSelectionChanged, this, [this] {
        const bool any = !m_messages->selectedItems().isEmpty();
        m_open->setEnabled(m_messages->selectedItems().size() == 1);
        m_send->setEnabled(any);
        m_delete->setEnabled(any);
    });
    connect(m_changes, &QTreeWidget::itemSelectionChanged, this, [this] {
        m_discard->setEnabled(!m_changes->selectedItems().isEmpty());
    });
    // The daemon works on: the lists follow
    m_refresh.setSingleShot(true);
    m_refresh.setInterval(500);
    connect(&m_refresh, &QTimer::timeout, this, &WaitingWindow::fill);
    connect(m_daemon, &DaemonClient::statusChanged, &m_refresh, qOverload<>(&QTimer::start));
    connect(m_daemon, &DaemonClient::outgoingChanged, &m_refresh, qOverload<>(&QTimer::start));
    fill();
}

void WaitingWindow::fill()
{
    m_messages->clear();
    m_changes->clear();
    bool offline = false;
    const QJsonArray status = m_daemon->status();
    for (const QJsonValue &v : status) {
        offline |= v.toObject().value(QLatin1String("state")).toString() == QLatin1String("offline");
    }
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        const Store *store = m_mailboxes->store(box.key());
        if (!store) {
            continue;
        }
        const QString label = box.own ? box.address : box.label();
        const QList<Outgoing> outgoing = store->allOutgoing();
        for (const Outgoing &o : outgoing) {
            // Drafts being saved are not waiting to be sent
            if (!o.send || o.state == Outgoing::State::Discard) {
                continue;
            }
            QStringList to;
            for (const OutgoingRecipient &r : o.to) {
                to.append(r.name.isEmpty() ? r.address : r.name);
            }
            const QString state = o.state == Outgoing::State::Failed
                ? i18nc("@item message state, %1 the reason", "Not sent: %1", o.lastError)
                : (offline ? i18nc("@item message state", "Waiting for the connection") : i18nc("@item message state", "Being sent"));
            auto *item = new QTreeWidgetItem(m_messages, {o.subject.isEmpty() ? i18nc("@item a message without a subject", "(no subject)") : o.subject,
                                                          to.join(QStringLiteral(", ")), state, label});
            item->setData(0, keyRole, box.key());
            item->setData(0, idRole, o.uuid);
            item->setToolTip(2, state);
            if (o.state == Outgoing::State::Failed) {
                item->setIcon(2, QIcon::fromTheme(QStringLiteral("dialog-error")));
            }
        }
        const QList<OperationRow> ops = store->allOperations();
        for (const OperationRow &op : ops) {
            // What it is about: a message's subject, an event's or an item's name when known
            QString what;
            if (const std::optional<MessageRow> m = store->messageByGraphId(op.target)) {
                what = m->subject;
            } else {
                const QJsonObject payload = QJsonDocument::fromJson(op.payload.toUtf8()).object();
                const QJsonObject body = payload.value(QLatin1String("body")).toObject();
                what = body.value(QLatin1String("subject")).toString();
                if (what.isEmpty()) {
                    what = body.value(QLatin1String("displayName")).toString();
                }
                if (what.isEmpty()) {
                    what = body.value(QLatin1String("title")).toString();
                }
            }
            const QString next = op.state == 2 ? i18nc("@item a change", "Failed") : when(op.nextAttempt);
            auto *item = new QTreeWidgetItem(m_changes, {describeKind(op.kind), what, QString::number(op.attempts), next, label});
            item->setData(0, keyRole, box.key());
            item->setData(0, idRole, op.id);
            if (!op.lastError.isEmpty()) {
                item->setToolTip(0, op.lastError);
                item->setToolTip(1, op.lastError);
                item->setIcon(0, QIcon::fromTheme(QStringLiteral("dialog-warning")));
            }
        }
    }
    const int messages = m_messages->topLevelItemCount();
    const int changes = m_changes->topLevelItemCount();
    if (messages == 0 && changes == 0) {
        m_state->setText(i18nc("@info", "Everything is in Microsoft 365."));
    } else {
        const QString counts = i18ncp("@info", "%1 message", "%1 messages", messages) + QStringLiteral(", ") + i18ncp("@info", "%1 change", "%1 changes", changes);
        m_state->setText(offline ? i18nc("@info %1 counts", "Offline: %1 waiting. They go out when the connection is back.", counts)
                                 : i18nc("@info %1 counts", "%1 waiting to be sent.", counts));
    }
    m_open->setEnabled(false);
    m_send->setEnabled(false);
    m_delete->setEnabled(false);
    m_discard->setEnabled(false);
    m_retry->setEnabled(messages + changes > 0);
}

void WaitingWindow::sendAgain()
{
    const QList<QTreeWidgetItem *> items = m_messages->selectedItems();
    for (const QTreeWidgetItem *item : items) {
        const QString key = item->data(0, keyRole).toString();
        const Store *store = m_mailboxes->store(key);
        std::optional<Outgoing> o = store ? store->outgoing(item->data(0, idRole).toString()) : std::nullopt;
        if (!o) {
            continue;
        }
        // Saved again with "send": the daemon starts over with it
        o->send = true;
        m_daemon->saveOutgoing(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QString::fromUtf8(QJsonDocument(o->toJson()).toJson(QJsonDocument::Compact)));
    }
    m_refresh.start();
}

void WaitingWindow::deleteMessage()
{
    const QList<QTreeWidgetItem *> items = m_messages->selectedItems();
    if (items.isEmpty()
        || KMessageBox::warningContinueCancel(this, i18ncp("@info", "Delete this message? It is not sent.", "Delete these %1 messages? They are not sent.", int(items.size())),
                                              i18nc("@title:window", "Delete Message"), KStandardGuiItem::del())
            != KMessageBox::Continue) {
        return;
    }
    for (const QTreeWidgetItem *item : items) {
        const QString key = item->data(0, keyRole).toString();
        m_daemon->discardOutgoing(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), item->data(0, idRole).toString());
    }
    m_refresh.start();
}

void WaitingWindow::retry()
{
    QSet<QString> keys;
    for (int i = 0; i < m_changes->topLevelItemCount(); ++i) {
        keys.insert(m_changes->topLevelItem(i)->data(0, keyRole).toString());
    }
    for (int i = 0; i < m_messages->topLevelItemCount(); ++i) {
        keys.insert(m_messages->topLevelItem(i)->data(0, keyRole).toString());
    }
    for (const QString &key : std::as_const(keys)) {
        m_daemon->retryNow(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1));
    }
    m_refresh.start();
}

void WaitingWindow::discard()
{
    const QList<QTreeWidgetItem *> items = m_changes->selectedItems();
    if (items.isEmpty()
        || KMessageBox::warningContinueCancel(this,
                                              i18ncp("@info", "Undo this change here? It will not reach Microsoft 365.", "Undo these %1 changes here? They will not reach Microsoft 365.", int(items.size())),
                                              i18nc("@title:window", "Discard Changes"), KStandardGuiItem::discard())
            != KMessageBox::Continue) {
        return;
    }
    for (const QTreeWidgetItem *item : items) {
        const QString key = item->data(0, keyRole).toString();
        m_daemon->discardOperation(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), item->data(0, idRole).toLongLong());
    }
    m_refresh.start();
}

int WaitingWindow::messageCount() const
{
    return m_messages->topLevelItemCount();
}

int WaitingWindow::changeCount() const
{
    return m_changes->topLevelItemCount();
}

}
