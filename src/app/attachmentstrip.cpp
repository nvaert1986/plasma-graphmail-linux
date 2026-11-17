// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "attachmentstrip.h"
#include "graphsynclink.h"

#include <KLocalizedString>

#include <QContextMenuEvent>
#include <QLocale>
#include <QMenu>
#include <QMimeDatabase>

namespace GraphMail
{

namespace
{
constexpr int idRole = Qt::UserRole + 1;
constexpr int maxVisibleRows = 2;
}

AttachmentStrip::AttachmentStrip(QWidget *parent)
    : QListWidget(parent)
{
    setViewMode(QListView::ListMode);
    setFlow(QListView::LeftToRight);
    setWrapping(true);
    setResizeMode(QListView::Adjust);
    setUniformItemSizes(true);
    setSpacing(4);
    setIconSize(QSize(32, 32));
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSelectionMode(QAbstractItemView::SingleSelection);
    // Activation follows the Plasma setting (single or double click), like Dolphin
    connect(this, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (const qint64 id = item->data(idRole).toLongLong(); id != 0) {
            Q_EMIT openRequested(id);
        }
    });
}

QString AttachmentStrip::formatSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QIcon AttachmentStrip::iconFor(const QString &name, const QString &contentType)
{
    QMimeDatabase db;
    QMimeType type = db.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
    if (type.isDefault() && !contentType.isEmpty()) {
        type = db.mimeTypeForName(contentType);
    }
    return QIcon::fromTheme(type.iconName(), QIcon::fromTheme(type.genericIconName(), QIcon::fromTheme(QStringLiteral("unknown"))));
}

bool AttachmentStrip::setAttachments(const QList<AttachmentRow> &attachments, bool listPending)
{
    clear();
    for (const AttachmentRow &a : attachments) {
        if (a.isInline) {
            continue;
        }
        const QString name = a.name.isEmpty() ? i18nc("@item an attachment without a name", "(no name)") : a.name;
        auto *item = new QListWidgetItem(iconFor(name, a.contentType), name + QLatin1Char('\n') + formatSize(a.size), this);
        item->setData(idRole, a.id);
        item->setToolTip(i18nc("@info:tooltip attachment name, size", "%1 (%2)\nOpen it, or right-click to save", name, formatSize(a.size)));
    }
    if (count() == 0 && listPending) {
        auto *item = new QListWidgetItem(QIcon::fromTheme(QStringLiteral("mail-attachment")), i18nc("@info", "Loading attachments..."), this);
        item->setFlags(Qt::NoItemFlags);
    }
    fitHeight();
    return count() > 0;
}

void AttachmentStrip::fitHeight()
{
    if (count() == 0) {
        setFixedHeight(0);
        return;
    }
    // Room for one row, or two when there are many; more scroll
    const int row = sizeHintForRow(0) + spacing() * 2;
    const int perRow = std::max(1, (viewport()->width() > 0 ? viewport()->width() : 600) / std::max(1, sizeHintForColumn(0) + spacing() * 2));
    const int rows = std::min(maxVisibleRows, int((count() + perRow - 1) / perRow));
    setFixedHeight(row * rows + frameWidth() * 2 + 4);
}

void AttachmentStrip::contextMenuEvent(QContextMenuEvent *event)
{
    QListWidgetItem *item = itemAt(event->pos());
    const qint64 id = item ? item->data(idRole).toLongLong() : 0;
    QMenu menu(this);
    if (id != 0) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("document-preview")), i18nc("@action:inmenu", "Preview"), this, [this, id] {
            Q_EMIT previewRequested(id);
        });
        menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), i18nc("@action:inmenu", "Open"), this, [this, id] {
            Q_EMIT openRequested(id);
        });
        menu.addAction(QIcon::fromTheme(QStringLiteral("document-save-as")), i18nc("@action:inmenu", "Save As..."), this, [this, id] {
            Q_EMIT saveRequested(id);
        });
        // Graph Sync (the suite's OneDrive/SharePoint client) syncs what lands in its folders
        if (!GraphSyncLink::locations().isEmpty()) {
            menu.addAction(QIcon::fromTheme(QStringLiteral("folder-cloud")), i18nc("@action:inmenu", "Save to OneDrive or SharePoint..."), this, [this, id] {
                Q_EMIT saveToCloudRequested(id);
            });
        }
    }
    menu.addAction(QIcon::fromTheme(QStringLiteral("document-save-all")), i18nc("@action:inmenu", "Save All Attachments..."), this, [this] {
        Q_EMIT saveAllRequested();
    });
    menu.exec(event->globalPos());
}

}
