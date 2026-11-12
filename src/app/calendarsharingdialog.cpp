// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "calendarsharingdialog.h"
#include "accesstext.h"
#include "daemonclient.h"
#include "mailboxset.h"
#include "recipientedit.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
// The levels a new person can get, least first (Outlook's order)
QStringList addableRoles()
{
    return {QStringLiteral("freeBusyRead"),
            QStringLiteral("limitedRead"),
            QStringLiteral("read"),
            QStringLiteral("write"),
            QStringLiteral("delegateWithoutPrivateEventAccess"),
            QStringLiteral("delegateWithPrivateEventAccess")};
}

QString who(const QJsonObject &permission)
{
    const QJsonObject email = permission.value(QLatin1String("emailAddress")).toObject();
    const QString name = email.value(QLatin1String("name")).toString();
    const QString address = email.value(QLatin1String("address")).toString();
    if (address.isEmpty()) {
        // The entry for everyone in the organisation: Graph names it in English ("My Organization")
        return i18nc("@item calendar sharing", "People in my organisation");
    }
    return name.isEmpty() || name == address ? address : name + QStringLiteral(" <") + address + QLatin1Char('>');
}
}

QString CalendarSharingDialog::roleText(const QString &role)
{
    if (role == QLatin1String("none")) {
        return i18nc("@item calendar sharing level", "Not shared");
    }
    if (role == QLatin1String("freeBusyRead")) {
        return i18nc("@item calendar sharing level", "Can view when I'm busy");
    }
    if (role == QLatin1String("limitedRead")) {
        return i18nc("@item calendar sharing level", "Can view titles and locations");
    }
    if (role == QLatin1String("read")) {
        return i18nc("@item calendar sharing level", "Can view all details");
    }
    if (role == QLatin1String("write")) {
        return i18nc("@item calendar sharing level", "Can edit");
    }
    if (role == QLatin1String("delegateWithoutPrivateEventAccess")) {
        return i18nc("@item calendar sharing level", "Delegate");
    }
    if (role == QLatin1String("delegateWithPrivateEventAccess")) {
        return i18nc("@item calendar sharing level", "Delegate, can view private events");
    }
    return i18nc("@item calendar sharing level", "Custom (set in Outlook)");
}

CalendarSharingDialog::CalendarSharingDialog(MailboxSet *mailboxes, DaemonClient *daemon, const CalendarInfo &calendar, QWidget *parent)
    : QDialog(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
    , m_calendar(calendar)
    , m_book(new AddressBook(mailboxes, this))
    , m_list(new QTreeWidget(this))
    , m_remove(new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), i18nc("@action:button", "Remove"), this))
    , m_address(new RecipientEdit(m_book, this))
    , m_role(new QComboBox(this))
    , m_add(new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), i18nc("@action:button", "Add"), this))
    , m_status(new QLabel(this))
{
    setWindowTitle(i18nc("@title:window %1 a calendar", "Sharing: %1", calendar.name));
    resize(720, 460);
    m_list->setHeaderLabels({i18nc("@title:column", "Person"), i18nc("@title:column", "Permission")});
    m_list->setRootIsDecorated(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_list->header()->setStretchLastSection(false);
    m_address->setPlaceholderText(i18nc("@info:placeholder", "Add a person from your organisation"));
    const QStringList roles = addableRoles();
    for (const QString &role : roles) {
        m_role->addItem(roleText(role), role);
    }
    m_role->setCurrentIndex(m_role->findData(QStringLiteral("read")));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_remove->setEnabled(false);
    m_add->setEnabled(false);

    auto *addRow = new QHBoxLayout;
    addRow->addWidget(m_address, 1);
    addRow->addWidget(m_role);
    addRow->addWidget(m_add);
    auto *removeRow = new QHBoxLayout;
    removeRow->addWidget(m_status, 1);
    removeRow->addWidget(m_remove);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(i18nc("@info", "Who can see this calendar. Changes take effect in Microsoft 365 at once."), this));
    layout->addWidget(m_list, 1);
    layout->addLayout(removeRow);
    layout->addLayout(addRow);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_add, &QPushButton::clicked, this, &CalendarSharingDialog::add);
    connect(m_address, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_add->setEnabled(m_loaded && !text.trimmed().isEmpty());
    });
    connect(m_list, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        m_remove->setEnabled(item && item->data(0, Qt::UserRole + 1).toBool());
    });
    connect(m_remove, &QPushButton::clicked, this, [this] {
        const QTreeWidgetItem *item = m_list->currentItem();
        if (!item) {
            return;
        }
        if (KMessageBox::warningContinueCancel(this,
                                               i18nc("@info %1 a person, %2 a calendar", "Stop sharing %2 with %1?", item->text(0), m_calendar.name),
                                               i18nc("@title:window", "Stop Sharing"),
                                               KStandardGuiItem::remove())
            == KMessageBox::Continue) {
            remove(item->data(0, Qt::UserRole).toString(), item->text(0));
        }
    });
    load();
}

void CalendarSharingDialog::call(const QString &kind, const QJsonObject &args, const std::function<void(const QJsonObject &)> &done)
{
    QJsonObject withCalendar = args;
    withCalendar.insert(QStringLiteral("calendar"), m_calendar.graphId);
    withCalendar.insert(QStringLiteral("isDefault"), m_calendar.isDefault);
    m_daemon->query(m_calendar.mailboxKey.section(QLatin1Char('/'), 0, 0), m_calendar.mailboxKey.section(QLatin1Char('/'), 1), kind,
                    QString::fromUtf8(QJsonDocument(withCalendar).toJson(QJsonDocument::Compact)), this, [this, done](const QString &json) {
                        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                        if (answer.isEmpty()) {
                            m_status->setText(i18nc("@info", "Graph Mail's background service did not answer."));
                        } else if (!answer.value(QLatin1String("ok")).toBool()) {
                            m_status->setText(accessProblemText(answer.value(QLatin1String("problem")).toString(), m_calendar.name));
                        }
                        qWarning() << "sharing test:" << answer.value(QLatin1String("ok")).toBool() << answer.value(QLatin1String("detail")).toString();
                        done(answer);
                    });
}

void CalendarSharingDialog::load()
{
    m_status->setText(i18nc("@info:status", "Loading..."));
    call(QStringLiteral("calendarPermissions"), {}, [this](const QJsonObject &answer) {
        if (!answer.value(QLatin1String("ok")).toBool()) {
            return;
        }
        m_permissions = answer.value(QLatin1String("body")).toObject().value(QLatin1String("value")).toArray();
        m_status->clear();
        fill();
        m_loaded = true;
        m_add->setEnabled(!m_address->text().trimmed().isEmpty());
        const QList<std::function<void()>> waiting = m_waiting;
        m_waiting.clear();
        for (const auto &action : waiting) {
            action();
        }
    });
}

void CalendarSharingDialog::fill()
{
    m_list->clear();
    for (const QJsonValue &v : std::as_const(m_permissions)) {
        const QJsonObject p = v.toObject();
        const QString role = p.value(QLatin1String("role")).toString();
        const QString id = p.value(QLatin1String("id")).toString();
        auto *item = new QTreeWidgetItem(m_list, {who(p)});
        item->setData(0, Qt::UserRole, id);
        item->setData(0, Qt::UserRole + 1, p.value(QLatin1String("isRemovable")).toBool());
        item->setData(0, Qt::UserRole + 2, p.value(QLatin1String("emailAddress")).toObject().value(QLatin1String("address")).toString());
        auto *levels = new QComboBox(m_list);
        const QJsonArray allowed = p.value(QLatin1String("allowedRoles")).toArray();
        for (const QJsonValue &a : allowed) {
            levels->addItem(roleText(a.toString()), a.toString());
        }
        if (levels->findData(role) < 0) {
            levels->addItem(roleText(role), role);
        }
        levels->setCurrentIndex(levels->findData(role));
        levels->setEnabled(allowed.size() > 1);
        m_list->setItemWidget(item, 1, levels);
        connect(levels, &QComboBox::activated, this, [this, levels, id] {
            setRole(id, levels->currentData().toString());
        });
    }
}

void CalendarSharingDialog::add()
{
    const QList<OutgoingRecipient> people = m_address->recipients();
    if (people.isEmpty()) {
        m_status->setText(i18nc("@info", "Type the address of a person in your organisation."));
        return;
    }
    const OutgoingRecipient person = people.first();
    m_add->setEnabled(false);
    m_status->setText(i18nc("@info:status", "Sharing with %1...", person.address));
    call(QStringLiteral("calendarPermissionAdd"),
         {{QStringLiteral("address"), person.address},
          {QStringLiteral("name"), person.name.isEmpty() ? person.address : person.name},
          {QStringLiteral("role"), m_role->currentData().toString()}},
         [this, person](const QJsonObject &answer) {
             if (answer.value(QLatin1String("ok")).toBool()) {
                 m_address->clear();
                 m_status->setText(i18nc("@info", "Shared with %1.", person.address));
                 load();
             } else if (answer.value(QLatin1String("problem")).toString() == QLatin1String("notShareable")) {
                 m_status->setText(accessProblemText(QStringLiteral("notShareable"), person.address));
             }
             m_add->setEnabled(!m_address->text().trimmed().isEmpty());
         });
}

void CalendarSharingDialog::setRole(const QString &permission, const QString &role)
{
    call(QStringLiteral("calendarPermissionUpdate"), {{QStringLiteral("permission"), permission}, {QStringLiteral("role"), role}}, [this](const QJsonObject &answer) {
        if (answer.value(QLatin1String("ok")).toBool()) {
            m_status->setText(i18nc("@info", "Permission changed."));
        }
        // Back to what Microsoft 365 has, also after a refusal
        load();
    });
}

void CalendarSharingDialog::remove(const QString &permission, const QString &person)
{
    call(QStringLiteral("calendarPermissionRemove"), {{QStringLiteral("permission"), permission}}, [this, person](const QJsonObject &answer) {
        if (answer.value(QLatin1String("ok")).toBool()) {
            m_status->setText(i18nc("@info %1 a person", "No longer shared with %1.", person));
            load();
        }
    });
}

void CalendarSharingDialog::whenLoaded(const std::function<void()> &action)
{
    if (m_loaded) {
        action();
    } else {
        m_waiting.append(action);
    }
}

QString CalendarSharingDialog::permissionOf(const QString &address) const
{
    for (const QJsonValue &v : m_permissions) {
        const QJsonObject p = v.toObject();
        if (p.value(QLatin1String("emailAddress")).toObject().value(QLatin1String("address")).toString().compare(address, Qt::CaseInsensitive) == 0) {
            return p.value(QLatin1String("id")).toString();
        }
    }
    return {};
}

void CalendarSharingDialog::addForTests(const QString &address, const QString &role)
{
    whenLoaded([this, address, role] {
        m_address->setText(address);
        m_role->setCurrentIndex(std::max(0, m_role->findData(role)));
        add();
    });
}

void CalendarSharingDialog::setRoleForTests(const QString &address, const QString &role)
{
    whenLoaded([this, address, role] {
        const QString id = permissionOf(address);
        if (id.isEmpty()) {
            qWarning() << "sharing test: no permission for" << address;
            return;
        }
        setRole(id, role);
    });
}

void CalendarSharingDialog::removeForTests(const QString &address)
{
    whenLoaded([this, address] {
        const QString id = permissionOf(address);
        if (id.isEmpty()) {
            qWarning() << "sharing test: no permission for" << address;
            return;
        }
        remove(id, address);
    });
}

}
