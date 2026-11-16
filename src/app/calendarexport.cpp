// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "calendarexport.h"

#include <KCalendarCore/Event>
#include <KCalendarCore/ICalFormat>
#include <KCalendarCore/MemoryCalendar>
#include <KLocalizedString>

#include <QComboBox>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonObject>
#include <QLabel>
#include <QSet>
#include <QTimeZone>

namespace GraphMail
{

QByteArray CalendarExport::toIcs(const QList<DisplayEvent> &events, const QString &calendarName)
{
    auto calendar = KCalendarCore::MemoryCalendar::Ptr::create(QTimeZone::systemTimeZone());
    calendar->setName(calendarName);
    QSet<QString> uids;
    for (const DisplayEvent &e : events) {
        auto event = KCalendarCore::Event::Ptr::create();
        // Exchange's iCalendar id; occurrences of a series share it, so they get their date
        QString uid = e.json.value(QLatin1String("iCalUId")).toString();
        if (uid.isEmpty()) {
            uid = e.graphId;
        }
        if (e.recurring() || uids.contains(uid)) {
            uid += QLatin1Char('-') + e.firstDay.toString(Qt::ISODate);
        }
        uids.insert(uid);
        event->setUid(uid);
        event->setSummary(e.subject);
        event->setLocation(e.location);
        event->setDescription(e.json.value(QLatin1String("bodyPreview")).toString());
        if (e.allDay) {
            event->setDtStart(QDateTime(e.firstDay, QTime(0, 0)));
            event->setDtEnd(QDateTime(e.lastDay, QTime(0, 0)));
            event->setAllDay(true);
        } else {
            event->setDtStart(e.start);
            event->setDtEnd(e.end);
        }
        // Free, tentative, busy: free items do not block time in other programs
        event->setTransparency(e.showAs == 0 ? KCalendarCore::Event::Transparent : KCalendarCore::Event::Opaque);
        if (e.isPrivate) {
            event->setSecrecy(KCalendarCore::Incidence::SecrecyPrivate);
        }
        calendar->addEvent(event);
    }
    return KCalendarCore::ICalFormat().toString(calendar).toUtf8();
}

CalendarExportDialog::CalendarExportDialog(CalendarData *data, QWidget *parent)
    : QDialog(parent)
    , m_calendars(data->calendars())
    , m_calendar(new QComboBox(this))
    , m_from(new QDateEdit(QDate::currentDate().addMonths(-1), this))
    , m_to(new QDateEdit(QDate::currentDate().addYears(1), this))
{
    setWindowTitle(i18nc("@title:window", "Export Calendar"));
    // Free/busy of colleagues and holidays are not calendars of the cache
    m_calendars.erase(std::remove_if(m_calendars.begin(), m_calendars.end(),
                                     [](const CalendarInfo &c) {
                                         return CalendarData::isFreeBusy(c.mailboxKey);
                                     }),
                      m_calendars.end());
    for (const CalendarInfo &c : std::as_const(m_calendars)) {
        m_calendar->addItem(c.group.isEmpty() ? c.name : QStringLiteral("%1 (%2)").arg(c.name, c.group));
    }
    m_from->setCalendarPopup(true);
    m_to->setCalendarPopup(true);
    auto *form = new QFormLayout(this);
    form->addRow(i18nc("@label:listbox", "Calendar:"), m_calendar);
    form->addRow(i18nc("@label:chooser", "From:"), m_from);
    form->addRow(i18nc("@label:chooser", "Until:"), m_to);
    auto *note = new QLabel(i18nc("@info", "Only what this computer has of the calendar goes into the file (see Offline Settings)."), this);
    note->setWordWrap(true);
    form->addRow(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);
}

CalendarInfo CalendarExportDialog::calendar() const
{
    return m_calendars.value(m_calendar->currentIndex());
}

QDate CalendarExportDialog::from() const
{
    return m_from->date();
}

QDate CalendarExportDialog::to() const
{
    return m_to->date();
}

}
