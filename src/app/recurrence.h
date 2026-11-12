// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDate>
#include <QDialog>
#include <QJsonObject>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDateEdit;
class QLabel;
class QRadioButton;
class QSpinBox;
class QStackedWidget;

namespace GraphMail
{

// Graph's recurrence ({pattern, range}) as people say it: "Every 2 weeks on Monday and
// Wednesday, until 31 December 2026"
QString describeRecurrence(const QJsonObject &recurrence);

// The quick choices of the Repeat box, starting on `start`
enum class RepeatPreset { Daily, Weekdays, Weekly, Monthly, Yearly };
QJsonObject presetRecurrence(RepeatPreset preset, QDate start);

// The full choice: daily, weekly on days, monthly on a day or "the second Tuesday", yearly; ends
// never, on a date or after a number of times
class RecurrenceDialog : public QDialog
{
    Q_OBJECT

public:
    RecurrenceDialog(const QJsonObject &recurrence, QDate start, QWidget *parent = nullptr);
    QJsonObject recurrence() const;

private:
    void updateSummary();

    QDate m_start;
    QComboBox *m_type;
    QSpinBox *m_interval;
    QLabel *m_unit;
    QStackedWidget *m_details;
    QList<QCheckBox *> m_days;
    QRadioButton *m_monthlyOnDay;
    QSpinBox *m_dayOfMonth;
    QRadioButton *m_monthlyRelative;
    QComboBox *m_index;
    QComboBox *m_weekday;
    QComboBox *m_month;
    QRadioButton *m_noEnd;
    QRadioButton *m_endDate;
    QDateEdit *m_until;
    QRadioButton *m_numbered;
    QSpinBox *m_count;
    QLabel *m_summary;
};

// Graph's day names (sunday ... saturday) from Qt's day numbers (1 Monday ... 7 Sunday) and back
QString graphDayName(int dayOfWeek);
int dayOfWeekFromGraph(const QString &name);

}
