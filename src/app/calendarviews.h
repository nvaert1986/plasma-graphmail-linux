// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "mailboxsettings.h"

#include <QTimeZone>

#include "calendardata.h"

#include <QWidget>

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

Q_SIGNALS:
    void eventActivated(const GraphMail::DisplayEvent &event);
    void eventMenuRequested(const GraphMail::DisplayEvent &event, const QPoint &globalPos);
    void newEventRequested(const QDateTime &start, const QDateTime &end, bool allDay);
    void dayActivated(QDate day);

protected:
    std::optional<DisplayEvent> m_selected;
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
