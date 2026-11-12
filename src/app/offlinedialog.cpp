// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "offlinedialog.h"
#include "daemonclient.h"
#include "mailboxset.h"
#include "mailboxworker.h"
#include "store.h"

#include <KConfigGroup>
#include <KFormat>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
KConfigGroup general()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("General"));
}
}

qint64 OfflineDialog::diskUse(const QString &account, const QString &mailbox)
{
    qint64 total = 0;
    QDirIterator it(cacheDirectory(account, mailbox), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        total += it.nextFileInfo().size();
    }
    return total;
}

OfflineDialog::OfflineDialog(MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent)
    : QDialog(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
    , m_window(new QComboBox(this))
    , m_attachments(new QCheckBox(i18nc("@option:check", "Also keep attachments offline"), this))
    , m_note(new QLabel(this))
    , m_space(new QTreeWidget(this))
{
    setWindowTitle(i18nc("@title:window", "Offline"));
    resize(620, 460);
    m_window->addItem(i18nc("@item:inlistbox offline window", "Mail of the last 30 days"), 30);
    m_window->addItem(i18nc("@item:inlistbox offline window", "Mail of the last 90 days"), 90);
    m_window->addItem(i18nc("@item:inlistbox offline window", "Mail of the last year"), 365);
    m_window->addItem(i18nc("@item:inlistbox offline window", "All mail (full offline copy)"), -1);
    m_window->addItem(i18nc("@item:inlistbox offline window", "None (download a message when it is opened)"), 0);
    const KConfigGroup group = general();
    const int days = group.readEntry("BodyWindowDays", 90);
    int index = m_window->findData(days);
    if (index < 0) {
        // A value set by hand: shown as it is
        m_window->addItem(i18ncp("@item:inlistbox offline window", "Mail of the last %1 day", "Mail of the last %1 days", days), days);
        index = m_window->count() - 1;
    }
    m_window->setCurrentIndex(index);
    m_attachments->setChecked(group.readEntry("AttachmentsOffline", false));
    m_note->setWordWrap(true);
    auto updateNote = [this] {
        const int value = m_window->currentData().toInt();
        m_note->setText(value < 0 ? i18nc("@info", "Every message is downloaded once; for a large mailbox that takes hours and several gigabytes. Search then finds the text of all mail without Microsoft 365.")
                        : value == 0 ? i18nc("@info", "Only the list of messages is kept; a message is downloaded when you open it and cannot be read offline.")
                                     : i18nc("@info", "Newer mail can be read and searched offline; older mail is downloaded when opened, and Microsoft 365 searches its text."));
    };
    connect(m_window, &QComboBox::currentIndexChanged, this, updateNote);
    updateNote();

    m_space->setHeaderLabels({i18nc("@title:column", "Mailbox"), i18nc("@title:column", "Space on this computer")});
    m_space->setRootIsDecorated(false);
    m_space->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_space->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_space->header()->setStretchLastSection(false);

    auto *free = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-clear")), i18nc("@action:button", "Free Up Space"), this);
    free->setToolTip(i18nc("@info:tooltip", "Removes the copies of mail outside the period chosen now and shrinks the files"));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:listbox", "Keep on this computer:"), m_window);
    form->addRow(QString(), m_attachments);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_note);
    layout->addWidget(m_space, 1);
    auto *freeRow = new QHBoxLayout;
    freeRow->addStretch();
    freeRow->addWidget(free);
    layout->addLayout(freeRow);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        save();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(free, &QPushButton::clicked, this, [this, free] {
        save();
        m_daemon->applyOfflineSettings(true);
        free->setEnabled(false);
        // The daemon works in the background: the sizes again a little later
        QTimer::singleShot(4000, this, [this, free] {
            fillSpace();
            free->setEnabled(true);
        });
    });
    fillSpace();
}

void OfflineDialog::fillSpace()
{
    m_space->clear();
    qint64 total = 0;
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        const qint64 bytes = diskUse(box.account, box.mailbox);
        total += bytes;
        new QTreeWidgetItem(m_space, {box.own ? box.address : box.label(), KFormat().formatByteSize(bytes)});
    }
    auto *sum = new QTreeWidgetItem(m_space, {i18nc("@item total of all mailboxes", "Total"), KFormat().formatByteSize(total)});
    QFont bold = sum->font(0);
    bold.setBold(true);
    sum->setFont(0, bold);
    sum->setFont(1, bold);
}

void OfflineDialog::save()
{
    KConfigGroup group = general();
    group.writeEntry("BodyWindowDays", m_window->currentData().toInt());
    group.writeEntry("AttachmentsOffline", m_attachments->isChecked());
    group.sync();
    m_daemon->applyOfflineSettings(false);
}

void OfflineDialog::applyForTests(int days, bool attachments, bool freeSpace)
{
    int index = m_window->findData(days);
    if (index < 0) {
        m_window->addItem(QString::number(days), days);
        index = m_window->count() - 1;
    }
    m_window->setCurrentIndex(index);
    m_attachments->setChecked(attachments);
    save();
    if (freeSpace) {
        m_daemon->applyOfflineSettings(true);
    }
    qWarning() << "offline test: saved" << days << attachments << freeSpace;
}

}
