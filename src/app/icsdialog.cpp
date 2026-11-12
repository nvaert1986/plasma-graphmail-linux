// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "icsdialog.h"
#include "calendardata.h"
#include "daemonclient.h"
#include "recurrence.h"

#include <KCalendarCore/Event>
#include <KCalendarCore/ICalFormat>
#include <KCalendarCore/MemoryCalendar>
#include <KCalendarCore/Recurrence>
#include <KLocalizedString>

#include <QBitArray>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QTimeZone>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
QString windowsZone()
{
    const QByteArray windows = QTimeZone::ianaIdToWindowsId(QTimeZone::systemTimeZoneId());
    return windows.isEmpty() ? QStringLiteral("UTC") : QString::fromLatin1(windows);
}

QJsonObject graphTime(const QDateTime &local)
{
    return {{QStringLiteral("dateTime"), local.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))}, {QStringLiteral("timeZone"), windowsZone()}};
}

// The simple iCalendar rules as Graph's recurrence; empty when the rule is more than that
QJsonObject recurrenceOf(const KCalendarCore::Event::Ptr &event)
{
    if (!event->recurs()) {
        return {};
    }
    const KCalendarCore::Recurrence *r = event->recurrence();
    const QDate start = event->dtStart().toLocalTime().date();
    RepeatPreset preset;
    switch (r->recurrenceType()) {
    case KCalendarCore::Recurrence::rDaily:
        preset = RepeatPreset::Daily;
        break;
    case KCalendarCore::Recurrence::rWeekly:
        preset = RepeatPreset::Weekly;
        break;
    case KCalendarCore::Recurrence::rMonthlyDay:
        preset = RepeatPreset::Monthly;
        break;
    case KCalendarCore::Recurrence::rYearlyMonth:
        preset = RepeatPreset::Yearly;
        break;
    default:
        return {};
    }
    QJsonObject recurrence = presetRecurrence(preset, start);
    QJsonObject pattern = recurrence.value(QLatin1String("pattern")).toObject();
    pattern.insert(QStringLiteral("interval"), std::max(1, r->frequency()));
    if (preset == RepeatPreset::Weekly) {
        const QBitArray days = r->days();
        QJsonArray names;
        for (int d = 0; d < days.size() && d < 7; ++d) {
            if (days.testBit(d)) {
                names.append(graphDayName(d + 1));
            }
        }
        if (!names.isEmpty()) {
            pattern.insert(QStringLiteral("daysOfWeek"), names);
        }
    }
    recurrence.insert(QStringLiteral("pattern"), pattern);
    QJsonObject range = recurrence.value(QLatin1String("range")).toObject();
    if (r->duration() > 0) {
        range.insert(QStringLiteral("type"), QStringLiteral("numbered"));
        range.insert(QStringLiteral("numberOfOccurrences"), r->duration());
    } else if (r->duration() == 0) {
        range.insert(QStringLiteral("type"), QStringLiteral("endDate"));
        range.insert(QStringLiteral("endDate"), r->endDate().toString(Qt::ISODate));
    }
    recurrence.insert(QStringLiteral("range"), range);
    return recurrence;
}
}

IcsDialog *IcsDialog::open(const QString &path, MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent)
{
    QFile file(path);
    if (!mailboxes || !daemon || !file.open(QIODevice::ReadOnly)) {
        return nullptr;
    }
    auto calendar = KCalendarCore::MemoryCalendar::Ptr(new KCalendarCore::MemoryCalendar(QTimeZone::systemTimeZone()));
    KCalendarCore::ICalFormat format;
    if (!format.fromRawString(calendar, file.readAll())) {
        return nullptr;
    }
    QList<QJsonObject> events;
    QStringList notes;
    const KCalendarCore::Event::List list = calendar->rawEvents();
    for (const KCalendarCore::Event::Ptr &e : list) {
        QJsonObject json{{QStringLiteral("subject"), e->summary()}};
        if (e->allDay()) {
            const QDate first = e->dtStart().date();
            // iCalendar's all-day end is exclusive already when it has one
            const QDate last = e->hasEndDate() ? std::max(first, e->dtEnd().date()) : first;
            json.insert(QStringLiteral("start"), graphTime(QDateTime(first, QTime(0, 0))));
            json.insert(QStringLiteral("end"), graphTime(QDateTime(last.addDays(1), QTime(0, 0))));
            json.insert(QStringLiteral("isAllDay"), true);
            json.insert(QStringLiteral("showAs"), QStringLiteral("free"));
        } else {
            const QDateTime start = e->dtStart().toLocalTime();
            const QDateTime end = e->hasEndDate() ? e->dtEnd().toLocalTime() : start.addSecs(1800);
            json.insert(QStringLiteral("start"), graphTime(start));
            json.insert(QStringLiteral("end"), graphTime(end));
        }
        if (!e->location().isEmpty()) {
            json.insert(QStringLiteral("location"), QJsonObject{{QStringLiteral("displayName"), e->location()}});
        }
        if (!e->description().isEmpty()) {
            json.insert(QStringLiteral("body"),
                        QJsonObject{{QStringLiteral("contentType"), e->descriptionIsRich() ? QStringLiteral("HTML") : QStringLiteral("Text")}, {QStringLiteral("content"), e->description()}});
        }
        const QJsonObject recurrence = recurrenceOf(e);
        if (!recurrence.isEmpty()) {
            json.insert(QStringLiteral("recurrence"), recurrence);
        } else if (e->recurs()) {
            notes.append(i18nc("@info", "\"%1\" repeats in a way Graph Mail cannot copy: it is added once.", e->summary()));
        }
        events.append(json);
    }
    if (events.isEmpty()) {
        return nullptr;
    }
    auto *dialog = new IcsDialog(events, notes, mailboxes, daemon, parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    return dialog;
}

IcsDialog::IcsDialog(const QList<QJsonObject> &events, const QStringList &notes, MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent)
    : QDialog(parent)
    , m_events(events)
    , m_daemon(daemon)
    , m_data(new CalendarData(mailboxes, this))
    , m_list(new QListWidget(this))
    , m_calendar(new QComboBox(this))
    , m_status(new QLabel(this))
{
    setWindowTitle(i18nc("@title:window", "Calendar File"));
    for (const QJsonObject &e : std::as_const(m_events)) {
        const QDateTime start = QDateTime::fromString(e.value(QLatin1String("start")).toObject().value(QLatin1String("dateTime")).toString(), Qt::ISODate);
        const QString when = e.value(QLatin1String("isAllDay")).toBool() ? QLocale().toString(start.date(), QStringLiteral("ddd d MMM yyyy"))
                                                                          : QLocale().toString(start, QStringLiteral("ddd d MMM yyyy HH:mm"));
        QString text = when + QStringLiteral("  ") + e.value(QLatin1String("subject")).toString();
        const QString location = e.value(QLatin1String("location")).toObject().value(QLatin1String("displayName")).toString();
        if (!location.isEmpty()) {
            text += QStringLiteral(" (") + location + QLatin1Char(')');
        }
        if (e.contains(QLatin1String("recurrence"))) {
            text += QStringLiteral("\n    ") + describeRecurrence(e.value(QLatin1String("recurrence")).toObject());
        }
        auto *item = new QListWidgetItem(text, m_list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
    const QList<CalendarInfo> writable = m_data->writable();
    for (const CalendarInfo &c : writable) {
        QPixmap swatch(12, 12);
        swatch.fill(c.color);
        const QString label = c.group == c.name ? c.name : c.name + QStringLiteral(" (") + c.group + QLatin1Char(')');
        m_calendar->addItem(QIcon(swatch), label, QString(c.mailboxKey + QLatin1Char('|') + QString::number(c.id)));
    }
    m_status->setWordWrap(true);
    m_status->setText(notes.join(QLatin1Char('\n')));
    m_status->setVisible(!notes.isEmpty());

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton *addButton = buttons->addButton(i18nc("@action:button", "Add to Calendar"), QDialogButtonBox::AcceptRole);
    addButton->setIcon(QIcon::fromTheme(QStringLiteral("appointment-new")));
    addButton->setEnabled(m_calendar->count() > 0);
    connect(addButton, &QPushButton::clicked, this, &IcsDialog::add);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(i18ncp("@info", "This file holds %1 event:", "This file holds %1 events:", int(m_events.size())), this));
    layout->addWidget(m_list, 1);
    layout->addWidget(m_status);
    layout->addWidget(new QLabel(i18nc("@label:listbox", "Add to:"), this));
    layout->addWidget(m_calendar);
    layout->addWidget(buttons);
    resize(560, 380);
}

void IcsDialog::add()
{
    const QString target = m_calendar->currentData().toString();
    const QString key = target.section(QLatin1Char('|'), 0, 0);
    const qint64 calendar = target.section(QLatin1Char('|'), 1).toLongLong();
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->checkState() == Qt::Checked) {
            m_daemon->createEvent(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), calendar,
                                  QString::fromUtf8(QJsonDocument(m_events.at(i)).toJson(QJsonDocument::Compact)));
        }
    }
    accept();
}

}
