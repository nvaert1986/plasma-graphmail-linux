// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "avatar.h"
#include <QPixmap>
#include <QPainterPath>
#include <QImage>
#include <QApplication>

#include <QPainter>
#include <QRegularExpression>

namespace GraphMail::Avatar
{

QString initials(const QString &name, const QString &address)
{
    static const QRegularExpression separators(QStringLiteral("[\\s|,;()<>\\[\\]\"'/\\\\._-]+"));
    QStringList words = name.split(separators, Qt::SkipEmptyParts);
    words.removeIf([](const QString &w) {
        return !w.front().isLetterOrNumber();
    });
    if (words.isEmpty()) {
        const QString local = address.section(QLatin1Char('@'), 0, 0);
        return local.isEmpty() ? QStringLiteral("?") : local.left(1).toUpper();
    }
    if (words.size() == 1) {
        return words.constFirst().left(1).toUpper();
    }
    return (words.constFirst().left(1) + words.constLast().left(1)).toUpper();
}

QColor color(const QString &address)
{
    // Soft, readable behind white text in light and dark themes
    static const QColor colors[] = {
        QColor(0xb8, 0x5a, 0x7a), QColor(0x5a, 0x7a, 0xb8), QColor(0x4a, 0x93, 0x7f), QColor(0xa8, 0x6f, 0x3e), QColor(0x7a, 0x5a, 0xb8),
        QColor(0x3e, 0x8e, 0xa8), QColor(0x8e, 0x8a, 0x3a), QColor(0xb8, 0x4a, 0x4a), QColor(0x4a, 0x7d, 0x4a), QColor(0x6b, 0x6b, 0x8e),
    };
    return colors[qHash(address.toLower()) % std::size(colors)];
}

void paint(QPainter *painter, const QRect &rect, const QString &name, const QString &address)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(color(address.isEmpty() ? name : address));
    painter->drawEllipse(rect);
    QFont font = painter->font();
    font.setBold(true);
    font.setPixelSize(std::max(8, rect.height() * 2 / 5));
    painter->setFont(font);
    painter->setPen(Qt::white);
    painter->drawText(rect, Qt::AlignCenter, initials(name, address));
    painter->restore();
}

QPixmap pixmap(const QImage &photo, int size, const QString &name, const QString &address)
{
    const qreal ratio = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap out(QSize(size, size) * ratio);
    out.setDevicePixelRatio(ratio);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRect rect(0, 0, size, size);
    if (photo.isNull()) {
        paint(&p, rect, name, address);
        return out;
    }
    QPainterPath circle;
    circle.addEllipse(QRectF(rect));
    p.setClipPath(circle);
    // The middle square of the photo
    const int side = std::min(photo.width(), photo.height());
    const QImage square = photo.copy((photo.width() - side) / 2, (photo.height() - side) / 2, side, side);
    p.drawImage(QRectF(rect), square);
    return out;
}

}
