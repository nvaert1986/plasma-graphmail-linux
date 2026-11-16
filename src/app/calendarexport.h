// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "calendardata.h"

#include <QDialog>

class QComboBox;
class QDateEdit;

namespace GraphMail
{

// File > Export Calendar (Outlook's Save Calendar, docs/PHASE9B.md 9b.29): one calendar's events
// in a date range as an iCalendar file. A repeating meeting comes along as its occurrences.
namespace CalendarExport
{
QByteArray toIcs(const QList<DisplayEvent> &events, const QString &calendarName);
}

class CalendarExportDialog : public QDialog
{
    Q_OBJECT

public:
    CalendarExportDialog(CalendarData *data, QWidget *parent = nullptr);
    // The chosen calendar's key in CalendarData, and the range
    CalendarInfo calendar() const;
    QDate from() const;
    QDate to() const;

private:
    QList<CalendarInfo> m_calendars;
    QComboBox *m_calendar;
    QDateEdit *m_from;
    QDateEdit *m_to;
};

}
