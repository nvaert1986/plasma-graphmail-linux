// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sweepdialog.h"
#include "movedialog.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace GraphMail
{

SweepDialog::SweepDialog(FolderModel *folders, const QString &mailboxKey, const QString &sender, const QString &folderName, int count, bool future,
                         QWidget *parent)
    : QDialog(parent)
    , m_folders(folders)
    , m_mailboxKey(mailboxKey)
    , m_all(new QRadioButton(i18ncp("@option:radio sweep", "The message from this sender in %2 (%1)", "All %1 messages from this sender in %2", count, folderName), this))
    , m_future(new QRadioButton(i18nc("@option:radio sweep", "All of them, and every future message from this sender"), this))
    , m_keepNewest(new QRadioButton(i18nc("@option:radio sweep", "All of them except the newest"), this))
    , m_action(new QComboBox(this))
{
    setWindowTitle(i18nc("@title:window", "Sweep"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(i18nc("@info", "Clean up the messages from <b>%1</b>:", sender.toHtmlEscaped()), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    layout->addWidget(m_all);
    layout->addWidget(m_future);
    layout->addWidget(m_keepNewest);
    m_all->setChecked(true);
    // Future messages need an Inbox rule, which only the own mailbox can have
    m_future->setEnabled(future);
    if (!future) {
        m_future->setToolTip(i18nc("@info:tooltip", "Rules can only be made in your own mailbox."));
    }
    m_keepNewest->setEnabled(count > 1);
    auto *form = new QFormLayout;
    m_action->addItem(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@item:inlistbox sweep", "Delete"), 0);
    m_action->addItem(QIcon::fromTheme(QStringLiteral("go-jump")), i18nc("@item:inlistbox sweep", "Move to a folder..."), 1);
    form->addRow(i18nc("@label:listbox", "Do:"), m_action);
    layout->addLayout(form);
    connect(m_action, &QComboBox::activated, this, [this](int index) {
        if (index != 1) {
            m_target = 0;
            return;
        }
        MoveDialog dialog(m_folders, m_mailboxKey, this);
        if (dialog.exec() == QDialog::Accepted && dialog.folder() != 0) {
            m_target = dialog.folder();
            m_action->setItemText(1, i18nc("@item:inlistbox sweep", "Move to a folder (chosen)"));
        } else {
            m_target = 0;
            m_action->setCurrentIndex(0);
        }
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Sweep"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

SweepDialog::Scope SweepDialog::scope() const
{
    if (m_future->isChecked()) {
        return Scope::AllAndFuture;
    }
    return m_keepNewest->isChecked() ? Scope::KeepNewest : Scope::All;
}

qint64 SweepDialog::target() const
{
    return m_target;
}

QJsonObject SweepDialog::rule(const QString &address, const QString &folderGraphId, int sequence)
{
    const QJsonObject from{{QStringLiteral("emailAddress"), QJsonObject{{QStringLiteral("address"), address}}}};
    QJsonObject actions{{QStringLiteral("stopProcessingRules"), true}};
    if (folderGraphId.isEmpty()) {
        actions.insert(QStringLiteral("delete"), true);
    } else {
        actions.insert(QStringLiteral("moveToFolder"), folderGraphId);
    }
    return {{QStringLiteral("displayName"), i18nc("@info rule name, %1 an e-mail address", "Sweep: %1", address)},
            {QStringLiteral("sequence"), sequence},
            {QStringLiteral("isEnabled"), true},
            {QStringLiteral("conditions"), QJsonObject{{QStringLiteral("fromAddresses"), QJsonArray{from}}}},
            {QStringLiteral("actions"), actions}};
}

}
