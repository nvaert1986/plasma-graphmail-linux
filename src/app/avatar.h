// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QRect>
#include <QString>

class QImage;
class QPainter;
class QPixmap;

// The sender's circle with initials, as in Outlook: the same colour for the same address
namespace GraphMail::Avatar
{
// "Example Support" -> "ES", "Helpdesk | Example Ltd" -> "HE", "it@example.com" -> "I"
QString initials(const QString &name, const QString &address);
QColor color(const QString &address);
void paint(QPainter *painter, const QRect &rect, const QString &name, const QString &address);
// A round picture: the photo when there is one, else the initials (docs/PHASE9.md)
QPixmap pixmap(const QImage &photo, int size, const QString &name, const QString &address);
}
