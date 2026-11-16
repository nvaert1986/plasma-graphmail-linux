// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "conditionalformat.h"

#include <QStyledItemDelegate>

namespace GraphMail
{

// One message in the list, as in Outlook: initials circle, sender (bold while unread, with a bar
// in the accent colour on the left), subject with the date on the right, one grey preview line,
// and icons for attachments, flags and high importance.
class CategoryList;

class MessageDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;
    // Category labels in their colours (docs/PHASE9.md)
    void setCategories(CategoryList *categories);
    // Conditional formatting (docs/PHASE9B.md 9b.27): read again after the rules changed
    void reloadFormatting();
    // A category label (also the reading pane's): the name on a rounded patch of its colour;
    // returns its width
    static int drawCategory(QPainter *painter, const QRect &line, int right, const QString &name, const QColor &color, const QFontMetrics &fm);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    // Time today, weekday and time this week, the date otherwise (in the user's locale)
    static QString shortDate(qint64 msecs);

private:
    CategoryList *m_categories = nullptr;
    QList<ConditionalFormat::Rule> m_formatting;
};

// A folder: name and, when there is unread mail, the count after it in bold accent colour
class FolderDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

}
