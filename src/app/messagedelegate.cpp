// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "messagedelegate.h"
#include "categorylist.h"
#include "avatar.h"
#include "foldermodel.h"
#include "messagelistmodel.h"

#include <KLocalizedString>

#include <QApplication>
#include <QDateTime>
#include <QIcon>
#include <QLocale>
#include <QPainter>
#include <QRegularExpression>

namespace GraphMail
{

namespace
{
constexpr int padding = 8;
constexpr int lineGap = 2;
constexpr int iconSize = 16;
constexpr int unreadBarWidth = 4;

// a moved a share of the way towards b
QColor blend(const QColor &a, const QColor &b, qreal share)
{
    return QColor(int(a.red() * (1 - share) + b.red() * share), int(a.green() * (1 - share) + b.green() * share), int(a.blue() * (1 - share) + b.blue() * share));
}

int avatarSize(const QFontMetrics &fm)
{
    return std::max(28, fm.height() * 2 + lineGap);
}

// The locale's short date without the year: "dd-MM-yyyy" -> "dd-MM", "M/d/yy" -> "M/d"
QString dayMonth(const QLocale &locale, const QDate &date)
{
    // The C locale ("C.UTF8", also a Plasma Formats choice) has no useful short date: use
    // Outlook's day-month
    if (locale.language() == QLocale::C) {
        return date.toString(QStringLiteral("dd-MM"));
    }
    static const QRegularExpression yearAtEnd(QStringLiteral("[^dM]*y+[^dM]*$"));
    static const QRegularExpression yearAtStart(QStringLiteral("^y+[^dM]*"));
    QString format = locale.dateFormat(QLocale::ShortFormat);
    format.remove(yearAtEnd);
    format.remove(yearAtStart);
    return locale.toString(date, format);
}
}

void MessageDelegate::setCategories(CategoryList *categories)
{
    m_categories = categories;
}

void MessageDelegate::reloadFormatting()
{
    m_formatting = ConditionalFormat::rules();
}

int MessageDelegate::drawCategory(QPainter *painter, const QRect &line, int right, const QString &name, const QColor &color, const QFontMetrics &fm)
{
    const QString text = fm.elidedText(name, Qt::ElideRight, 110);
    const int width = fm.horizontalAdvance(text) + 10;
    const QRect patch(right - width, line.top() + 1, width, line.height() - 2);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawRoundedRect(patch, 4, 4);
    // Text that stays readable on light and dark colours
    painter->setPen(color.lightnessF() > 0.6 ? Qt::black : Qt::white);
    painter->drawText(patch, Qt::AlignCenter, text);
    painter->restore();
    return width;
}

QString MessageDelegate::shortDate(qint64 msecs)
{
    const QDateTime time = QDateTime::fromMSecsSinceEpoch(msecs);
    const QDate today = QDate::currentDate();
    const QLocale locale;
    const bool plain = locale.language() == QLocale::C;
    const QString clock = plain ? time.time().toString(QStringLiteral("HH:mm")) : locale.toString(time.time(), QLocale::ShortFormat);
    if (time.date() == today) {
        return clock;
    }
    if (time.date() > today.addDays(-7)) {
        return locale.toString(time, QStringLiteral("ddd ")) + clock;
    }
    if (time.date().year() == today.year()) {
        // "Sun 25-10", as in Outlook: weekday and day-month in the user's date order
        return locale.toString(time, QStringLiteral("ddd ")) + dayMonth(locale, time.date());
    }
    return plain ? time.date().toString(QStringLiteral("dd-MM-yyyy")) : locale.toString(time.date(), QLocale::ShortFormat);
}

QSize MessageDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const
{
    const QFontMetrics fm(option.font);
    // The width comes from the view (rows span it)
    return {0, padding * 2 + fm.height() * 3 + lineGap * 2};
}

void MessageDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QWidget *widget = opt.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    // A flagged message has a light yellow row, as in Outlook (also when read). Mixed into the list's
    // own background, so a dark colour scheme gets a dark yellow and the text stays readable; drawn
    // first, so the selection and hover the style draws next still show (0.14.2)
    if (index.data(MessageListModel::FlagRole).toInt() == 1 && !(opt.state & QStyle::State_Selected)) {
        const QColor base = opt.palette.color(QPalette::Base);
        const QColor yellow(255, 214, 0);
        const qreal share = base.lightness() < 128 ? 0.22 : 0.2;
        const QColor row(int(base.red() * (1 - share) + yellow.red() * share), int(base.green() * (1 - share) + yellow.green() * share),
                         int(base.blue() * (1 - share) + yellow.blue() * share));
        painter->fillRect(opt.rect, row);
    }
    // The selection and hover background the style draws for every item view
    opt.text.clear();
    opt.icon = QIcon();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    const bool selected = opt.state & QStyle::State_Selected;
    const bool unread = !index.data(MessageListModel::IsReadRole).toBool();
    const QPalette &palette = opt.palette;
    const QColor text = palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
    QColor dim = palette.color(selected ? QPalette::HighlightedText : QPalette::PlaceholderText);
    QColor accent = selected ? text : palette.color(QPalette::Highlight);
    // Read and unread apart at a glance, as Outlook does, also in a dark colour scheme (where bold
    // shows less and the accent is about as bright as the text, 0.14.2): read mail a softer colour,
    // unread mail's subject a lighter accent on a dark background
    const bool dark = palette.color(QPalette::Base).lightness() < 128;
    const QColor readText = (selected || unread) ? text : blend(text, palette.color(QPalette::Base), dark ? 0.32 : 0.18);
    if (!selected && dark) {
        accent = blend(accent, QColor(Qt::white), 0.35);
    }

    painter->save();
    const QRect r = opt.rect;
    if (unread) {
        painter->fillRect(QRect(r.left(), r.top() + 2, unreadBarWidth, r.height() - 4), palette.color(QPalette::Highlight));
    }
    const QFontMetrics fm(opt.font);
    const int avatar = avatarSize(fm);
    QString name = index.data(MessageListModel::SenderNameRole).toString();
    QString address = index.data(MessageListModel::SenderAddressRole).toString();
    // Sent Items, Drafts, Outbox: who it went to ("Name <address>; ..." from the cache)
    QString recipients;
    if (index.data(MessageListModel::ShowRecipientsRole).toBool()) {
        const QString toList = index.data(MessageListModel::ToListRole).toString();
        QStringList names;
        const QStringList parts = toList.split(QStringLiteral("; "), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            names.append(part.section(QStringLiteral(" <"), 0, 0));
        }
        if (!parts.isEmpty()) {
            const QString first = parts.constFirst();
            name = first.section(QStringLiteral(" <"), 0, 0);
            address = first.contains(QLatin1Char('<')) ? first.section(QLatin1Char('<'), 1).chopped(1) : first;
        }
        recipients = names.isEmpty() ? i18nc("@item a draft without recipients", "(no recipients)")
                                     : i18nc("@item message list in Sent Items: the recipients", "To: %1", names.join(QStringLiteral("; ")));
    }
    Avatar::paint(painter, QRect(r.left() + padding + unreadBarWidth, r.top() + padding, avatar, avatar), name, address);

    const int left = r.left() + padding * 2 + unreadBarWidth + avatar;
    const int right = r.right() - padding;
    int y = r.top() + padding;

    // Line 1: sender, icons on the right
    int iconsLeft = right;
    auto drawIcon = [&](const QString &iconName) {
        iconsLeft -= iconSize;
        QIcon::fromTheme(iconName).paint(painter, QRect(iconsLeft, y + (fm.height() - iconSize) / 2, iconSize, iconSize));
        iconsLeft -= 4;
    };
    const int flag = index.data(MessageListModel::FlagRole).toInt();
    if (flag == 1) {
        drawIcon(QStringLiteral("flag-red"));
    } else if (flag == 2) {
        drawIcon(QStringLiteral("checkmark"));
    }
    if (index.data(MessageListModel::HasAttachmentsRole).toBool()) {
        drawIcon(QStringLiteral("mail-attachment"));
    }
    if (index.data(MessageListModel::ImportanceRole).toInt() == 2) {
        drawIcon(QStringLiteral("mail-mark-important"));
    }
    // A conditional formatting rule colours the sender and subject (not while selected)
    const std::optional<QColor> ruleColor = selected ? std::nullopt
                                                     : ConditionalFormat::colorFor(m_formatting,
                                                                                   index.data(MessageListModel::SenderNameRole).toString(),
                                                                                   index.data(MessageListModel::SenderAddressRole).toString(),
                                                                                   index.data(MessageListModel::SubjectRole).toString());
    if (ruleColor) {
        accent = *ruleColor;
    }
    QFont senderFont = opt.font;
    senderFont.setBold(unread);
    painter->setFont(senderFont);
    painter->setPen(ruleColor.value_or(readText));
    const QFontMetrics sfm(senderFont);
    QString sender = !recipients.isEmpty() ? recipients : (name.isEmpty() ? address : name);
    // Conversation view: how many messages, as in Outlook
    if (const int count = index.data(MessageListModel::ConversationCountRole).toInt(); count > 1) {
        sender += QStringLiteral(" (%1)").arg(count);
    }
    painter->drawText(QRect(left, y, iconsLeft - left - padding, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                      sfm.elidedText(sender, Qt::ElideRight, iconsLeft - left - padding));
    y += fm.height() + lineGap;

    // Line 2: subject (accent colour while unread, as in Outlook), date on the right
    const QString date = shortDate(index.data(MessageListModel::ReceivedRole).toLongLong());
    QFont subjectFont = opt.font;
    subjectFont.setBold(unread);
    const QFontMetrics subfm(subjectFont);
    painter->setFont(opt.font);
    painter->setPen(unread || ruleColor ? accent : readText);
    const int dateWidth = fm.horizontalAdvance(date);
    painter->drawText(QRect(right - dateWidth, y, dateWidth, fm.height()), Qt::AlignRight | Qt::AlignVCenter, date);
    painter->setFont(subjectFont);
    QString subject = index.data(MessageListModel::SubjectRole).toString();
    if (subject.isEmpty()) {
        subject = i18nc("@item a message without a subject", "(no subject)");
    }
    const int subjectWidth = right - dateWidth - padding - left;
    painter->drawText(QRect(left, y, subjectWidth, fm.height()), Qt::AlignLeft | Qt::AlignVCenter, subfm.elidedText(subject, Qt::ElideRight, subjectWidth));
    y += fm.height() + lineGap;

    // Line 3: the preview, one line, grey
    painter->setFont(opt.font);
    painter->setPen(dim);
    const QString preview = index.data(MessageListModel::PreviewRole).toString().simplified();
    // Search results: the folder on the right of the preview line
    int previewRight = right;
    // Categories as labels in their colours, at the right end of the preview line (two, then "+n")
    if (const QStringList categories = index.data(MessageListModel::CategoriesRole).toStringList(); !categories.isEmpty() && m_categories) {
        const QString key = index.data(MessageListModel::MailboxKeyRole).toString();
        int x = previewRight;
        const qsizetype shown = std::min<qsizetype>(categories.size(), 2);
        if (categories.size() > shown) {
            const QString more = QStringLiteral("+%1").arg(categories.size() - shown);
            const int width = fm.horizontalAdvance(more);
            painter->drawText(QRect(x - width, y, width, fm.height()), Qt::AlignRight | Qt::AlignVCenter, more);
            x -= width + 4;
        }
        for (qsizetype i = shown - 1; i >= 0; --i) {
            x -= drawCategory(painter, QRect(left, y, right - left, fm.height()), x, categories.at(i), m_categories->color(key, categories.at(i)), fm) + 4;
        }
        previewRight = x;
        painter->setPen(dim);
    }
    if (const QString folder = index.data(MessageListModel::FolderNameRole).toString(); !folder.isEmpty()) {
        const QString shown = fm.elidedText(folder, Qt::ElideRight, (right - left) / 3);
        const int width = fm.horizontalAdvance(shown);
        painter->drawText(QRect(right - width, y, width, fm.height()), Qt::AlignRight | Qt::AlignVCenter, shown);
        previewRight = right - width - fm.horizontalAdvance(QStringLiteral("  "));
    }
    painter->drawText(QRect(left, y, previewRight - left, fm.height()), Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText(preview, Qt::ElideRight, previewRight - left));

    // A thin separator between messages
    if (!selected) {
        QColor line = palette.color(QPalette::Mid);
        line.setAlpha(60);
        painter->setPen(line);
        painter->drawLine(left, r.bottom(), r.right(), r.bottom());
    }
    painter->restore();
}

void FolderDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const int unread = index.data(FolderModel::UnreadRole).toInt();
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    if (index.data(FolderModel::IsMailboxRole).toBool()) {
        opt.font.setBold(true);
    }
    // Change Folder Count: the total in brackets, in the normal text colour (Outlook's Drafts [12])
    if (index.data(FolderModel::ShowsTotalRole).toBool()) {
        const int total = index.data(FolderModel::TotalRole).toInt();
        QStyledItemDelegate::paint(painter, opt, index);
        if (total > 0) {
            const QString count = QStringLiteral("[%1]").arg(total);
            const int countWidth = QFontMetrics(opt.font).horizontalAdvance(count) + 8;
            const bool selected = opt.state & QStyle::State_Selected;
            painter->save();
            painter->setFont(opt.font);
            painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
            painter->drawText(QRect(opt.rect.right() - countWidth, opt.rect.top(), countWidth - 4, opt.rect.height()), Qt::AlignRight | Qt::AlignVCenter, count);
            painter->restore();
        }
        return;
    }
    if (unread <= 0) {
        QStyledItemDelegate::paint(painter, opt, index);
        return;
    }
    // Draw the name, then the count in bold accent colour right after it
    const QString count = QString::number(unread);
    QFont countFont = opt.font;
    countFont.setBold(true);
    const QFontMetrics cfm(countFont);
    const int countWidth = cfm.horizontalAdvance(count) + 8;
    opt.font.setBold(true); // folders with unread mail are bold, as in Outlook
    QStyledItemDelegate::paint(painter, opt, index);
    const bool selected = opt.state & QStyle::State_Selected;
    painter->save();
    painter->setFont(countFont);
    painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Highlight));
    painter->drawText(QRect(opt.rect.right() - countWidth, opt.rect.top(), countWidth - 4, opt.rect.height()), Qt::AlignRight | Qt::AlignVCenter, count);
    painter->restore();
}

}
