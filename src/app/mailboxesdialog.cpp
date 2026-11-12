// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mailboxesdialog.h"
#include "accesstext.h"
#include "daemonclient.h"
#include "mailboxset.h"
#include "recipientedit.h"
#include "store.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace GraphMail
{

MailboxesDialog::MailboxesDialog(MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent)
    : QDialog(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
    , m_book(new AddressBook(mailboxes, this))
    , m_list(new QTreeWidget(this))
    , m_remove(new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), i18nc("@action:button", "Remove"), this))
    , m_account(new QComboBox(this))
    , m_address(new RecipientEdit(m_book, this))
    , m_check(new QPushButton(i18nc("@action:button", "Check"), this))
    , m_add(new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), i18nc("@action:button", "Add"), this))
    , m_result(new QLabel(this))
{
    setWindowTitle(i18nc("@title:window", "Mailboxes"));
    resize(720, 520);

    m_list->setHeaderLabels({i18nc("@title:column", "Mailbox"), i18nc("@title:column", "Address"), i18nc("@title:column", "Gives")});
    m_list->setRootIsDecorated(true);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_list->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_list->header()->setStretchLastSection(false);

    auto *add = new QGroupBox(i18nc("@title:group", "Add a Mailbox or Calendar"), this);
    auto *form = new QFormLayout(add);
    form->addRow(i18nc("@label:listbox", "Account:"), m_account);
    auto *addressRow = new QHBoxLayout;
    m_address->setPlaceholderText(i18nc("@info:placeholder", "Address of the shared mailbox or colleague"));
    addressRow->addWidget(m_address, 1);
    addressRow->addWidget(m_check);
    form->addRow(i18nc("@label:textbox", "Address:"), addressRow);
    m_result->setWordWrap(true);
    m_result->setTextFormat(Qt::PlainText);
    form->addRow(m_result);
    auto *addRow = new QHBoxLayout;
    addRow->addStretch();
    addRow->addWidget(m_add);
    form->addRow(addRow);
    m_add->setEnabled(false);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *listButtons = new QHBoxLayout;
    listButtons->addStretch();
    listButtons->addWidget(m_remove);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(i18nc("@info", "Other mailboxes and calendars you have access to. Graph Mail keeps a copy of what each gives."), this));
    layout->addWidget(m_list, 1);
    layout->addLayout(listButtons);
    layout->addWidget(add);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_remove, &QPushButton::clicked, this, &MailboxesDialog::remove);
    connect(m_list, &QTreeWidget::currentItemChanged, this, [this] {
        const QTreeWidgetItem *item = m_list->currentItem();
        m_remove->setEnabled(item && !item->data(0, Qt::UserRole).toString().isEmpty());
    });
    connect(m_check, &QPushButton::clicked, this, [this] {
        check(false);
    });
    connect(m_address, &QLineEdit::returnPressed, this, [this] {
        check(false);
    });
    connect(m_address, &QLineEdit::textEdited, this, [this] {
        // A new address: the last check no longer applies
        m_add->setEnabled(false);
        m_result->clear();
    });
    connect(m_add, &QPushButton::clicked, this, [this] {
        check(true);
    });
    fill();
}

void MailboxesDialog::fill()
{
    m_list->clear();
    const QString was = m_account->currentData().toString();
    m_account->clear();
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    QHash<QString, QTreeWidgetItem *> accounts;
    for (const MailboxInfo &box : boxes) {
        if (box.own) {
            auto *item = new QTreeWidgetItem(m_list, {box.address});
            QFont bold = item->font(0);
            bold.setBold(true);
            item->setFont(0, bold);
            item->setFirstColumnSpanned(true);
            accounts.insert(box.account, item);
            m_account->addItem(box.address, box.account);
        }
    }
    for (const MailboxInfo &box : boxes) {
        if (box.own || !accounts.contains(box.account)) {
            continue;
        }
        auto *item = new QTreeWidgetItem(accounts.value(box.account), {box.displayName.isEmpty() ? box.address : box.displayName, box.address,
                                                                       accessSummary(box.mail, box.calendar, box.contacts)});
        item->setData(0, Qt::UserRole, box.key());
    }
    for (QTreeWidgetItem *item : std::as_const(accounts)) {
        item->setExpanded(true);
        if (item->childCount() == 0) {
            auto *none = new QTreeWidgetItem(item, {i18nc("@item no other mailboxes yet", "(none)")});
            none->setDisabled(true);
        }
    }
    m_account->setCurrentIndex(std::max(0, m_account->findData(was)));
    m_account->setVisible(m_account->count() > 1);
    if (auto *form = qobject_cast<QFormLayout *>(m_account->parentWidget()->layout())) {
        form->setRowVisible(m_account, m_account->count() > 1);
    }
    m_remove->setEnabled(false);
}

QString MailboxesDialog::account() const
{
    return m_account->currentData().toString();
}

bool MailboxesDialog::inOwnCalendars(const QString &account, const QString &address) const
{
    const Store *store = m_mailboxes->store(account + QStringLiteral("/me"));
    if (!store) {
        return false;
    }
    const QList<CalendarRow> calendars = store->calendars();
    return std::any_of(calendars.cbegin(), calendars.cend(), [&address](const CalendarRow &c) {
        return c.owner.compare(address, Qt::CaseInsensitive) == 0;
    });
}

void MailboxesDialog::check(bool add)
{
    const QList<OutgoingRecipient> recipients = m_address->recipients();
    const QString address = recipients.isEmpty() ? m_address->text().trimmed() : recipients.first().address;
    if (address.isEmpty()) {
        return;
    }
    m_check->setEnabled(false);
    m_add->setEnabled(false);
    m_result->setText(add ? i18nc("@info:status", "Adding %1...", address) : i18nc("@info:status", "Checking what %1 gives you...", address));
    m_daemon->checkMailbox(account(), address, add, this, [this, address, add](const QString &reply) {
        m_check->setEnabled(true);
        const QJsonObject access = QJsonDocument::fromJson(reply.toUtf8()).object();
        if (access.isEmpty()) {
            m_result->setText(reply.startsWith(QLatin1String("error")) ? accessProblemText(QStringLiteral("error"), address)
                                                                        : i18nc("@info", "Graph Mail's background service did not answer."));
            qWarning() << "mailbox check test:" << (reply.isEmpty() ? QStringLiteral("no answer") : reply);
            return;
        }
        showCheck(access, add);
        if (add && access.value(QLatin1String("problem")).toString().isEmpty()) {
            m_mailboxes->reload();
            fill();
            Q_EMIT mailboxesChanged();
            // The cache appears once the daemon opened it: look again shortly
            QTimer::singleShot(3000, this, [this] {
                m_mailboxes->reload();
                fill();
                Q_EMIT mailboxesChanged();
            });
        }
    });
}

void MailboxesDialog::showCheck(const QJsonObject &access, bool added)
{
    m_lastCheck = access;
    const QString problem = access.value(QLatin1String("problem")).toString();
    const QString name = access.value(QLatin1String("name")).toString();
    const QString address = access.value(QLatin1String("address")).toString();
    const QString who = name.isEmpty() ? m_address->text().trimmed() : name + QStringLiteral(" <") + address + QLatin1Char('>');
    const bool mail = access.value(QLatin1String("mail")).toBool();
    const bool calendar = access.value(QLatin1String("calendar")).toBool();
    const bool contacts = access.value(QLatin1String("contacts")).toBool();
    const QString key = account() + QLatin1Char('/') + access.value(QLatin1String("id")).toString();
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    const bool present = std::any_of(boxes.cbegin(), boxes.cend(), [&key](const MailboxInfo &b) {
        return b.key() == key;
    });
    qWarning() << "mailbox check test:" << address << "mail" << mail << "calendar" << calendar << "contacts" << contacts << "problem" << problem << "added" << added;

    if (!problem.isEmpty()) {
        m_result->setText(accessProblemText(problem, who));
        return;
    }
    if (added) {
        m_result->setText(i18nc("@info %1 a mailbox, %2 what it gives", "Added %1: %2. Graph Mail is copying it now.", who,
                                accessSummary(mail, calendar, contacts, access.value(QLatin1String("calendarEdit")).toBool())));
        m_address->clear();
        return;
    }
    QString text = who + QStringLiteral(": ") + accessSummary(mail, calendar, contacts, access.value(QLatin1String("calendarEdit")).toBool()) + QLatin1Char('.');
    bool canAdd = mail || calendar || contacts;
    if (!mail && !contacts && calendar && inOwnCalendars(account(), address)) {
        text += QLatin1Char(' ') + i18nc("@info", "This calendar is already among your calendars (it was shared with you).");
        canAdd = false;
    } else if (!canAdd) {
        text += QLatin1Char(' ') + i18nc("@info", "Their free/busy times show when you invite them to a meeting.");
    } else if (present) {
        text += QLatin1Char(' ') + i18nc("@info", "It is already in Graph Mail; Add brings its access up to date.");
    }
    m_result->setText(text);
    m_add->setEnabled(canAdd);
}

void MailboxesDialog::remove()
{
    const QTreeWidgetItem *item = m_list->currentItem();
    const QString key = item ? item->data(0, Qt::UserRole).toString() : QString();
    if (key.isEmpty()) {
        return;
    }
    if (KMessageBox::warningContinueCancel(this,
                                           i18nc("@info", "Remove %1 from Graph Mail? Its copy on this computer is deleted; nothing changes in Microsoft 365.", item->text(0)),
                                           i18nc("@title:window", "Remove Mailbox"),
                                           KStandardGuiItem::remove())
        != KMessageBox::Continue) {
        return;
    }
    removeMailbox(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), item->text(0));
}

void MailboxesDialog::removeMailbox(const QString &account, const QString &mailbox, const QString &label)
{
    m_daemon->removeMailbox(account, mailbox, this, [this, label](const QString &reply) {
        qWarning() << "mailbox remove test:" << reply;
        m_result->setText(reply == QLatin1String("removed") ? i18nc("@info", "Removed %1.", label) : accessProblemText(QStringLiteral("error"), label));
        m_mailboxes->reload();
        fill();
        Q_EMIT mailboxesChanged();
    });
}

void MailboxesDialog::checkForTests(const QString &address, bool add)
{
    m_address->setText(address);
    check(add);
}

void MailboxesDialog::removeForTests(const QString &address)
{
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        if (!box.own && box.address.compare(address, Qt::CaseInsensitive) == 0) {
            removeMailbox(box.account, box.mailbox, box.label());
            return;
        }
    }
    qWarning() << "mailbox remove test: not found";
}

}
