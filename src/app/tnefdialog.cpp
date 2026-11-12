// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tnefdialog.h"
#include "attachmentstrip.h"

#include <KIO/JobUiDelegateFactory>
#include <KIO/OpenUrlJob>
#include <KLocalizedString>
#include <KTNEF/KTNEFAttach>
#include <KTNEF/KTNEFMessage>
#include <KTNEF/KTNEFParser>

#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
constexpr int nameRole = Qt::UserRole + 1;
}

TnefDialog *TnefDialog::open(const QString &path, const QString &extractDir, QWidget *parent)
{
    auto parser = std::make_unique<KTnef::KTNEFParser>();
    if (!parser->openFile(path) || !parser->message() || parser->message()->attachmentList().isEmpty()) {
        return nullptr;
    }
    return new TnefDialog(std::move(parser), extractDir, parent);
}

TnefDialog::TnefDialog(std::unique_ptr<KTnef::KTNEFParser> parser, const QString &extractDir, QWidget *parent)
    : QDialog(parent)
    , m_parser(std::move(parser))
    , m_extractDir(extractDir)
    , m_list(new QListWidget(this))
{
    setWindowTitle(i18nc("@title:window", "Files in winmail.dat"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(i18nc("@info", "This attachment is a container from Microsoft Outlook. It holds these files:"), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    m_list->setIconSize(QSize(32, 32));
    const QList<KTnef::KTNEFAttach *> &attachments = m_parser->message()->attachmentList();
    for (KTnef::KTNEFAttach *attachment : attachments) {
        const QString name = attachment->name().isEmpty() ? attachment->displayName() : attachment->name();
        auto *item = new QListWidgetItem(AttachmentStrip::iconFor(name, attachment->mimeTag()),
                                         QStringLiteral("%1 (%2)").arg(name, AttachmentStrip::formatSize(attachment->size())),
                                         m_list);
        item->setData(nameRole, attachment->name());
    }
    layout->addWidget(m_list);
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        openFile(item->data(nameRole).toString());
    });
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *item = m_list->itemAt(pos);
        if (!item) {
            return;
        }
        const QString name = item->data(nameRole).toString();
        QMenu menu(this);
        menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), i18nc("@action:inmenu", "Open"), this, [this, name] {
            openFile(name);
        });
        menu.addAction(QIcon::fromTheme(QStringLiteral("document-save-as")), i18nc("@action:inmenu", "Save As..."), this, [this, name] {
            saveFile(name);
        });
        menu.exec(m_list->viewport()->mapToGlobal(pos));
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton *saveAllButton = buttons->addButton(i18nc("@action:button", "Save All..."), QDialogButtonBox::ActionRole);
    saveAllButton->setIcon(QIcon::fromTheme(QStringLiteral("document-save-all")));
    connect(saveAllButton, &QPushButton::clicked, this, &TnefDialog::saveAll);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    resize(460, 340);
}

TnefDialog::~TnefDialog() = default;

void TnefDialog::openFile(const QString &name)
{
    QDir().mkpath(m_extractDir);
    if (!m_parser->extractFileTo(name, m_extractDir)) {
        return;
    }
    auto *job = new KIO::OpenUrlJob(QUrl::fromLocalFile(QDir(m_extractDir).filePath(name)));
    job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, this));
    job->start();
}

void TnefDialog::saveFile(const QString &name)
{
    const QString target = QFileDialog::getSaveFileName(this,
                                                        i18nc("@title:window", "Save File"),
                                                        QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).filePath(name));
    if (target.isEmpty() || !m_parser->extractFileTo(name, m_extractDir)) {
        return;
    }
    QFile::remove(target);
    QFile::copy(QDir(m_extractDir).filePath(name), target);
}

void TnefDialog::saveAll()
{
    const QString dir = QFileDialog::getExistingDirectory(this, i18nc("@title:window", "Save All Files"),
                                                          QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    if (dir.isEmpty()) {
        return;
    }
    for (int row = 0; row < m_list->count(); ++row) {
        const QString name = m_list->item(row)->data(nameRole).toString();
        if (!QFileInfo::exists(QDir(dir).filePath(name))) {
            (void)m_parser->extractFileTo(name, dir);
        }
    }
}

}
