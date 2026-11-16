// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "mailboxsettings.h"

#include <QTimeZone>

#include "calendardata.h"

#include <QWidget>

class QPainter;
class QScrollArea;
class QTreeWidget;

namespace GraphMail
{

// What every view of the calendar window offers
class CalendarView : public QWidget
{
    Q_OBJECT

public:
    using QWidget::QWidget;
    // The events of the days [first, last]
    virtual void setEvents(QDate first, QDate last, const QList<DisplayEvent> &events) = 0;
    // The selected event, if any (Delete, the context menu)
    std::optional<DisplayEvent> selected() const;
    // The selected time without an event, as Outlook marks it blue: a click selects a half hour
    // (a day in the all-day strip and the month), a drag a longer time; New Event takes it
    struct Slot {
        QDateTime start;
        QDateTime end;
        bool allDay = false;
    };
    std::optional<Slot> selectedSlot() const
    {
        return m_slot;
    }
    void setSelectedSlot(const Slot &slot)
    {
        m_slot = slot;
        m_selected.reset();
        repaintSelection();
    }

protected:
    virtual void repaintSelection()
    {
        update();
    }

public:

Q_SIGNALS:
    void eventActivated(const GraphMail::DisplayEvent &event);
    void eventMenuRequested(const GraphMail::DisplayEvent &event, const QPoint &globalPos);
    void newEventRequested(const QDateTime &start, const QDateTime &end, bool allDay);
    // Typing on a selected time: a new event with what was typed as its title, as in Outlook
    void newEventTyped(const QDateTime &start, const QDateTime &end, bool allDay, const QString &text);
    // Right click on empty time (the selection is set first): New Event, New Meeting, ...
    void slotMenuRequested(const QPoint &globalPos);
    void dayActivated(QDate day);
    // The selected event changed (a click), for actions such as Join
    void selectionChanged();

protected:
    std::optional<DisplayEvent> m_selected;
    std::optional<Slot> m_slot;
};

// Day, work week and week: a column per day, the hours down, all-day events on top
class TimeGridView : public CalendarView
{
    Q_OBJECT

public:
    explicit TimeGridView(QWidget *parent = nullptr);
    void setEvents(QDate first, QDate last, const QList<DisplayEvent> &events) override;
    void scrollToHour(int hour);
    // The mailbox's working hours (docs/PHASE9.md): what is shaded lighter
    void setWorkingHours(const WorkingHours &hours);
    // A second time zone (docs/PHASE9.md): its hours in a column of their own, as Outlook's;
    // an invalid zone takes it away
    void setSecondTimeZone(const QTimeZone &zone, const QString &label, const QString &ownLabel);
    // Printing (docs/PHASE9B.md): the day headers and the hours fromHour..toHour, scaled into the
    // rectangle, on paper colours
    void printTo(QPainter *painter, const QRectF &target, int fromHour, int toHour);
    // The working day of the first day shown, for the hours to print
    QPair<int, int> printHours() const;
    // Tests: the hour grid, to send it mouse events
    QWidget *gridForTests() const;

protected:
    void repaintSelection() override
    {
        repaintAll();
    }

private:
    class Header;
    class Grid;
    friend class Header;
    friend class Grid;

    struct Placed {
        QRectF rect;
        DisplayEvent event;
    };
    int days() const;
    void repaintAll();
    // Column geometry, shared by the header and the grid
    qreal columnX(int day, int width) const;
    qreal columnWidth(int width) const;
    // The hours on the left: one column, two with a second time zone
    int timeColumnWidth() const;
    QColor dayBackground(QDate day, bool working) const;

    QDate m_first;
    QDate m_last;
    QList<DisplayEvent> m_events;
    WorkingHours m_workingHours;
    QTimeZone m_secondZone;
    QString m_secondLabel;
    QString m_ownLabel;
    Header *m_header;
    QScrollArea *m_scroll;
    Grid *m_grid;
};

// A month: six weeks of days with their events
class MonthView : public CalendarView
{
    Q_OBJECT

public:
    explicit MonthView(QWidget *parent = nullptr);
    // first/last: the whole weeks shown; the month is the one of the middle day
    void setEvents(QDate first, QDate last, const QList<DisplayEvent> &events) override;
    // ISO week numbers in a column on the left (docs/PHASE9.md)
    void setWeekNumbers(bool on);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    // The rows shown: six for a month, fewer for a span of weeks
    int weeks() const;
    QRectF cellRect(int index) const;
    std::optional<DisplayEvent> eventAt(const QPointF &pos) const;
    QDate dayAt(const QPointF &pos) const;

    QDate m_first;
    QDate m_last;
    int m_month = 0;
    bool m_weekNumbers = false;
    QList<DisplayEvent> m_events;
    QList<QPair<QRectF, DisplayEvent>> m_placed;
    QList<QPair<QRectF, QDate>> m_more;
};

// A list of the coming days with their events
class AgendaView : public CalendarView
{
    Q_OBJECT

public:
    explicit AgendaView(QWidget *parent = nullptr);
    void setEvents(QDate first, QDate last, const QList<DisplayEvent> &events) override;

private:
    QTreeWidget *m_tree;
    QList<DisplayEvent> m_events;
};

// "09:00 - 10:30", or "All day"
QString eventTimeText(const DisplayEvent &event);

}
