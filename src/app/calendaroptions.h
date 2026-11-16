// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDate>
#include <QDialog>

class QComboBox;

// Settings > Calendar Options (Outlook's Calendar options "Calendar" and "Reminders"): the first
// day of the week, how the first week of the year is counted and the default reminders of new
// events. Kept in graphmailrc [Calendar].
namespace GraphMail::CalendarOptions
{

enum class FirstWeek {
    // The week with 1 January in it (Outlook's default "Starts on Jan 1")
    JanuaryFirst,
    // The first week with four days in the new year (ISO 8601, as before)
    FirstFourDays,
    // The first week that lies wholly in the new year
    FirstFullWeek,
};

// The chosen day, or the one of the language settings
Qt::DayOfWeek firstDayOfWeek();
FirstWeek firstWeek();
// Minutes before the start, -1 for none
int defaultReminder(bool allDay);
// KDE's holiday region shown in the calendar ("nl_nl"), empty for none
QString holidayRegion();

// The first day of the week that holds the date
QDate weekStart(QDate date, Qt::DayOfWeek first);
QDate weekStart(QDate date);
int weekNumber(QDate date, Qt::DayOfWeek first, FirstWeek rule);
int weekNumber(QDate date);

// The reminder choices of the event dialog, minutes before the start (-1 none)
const QList<int> &reminderChoices();
QString reminderText(int minutes);

class Dialog : public QDialog
{
    Q_OBJECT

public:
    explicit Dialog(QWidget *parent = nullptr);
    void accept() override;

private:
    QComboBox *m_firstDay;
    QComboBox *m_firstWeek;
    QComboBox *m_reminder;
    QComboBox *m_allDayReminder;
    QComboBox *m_holidays;
};

}
