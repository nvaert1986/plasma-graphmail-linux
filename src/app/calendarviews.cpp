// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "calendarviews.h"

#include <KLocalizedString>

#include <QApplication>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <cmath>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
constexpr int timeColumn = 56;
constexpr int hourHeight = 48;
constexpr int allDayRow = 22;
constexpr int dayHeader = 38;

QString timeFormat()
{
    // The C locale has no sensible short time: 24 hours as in the rest of the app
    return QLocale().name() == QLatin1String("C") ? QStringLiteral("HH:mm") : QLocale().timeFormat(QLocale::ShortFormat);
}

// The block's fill: a tint of the calendar's colour on the background, paler while not
// firmly in the calendar (tentative, free, not answered, declined, cancelled)
QColor fillFor(const DisplayEvent &e, const QPalette &palette)
{
    const QColor base = palette.color(QPalette::Base);
    const bool pale = e.showAs == 0 || e.showAs == 1 || e.response == 5 || e.response == 4 || e.cancelled;
    const qreal amount = pale ? 0.12 : 0.30;
    return QColor::fromRgbF(base.redF() + (e.color.redF() - base.redF()) * amount,
                            base.greenF() + (e.color.greenF() - base.greenF()) * amount,
                            base.blueF() + (e.color.blueF() - base.blueF()) * amount);
}

void drawEventBlock(QPainter &p, const QRectF &rect, const DisplayEvent &e, bool selected, const QPalette &palette, bool showTime)
{
    p.save();
    QPainterPath path;
    path.addRoundedRect(rect.adjusted(1, 1, -1, -1), 3, 3);
    p.fillPath(path, fillFor(e, palette));
    // The calendar's colour as a bar on the left, as in Outlook
    p.fillRect(QRectF(rect.left() + 1, rect.top() + 1, 4, rect.height() - 2), e.color);
    QPen border(selected ? palette.color(QPalette::Highlight) : e.color.darker(115));
    border.setWidthF(selected ? 2 : 1);
    // Not answered yet: a dashed border
    if (e.response == 5 || e.response == 0) {
        border.setStyle(e.isOrganizer ? Qt::SolidLine : Qt::DashLine);
    }
    p.setPen(border);
    p.drawPath(path);
    QFont font = QApplication::font();
    QFont bold = font;
    bold.setBold(true);
    const bool struck = e.cancelled || e.response == 4;
    bold.setStrikeOut(struck);
    font.setStrikeOut(struck);
    p.setPen(palette.color(QPalette::Text));
    const QRectF text = rect.adjusted(8, 2, -3, -2);
    p.setClipRect(text);
    QString title = e.subject.isEmpty() ? i18nc("@item an event without a subject", "(no subject)") : e.subject;
    if (e.isPrivate) {
        title = QStringLiteral("\U0001F512 ") + title;
    }
    p.setFont(bold);
    const QFontMetricsF fm(bold);
    QString second = e.location;
    if (showTime) {
        second = eventTimeText(e) + (e.location.isEmpty() ? QString() : QStringLiteral("  ") + e.location);
    }
    if (text.height() < fm.height() * 2) {
        // A short event: one line
        p.drawText(text, Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine, title + (second.isEmpty() ? QString() : QStringLiteral("  ") + second));
    } else {
        p.drawText(text, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, title);
        const qreal used = fm.boundingRect(text, Qt::AlignLeft | Qt::TextWordWrap, title).height();
        p.setFont(font);
        p.drawText(text.adjusted(0, used, 0, 0), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, second);
    }
    p.restore();
}
}

QString eventTimeText(const DisplayEvent &event)
{
    if (event.allDay) {
        return event.firstDay == event.lastDay ? i18nc("@info an event lasting the whole day", "All day")
                                               : i18nc("@info an all-day event over several days", "%1 - %2",
                                                       QLocale().toString(event.firstDay, QStringLiteral("d MMM")),
                                                       QLocale().toString(event.lastDay, QStringLiteral("d MMM")));
    }
    const QString start = QLocale().toString(event.start.time(), timeFormat());
    const QString end = QLocale().toString(event.end.time(), timeFormat());
    if (event.start.date() != event.end.addMSecs(-1).date()) {
        return i18nc("@info event time over several days", "%1 %2 - %3 %4",
                     QLocale().toString(event.start.date(), QStringLiteral("d MMM")), start,
                     QLocale().toString(event.end.date(), QStringLiteral("d MMM")), end);
    }
    return i18nc("@info event start and end time", "%1 - %2", start, end);
}

std::optional<DisplayEvent> CalendarView::selected() const
{
    return m_selected;
}

// The day names and the all-day events above the hours
class TimeGridView::Header : public QWidget
{
public:
    explicit Header(TimeGridView *view)
        : QWidget(view)
        , m_view(view)
    {
    }

    int rows() const
    {
        return m_rows;
    }

    void layoutEvents()
    {
        m_placed.clear();
        // Rows of all-day events, each event in the first row free over its days
        QList<QList<QPair<QDate, QDate>>> rows;
        for (const DisplayEvent &e : std::as_const(m_view->m_events)) {
            if (!e.allDay && e.start.date() == e.end.addMSecs(-1).date()) {
                continue;
            }
            const QDate from = std::max(e.firstDay, m_view->m_first);
            const QDate to = std::min(e.lastDay, m_view->m_last);
            int row = 0;
            for (; row < rows.size(); ++row) {
                const bool free = std::none_of(rows[row].cbegin(), rows[row].cend(), [&](const QPair<QDate, QDate> &span) {
                    return span.first <= to && span.second >= from;
                });
                if (free) {
                    break;
                }
            }
            if (row == rows.size()) {
                rows.append(QList<QPair<QDate, QDate>>());
            }
            rows[row].append({from, to});
            m_spans.append(Span{from, to, row, e});
        }
        m_rows = int(rows.size());
        setFixedHeight(dayHeader + std::max(1, m_rows) * allDayRow + 4);
    }

    void reset()
    {
        m_spans.clear();
        m_placed.clear();
        layoutEvents();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QPalette pal = palette();
        p.fillRect(rect(), pal.color(QPalette::Window));
        const int width = this->width();
        const QDate today = QDate::currentDate();
        m_placed.clear();
        for (int d = 0; d < m_view->days(); ++d) {
            const QDate day = m_view->m_first.addDays(d);
            const qreal x = m_view->columnX(d, width);
            const qreal w = m_view->columnWidth(width);
            QFont font = QApplication::font();
            if (day == today) {
                font.setBold(true);
                p.setPen(pal.color(QPalette::Highlight));
            } else {
                p.setPen(pal.color(QPalette::WindowText));
            }
            p.setFont(font);
            const QString label = QLocale().standaloneDayName(day.dayOfWeek(), QLocale::ShortFormat) + QLatin1Char(' ') + QString::number(day.day());
            p.drawText(QRectF(x + 6, 4, w - 8, dayHeader - 8), Qt::AlignLeft | Qt::AlignVCenter, label);
            p.setPen(pal.color(QPalette::Mid));
            p.drawLine(QPointF(x, 0), QPointF(x, height()));
        }
        if (m_view->m_secondZone.isValid()) {
            QFont small = QApplication::font();
            small.setPointSizeF(small.pointSizeF() * 0.85);
            p.setFont(small);
            p.setPen(pal.color(QPalette::PlaceholderText));
            p.drawText(QRectF(2, 4, timeColumn - 6, dayHeader - 8), Qt::AlignRight | Qt::AlignBottom | Qt::TextWordWrap, m_view->m_secondLabel);
            p.drawText(QRectF(timeColumn + 2, 4, timeColumn - 6, dayHeader - 8), Qt::AlignRight | Qt::AlignBottom | Qt::TextWordWrap, m_view->m_ownLabel);
            p.setPen(pal.color(QPalette::Mid));
        }
        p.drawLine(0, height() - 1, width, height() - 1);
        for (const Span &s : std::as_const(m_spans)) {
            const int fromCol = int(m_view->m_first.daysTo(s.from));
            const int toCol = int(m_view->m_first.daysTo(s.to));
            const QRectF r(m_view->columnX(fromCol, width) + 1, dayHeader + s.row * allDayRow, m_view->columnX(toCol, width) + m_view->columnWidth(width) - m_view->columnX(fromCol, width) - 2,
                           allDayRow - 2);
            drawEventBlock(p, r, s.event, m_view->m_selected && m_view->m_selected->id == s.event.id && m_view->m_selected->mailboxKey == s.event.mailboxKey, pal, false);
            m_placed.append({r, s.event});
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_view->m_selected = hit(event->position());
        m_view->repaintAll();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (const std::optional<DisplayEvent> e = hit(event->position())) {
            Q_EMIT m_view->eventActivated(*e);
            return;
        }
        const int width = this->width();
        for (int d = 0; d < m_view->days(); ++d) {
            const qreal x = m_view->columnX(d, width);
            if (event->position().x() >= x && event->position().x() < x + m_view->columnWidth(width)) {
                const QDate day = m_view->m_first.addDays(d);
                if (event->position().y() < dayHeader) {
                    Q_EMIT m_view->dayActivated(day);
                } else {
                    Q_EMIT m_view->newEventRequested(QDateTime(day, QTime(0, 0)), QDateTime(day.addDays(1), QTime(0, 0)), true);
                }
            }
        }
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (const std::optional<DisplayEvent> e = hit(event->pos())) {
            m_view->m_selected = e;
            update();
            Q_EMIT m_view->eventMenuRequested(*e, event->globalPos());
        }
    }

private:
    struct Span {
        QDate from;
        QDate to;
        int row;
        DisplayEvent event;
    };

    std::optional<DisplayEvent> hit(const QPointF &pos) const
    {
        for (const auto &placed : m_placed) {
            if (placed.first.contains(pos)) {
                return placed.second;
            }
        }
        return std::nullopt;
    }

    TimeGridView *m_view;
    QList<Span> m_spans;
    QList<QPair<QRectF, DisplayEvent>> m_placed;
    int m_rows = 0;
};

// The hours with the timed events
class TimeGridView::Grid : public QWidget
{
public:
    explicit Grid(TimeGridView *view)
        : QWidget(view)
        , m_view(view)
    {
        setFixedHeight(24 * hourHeight + 1);
        // The red line for now moves on
        auto *timer = new QTimer(this);
        timer->setInterval(60 * 1000);
        connect(timer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
        timer->start();
    }

    void layoutEvents()
    {
        m_placed.clear();
        const int width = this->width();
        for (int d = 0; d < m_view->days(); ++d) {
            const QDate day = m_view->m_first.addDays(d);
            const QDateTime dayStart(day, QTime(0, 0));
            const QDateTime dayEnd(day.addDays(1), QTime(0, 0));
            QList<DisplayEvent> list;
            for (const DisplayEvent &e : std::as_const(m_view->m_events)) {
                // Timed events of this day; those spanning days are in the all-day strip
                if (!e.allDay && e.start < dayEnd && e.end > dayStart && e.start.date() == e.end.addMSecs(-1).date()) {
                    list.append(e);
                }
            }
            // Overlapping events side by side: clusters, then the first free column in each
            int i = 0;
            while (i < list.size()) {
                QDateTime clusterEnd = list[i].end;
                int j = i + 1;
                while (j < list.size() && list[j].start < clusterEnd) {
                    clusterEnd = std::max(clusterEnd, list[j].end);
                    ++j;
                }
                QList<QDateTime> columnEnds;
                QList<int> column(j - i);
                for (int k = i; k < j; ++k) {
                    int c = 0;
                    while (c < columnEnds.size() && columnEnds[c] > list[k].start) {
                        ++c;
                    }
                    if (c == columnEnds.size()) {
                        columnEnds.append(list[k].end);
                    } else {
                        columnEnds[c] = list[k].end;
                    }
                    column[k - i] = c;
                }
                const qreal colW = (m_view->columnWidth(width) - 6) / std::max<qsizetype>(1, columnEnds.size());
                for (int k = i; k < j; ++k) {
                    const DisplayEvent &e = list[k];
                    const qreal top = std::max<qint64>(0, dayStart.secsTo(e.start)) / 3600.0 * hourHeight;
                    const qreal bottom = std::min<qint64>(24 * 3600, dayStart.secsTo(e.end)) / 3600.0 * hourHeight;
                    const QRectF r(m_view->columnX(d, width) + 1 + column[k - i] * colW, top, colW, std::max<qreal>(bottom - top, hourHeight / 2.0 - 2));
                    m_placed.append({r, e});
                }
                i = j;
            }
        }
    }

protected:
    void resizeEvent(QResizeEvent *) override
    {
        layoutEvents();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QPalette pal = palette();
        const int width = this->width();
        p.fillRect(rect(), pal.color(QPalette::Base));
        for (int d = 0; d < m_view->days(); ++d) {
            const QDate day = m_view->m_first.addDays(d);
            const qreal x = m_view->columnX(d, width);
            const qreal w = m_view->columnWidth(width);
            // Outside working hours and weekends a little darker, as in Outlook
            const QPair<double, double> work = m_view->m_workingHours.localHours(day);
            p.fillRect(QRectF(x, 0, w, 24 * hourHeight), m_view->dayBackground(day, false));
            if (work.second > work.first) {
                p.fillRect(QRectF(x, work.first * hourHeight, w, (work.second - work.first) * hourHeight), m_view->dayBackground(day, true));
            }
        }
        p.setFont(QApplication::font());
        for (int h = 0; h < 24; ++h) {
            const qreal y = h * hourHeight;
            p.setPen(pal.color(QPalette::Mid));
            p.drawLine(QPointF(m_view->timeColumnWidth(), y), QPointF(width, y));
            QPen half(pal.color(QPalette::Midlight));
            half.setStyle(Qt::DotLine);
            p.setPen(half);
            p.drawLine(QPointF(m_view->timeColumnWidth(), y + hourHeight / 2.0), QPointF(width, y + hourHeight / 2.0));
            p.setPen(pal.color(QPalette::PlaceholderText));
            const int own = m_view->timeColumnWidth() - timeColumn;
            p.drawText(QRectF(own, y + 2, timeColumn - 6, hourHeight), Qt::AlignRight | Qt::AlignTop, QLocale().toString(QTime(h, 0), timeFormat()));
            if (m_view->m_secondZone.isValid()) {
                // The same moment in the other zone (for the first day shown: zones move apart on
                // a change to summer time only for a week or two)
                const QDateTime here(m_view->m_first, QTime(h, 0));
                const QTime there = here.toTimeZone(m_view->m_secondZone).time();
                p.drawText(QRectF(0, y + 2, timeColumn - 6, hourHeight), Qt::AlignRight | Qt::AlignTop, QLocale().toString(there, timeFormat()));
            }
        }
        p.setPen(pal.color(QPalette::Mid));
        for (int d = 0; d <= m_view->days(); ++d) {
            const qreal x = d == m_view->days() ? width - 1 : m_view->columnX(d, width);
            p.drawLine(QPointF(x, 0), QPointF(x, height()));
        }
        for (const auto &placed : std::as_const(m_placed)) {
            const bool selected = m_view->m_selected && m_view->m_selected->id == placed.second.id && m_view->m_selected->mailboxKey == placed.second.mailboxKey;
            drawEventBlock(p, placed.first, placed.second, selected, pal, placed.first.height() >= hourHeight * 0.9);
        }
        // Now
        const QDateTime now = QDateTime::currentDateTime();
        const qint64 col = m_view->m_first.daysTo(now.date());
        if (col >= 0 && col < m_view->days()) {
            const qreal y = QTime(0, 0).secsTo(now.time()) / 3600.0 * hourHeight;
            p.setPen(QPen(QColor(0xda, 0x44, 0x53), 2));
            const qreal x = m_view->columnX(int(col), width);
            p.drawLine(QPointF(x, y), QPointF(x + m_view->columnWidth(width), y));
            p.setBrush(QColor(0xda, 0x44, 0x53));
            p.drawEllipse(QPointF(x, y), 4, 4);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_view->m_selected = hit(event->position());
        update();
        m_view->m_header->update();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (const std::optional<DisplayEvent> e = hit(event->position())) {
            Q_EMIT m_view->eventActivated(*e);
            return;
        }
        // A new event in the half hour clicked
        const int width = this->width();
        for (int d = 0; d < m_view->days(); ++d) {
            const qreal x = m_view->columnX(d, width);
            if (event->position().x() >= x && event->position().x() < x + m_view->columnWidth(width)) {
                const int halfHours = int(event->position().y() / (hourHeight / 2.0));
                const QDateTime start(m_view->m_first.addDays(d), QTime(0, 0).addSecs(halfHours * 1800));
                Q_EMIT m_view->newEventRequested(start, start.addSecs(1800), false);
            }
        }
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (const std::optional<DisplayEvent> e = hit(event->pos())) {
            m_view->m_selected = e;
            update();
            Q_EMIT m_view->eventMenuRequested(*e, event->globalPos());
        }
    }

private:
    std::optional<DisplayEvent> hit(const QPointF &pos) const
    {
        // The last drawn is on top
        for (auto it = m_placed.crbegin(); it != m_placed.crend(); ++it) {
            if (it->first.contains(pos)) {
                return it->second;
            }
        }
        return std::nullopt;
    }

    TimeGridView *m_view;
    QList<QPair<QRectF, DisplayEvent>> m_placed;
};

TimeGridView::TimeGridView(QWidget *parent)
    : CalendarView(parent)
    , m_header(new Header(this))
    , m_scroll(new QScrollArea(this))
    , m_grid(new Grid(this))
{
    m_scroll->setWidget(m_grid);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The header leaves room for the scroll bar, so the columns line up
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, m_scroll->verticalScrollBar()->sizeHint().width(), 0);
    headerRow->addWidget(m_header);
    layout->addLayout(headerRow);
    layout->addWidget(m_scroll, 1);
}

int TimeGridView::days() const
{
    return int(m_first.daysTo(m_last)) + 1;
}

qreal TimeGridView::columnWidth(int width) const
{
    return (width - timeColumnWidth()) / qreal(std::max(1, days()));
}

qreal TimeGridView::columnX(int day, int width) const
{
    return timeColumnWidth() + day * columnWidth(width);
}

QColor TimeGridView::dayBackground(QDate day, bool working) const
{
    const QPalette pal = palette();
    QColor base = pal.color(QPalette::Base);
    if (!working) {
        base = pal.color(QPalette::AlternateBase);
    }
    if (day == QDate::currentDate()) {
        QColor highlight = pal.color(QPalette::Highlight);
        highlight.setAlpha(18);
        QColor mixed = base;
        mixed.setRedF(base.redF() * 0.93 + highlight.redF() * 0.07);
        mixed.setGreenF(base.greenF() * 0.93 + highlight.greenF() * 0.07);
        mixed.setBlueF(base.blueF() * 0.93 + highlight.blueF() * 0.07);
        return mixed;
    }
    return base;
}

void TimeGridView::repaintAll()
{
    m_header->update();
    m_grid->update();
}

void TimeGridView::setEvents(QDate first, QDate last, const QList<DisplayEvent> &events)
{
    m_first = first;
    m_last = last;
    m_events = events;
    if (m_selected) {
        const auto still = std::find_if(events.cbegin(), events.cend(), [this](const DisplayEvent &e) {
            return e.id == m_selected->id && e.mailboxKey == m_selected->mailboxKey;
        });
        m_selected = still == events.cend() ? std::nullopt : std::optional<DisplayEvent>(*still);
    }
    m_header->reset();
    m_grid->layoutEvents();
    m_grid->update();
}

void TimeGridView::scrollToHour(int hour)
{
    m_scroll->verticalScrollBar()->setValue(hour * hourHeight);
}

QPair<int, int> TimeGridView::printHours() const
{
    int from = 24;
    int to = 0;
    for (QDate day = m_first; day.isValid() && day <= m_last; day = day.addDays(1)) {
        const QPair<double, double> work = m_workingHours.localHours(day);
        if (work.second > work.first) {
            from = std::min(from, int(work.first));
            to = std::max(to, int(std::ceil(work.second)));
        }
    }
    // An hour before and after, as Outlook prints a day
    if (from >= to) {
        return {7, 19};
    }
    return {std::max(0, from - 1), std::min(24, to + 1)};
}

void TimeGridView::printTo(QPainter *painter, const QRectF &target, int fromHour, int toHour)
{
    const QPalette before = palette();
    const bool own = testAttribute(Qt::WA_SetPalette);
    QPalette paper = QApplication::style()->standardPalette();
    paper.setColor(QPalette::Base, Qt::white);
    paper.setColor(QPalette::Window, Qt::white);
    paper.setColor(QPalette::AlternateBase, QColor(0xf0, 0xf0, 0xf0));
    paper.setColor(QPalette::Text, Qt::black);
    paper.setColor(QPalette::WindowText, Qt::black);
    setPalette(paper);
    const int width = m_grid->width();
    const int headerHeight = m_header->height();
    const QRect hours(0, fromHour * hourHeight, width, (toHour - fromHour) * hourHeight);
    const qreal scale = std::min(target.width() / width, target.height() / (headerHeight + hours.height()));
    painter->save();
    painter->translate(target.topLeft());
    painter->scale(scale, scale);
    m_header->render(painter, QPoint(0, 0), QRegion(), QWidget::DrawChildren);
    // The region's top-left lands at the offset: the hours right under the header
    m_grid->render(painter, QPoint(0, headerHeight), QRegion(hours), QWidget::DrawChildren);
    painter->restore();
    if (own) {
        setPalette(before);
    } else {
        setPalette(QPalette());
        setAttribute(Qt::WA_SetPalette, false);
    }
}

int TimeGridView::timeColumnWidth() const
{
    return m_secondZone.isValid() ? 2 * timeColumn : timeColumn;
}

void TimeGridView::setSecondTimeZone(const QTimeZone &zone, const QString &label, const QString &ownLabel)
{
    m_secondZone = zone;
    m_secondLabel = label;
    m_ownLabel = ownLabel;
    repaintAll();
}

void TimeGridView::setWorkingHours(const WorkingHours &hours)
{
    m_workingHours = hours;
    repaintAll();
}

MonthView::MonthView(QWidget *parent)
    : CalendarView(parent)
{
    setMinimumSize(400, 300);
}

void MonthView::setEvents(QDate first, QDate last, const QList<DisplayEvent> &events)
{
    m_first = first;
    m_last = last;
    m_month = first.addDays(first.daysTo(last) / 2).month();
    m_events = events;
    m_selected.reset();
    update();
}

void MonthView::setWeekNumbers(bool on)
{
    m_weekNumbers = on;
    update();
}

namespace
{
constexpr int weekColumn = 30;
}

QRectF MonthView::cellRect(int index) const
{
    const qreal top = dayHeader / 1.5;
    const qreal left = m_weekNumbers ? weekColumn : 0;
    const qreal w = (width() - left) / 7.0;
    const qreal h = (height() - top) / 6.0;
    return {left + (index % 7) * w, top + (index / 7) * h, w, h};
}

void MonthView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QPalette pal = palette();
    p.fillRect(rect(), pal.color(QPalette::Base));
    m_placed.clear();
    m_more.clear();
    const QFont font = QApplication::font();
    p.setFont(font);
    for (int c = 0; c < 7; ++c) {
        const QDate day = m_first.addDays(c);
        const QRectF cell = cellRect(c);
        p.setPen(pal.color(QPalette::WindowText));
        p.drawText(QRectF(cell.left() + 6, 0, cell.width() - 8, dayHeader / 1.5), Qt::AlignLeft | Qt::AlignVCenter, QLocale().standaloneDayName(day.dayOfWeek(), QLocale::LongFormat));
    }
    // ISO week numbers, as Outlook's (the week of the row's Thursday)
    if (m_weekNumbers) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        for (int row = 0; row < 6; ++row) {
            const QRectF cell = cellRect(row * 7);
            p.drawText(QRectF(0, cell.top() + 2, weekColumn - 4, cell.height()), Qt::AlignRight | Qt::AlignTop,
                       QString::number(m_first.addDays(row * 7 + (4 - m_first.dayOfWeek() + 7) % 7).weekNumber()));
        }
    }
    const QFontMetricsF fm(font);
    const qreal line = fm.height() + 3;
    for (int i = 0; i < 42; ++i) {
        const QDate day = m_first.addDays(i);
        const QRectF cell = cellRect(i);
        p.fillRect(cell, day.month() == m_month ? pal.color(QPalette::Base) : pal.color(QPalette::AlternateBase));
        p.setPen(pal.color(QPalette::Mid));
        p.drawRect(cell);
        QFont number = font;
        if (day == QDate::currentDate()) {
            number.setBold(true);
            p.setPen(pal.color(QPalette::Highlight));
        } else {
            p.setPen(day.month() == m_month ? pal.color(QPalette::Text) : pal.color(QPalette::PlaceholderText));
        }
        p.setFont(number);
        const QString label = day.day() == 1 ? QLocale().toString(day, QStringLiteral("d MMM")) : QString::number(day.day());
        p.drawText(cell.adjusted(6, 2, -4, 0), Qt::AlignLeft | Qt::AlignTop, label);
        p.setFont(font);
        QList<DisplayEvent> list;
        for (const DisplayEvent &e : std::as_const(m_events)) {
            if (e.firstDay <= day && e.lastDay >= day) {
                list.append(e);
            }
        }
        const int fit = std::max(0, int((cell.height() - line - 4) / line));
        for (int k = 0; k < list.size() && k < fit; ++k) {
            const bool last = k == fit - 1 && list.size() > fit;
            const QRectF r(cell.left() + 2, cell.top() + line + k * line, cell.width() - 4, line - 1);
            if (last) {
                p.setPen(pal.color(QPalette::Link));
                const QString more = i18ncp("@info month view", "%1 more", "%1 more", int(list.size() - k));
                p.drawText(r.adjusted(6, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, more);
                m_more.append({r, day});
                break;
            }
            const DisplayEvent &e = list.at(k);
            const bool selected = m_selected && m_selected->id == e.id && m_selected->mailboxKey == e.mailboxKey;
            if (e.allDay || e.firstDay != e.lastDay) {
                drawEventBlock(p, r, e, selected, pal, false);
            } else {
                // Timed: a coloured dot, the time and the subject
                p.save();
                if (selected) {
                    QColor h = pal.color(QPalette::Highlight);
                    h.setAlpha(60);
                    p.fillRect(r, h);
                }
                p.setBrush(e.color);
                p.setPen(Qt::NoPen);
                p.drawEllipse(QPointF(r.left() + 6, r.center().y()), 3.5, 3.5);
                p.setPen(pal.color(QPalette::Text));
                QFont f = font;
                f.setStrikeOut(e.cancelled || e.response == 4);
                p.setFont(f);
                p.drawText(r.adjusted(14, 0, -2, 0), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                           QLocale().toString(e.start.time(), timeFormat()) + QLatin1Char(' ') + e.subject);
                p.restore();
            }
            m_placed.append({r, e});
        }
    }
}

std::optional<DisplayEvent> MonthView::eventAt(const QPointF &pos) const
{
    for (const auto &placed : m_placed) {
        if (placed.first.contains(pos)) {
            return placed.second;
        }
    }
    return std::nullopt;
}

QDate MonthView::dayAt(const QPointF &pos) const
{
    for (int i = 0; i < 42; ++i) {
        if (cellRect(i).contains(pos)) {
            return m_first.addDays(i);
        }
    }
    return {};
}

void MonthView::mousePressEvent(QMouseEvent *event)
{
    for (const auto &more : std::as_const(m_more)) {
        if (more.first.contains(event->position())) {
            Q_EMIT dayActivated(more.second);
            return;
        }
    }
    m_selected = eventAt(event->position());
    update();
}

void MonthView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (const std::optional<DisplayEvent> e = eventAt(event->position())) {
        Q_EMIT eventActivated(*e);
        return;
    }
    const QDate day = dayAt(event->position());
    if (day.isValid()) {
        Q_EMIT newEventRequested(QDateTime(day, QTime(9, 0)), QDateTime(day, QTime(9, 30)), false);
    }
}

void MonthView::contextMenuEvent(QContextMenuEvent *event)
{
    if (const std::optional<DisplayEvent> e = eventAt(event->pos())) {
        m_selected = e;
        update();
        Q_EMIT eventMenuRequested(*e, event->globalPos());
    }
}

AgendaView::AgendaView(QWidget *parent)
    : CalendarView(parent)
    , m_tree(new QTreeWidget(this))
{
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({i18nc("@title:column", "Time"), i18nc("@title:column", "Subject"), i18nc("@title:column", "Location"), i18nc("@title:column", "Organizer")});
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_tree);
    auto eventOf = [this](QTreeWidgetItem *item) -> std::optional<DisplayEvent> {
        const int index = item ? item->data(0, Qt::UserRole).toInt() : -1;
        return index >= 0 && index < m_events.size() ? std::optional<DisplayEvent>(m_events.at(index)) : std::nullopt;
    };
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this, eventOf](QTreeWidgetItem *item) {
        if (const std::optional<DisplayEvent> e = eventOf(item)) {
            Q_EMIT eventActivated(*e);
        }
    });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this, eventOf](QTreeWidgetItem *item) {
        m_selected = eventOf(item);
    });
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this, eventOf](const QPoint &pos) {
        if (const std::optional<DisplayEvent> e = eventOf(m_tree->itemAt(pos))) {
            Q_EMIT eventMenuRequested(*e, m_tree->viewport()->mapToGlobal(pos));
        }
    });
}

void AgendaView::setEvents(QDate first, QDate last, const QList<DisplayEvent> &events)
{
    m_events = events;
    m_selected.reset();
    m_tree->clear();
    for (QDate day = first; day <= last; day = day.addDays(1)) {
        QList<int> indexes;
        for (int i = 0; i < m_events.size(); ++i) {
            if (m_events.at(i).firstDay <= day && m_events.at(i).lastDay >= day) {
                indexes.append(i);
            }
        }
        if (indexes.isEmpty()) {
            continue;
        }
        auto *header = new QTreeWidgetItem(m_tree, {QLocale().toString(day, QStringLiteral("dddd d MMMM yyyy"))});
        header->setFirstColumnSpanned(true);
        QFont bold = header->font(0);
        bold.setBold(true);
        header->setFont(0, bold);
        header->setFlags(Qt::ItemIsEnabled);
        header->setData(0, Qt::UserRole, -1);
        for (int i : std::as_const(indexes)) {
            const DisplayEvent &e = m_events.at(i);
            auto *item = new QTreeWidgetItem(m_tree, {eventTimeText(e), e.subject, e.location, QString()});
            item->setData(0, Qt::UserRole, i);
            QPixmap swatch(12, 12);
            swatch.fill(e.color);
            item->setIcon(3, QIcon(swatch));
            item->setText(3, e.organizer);
            if (e.cancelled || e.response == 4) {
                QFont struck = item->font(1);
                struck.setStrikeOut(true);
                item->setFont(1, struck);
            }
        }
    }
    m_tree->resizeColumnToContents(0);
}

}
