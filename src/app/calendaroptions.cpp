// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "calendaroptions.h"

#include <KConfigGroup>
#include <KHolidays/HolidayRegion>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>
#include <QVBoxLayout>

namespace GraphMail::CalendarOptions
{

namespace
{
KConfigGroup group()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Calendar"));
}

QString name(FirstWeek rule)
{
    switch (rule) {
    case FirstWeek::JanuaryFirst:
        return QStringLiteral("januaryFirst");
    case FirstWeek::FirstFullWeek:
        return QStringLiteral("firstFullWeek");
    case FirstWeek::FirstFourDays:
        break;
    }
    return QStringLiteral("firstFourDays");
}

// The start of week 1 of the year
QDate firstWeekStart(int year, Qt::DayOfWeek first, FirstWeek rule)
{
    const QDate january = QDate(year, 1, 1);
    const QDate start = weekStart(january, first);
    const qint64 before = start.daysTo(january);
    switch (rule) {
    case FirstWeek::JanuaryFirst:
        return start;
    case FirstWeek::FirstFullWeek:
        return before == 0 ? start : start.addDays(7);
    case FirstWeek::FirstFourDays:
        break;
    }
    return 7 - before >= 4 ? start : start.addDays(7);
}

void fillReminders(QComboBox *box, int current)
{
    for (int minutes : reminderChoices()) {
        box->addItem(reminderText(minutes), minutes);
    }
    box->setCurrentIndex(std::max(0, box->findData(current)));
}
}

Qt::DayOfWeek firstDayOfWeek()
{
    const int day = group().readEntry("FirstDayOfWeek", 0);
    return day >= 1 && day <= 7 ? Qt::DayOfWeek(day) : QLocale().firstDayOfWeek();
}

FirstWeek firstWeek()
{
    const QString text = group().readEntry("FirstWeekOfYear", QString());
    if (text == QLatin1String("januaryFirst")) {
        return FirstWeek::JanuaryFirst;
    }
    if (text == QLatin1String("firstFullWeek")) {
        return FirstWeek::FirstFullWeek;
    }
    return FirstWeek::FirstFourDays;
}

int defaultReminder(bool allDay)
{
    // Outlook's defaults: 15 minutes, and 18 hours (the morning before) for a whole day
    return allDay ? group().readEntry("DefaultAllDayReminder", 18 * 60) : group().readEntry("DefaultReminder", 15);
}

QString holidayRegion()
{
    return group().readEntry("HolidayRegion", QString());
}

QDate weekStart(QDate date, Qt::DayOfWeek first)
{
    return date.addDays(-((date.dayOfWeek() - first + 7) % 7));
}

QDate weekStart(QDate date)
{
    return weekStart(date, firstDayOfWeek());
}

int weekNumber(QDate date, Qt::DayOfWeek first, FirstWeek rule)
{
    const QDate week = weekStart(date, first);
    // A week at the end of December can already be week 1 of the next year
    if (week >= firstWeekStart(date.year() + 1, first, rule)) {
        return 1;
    }
    QDate one = firstWeekStart(date.year(), first, rule);
    if (week < one) {
        one = firstWeekStart(date.year() - 1, first, rule);
    }
    return int(one.daysTo(week) / 7) + 1;
}

int weekNumber(QDate date)
{
    return weekNumber(date, firstDayOfWeek(), firstWeek());
}

const QList<int> &reminderChoices()
{
    static const QList<int> minutes{-1, 0, 5, 10, 15, 30, 60, 120, 12 * 60, 18 * 60, 24 * 60, 2 * 24 * 60, 7 * 24 * 60};
    return minutes;
}

QString reminderText(int minutes)
{
    if (minutes < 0) {
        return i18nc("@item:inlistbox no reminder", "None");
    }
    if (minutes == 0) {
        return i18nc("@item:inlistbox reminder", "At the start");
    }
    if (minutes < 60) {
        return i18ncp("@item:inlistbox reminder", "%1 minute before", "%1 minutes before", minutes);
    }
    if (minutes < 24 * 60) {
        return i18ncp("@item:inlistbox reminder", "%1 hour before", "%1 hours before", minutes / 60);
    }
    if (minutes < 7 * 24 * 60) {
        return i18ncp("@item:inlistbox reminder", "%1 day before", "%1 days before", minutes / (24 * 60));
    }
    return i18ncp("@item:inlistbox reminder", "%1 week before", "%1 weeks before", minutes / (7 * 24 * 60));
}

Dialog::Dialog(QWidget *parent)
    : QDialog(parent)
    , m_firstDay(new QComboBox(this))
    , m_firstWeek(new QComboBox(this))
    , m_reminder(new QComboBox(this))
    , m_allDayReminder(new QComboBox(this))
    , m_holidays(new QComboBox(this))
{
    setWindowTitle(i18nc("@title:window", "Calendar Options"));
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_firstDay->addItem(i18nc("@item:inlistbox first day of the week", "As in the language settings (%1)", QLocale().standaloneDayName(QLocale().firstDayOfWeek())), 0);
    for (int day = 1; day <= 7; ++day) {
        m_firstDay->addItem(QLocale().standaloneDayName(day), day);
    }
    m_firstDay->setCurrentIndex(std::max(0, m_firstDay->findData(group().readEntry("FirstDayOfWeek", 0))));
    m_firstWeek->addItem(i18nc("@item:inlistbox first week of the year", "Starts on 1 January"), int(FirstWeek::JanuaryFirst));
    m_firstWeek->addItem(i18nc("@item:inlistbox first week of the year", "First four-day week (ISO 8601)"), int(FirstWeek::FirstFourDays));
    m_firstWeek->addItem(i18nc("@item:inlistbox first week of the year", "First full week"), int(FirstWeek::FirstFullWeek));
    m_firstWeek->setCurrentIndex(m_firstWeek->findData(int(firstWeek())));
    fillReminders(m_reminder, defaultReminder(false));
    fillReminders(m_allDayReminder, defaultReminder(true));
    // Outlook's Add Holidays: one region, its public holidays in every view
    QList<QPair<QString, QString>> regions;
    const QStringList codes = KHolidays::HolidayRegion::regionCodes();
    for (const QString &code : codes) {
        regions.append({KHolidays::HolidayRegion::name(code), code});
    }
    std::sort(regions.begin(), regions.end(), [](const auto &a, const auto &b) {
        return QString::localeAwareCompare(a.first, b.first) < 0;
    });
    m_holidays->addItem(i18nc("@item:inlistbox no holidays", "None"), QString());
    for (const auto &[name, code] : std::as_const(regions)) {
        m_holidays->addItem(name, code);
    }
    m_holidays->setCurrentIndex(std::max(0, m_holidays->findData(holidayRegion())));

    form->addRow(i18nc("@label:listbox", "First day of the week:"), m_firstDay);
    form->addRow(i18nc("@label:listbox", "First week of the year:"), m_firstWeek);
    form->addRow(i18nc("@label:listbox", "Default reminder:"), m_reminder);
    form->addRow(i18nc("@label:listbox", "Default reminder for all-day events:"), m_allDayReminder);
    form->addRow(i18nc("@label:listbox", "Holidays:"), m_holidays);
    layout->addLayout(form);
    auto *note = new QLabel(i18nc("@info", "These settings are kept on this computer. The reminders apply to new events, not to invitations you receive."), this);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void Dialog::accept()
{
    KConfigGroup g = group();
    g.writeEntry("FirstDayOfWeek", m_firstDay->currentData().toInt());
    g.writeEntry("FirstWeekOfYear", name(FirstWeek(m_firstWeek->currentData().toInt())));
    g.writeEntry("DefaultReminder", m_reminder->currentData().toInt());
    g.writeEntry("DefaultAllDayReminder", m_allDayReminder->currentData().toInt());
    g.writeEntry("HolidayRegion", m_holidays->currentData().toString());
    g.sync();
    QDialog::accept();
}

}
