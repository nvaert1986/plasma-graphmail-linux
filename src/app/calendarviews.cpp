// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "calendarviews.h"
#include "calendaroptions.h"

#include <KLocalizedString>

#include <QApplication>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QKeyEvent>
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

// The selected time without an event (Outlook's blue): the selection colour, light enough for the
// hour lines to show through
QColor selectionColor(const QPalette &palette)
{
    QColor colour = palette.color(QPalette::Highlight);
    colour.setAlpha(110);
    return colour;
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

void CalendarView::showMoved(const DisplayEvent &event, const QDateTime &start, const QDateTime &end, bool allDay)
{
    for (DisplayEvent &e : m_events) {
        if (e.id == event.id && e.mailboxKey == event.mailboxKey) {
            e.start = start;
            e.end = end;
            e.allDay = allDay;
            e.firstDay = start.date();
            e.lastDay = allDay ? end.date().addDays(-1) : end.addMSecs(-1).date();
            if (m_selected && m_selected->id == e.id && m_selected->mailboxKey == e.mailboxKey) {
                m_selected = e;
            }
        }
    }
    eventsMoved();
}

// The day names and the all-day events above the hours
class TimeGridView::Header : public QWidget
{
public:
    explicit Header(TimeGridView *view)
        : QWidget(view)
        , m_view(view)
    {
        // Esc during a drag, and the resize cursor on an event's right edge
        setFocusPolicy(Qt::ClickFocus);
        setMouseTracking(true);
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
        // A day selected in the all-day strip
        if (m_view->m_slot && m_view->m_slot->allDay) {
            const qint64 col = m_view->m_first.daysTo(m_view->m_slot->start.date());
            if (col >= 0 && col < m_view->days()) {
                p.fillRect(QRectF(m_view->columnX(int(col), width) + 1, dayHeader, m_view->columnWidth(width) - 1, height() - dayHeader - 1), selectionColor(pal));
            }
        }
        const auto spanRect = [this, width](QDate from, QDate to, int row) {
            const int fromCol = int(m_view->m_first.daysTo(from));
            const int toCol = int(m_view->m_first.daysTo(to));
            return QRectF(m_view->columnX(fromCol, width) + 1, dayHeader + row * allDayRow, m_view->columnX(toCol, width) + m_view->columnWidth(width) - m_view->columnX(fromCol, width) - 2,
                          allDayRow - 2);
        };
        int draggedRow = 0;
        for (const Span &s : std::as_const(m_spans)) {
            const QRectF r = spanRect(s.from, s.to, s.row);
            // An event being dragged stays where it was, faded, until it is dropped
            const bool dragged = m_view->isDragged(s.event);
            if (dragged) {
                draggedRow = s.row;
                p.setOpacity(0.35);
            }
            drawEventBlock(p, r, s.event, m_view->m_selected && m_view->m_selected->id == s.event.id && m_view->m_selected->mailboxKey == s.event.mailboxKey, pal, false);
            p.setOpacity(1.0);
            m_placed.append({r, s.event});
        }
        // Where a dragged event lands, when that is in the strip
        if (const std::optional<Drag> &drag = m_view->m_drag; drag && drag->started && !inHours(*drag)) {
            DisplayEvent landing = drag->event;
            landing.start = drag->start;
            landing.end = drag->end;
            landing.allDay = drag->allDay;
            landing.firstDay = drag->start.date();
            landing.lastDay = drag->allDay ? drag->end.date().addDays(-1) : drag->end.addMSecs(-1).date();
            const QDate from = std::max(landing.firstDay, m_view->m_first);
            const QDate to = std::min(landing.lastDay, m_view->m_last);
            if (from <= to) {
                drawEventBlock(p, spanRect(from, to, drag->fromHeader ? draggedRow : 0), landing, true, pal, false);
            }
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_view->m_drag) {
            if (event->buttons() & Qt::LeftButton) {
                m_view->dragMoved(event->globalPosition().toPoint());
            }
            return;
        }
        if (event->buttons() == Qt::NoButton) {
            const Placed *placed = hitPlaced(event->position());
            const bool edge = placed && onRightEdge(*placed, event->position());
            if (edge) {
                setCursor(Qt::SizeHorCursor);
            } else {
                unsetCursor();
            }
        }
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (m_view->m_drag) {
            m_view->dragReleased();
        }
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape && m_view->m_drag) {
            m_view->cancelDrag();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_view->m_selected = hit(event->position());
        Q_EMIT m_view->selectionChanged();
        m_view->m_slot.reset();
        // An event can be dragged to other days, an all-day one into the hours, and an all-day
        // one made longer or shorter by its right edge
        if (const Placed *placed = hitPlaced(event->position()); placed && event->button() == Qt::LeftButton && m_view->movable(placed->second)) {
            setFocus(Qt::MouseFocusReason);
            m_view->beginDrag(placed->second, onRightEdge(*placed, event->position()) ? Drag::Mode::ResizeEnd : Drag::Mode::Move, true,
                              event->globalPosition().toPoint());
        }
        // An empty spot below the day names: the whole day
        if (!m_view->m_selected && event->position().y() >= dayHeader) {
            const int width = this->width();
            for (int d = 0; d < m_view->days(); ++d) {
                const qreal x = m_view->columnX(d, width);
                if (event->position().x() >= x && event->position().x() < x + m_view->columnWidth(width)) {
                    const QDate day = m_view->m_first.addDays(d);
                    m_view->m_slot = Slot{QDateTime(day, QTime(0, 0)), QDateTime(day.addDays(1), QTime(0, 0)), true};
                }
            }
        }
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
            Q_EMIT m_view->selectionChanged();
            m_view->m_slot.reset();
            m_view->repaintAll();
            Q_EMIT m_view->eventMenuRequested(*e, event->globalPos());
            return;
        }
        // An empty day in the all-day strip: selected, then its menu
        const int width = this->width();
        for (int d = 0; d < m_view->days(); ++d) {
            const qreal x = m_view->columnX(d, width);
            if (event->pos().x() >= x && event->pos().x() < x + m_view->columnWidth(width)) {
                const QDate day = m_view->m_first.addDays(d);
                m_view->m_selected.reset();
                m_view->m_slot = Slot{QDateTime(day, QTime(0, 0)), QDateTime(day.addDays(1), QTime(0, 0)), true};
                m_view->repaintAll();
                Q_EMIT m_view->slotMenuRequested(event->globalPos());
            }
        }
    }

private:
    using Drag = TimeGridView::Drag;
    using Placed = QPair<QRectF, DisplayEvent>;
    struct Span {
        QDate from;
        QDate to;
        int row;
        DisplayEvent event;
    };

    // A drag that lands in the hours rather than in this strip
    static bool inHours(const Drag &drag)
    {
        return !drag.allDay && drag.start.date() == drag.end.addMSecs(-1).date();
    }

    const Placed *hitPlaced(const QPointF &pos) const
    {
        for (const auto &placed : m_placed) {
            if (placed.first.contains(pos)) {
                return &placed;
            }
        }
        return nullptr;
    }

    std::optional<DisplayEvent> hit(const QPointF &pos) const
    {
        const Placed *placed = hitPlaced(pos);
        return placed ? std::optional<DisplayEvent>(placed->second) : std::nullopt;
    }

    // The right edge of an all-day event that ends within the days shown: drag it for more or
    // fewer days
    bool onRightEdge(const Placed &placed, const QPointF &pos) const
    {
        return placed.second.allDay && placed.second.lastDay <= m_view->m_last && m_view->movable(placed.second) && placed.first.right() - pos.x() < 6;
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
        // Enter and typing on a selected time
        setFocusPolicy(Qt::ClickFocus);
        // The resize cursor on an event's top and bottom edge
        setMouseTracking(true);
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
        // The selected time, below the events
        if (m_view->m_slot && !m_view->m_slot->allDay) {
            const qint64 col = m_view->m_first.daysTo(m_view->m_slot->start.date());
            if (col >= 0 && col < m_view->days()) {
                const QDateTime dayStart(m_view->m_slot->start.date(), QTime(0, 0));
                const qreal top = dayStart.secsTo(m_view->m_slot->start) / 3600.0 * hourHeight;
                const qreal bottom = std::min<qint64>(24 * 3600, dayStart.secsTo(m_view->m_slot->end)) / 3600.0 * hourHeight;
                p.fillRect(QRectF(m_view->columnX(int(col), width) + 1, top + 1, m_view->columnWidth(width) - 1, bottom - top - 1), selectionColor(pal));
            }
        }
        for (const auto &placed : std::as_const(m_placed)) {
            const bool selected = m_view->m_selected && m_view->m_selected->id == placed.second.id && m_view->m_selected->mailboxKey == placed.second.mailboxKey;
            // An event being dragged stays where it was, faded, until it is dropped
            p.setOpacity(m_view->isDragged(placed.second) ? 0.35 : 1.0);
            drawEventBlock(p, placed.first, placed.second, selected, pal, placed.first.height() >= hourHeight * 0.9);
        }
        p.setOpacity(1.0);
        // Where a dragged event lands, with its new times, when that is in the hours
        if (const std::optional<Drag> &drag = m_view->m_drag; drag && drag->started && !drag->allDay && drag->start.date() == drag->end.addMSecs(-1).date()) {
            const qint64 col = m_view->m_first.daysTo(drag->start.date());
            if (col >= 0 && col < m_view->days()) {
                DisplayEvent landing = drag->event;
                landing.start = drag->start;
                landing.end = drag->end;
                landing.allDay = false;
                landing.firstDay = landing.lastDay = drag->start.date();
                const QDateTime dayStart(drag->start.date(), QTime(0, 0));
                const qreal top = dayStart.secsTo(drag->start) / 3600.0 * hourHeight;
                const qreal bottom = std::min<qint64>(24 * 3600, dayStart.secsTo(drag->end)) / 3600.0 * hourHeight;
                const QRectF r(m_view->columnX(int(col), width) + 1, top, m_view->columnWidth(width) - 6, std::max<qreal>(bottom - top, hourHeight / 2.0 - 2));
                drawEventBlock(p, r, landing, true, pal, true);
            }
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
        setFocus(Qt::MouseFocusReason);
        m_view->m_selected = hit(event->position());
        Q_EMIT m_view->selectionChanged();
        m_anchor.reset();
        m_collapse.reset();
        // An event can be dragged to another time or day, or by its top or bottom edge to
        // another length, as in Outlook
        if (const Placed *placed = hitPlaced(event->position()); placed && event->button() == Qt::LeftButton && m_view->movable(placed->second)) {
            m_view->beginDrag(placed->second, edgeAt(*placed, event->position()), false, event->globalPosition().toPoint());
        }
        const std::optional<QPair<int, int>> at = m_view->m_selected ? std::nullopt : halfHourAt(event->position());
        // Inside the selection: it stays for a double click or a right click; a single click
        // narrows it to the half hour clicked once the double click time is over
        if (at && inSelection(*at)) {
            if (event->button() == Qt::LeftButton) {
                m_collapse = at;
                QTimer::singleShot(QApplication::doubleClickInterval(), this, [this] {
                    if (m_collapse) {
                        selectHalfHours(m_collapse->first, m_collapse->second, m_collapse->second);
                        m_collapse.reset();
                        update();
                    }
                });
            }
            return;
        }
        m_view->m_slot.reset();
        // An empty spot: its half hour, a drag makes it longer
        if (at) {
            m_anchor = at;
            selectHalfHours(at->first, at->second, at->second);
        }
        update();
        m_view->m_header->update();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_view->m_drag) {
            if (event->buttons() & Qt::LeftButton) {
                m_view->dragMoved(event->globalPosition().toPoint());
            }
            return;
        }
        if (event->buttons() == Qt::NoButton) {
            const Placed *placed = hitPlaced(event->position());
            if (placed && edgeAt(*placed, event->position()) != Drag::Mode::Move) {
                setCursor(Qt::SizeVerCursor);
            } else {
                unsetCursor();
            }
            return;
        }
        if (!(event->buttons() & Qt::LeftButton)) {
            return;
        }
        // A drag that starts inside the selection makes a new one from there
        if (!m_anchor && m_collapse) {
            m_anchor = m_collapse;
            m_collapse.reset();
        }
        if (!m_anchor) {
            return;
        }
        // Up or down within the day the drag started in
        const int half = std::clamp(int(event->position().y() / (hourHeight / 2.0)), 0, 47);
        selectHalfHours(m_anchor->first, std::min(half, m_anchor->second), std::max(half, m_anchor->second));
        update();
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        m_anchor.reset();
        if (m_view->m_drag) {
            m_view->dragReleased();
        }
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (const std::optional<DisplayEvent> e = hit(event->position())) {
            Q_EMIT m_view->eventActivated(*e);
            return;
        }
        // Inside the selection: that whole time, as Outlook does
        m_collapse.reset();
        if (const std::optional<QPair<int, int>> at = halfHourAt(event->position()); at && inSelection(*at)) {
            Q_EMIT m_view->newEventRequested(m_view->m_slot->start, m_view->m_slot->end, false);
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
            Q_EMIT m_view->selectionChanged();
            m_view->m_slot.reset();
            update();
            Q_EMIT m_view->eventMenuRequested(*e, event->globalPos());
            return;
        }
        // Empty time: selected (unless inside the selection already), then its menu
        const std::optional<QPair<int, int>> at = halfHourAt(event->pos());
        if (!at) {
            return;
        }
        if (!inSelection(*at)) {
            m_view->m_selected.reset();
            selectHalfHours(at->first, at->second, at->second);
        }
        m_collapse.reset();
        update();
        m_view->m_header->update();
        Q_EMIT m_view->slotMenuRequested(event->globalPos());
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape && m_view->m_drag) {
            m_view->cancelDrag();
            return;
        }
        const std::optional<CalendarView::Slot> slot = m_view->m_slot;
        if (!slot || slot->allDay) {
            QWidget::keyPressEvent(event);
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            Q_EMIT m_view->newEventRequested(slot->start, slot->end, false);
            return;
        }
        // Typing starts a new event with the text as its title (Outlook types in place)
        if (const QString text = event->text(); !text.isEmpty() && text.at(0).isPrint() && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
            Q_EMIT m_view->newEventTyped(slot->start, slot->end, false, text);
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    // Whether a half hour lies in the selected time of this grid
    bool inSelection(const QPair<int, int> &at) const
    {
        if (!m_view->m_slot || m_view->m_slot->allDay) {
            return false;
        }
        const QDateTime moment(m_view->m_first.addDays(at.first), QTime(0, 0).addSecs(at.second * 1800));
        return moment >= m_view->m_slot->start && moment < m_view->m_slot->end;
    }

    using Drag = TimeGridView::Drag;
    using Placed = QPair<QRectF, DisplayEvent>;

    const Placed *hitPlaced(const QPointF &pos) const
    {
        // The last drawn is on top
        for (auto it = m_placed.crbegin(); it != m_placed.crend(); ++it) {
            if (it->first.contains(pos)) {
                return &*it;
            }
        }
        return nullptr;
    }

    std::optional<DisplayEvent> hit(const QPointF &pos) const
    {
        const Placed *placed = hitPlaced(pos);
        return placed ? std::optional<DisplayEvent>(placed->second) : std::nullopt;
    }

    // The top or bottom edge of an event that may be changed: drag it for another start or end
    Drag::Mode edgeAt(const Placed &placed, const QPointF &pos) const
    {
        if (!m_view->movable(placed.second) || placed.first.height() < 16) {
            return Drag::Mode::Move;
        }
        if (pos.y() - placed.first.top() < 5) {
            return Drag::Mode::ResizeStart;
        }
        if (placed.first.bottom() - pos.y() < 5) {
            return Drag::Mode::ResizeEnd;
        }
        return Drag::Mode::Move;
    }

    // The day column and the half hour (0..47) under a point
    std::optional<QPair<int, int>> halfHourAt(const QPointF &pos) const
    {
        const int width = this->width();
        for (int d = 0; d < m_view->days(); ++d) {
            const qreal x = m_view->columnX(d, width);
            if (pos.x() >= x && pos.x() < x + m_view->columnWidth(width)) {
                return QPair<int, int>{d, std::clamp(int(pos.y() / (hourHeight / 2.0)), 0, 47)};
            }
        }
        return std::nullopt;
    }

    void selectHalfHours(int day, int from, int to)
    {
        const QDate date = m_view->m_first.addDays(day);
        const QDateTime start(date, QTime(0, 0).addSecs(from * 1800));
        // The last half hour of the day ends at midnight
        const QDateTime end = to == 47 ? QDateTime(date.addDays(1), QTime(0, 0)) : QDateTime(date, QTime(0, 0).addSecs((to + 1) * 1800));
        m_view->m_slot = Slot{start, end, false};
    }

    TimeGridView *m_view;
    QList<QPair<QRectF, DisplayEvent>> m_placed;
    // Where a drag selection started: day column and half hour
    std::optional<QPair<int, int>> m_anchor;
    // A click inside the selection, narrowed to it unless a double click follows
    std::optional<QPair<int, int>> m_collapse;
};

QWidget *TimeGridView::gridForTests() const
{
    return m_grid;
}

namespace
{
constexpr int halfHour = 30 * 60;

// Seconds rounded to the nearest half hour, as the grid's lines
int snapped(int seconds)
{
    return int(std::lround(seconds / double(halfHour))) * halfHour;
}
}

void TimeGridView::beginDrag(const DisplayEvent &event, Drag::Mode mode, bool fromHeader, const QPoint &global)
{
    Drag drag;
    drag.event = event;
    drag.mode = mode;
    drag.fromHeader = fromHeader;
    drag.pressGlobal = global;
    drag.pressColumn = columnAt(global);
    drag.pressSeconds = secondsAt(global);
    drag.start = event.start;
    drag.end = event.end;
    drag.allDay = event.allDay;
    m_drag = drag;
}

int TimeGridView::columnAt(const QPoint &global) const
{
    const QPoint at = m_header->mapFromGlobal(global);
    const qreal width = columnWidth(m_header->width());
    const int column = width > 0 ? int(std::floor((at.x() - timeColumnWidth()) / width)) : 0;
    return std::clamp(column, 0, days() - 1);
}

int TimeGridView::secondsAt(const QPoint &global) const
{
    const int y = m_grid->mapFromGlobal(global).y();
    return std::clamp(int(y * 3600.0 / hourHeight), 0, 24 * 3600 - 1);
}

bool TimeGridView::isDragged(const DisplayEvent &event) const
{
    return m_drag && m_drag->started && m_drag->event.id == event.id && m_drag->event.mailboxKey == event.mailboxKey;
}

void TimeGridView::dragMoved(const QPoint &global)
{
    if (!m_drag) {
        return;
    }
    Drag &d = *m_drag;
    if (!d.started) {
        if ((global - d.pressGlobal).manhattanLength() < QApplication::startDragDistance()) {
            return;
        }
        d.started = true;
        const Qt::CursorShape shape = d.mode == Drag::Mode::Move ? Qt::SizeAllCursor : (d.fromHeader ? Qt::SizeHorCursor : Qt::SizeVerCursor);
        m_header->setCursor(shape);
        m_grid->setCursor(shape);
    }
    const DisplayEvent &e = d.event;
    const int column = columnAt(global);
    const QDate day = m_first.addDays(column);
    const int dayShift = column - d.pressColumn;
    // Above the hours (the strip, the day names, or further up) counts as the strip
    const bool inStrip = m_header->mapFromGlobal(global).y() < m_header->height();
    const int seconds = secondsAt(global);
    const qint64 length = e.start.secsTo(e.end);
    // Never shorter than half an hour, or than it was
    const int shortest = int(std::min<qint64>(halfHour, std::max<qint64>(60, length)));

    if (d.fromHeader && d.mode == Drag::Mode::ResizeEnd) {
        // An all-day event: more or fewer days
        const QDate last = std::max(e.firstDay, e.lastDay.addDays(dayShift));
        d.start = QDateTime(e.firstDay, QTime(0, 0));
        d.end = QDateTime(last.addDays(1), QTime(0, 0));
        d.allDay = true;
    } else if (d.mode == Drag::Mode::ResizeEnd || d.mode == Drag::Mode::ResizeStart) {
        // A timed event in its own day: another end or start, in half hours
        const QDateTime dayStart(e.start.date(), QTime(0, 0));
        const int shift = snapped(seconds - d.pressSeconds);
        if (d.mode == Drag::Mode::ResizeEnd) {
            d.start = e.start;
            d.end = std::max(std::min(e.end.addSecs(shift), dayStart.addDays(1)), e.start.addSecs(shortest));
        } else {
            d.start = std::min(std::max(e.start.addSecs(shift), dayStart), e.end.addSecs(-shortest));
            d.end = e.end;
        }
        d.allDay = false;
    } else if (inStrip && (d.fromHeader || !e.allDay)) {
        if (!d.fromHeader) {
            // A timed event dropped in the strip becomes an all-day event that day, as in Outlook
            d.start = QDateTime(day, QTime(0, 0));
            d.end = QDateTime(day.addDays(1), QTime(0, 0));
            d.allDay = true;
        } else if (e.allDay) {
            d.start = QDateTime(e.firstDay.addDays(dayShift), QTime(0, 0));
            d.end = QDateTime(e.lastDay.addDays(dayShift + 1), QTime(0, 0));
            d.allDay = true;
        } else {
            // A timed event over several days: other days, the same times
            d.start = e.start.addDays(dayShift);
            d.end = e.end.addDays(dayShift);
            d.allDay = false;
        }
    } else if (d.fromHeader && e.allDay) {
        // An all-day event dropped in the hours: half an hour from there, as in Outlook
        d.start = QDateTime(day, QTime(0, 0).addSecs(seconds / halfHour * halfHour));
        d.end = d.start.addSecs(halfHour);
        d.allDay = false;
    } else if (d.fromHeader) {
        // A timed event over several days stays in the strip
        d.start = e.start.addDays(dayShift);
        d.end = e.end.addDays(dayShift);
        d.allDay = false;
    } else {
        // A timed event to another time or day: as long as it was, within the day it lands on
        const QDateTime dayStart(day, QTime(0, 0));
        QDateTime start = e.start.addDays(e.start.date().daysTo(day)).addSecs(snapped(seconds - d.pressSeconds));
        if (start < dayStart) {
            start = dayStart;
        }
        if (length <= 24 * 3600 && start.addSecs(length) > dayStart.addDays(1)) {
            start = dayStart.addDays(1).addSecs(-length);
        }
        d.start = start;
        d.end = start.addSecs(length);
        d.allDay = false;
    }
    // Near the top or bottom of the hours they scroll along
    if (!inStrip) {
        const QPoint at = m_grid->mapFromGlobal(global);
        m_scroll->ensureVisible(at.x(), std::clamp(at.y(), 0, m_grid->height() - 1), 0, hourHeight / 2);
    }
    repaintAll();
}

void TimeGridView::dragReleased()
{
    const std::optional<Drag> drag = m_drag;
    cancelDrag();
    if (!drag || !drag->started) {
        return;
    }
    const DisplayEvent &e = drag->event;
    const bool same = drag->allDay && e.allDay ? drag->start.date() == e.firstDay && drag->end.date().addDays(-1) == e.lastDay
                                               : drag->allDay == e.allDay && drag->start == e.start && drag->end == e.end;
    if (same) {
        return;
    }
    // Where it was dropped at once, until the calendar has it from the cache
    showMoved(e, drag->start, drag->end, drag->allDay);
    Q_EMIT eventMoveRequested(e, drag->start, drag->end, drag->allDay);
}

void TimeGridView::eventsMoved()
{
    m_header->reset();
    m_grid->layoutEvents();
    repaintAll();
}

void TimeGridView::cancelDrag()
{
    m_drag.reset();
    m_header->unsetCursor();
    m_grid->unsetCursor();
    repaintAll();
}

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
    // Esc during a drag
    setFocusPolicy(Qt::ClickFocus);
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

int MonthView::weeks() const
{
    // Six for a month; a span chosen in the date picker shows its own whole weeks
    return m_first.isValid() && m_last.isValid() ? std::clamp(int(m_first.daysTo(m_last) + 1 + 6) / 7, 1, 6) : 6;
}

QRectF MonthView::cellRect(int index) const
{
    const qreal top = dayHeader / 1.5;
    const qreal left = m_weekNumbers ? weekColumn : 0;
    const qreal w = (width() - left) / 7.0;
    const qreal h = (height() - top) / std::max(1, weeks());
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
    // Week numbers, counted as chosen in the calendar options
    if (m_weekNumbers) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        for (int row = 0; row < weeks(); ++row) {
            const QRectF cell = cellRect(row * 7);
            p.drawText(QRectF(0, cell.top() + 2, weekColumn - 4, cell.height()), Qt::AlignRight | Qt::AlignTop,
                       QString::number(CalendarOptions::weekNumber(m_first.addDays(row * 7))));
        }
    }
    const QFontMetricsF fm(font);
    const qreal line = fm.height() + 3;
    for (int i = 0; i < weeks() * 7; ++i) {
        const QDate day = m_first.addDays(i);
        const QRectF cell = cellRect(i);
        p.fillRect(cell, day.month() == m_month ? pal.color(QPalette::Base) : pal.color(QPalette::AlternateBase));
        if (m_slot && m_slot->start.date() == day) {
            p.fillRect(cell, selectionColor(pal));
        }
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
    // The days a dragged event lands on
    if (m_drag && m_drag->started && m_drag->day.isValid()) {
        const qint64 length = m_drag->event.firstDay.daysTo(m_drag->event.lastDay);
        QColor frame = pal.color(QPalette::Highlight);
        for (int i = 0; i < weeks() * 7; ++i) {
            const QDate day = m_first.addDays(i);
            if (day >= m_drag->day && day <= m_drag->day.addDays(length)) {
                p.fillRect(cellRect(i), selectionColor(pal));
                p.setPen(QPen(frame, 2));
                p.drawRect(cellRect(i).adjusted(1, 1, -1, -1));
            }
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
    for (int i = 0; i < weeks() * 7; ++i) {
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
    Q_EMIT selectionChanged();
    m_slot.reset();
    m_drag.reset();
    // An event can be dragged to another day (0.14.5)
    if (m_selected && event->button() == Qt::LeftButton && movable(*m_selected)) {
        setFocus(Qt::MouseFocusReason);
        m_drag = Drag{*m_selected, event->globalPosition().toPoint(), dayAt(event->position()), dayAt(event->position()), false};
    }
    // An empty spot: the day, for New Event (at nine, as a double click makes it)
    if (const QDate day = dayAt(event->position()); !m_selected && day.isValid()) {
        m_slot = Slot{QDateTime(day, QTime(9, 0)), QDateTime(day, QTime(9, 30)), false};
    }
    update();
}

void MonthView::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_drag || !(event->buttons() & Qt::LeftButton)) {
        return;
    }
    if (!m_drag->started) {
        if ((event->globalPosition().toPoint() - m_drag->pressGlobal).manhattanLength() < QApplication::startDragDistance()) {
            return;
        }
        m_drag->started = true;
        setCursor(Qt::SizeAllCursor);
    }
    if (const QDate day = dayAt(event->position()); day.isValid()) {
        m_drag->day = day;
    }
    update();
}

void MonthView::mouseReleaseEvent(QMouseEvent *)
{
    const std::optional<Drag> drag = m_drag;
    m_drag.reset();
    unsetCursor();
    update();
    if (!drag || !drag->started || !drag->pressDay.isValid() || !drag->day.isValid() || drag->day == drag->pressDay) {
        return;
    }
    // Another day, the same times (an all-day event its days)
    const DisplayEvent &e = drag->event;
    const qint64 shift = drag->pressDay.daysTo(drag->day);
    const QDateTime start = e.allDay ? QDateTime(e.firstDay.addDays(shift), QTime(0, 0)) : e.start.addDays(shift);
    const QDateTime end = e.allDay ? QDateTime(e.lastDay.addDays(shift + 1), QTime(0, 0)) : e.end.addDays(shift);
    // On its new day at once, until the calendar has it from the cache
    showMoved(e, start, end, e.allDay);
    Q_EMIT eventMoveRequested(e, start, end, e.allDay);
}

void MonthView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && m_drag) {
        m_drag.reset();
        unsetCursor();
        update();
        return;
    }
    CalendarView::keyPressEvent(event);
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
        Q_EMIT selectionChanged();
        m_slot.reset();
        update();
        Q_EMIT eventMenuRequested(*e, event->globalPos());
        return;
    }
    // An empty day: selected, then its menu
    if (const QDate day = dayAt(event->pos()); day.isValid()) {
        m_selected.reset();
        m_slot = Slot{QDateTime(day, QTime(9, 0)), QDateTime(day, QTime(9, 30)), false};
        update();
        Q_EMIT slotMenuRequested(event->globalPos());
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
        Q_EMIT selectionChanged();
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
