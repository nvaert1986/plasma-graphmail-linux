// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "messagedrag.h"
#include "messagelistmodel.h"

#include <QApplication>
#include <QDrag>
#include <QIcon>
#include <QPainter>

namespace GraphMail
{

namespace
{
MessageDragData::Writer &writer()
{
    static MessageDragData::Writer w;
    return w;
}

QString uriList()
{
    return QStringLiteral("text/uri-list");
}
}

void MessageDragData::setWriter(Writer w)
{
    writer() = std::move(w);
}

MessageDragData::MessageDragData(const QString &mailboxKey, const QList<qint64> &messages)
    : m_mailboxKey(mailboxKey)
    , m_messages(messages)
{
    QStringList lines{mailboxKey};
    for (qint64 id : messages) {
        lines.append(QString::number(id));
    }
    setData(MessageListModel::mimeType(), lines.join(QLatin1Char('\n')).toLatin1());
}

QStringList MessageDragData::formats() const
{
    QStringList list = QMimeData::formats();
    if (writer() && !m_messages.isEmpty() && !list.contains(uriList())) {
        list.append(uriList());
    }
    return list;
}

bool MessageDragData::hasFormat(const QString &mimeType) const
{
    return (mimeType == uriList() && writer() && !m_messages.isEmpty()) || QMimeData::hasFormat(mimeType);
}

QVariant MessageDragData::retrieveData(const QString &mimeType, QMetaType type) const
{
    if (mimeType != uriList() || !writer()) {
        return QMimeData::retrieveData(mimeType, type);
    }
    if (!m_written) {
        m_written = true;
        m_urls = writer()(m_mailboxKey, m_messages);
    }
    QVariantList list;
    for (const QUrl &url : std::as_const(m_urls)) {
        list.append(url);
    }
    return list;
}

QPixmap MessageListView::dragPixmap(int count, qreal ratio)
{
    const int size = 32;
    const int badge = count > 1 ? 18 : 0;
    QPixmap pixmap(QSize(size + badge / 2, size + badge / 2) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QIcon::fromTheme(QStringLiteral("mail-message")).paint(&painter, QRect(0, badge / 2, size, size));
    if (count > 1) {
        // The number on a round patch in the accent colour, top right
        const QRect circle(size + badge / 2 - badge, 0, badge, badge);
        const QPalette palette = QApplication::palette();
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette.color(QPalette::Highlight));
        painter.drawEllipse(circle);
        QFont font = QApplication::font();
        font.setBold(true);
        font.setPixelSize(count > 99 ? 8 : 11);
        painter.setFont(font);
        painter.setPen(palette.color(QPalette::HighlightedText));
        painter.drawText(circle, Qt::AlignCenter, count > 999 ? QStringLiteral("999+") : QString::number(count));
    }
    return pixmap;
}

void MessageListView::startDrag(Qt::DropActions supportedActions)
{
    const QModelIndexList rows = selectionModel()->selectedRows();
    if (rows.isEmpty() || !model()) {
        return;
    }
    QMimeData *data = model()->mimeData(rows);
    if (!data) {
        return;
    }
    const auto *messages = qobject_cast<MessageDragData *>(data);
    auto *drag = new QDrag(this);
    drag->setMimeData(data);
    drag->setPixmap(dragPixmap(messages ? int(messages->count()) : int(rows.size()), devicePixelRatioF()));
    // The cursor at the envelope's top left corner, the folder under it stays visible
    drag->setHotSpot(QPoint(-6, -6));
    // A move by default, as in Outlook (Qt's models only offer a copy, which shows a "+"); a file
    // manager may still copy. The daemon moves; the list follows the cache, nothing is removed here
    Q_UNUSED(supportedActions)
    drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::MoveAction);
}

}
