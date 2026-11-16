// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "eventdialog.h"
#include "accesstext.h"
#include "calendaroptions.h"
#include "calendarviews.h"
#include "daemonclient.h"
#include "mailboxset.h"
#include "recipientedit.h"
#include "recurrence.h"
#include "store.h"

#include <KLocalizedString>
#include <KMessageBox>
#include <QDateTimeEdit>
#include <KSharedConfig>
#include <KConfigGroup>

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QTextEdit>
#include <QToolButton>
#include <QTimeEdit>
#include <QTimeZone>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
const QStringList &showAsNames()
{
    static const QStringList names{QStringLiteral("free"), QStringLiteral("tentative"), QStringLiteral("busy"), QStringLiteral("oof"), QStringLiteral("workingElsewhere")};
    return names;
}

QString windowsZone()
{
    const QByteArray windows = QTimeZone::ianaIdToWindowsId(QTimeZone::systemTimeZoneId());
    return windows.isEmpty() ? QStringLiteral("UTC") : QString::fromLatin1(windows);
}

// Graph's dateTimeTimeZone for a time in a zone of its own (Windows names, as Exchange keeps them)
QJsonObject graphTime(const QDateTime &time, const QTimeZone &zone)
{
    const QByteArray windows = QTimeZone::ianaIdToWindowsId(zone.id());
    if (windows.isEmpty() || windows == "UTC") {
        return {{QStringLiteral("dateTime"), time.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}};
    }
    return {{QStringLiteral("dateTime"), time.toTimeZone(zone).toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))}, {QStringLiteral("timeZone"), QString::fromLatin1(windows)}};
}

// The zones the pickers offer: those Exchange knows (they have a Windows name), by offset
QList<QPair<QString, QByteArray>> zoneChoices()
{
    static QList<QPair<QString, QByteArray>> choices;
    if (choices.isEmpty()) {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QList<QPair<int, QByteArray>> zones;
        const QList<QByteArray> ids = QTimeZone::availableTimeZoneIds();
        for (const QByteArray &id : ids) {
            if (id.contains('/') && !QTimeZone::ianaIdToWindowsId(id).isEmpty()) {
                zones.append({QTimeZone(id).offsetFromUtc(now), id});
            }
        }
        std::sort(zones.begin(), zones.end());
        for (const auto &[offset, id] : std::as_const(zones)) {
            const QString sign = offset < 0 ? QStringLiteral("-") : QStringLiteral("+");
            const int minutes = std::abs(offset) / 60;
            const QString label = QStringLiteral("(UTC%1%2:%3) %4")
                                      .arg(sign)
                                      .arg(minutes / 60, 2, 10, QLatin1Char('0'))
                                      .arg(minutes % 60, 2, 10, QLatin1Char('0'))
                                      .arg(QString::fromLatin1(id).replace(QLatin1Char('_'), QLatin1Char(' ')));
            choices.append({label, id});
        }
    }
    return choices;
}

// The zone of Exchange's originalStartTimeZone / originalEndTimeZone (a Windows name)
QTimeZone zoneOf(const QString &windows)
{
    if (windows.isEmpty()) {
        return {};
    }
    QTimeZone zone(QTimeZone::windowsIdToDefaultIanaId(windows.toUtf8()));
    if (!zone.isValid()) {
        zone = QTimeZone(windows.toUtf8());
    }
    return zone;
}

// Graph's dateTimeTimeZone for a local time
QJsonObject graphTime(const QDateTime &local)
{
    if (windowsZone() == QLatin1String("UTC")) {
        return {{QStringLiteral("dateTime"), local.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}};
    }
    return {{QStringLiteral("dateTime"), local.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))}, {QStringLiteral("timeZone"), windowsZone()}};
}

QList<OutgoingRecipient> attendeesOfType(const QJsonObject &event, const QString &type)
{
    QList<OutgoingRecipient> result;
    const QJsonArray attendees = event.value(QLatin1String("attendees")).toArray();
    for (const QJsonValue &v : attendees) {
        const QJsonObject a = v.toObject();
        if (a.value(QLatin1String("type")).toString() == type) {
            const QJsonObject address = a.value(QLatin1String("emailAddress")).toObject();
            result.append({address.value(QLatin1String("name")).toString(), address.value(QLatin1String("address")).toString()});
        }
    }
    return result;
}

// What identifies the attendee list, for "did it change"
QStringList attendeeKeys(const QJsonArray &attendees)
{
    QStringList keys;
    for (const QJsonValue &v : attendees) {
        const QJsonObject a = v.toObject();
        keys.append(a.value(QLatin1String("type")).toString() + QLatin1Char(':')
                    + a.value(QLatin1String("emailAddress")).toObject().value(QLatin1String("address")).toString().toLower());
    }
    keys.sort();
    return keys;
}

qint64 timeOf(const QJsonValue &value)
{
    const QJsonObject t = value.toObject();
    QDateTime dt = QDateTime::fromString(t.value(QLatin1String("dateTime")).toString().left(19), Qt::ISODate);
    const QString zone = t.value(QLatin1String("timeZone")).toString();
    if (zone.isEmpty() || zone == QLatin1String("UTC")) {
        dt.setTimeZone(QTimeZone::UTC);
    } else {
        QTimeZone tz(zone.toUtf8());
        if (!tz.isValid()) {
            tz = QTimeZone(QTimeZone::windowsIdToDefaultIanaId(zone.toUtf8()));
        }
        dt.setTimeZone(tz.isValid() ? tz : QTimeZone::systemTimeZone());
    }
    return dt.toMSecsSinceEpoch();
}
}

EventDialog::EventDialog(MailboxSet *mailboxes, DaemonClient *daemon, CalendarData *data, const EventEditRequest &request, QWidget *parent)
    : QDialog(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
    , m_data(data)
    , m_request(request)
    , m_book(new AddressBook(mailboxes, this))
    , m_calendar(new QComboBox(this))
    , m_calendarLabel(new QLabel(this))
    , m_subject(new QLineEdit(this))
    , m_required(new RecipientEdit(m_book, this))
    , m_optional(new RecipientEdit(m_book, this))
    , m_rooms(new QPushButton(QIcon::fromTheme(QStringLiteral("go-home")), i18nc("@action:button", "Rooms..."), this))
    , m_startDate(new QDateEdit(this))
    , m_startTime(new QTimeEdit(this))
    , m_endDate(new QDateEdit(this))
    , m_endTime(new QTimeEdit(this))
    , m_allDay(new QCheckBox(i18nc("@option:check", "All day"), this))
    , m_repeat(new QComboBox(this))
    , m_location(new QLineEdit(this))
    , m_teams(new QCheckBox(i18nc("@option:check", "Teams meeting"), this))
    , m_showAs(new QComboBox(this))
    , m_reminder(new QComboBox(this))
    , m_private(new QCheckBox(i18nc("@option:check", "Private"), this))
    , m_allowProposals(new QCheckBox(i18nc("@option:check", "Allow new time proposals"), this))
    , m_hideAttendees(new QCheckBox(i18nc("@option:check", "Hide attendee list"), this))
    , m_description(new QTextEdit(this))
    , m_schedule(new SchedulingStrip(this))
    , m_responseLabel(new QLabel(this))
    , m_responseRow(new QWidget(this))
    , m_join(new QPushButton(QIcon::fromTheme(QStringLiteral("call-start")), i18nc("@action:button", "Join Teams Meeting"), this))
    , m_delete(new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action:button", "Delete"), this))
{
    setWindowTitle(i18nc("@title:window", "New Event"));
    for (QDateEdit *d : {m_startDate, m_endDate}) {
        d->setCalendarPopup(true);
        d->setDisplayFormat(QLocale().name() == QLatin1String("C") ? QStringLiteral("ddd dd-MM-yyyy") : QLocale().dateFormat(QLocale::ShortFormat));
    }
    for (QTimeEdit *t : {m_startTime, m_endTime}) {
        t->setDisplayFormat(QStringLiteral("HH:mm"));
    }
    m_subject->setPlaceholderText(i18nc("@info:placeholder", "Add a title"));
    m_required->setPlaceholderText(i18nc("@info:placeholder", "Invite people"));
    m_optional->setPlaceholderText(i18nc("@info:placeholder", "Optional attendees"));
    m_location->setPlaceholderText(i18nc("@info:placeholder", "Add a location or a room"));
    m_showAs->addItems({i18nc("@item:inlistbox show as", "Free"), i18nc("@item:inlistbox show as", "Tentative"), i18nc("@item:inlistbox show as", "Busy"),
                        i18nc("@item:inlistbox show as", "Away"), i18nc("@item:inlistbox show as", "Working elsewhere")});
    for (int minutes : CalendarOptions::reminderChoices()) {
        m_reminder->addItem(CalendarOptions::reminderText(minutes), minutes);
    }
    m_repeat->addItems({i18nc("@item:inlistbox repeat", "Does not repeat"), i18nc("@item:inlistbox repeat", "Every day"),
                        i18nc("@item:inlistbox repeat", "Every weekday"), i18nc("@item:inlistbox repeat", "Every week"),
                        i18nc("@item:inlistbox repeat", "Every month"), i18nc("@item:inlistbox repeat", "Every year"),
                        i18nc("@item:inlistbox repeat", "Custom...")});
    const QList<CalendarInfo> writable = m_data->writable();
    for (const CalendarInfo &c : writable) {
        QPixmap swatch(12, 12);
        swatch.fill(c.color);
        const QString label = c.group == c.name ? c.name : c.name + QStringLiteral(" (") + c.group + QLatin1Char(')');
        // A QString, not a string builder: the data must read back with toString()
        m_calendar->addItem(QIcon(swatch), label, QString(c.mailboxKey + QLatin1Char('|') + QString::number(c.id)));
    }

    // Answering as an attendee
    auto *responses = new QHBoxLayout(m_responseRow);
    responses->setContentsMargins(0, 0, 0, 0);
    responses->addWidget(m_responseLabel, 1);
    const struct {
        const char *action;
        const char *icon;
        KLocalizedString text;
    } answers[] = {{"accept", "dialog-ok-apply", ki18nc("@action:button", "Accept")},
                   {"tentativelyAccept", "dialog-question", ki18nc("@action:button", "Tentative")},
                   {"decline", "dialog-cancel", ki18nc("@action:button", "Decline")}};
    for (const auto &answer : answers) {
        auto *button = new QPushButton(QIcon::fromTheme(QLatin1String(answer.icon)), answer.text.toString(), m_responseRow);
        const QString action = QLatin1String(answer.action);
        connect(button, &QPushButton::clicked, this, [this, action] {
            respond(action);
        });
        responses->addWidget(button);
    }
    m_propose = new QPushButton(QIcon::fromTheme(QStringLiteral("appointment-new")), i18nc("@action:button", "Propose New Time..."), m_responseRow);
    connect(m_propose, &QPushButton::clicked, this, [this] {
        const DisplayEvent &e = *m_request.event;
        if (const std::optional<NewTime> t = askNewTime(this, e.start, e.end)) {
            m_daemon->proposeNewTime(e.mailboxKey.section(QLatin1Char('/'), 0, 0), e.mailboxKey.section(QLatin1Char('/'), 1), e.graphId, t->action, t->comment,
                                     t->start.toMSecsSinceEpoch(), t->end.toMSecsSinceEpoch());
            QDialog::accept();
        }
    });
    responses->addWidget(m_propose);
    m_responseRow->setVisible(false);

    auto *people = new QHBoxLayout;
    people->addWidget(m_required, 1);
    people->addWidget(m_rooms);
    auto *startRow = new QHBoxLayout;
    startRow->addWidget(m_startDate);
    startRow->addWidget(m_startTime);
    startRow->addWidget(m_allDay);
    startRow->addStretch();
    m_startZone = new QComboBox(this);
    m_endZone = new QComboBox(this);
    const QByteArray local = QTimeZone::systemTimeZoneId();
    for (QComboBox *box : {m_startZone, m_endZone}) {
        for (const auto &[label, id] : zoneChoices()) {
            box->addItem(label, id);
        }
        box->setCurrentIndex(std::max(0, box->findData(local)));
        box->setMaximumWidth(320);
    }
    // The start's zone goes to the end too, until the end gets one of its own
    connect(m_startZone, &QComboBox::activated, this, [this](int index) {
        if (!m_endZone->property("own").toBool()) {
            m_endZone->setCurrentIndex(index);
        }
    });
    connect(m_endZone, &QComboBox::activated, this, [this] {
        m_endZone->setProperty("own", true);
    });
    m_zones = new QToolButton(this);
    m_zones->setText(i18nc("@action:button", "Time Zones"));
    m_zones->setIcon(QIcon::fromTheme(QStringLiteral("preferences-system-time")));
    m_zones->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_zones->setCheckable(true);
    connect(m_zones, &QToolButton::toggled, this, [this](bool on) {
        showZones(on);
        m_scheduleTimer.start();
    });
    for (QComboBox *box : {m_startZone, m_endZone}) {
        connect(box, &QComboBox::activated, this, [this] {
            m_scheduleTimer.start();
        });
    }
    startRow->insertWidget(2, m_startZone);
    startRow->insertWidget(4, m_zones);
    auto *endRow = new QHBoxLayout;
    endRow->addWidget(m_endDate);
    endRow->addWidget(m_endTime);
    endRow->addWidget(m_endZone);
    endRow->addStretch();
    showZones(false);
    auto *options = new QHBoxLayout;
    options->addWidget(new QLabel(i18nc("@label:listbox", "Show as:"), this));
    options->addWidget(m_showAs);
    options->addSpacing(12);
    options->addWidget(new QLabel(i18nc("@label:listbox", "Reminder:"), this));
    options->addWidget(m_reminder);
    options->addSpacing(12);
    options->addWidget(m_private);
    options->addSpacing(12);
    options->addWidget(m_allowProposals);
    options->addWidget(m_hideAttendees);
    options->addStretch();
    m_allowProposals->setChecked(true);
    m_allowProposals->setToolTip(i18nc("@info:tooltip", "Attendees may propose another time for this meeting"));
    m_hideAttendees->setToolTip(i18nc("@info:tooltip", "Each attendee sees only themselves in the invitation"));
    auto *where = new QHBoxLayout;
    where->addWidget(m_location, 1);
    where->addWidget(m_teams);
    where->addWidget(m_join);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(i18nc("@label:listbox", "Calendar:"), m_calendar);
    form->addRow(i18nc("@label", "Calendar:"), m_calendarLabel);
    form->addRow(i18nc("@label:textbox", "Title:"), m_subject);
    form->addRow(i18nc("@label:textbox", "Attendees:"), people);
    form->addRow(i18nc("@label:textbox", "Optional:"), m_optional);
    form->addRow(i18nc("@label", "Start:"), startRow);
    form->addRow(i18nc("@label", "End:"), endRow);
    form->addRow(i18nc("@label:listbox", "Repeat:"), m_repeat);
    form->addRow(i18nc("@label:textbox", "Location:"), where);
    form->addRow(QString(), options);
    form->addRow(i18nc("@label", "Busy times:"), m_schedule);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons->addButton(m_delete, QDialogButtonBox::DestructiveRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &EventDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_required, &QLineEdit::textChanged, this, &EventDialog::updateSaveLabel);
    connect(m_optional, &QLineEdit::textChanged, this, &EventDialog::updateSaveLabel);
    connect(m_delete, &QPushButton::clicked, this, [this] {
        Q_EMIT deleteRequested();
        QDialog::reject();
    });
    connect(m_join, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl(m_joinUrl));
    });

    auto *left = new QWidget(this);
    auto *layout = new QVBoxLayout(left);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_responseRow);
    layout->addLayout(form);
    layout->addWidget(m_description, 1);
    m_day = new TimeGridView(this);
    m_day->setMinimumWidth(240);
    connect(m_day, &CalendarView::newEventRequested, this, [this](const QDateTime &start, const QDateTime &end, bool allDay) {
        if (!m_startDate->isEnabled()) {
            return;
        }
        m_allDay->setChecked(allDay);
        m_startDate->setDate(start.date());
        m_startTime->setTime(start.time());
        m_endDate->setDate(allDay ? end.date().addDays(-1) : end.date());
        m_endTime->setTime(end.time());
    });
    auto *split = new QSplitter(this);
    split->addWidget(left);
    split->addWidget(m_day);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    split->setChildrenCollapsible(false);
    auto *outer = new QVBoxLayout(this);
    outer->addWidget(split, 1);
    outer->addWidget(buttons);
    resize(1080, 700);

    connect(m_allDay, &QCheckBox::toggled, this, &EventDialog::allDayToggled);
    connect(m_repeat, &QComboBox::activated, this, &EventDialog::repeatChosen);
    connect(m_rooms, &QPushButton::clicked, this, &EventDialog::pickRoom);
    // Moving the start keeps the length, as in Outlook
    connect(m_startDate, &QDateEdit::dateChanged, this, [this](QDate date) {
        const qint64 days = m_startDate->property("was").toDate().isValid() ? m_startDate->property("was").toDate().daysTo(date) : 0;
        m_endDate->setDate(m_endDate->date().addDays(days));
        m_startDate->setProperty("was", date);
        m_scheduleTimer.start();
    });
    connect(m_startTime, &QTimeEdit::timeChanged, this, [this](QTime time) {
        const QTime was = m_startTime->property("was").toTime();
        if (was.isValid() && m_startDate->date() == m_endDate->date()) {
            m_endTime->setTime(m_endTime->time().addSecs(was.secsTo(time)));
        }
        m_startTime->setProperty("was", time);
        m_scheduleTimer.start();
    });
    connect(m_endTime, &QTimeEdit::timeChanged, this, [this] {
        m_scheduleTimer.start();
    });
    for (RecipientEdit *edit : {m_required, m_optional}) {
        connect(edit, &QLineEdit::editingFinished, this, [this] {
            m_scheduleTimer.start();
        });
    }
    m_scheduleTimer.setSingleShot(true);
    m_scheduleTimer.setInterval(600);
    connect(&m_scheduleTimer, &QTimer::timeout, this, &EventDialog::updateSchedule);

    if (m_request.event) {
        fillFromEvent(*m_request.event);
    } else {
        const QString wanted = m_request.mailboxKey + QLatin1Char('|') + QString::number(m_request.calendar);
        m_calendar->setCurrentIndex(std::max(0, m_calendar->findData(wanted)));
        m_startDate->setProperty("was", m_request.start.date());
        m_startDate->setDate(m_request.start.date());
        m_startTime->setTime(m_request.start.time());
        m_startTime->setProperty("was", m_request.start.time());
        m_endDate->setDate(m_request.allDay ? m_request.end.date().addDays(-1) : m_request.end.date());
        m_endTime->setTime(m_request.end.time());
        m_allDay->setChecked(m_request.allDay);
        m_showAs->setCurrentIndex(m_request.allDay ? 0 : 2);
        m_reminder->setCurrentIndex(std::max(0, m_reminder->findData(CalendarOptions::defaultReminder(m_request.allDay))));
        m_join->hide();
        m_delete->hide();
        m_subject->setText(m_request.subject);
        m_required->setRecipients(m_request.required);
        m_optional->setRecipients(m_request.optional);
        if (m_request.meeting) {
            setWindowTitle(i18nc("@title:window", "New Meeting"));
            m_required->setFocus();
        } else if (!m_request.subject.isEmpty()) {
            // Typed straight into the calendar: go on typing after it
            m_subject->setFocus();
            m_subject->deselect();
            m_subject->setCursorPosition(int(m_subject->text().size()));
        }
        if (!m_request.description.isEmpty()) {
            m_description->setPlainText(m_request.description);
        }
    }
    if (!m_request.event) {
        form->setRowVisible(m_calendarLabel, false);
    } else {
        form->setRowVisible(m_calendar, false);
    }
    allDayToggled(m_allDay->isChecked());
    m_scheduleTimer.start();
    m_subject->setFocus();
}

void EventDialog::fillFromEvent(const DisplayEvent &e)
{
    m_original = e.json;
    const std::optional<CalendarInfo> calendar = m_data->calendar(e.mailboxKey, e.calendar);
    m_calendarLabel->setText(calendar ? (calendar->group == calendar->name ? calendar->name : calendar->name + QStringLiteral(" (") + calendar->group + QLatin1Char(')')) : QString());
    m_subject->setText(e.subject);
    setWindowTitle(e.subject.isEmpty() ? i18nc("@title:window", "Event") : e.subject);
    m_required->setRecipients(attendeesOfType(m_original, QStringLiteral("required")));
    m_optional->setRecipients(attendeesOfType(m_original, QStringLiteral("optional")));
    // The occurrence keeps its own times; the series shows the first occurrence's
    QDateTime start = e.start;
    QDateTime end = e.end;
    if (m_request.series) {
        start = QDateTime::fromMSecsSinceEpoch(timeOf(m_original.value(QLatin1String("start"))));
        end = QDateTime::fromMSecsSinceEpoch(timeOf(m_original.value(QLatin1String("end"))));
        if (e.allDay) {
            start = QDateTime(start.toUTC().date(), QTime(0, 0));
            end = QDateTime(end.toUTC().date(), QTime(0, 0));
        }
    }
    // Made in other zones (Exchange's originalStartTimeZone/originalEndTimeZone): shown in those
    const QTimeZone originalStart = zoneOf(m_original.value(QLatin1String("originalStartTimeZone")).toString());
    const QTimeZone originalEnd = zoneOf(m_original.value(QLatin1String("originalEndTimeZone")).toString());
    const QByteArray local = QTimeZone::ianaIdToWindowsId(QTimeZone::systemTimeZoneId());
    const auto foreign = [&local](const QTimeZone &zone) {
        return zone.isValid() && QTimeZone::ianaIdToWindowsId(zone.id()) != local;
    };
    if (!e.allDay && (foreign(originalStart) || foreign(originalEnd))) {
        const QTimeZone startIn = originalStart.isValid() ? originalStart : QTimeZone::systemTimeZone();
        const QTimeZone endIn = originalEnd.isValid() ? originalEnd : startIn;
        m_startZone->setCurrentIndex(std::max(0, m_startZone->findData(startIn.id())));
        m_endZone->setCurrentIndex(std::max(0, m_endZone->findData(endIn.id())));
        m_endZone->setProperty("own", true);
        start = start.toTimeZone(startIn);
        end = end.toTimeZone(endIn);
        m_zones->setChecked(true);
    }
    m_startDate->setProperty("was", start.date());
    m_startDate->setDate(start.date());
    m_startTime->setTime(start.time());
    m_startTime->setProperty("was", start.time());
    m_endDate->setDate(e.allDay ? end.date().addDays(-1) : end.date());
    m_endTime->setTime(end.time());
    m_allDay->setChecked(e.allDay);
    m_location->setText(e.location);
    // An occurrence of a Teams series carries no flag of its own; unticked, a save would end Teams
    m_teams->setChecked(e.online || !m_data->joinUrlOf(e).isEmpty());
    m_showAs->setCurrentIndex(std::max(0, int(showAsNames().indexOf(m_original.value(QLatin1String("showAs")).toString()))));
    const int minutes = m_original.value(QLatin1String("isReminderOn")).toBool() ? m_original.value(QLatin1String("reminderMinutesBeforeStart")).toInt() : -1;
    int index = m_reminder->findData(minutes);
    if (index < 0) {
        m_reminder->addItem(CalendarOptions::reminderText(minutes), minutes);
        index = m_reminder->count() - 1;
    }
    m_reminder->setCurrentIndex(index);
    m_private->setChecked(e.isPrivate);
    m_allowProposals->setChecked(m_original.value(QLatin1String("allowNewTimeProposals")).toBool(true));
    m_hideAttendees->setChecked(m_original.value(QLatin1String("hideAttendees")).toBool(false));
    if (m_propose) {
        m_propose->setVisible(m_original.value(QLatin1String("allowNewTimeProposals")).toBool(true));
    }
    const QJsonObject body = m_original.value(QLatin1String("body")).toObject();
    if (body.value(QLatin1String("contentType")).toString().compare(QLatin1String("html"), Qt::CaseInsensitive) == 0) {
        m_description->setHtml(body.value(QLatin1String("content")).toString());
    } else {
        m_description->setPlainText(body.value(QLatin1String("content")).toString());
    }
    connect(m_description, &QTextEdit::textChanged, this, [this] {
        m_descriptionChanged = true;
    });
    // Repeat: only for a new event or the whole series
    m_recurrence = m_request.series ? m_original.value(QLatin1String("recurrence")).toObject() : QJsonObject();
    if (!m_recurrence.isEmpty()) {
        m_repeat->insertItem(m_repeat->count() - 1, describeRecurrence(m_recurrence));
        m_repeat->setCurrentIndex(m_repeat->count() - 2);
    }
    m_repeat->setEnabled(m_request.series || !e.recurring());
    if (e.recurring() && !m_request.series) {
        m_repeat->setItemText(0, i18nc("@item:inlistbox", "One occurrence of a series"));
    }
    m_joinUrl = m_data->joinUrlOf(e);
    m_join->setVisible(!m_joinUrl.isEmpty());

    // Who may change it: the organizer, in a calendar the user may edit
    const QString organizer = m_original.value(QLatin1String("organizer")).toObject().value(QLatin1String("emailAddress")).toObject().value(QLatin1String("address")).toString().toLower();
    const bool othersMeeting = !e.isOrganizer && !organizer.isEmpty() && !m_data->ownAddresses().contains(organizer);
    m_attendee = othersMeeting && e.canEdit;
    setReadOnly(!e.canEdit || othersMeeting);
    if (m_attendee) {
        const QString state = e.response == 3 ? i18nc("@info", "You accepted.") : e.response == 2 ? i18nc("@info", "You accepted tentatively.")
                            : e.response == 4                                                 ? i18nc("@info", "You declined.")
                                                                                              : i18nc("@info", "Please respond.");
        m_responseLabel->setText(i18nc("@info meeting organizer, then the state", "Organized by %1. %2", e.organizer, state));
        m_responseRow->setVisible(!e.cancelled);
    }
    m_delete->setVisible(e.canEdit);
    if (!e.canEdit && !othersMeeting) {
        // A colleague's calendar shared for reading: say why nothing can be changed
        const std::optional<CalendarInfo> calendar = m_data->calendar(e.mailboxKey, e.calendar);
        m_responseLabel->setText(accessProblemText(QStringLiteral("readOnly"), calendar ? calendar->name : e.organizer));
        m_responseRow->setVisible(true);
    }
    if (e.cancelled) {
        m_responseLabel->setText(i18nc("@info", "This meeting was cancelled."));
        m_responseRow->setVisible(true);
    }
    // The answer buttons only for an attendee who can still answer; the row also carries notes
    const QList<QPushButton *> answerButtons = m_responseRow->findChildren<QPushButton *>();
    for (QPushButton *button : answerButtons) {
        button->setVisible(m_attendee && !e.cancelled);
    }
}

void EventDialog::setReadOnly(bool readOnly)
{
    m_readOnly = readOnly;
    for (QWidget *w : std::initializer_list<QWidget *>{m_subject, m_required, m_optional, m_rooms, m_startDate, m_startTime, m_endDate, m_endTime, m_allDay, m_repeat,
                                                      m_location, m_teams, m_showAs, m_private, m_allowProposals, m_hideAttendees}) {
        w->setEnabled(!readOnly);
    }
    m_description->setReadOnly(readOnly);
    // An attendee still chooses their own reminder in Outlook; Graph allows it too
    m_reminder->setEnabled(!readOnly || m_attendee);
    // The organizer's meeting options mean nothing to an attendee
    m_allowProposals->setVisible(!m_attendee);
    m_hideAttendees->setVisible(!m_attendee);
    if (auto *save = findChild<QDialogButtonBox *>()) {
        save->button(QDialogButtonBox::Save)->setVisible(!readOnly || m_attendee);
        save->button(QDialogButtonBox::Cancel)->setText(readOnly && !m_attendee ? i18nc("@action:button", "Close") : save->button(QDialogButtonBox::Cancel)->text());
    }
}

void EventDialog::typeIntoSubject(const QString &text)
{
    // After what is there: nothing typed so far may be selected and replaced
    m_subject->setCursorPosition(int(m_subject->text().size()));
    m_subject->insert(text);
    m_subject->setFocus();
}

void EventDialog::setTestZones(const QByteArray &start, const QByteArray &end)
{
    m_startZone->setCurrentIndex(std::max(0, m_startZone->findData(start)));
    m_endZone->setCurrentIndex(std::max(0, m_endZone->findData(end)));
    m_zones->setChecked(true);
}

QTimeZone EventDialog::startZone() const
{
    const QTimeZone zone(m_startZone->currentData().toByteArray());
    return zone.isValid() ? zone : QTimeZone::systemTimeZone();
}

QTimeZone EventDialog::endZone() const
{
    const QTimeZone zone(m_endZone->currentData().toByteArray());
    return zone.isValid() ? zone : startZone();
}

void EventDialog::showZones(bool on)
{
    m_startZone->setVisible(on && !m_allDay->isChecked());
    m_endZone->setVisible(on && !m_allDay->isChecked());
}

void EventDialog::updateSaveLabel()
{
    auto *box = findChild<QDialogButtonBox *>();
    QPushButton *save = box ? box->button(QDialogButtonBox::Save) : nullptr;
    if (!save || m_attendee) {
        return;
    }
    QStringList invalid;
    const bool people = !m_required->recipients(&invalid).isEmpty() || !m_optional->recipients(&invalid).isEmpty() || !invalid.isEmpty();
    // Outlook's Send / Send Update: with attendees the change goes out as an invitation
    save->setText(people ? (m_request.event ? i18nc("@action:button", "Send Update") : i18nc("@action:button", "Send")) : i18nc("@action:button", "Save"));
    save->setIcon(QIcon::fromTheme(people ? QStringLiteral("mail-send") : QStringLiteral("document-save")));
}

void EventDialog::allDayToggled(bool allDay)
{
    m_startTime->setVisible(!allDay);
    m_endTime->setVisible(!allDay);
    m_zones->setVisible(!allDay);
    showZones(m_zones->isChecked());
    // A new event still on the default reminder takes the default of the other kind, as Outlook does
    if (!m_request.event && m_reminder->currentData().toInt() == CalendarOptions::defaultReminder(!allDay)) {
        m_reminder->setCurrentIndex(std::max(0, m_reminder->findData(CalendarOptions::defaultReminder(allDay))));
    }
    m_scheduleTimer.start();
}

void EventDialog::repeatChosen(int index)
{
    const QDate start = m_startDate->date();
    const int custom = m_repeat->count() - 1;
    if (index == 0) {
        m_recurrence = {};
    } else if (index >= 1 && index <= 5) {
        m_recurrence = presetRecurrence(RepeatPreset(index - 1), start);
    } else if (index == custom) {
        RecurrenceDialog dialog(m_recurrence, start, this);
        if (dialog.exec() == QDialog::Accepted) {
            m_recurrence = dialog.recurrence();
        }
        // Show the rule chosen as its own entry
        if (m_repeat->count() > 7) {
            m_repeat->removeItem(custom - 1);
        }
        if (!m_recurrence.isEmpty()) {
            m_repeat->insertItem(m_repeat->count() - 1, describeRecurrence(m_recurrence));
            m_repeat->setCurrentIndex(m_repeat->count() - 2);
        } else {
            m_repeat->setCurrentIndex(0);
        }
    }
}

QStringList EventDialog::attendeeAddresses() const
{
    QStringList result;
    const QList<OutgoingRecipient> all = m_required->recipients() + m_optional->recipients();
    for (const OutgoingRecipient &r : all) {
        result.append(r.address);
    }
    return result;
}

void EventDialog::updateDay()
{
    const QDate day = m_startDate->date();
    QList<DisplayEvent> events = m_data->events(day, day);
    if (m_request.event) {
        // The event itself is the blue selection, not one of the others
        events.erase(std::remove_if(events.begin(), events.end(),
                                    [this](const DisplayEvent &e) {
                                        return e.mailboxKey == m_request.event->mailboxKey && e.id == m_request.event->id;
                                    }),
                     events.end());
    }
    m_day->setEvents(day, day, events);
    QDateTime start(day, m_allDay->isChecked() ? QTime(0, 0) : m_startTime->time());
    QDateTime end(m_allDay->isChecked() ? m_endDate->date().addDays(1) : m_endDate->date(), m_allDay->isChecked() ? QTime(0, 0) : m_endTime->time());
    if (m_zones->isChecked() && !m_allDay->isChecked()) {
        start = QDateTime(start.date(), start.time(), startZone()).toLocalTime();
        end = QDateTime(end.date(), end.time(), endZone()).toLocalTime();
    }
    m_day->setSelectedSlot({start, end.date() == start.date() || m_allDay->isChecked() ? end : QDateTime(start.date().addDays(1), QTime(0, 0)), m_allDay->isChecked()});
    m_day->scrollToHour(std::max(0, start.time().hour() - 2));
}

void EventDialog::updateSchedule()
{
    updateDay();
    const QDate day = m_startDate->date();
    QDateTime start(day, m_allDay->isChecked() ? QTime(0, 0) : m_startTime->time());
    QDateTime end(m_endDate->date(), m_allDay->isChecked() ? QTime(23, 59) : m_endTime->time());
    if (m_zones->isChecked() && !m_allDay->isChecked()) {
        // Times typed in other zones: the busy view is in local time
        start = QDateTime(start.date(), start.time(), startZone()).toLocalTime();
        end = QDateTime(end.date(), end.time(), endZone()).toLocalTime();
    }
    m_schedule->setSlot(start, end);
    QStringList addresses;
    const QString key = m_request.event ? m_request.event->mailboxKey : m_calendar->currentData().toString().section(QLatin1Char('|'), 0, 0);
    if (const std::optional<MailboxInfo> box = m_mailboxes->mailbox(key)) {
        addresses.append(box->address);
    }
    addresses += attendeeAddresses();
    addresses.removeDuplicates();
    if (addresses.isEmpty() || key.isEmpty()) {
        return;
    }
    m_schedule->setMessage(i18nc("@info", "Looking up busy times..."));
    const QJsonObject args{{QStringLiteral("addresses"), QJsonArray::fromStringList(addresses)},
                           {QStringLiteral("start"), QDateTime(day, QTime(0, 0)).toMSecsSinceEpoch()},
                           {QStringLiteral("end"), QDateTime(day.addDays(1), QTime(0, 0)).toMSecsSinceEpoch()},
                           {QStringLiteral("interval"), 30}};
    m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("schedule"),
                    QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this, [this, day](const QString &json) {
                        if (json.isEmpty()) {
                            m_schedule->setMessage(i18nc("@info", "Busy times are not available (offline?)"));
                            return;
                        }
                        // The view's half hours start at the start asked for: local midnight
                        QList<QPair<QString, QString>> rows;
                        const QJsonArray value = QJsonDocument::fromJson(json.toUtf8()).object().value(QLatin1String("value")).toArray();
                        for (const QJsonValue &v : value) {
                            const QJsonObject s = v.toObject();
                            QString local = s.contains(QLatin1String("error")) ? QString() : s.value(QLatin1String("availabilityView")).toString();
                            local = local.leftJustified(48, QLatin1Char('?'), true);
                            rows.append({s.value(QLatin1String("scheduleId")).toString(), local});
                        }
                        m_schedule->setSchedules(day, rows);
                    });
}

void EventDialog::pickRoom()
{
    const QString key = m_request.event ? m_request.event->mailboxKey : m_calendar->currentData().toString().section(QLatin1Char('|'), 0, 0);
    auto *dialog = new QDialog(this);
    dialog->setWindowTitle(i18nc("@title:window", "Rooms"));
    auto *list = new QListWidget(dialog);
    auto *status = new QLabel(i18nc("@info", "Looking up rooms..."), dialog);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    auto *layout = new QVBoxLayout(dialog);
    layout->addWidget(status);
    layout->addWidget(list);
    layout->addWidget(buttons);
    dialog->resize(420, 420);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, dialog, &QDialog::accept);
    m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("rooms"), QStringLiteral("{}"), dialog, [list, status](const QString &json) {
        const QJsonArray rooms = QJsonDocument::fromJson(json.toUtf8()).object().value(QLatin1String("value")).toArray();
        status->setText(json.isEmpty() ? i18nc("@info", "Rooms are not available (offline, or no room list in this organization)")
                                       : i18ncp("@info", "%1 room", "%1 rooms", int(rooms.size())));
        for (const QJsonValue &v : rooms) {
            const QJsonObject r = v.toObject();
            QString text = r.value(QLatin1String("displayName")).toString();
            const int capacity = r.value(QLatin1String("capacity")).toInt();
            if (capacity > 0) {
                text += QStringLiteral(" (") + i18ncp("@item room capacity", "%1 person", "%1 people", capacity) + QLatin1Char(')');
            }
            auto *item = new QListWidgetItem(text, list);
            item->setData(Qt::UserRole, r);
        }
    });
    if (dialog->exec() == QDialog::Accepted && list->currentItem()) {
        m_room = list->currentItem()->data(Qt::UserRole).toJsonObject();
        m_location->setText(m_room.value(QLatin1String("displayName")).toString());
    }
    dialog->deleteLater();
}

QJsonObject EventDialog::eventJson() const
{
    QJsonObject e;
    e.insert(QStringLiteral("subject"), m_subject->text().trimmed());
    QDateTime start(m_startDate->date(), m_allDay->isChecked() ? QTime(0, 0) : m_startTime->time());
    QDateTime end(m_allDay->isChecked() ? m_endDate->date().addDays(1) : m_endDate->date(), m_allDay->isChecked() ? QTime(0, 0) : m_endTime->time());
    if (end <= start) {
        end = m_allDay->isChecked() ? start.addDays(1) : start.addSecs(1800);
    }
    if (m_zones->isChecked() && !m_allDay->isChecked()) {
        // The times as typed, each in its own zone
        start.setTimeZone(startZone());
        end.setTimeZone(endZone());
        if (end <= start) {
            end = start.addSecs(1800);
        }
        e.insert(QStringLiteral("start"), graphTime(start, startZone()));
        e.insert(QStringLiteral("end"), graphTime(end, endZone()));
    } else {
        e.insert(QStringLiteral("start"), graphTime(start));
        e.insert(QStringLiteral("end"), graphTime(end));
    }
    e.insert(QStringLiteral("isAllDay"), m_allDay->isChecked());
    QJsonObject location{{QStringLiteral("displayName"), m_location->text().trimmed()}};
    if (!m_room.isEmpty() && m_room.value(QLatin1String("displayName")).toString() == m_location->text().trimmed()) {
        location.insert(QStringLiteral("locationEmailAddress"), m_room.value(QLatin1String("emailAddress")).toString());
        location.insert(QStringLiteral("locationType"), QStringLiteral("conferenceRoom"));
    }
    e.insert(QStringLiteral("location"), location);
    QJsonArray attendees;
    auto add = [&attendees](const QList<OutgoingRecipient> &list, const QString &type) {
        for (const OutgoingRecipient &r : list) {
            attendees.append(QJsonObject{{QStringLiteral("emailAddress"), QJsonObject{{QStringLiteral("address"), r.address}, {QStringLiteral("name"), r.name.isEmpty() ? r.address : r.name}}},
                                         {QStringLiteral("type"), type}});
        }
    };
    add(m_required->recipients(), QStringLiteral("required"));
    add(m_optional->recipients(), QStringLiteral("optional"));
    // The room books itself when it is invited as a resource
    const QString roomAddress = m_room.value(QLatin1String("emailAddress")).toString();
    if (!roomAddress.isEmpty() && m_room.value(QLatin1String("displayName")).toString() == m_location->text().trimmed()) {
        add({{m_room.value(QLatin1String("displayName")).toString(), roomAddress}}, QStringLiteral("resource"));
    } else {
        // Rooms already on the event stay
        add(attendeesOfType(m_original, QStringLiteral("resource")), QStringLiteral("resource"));
    }
    e.insert(QStringLiteral("attendees"), attendees);
    e.insert(QStringLiteral("isOnlineMeeting"), m_teams->isChecked());
    if (m_teams->isChecked()) {
        e.insert(QStringLiteral("onlineMeetingProvider"), QStringLiteral("teamsForBusiness"));
    }
    e.insert(QStringLiteral("showAs"), showAsNames().value(m_showAs->currentIndex()));
    const int minutes = m_reminder->currentData().toInt();
    e.insert(QStringLiteral("isReminderOn"), minutes >= 0);
    if (minutes >= 0) {
        e.insert(QStringLiteral("reminderMinutesBeforeStart"), minutes);
    }
    e.insert(QStringLiteral("sensitivity"), m_private->isChecked() ? QStringLiteral("private") : QStringLiteral("normal"));
    e.insert(QStringLiteral("allowNewTimeProposals"), m_allowProposals->isChecked());
    e.insert(QStringLiteral("hideAttendees"), m_hideAttendees->isChecked());
    e.insert(QStringLiteral("body"), QJsonObject{{QStringLiteral("contentType"), QStringLiteral("HTML")}, {QStringLiteral("content"), m_description->toHtml()}});
    if (!m_recurrence.isEmpty()) {
        QJsonObject recurrence = m_recurrence;
        QJsonObject range = recurrence.value(QLatin1String("range")).toObject();
        // The series starts on the event's day
        range.insert(QStringLiteral("startDate"), m_startDate->date().toString(Qt::ISODate));
        recurrence.insert(QStringLiteral("range"), range);
        e.insert(QStringLiteral("recurrence"), recurrence);
    }
    return e;
}

QJsonObject EventDialog::changes() const
{
    const QJsonObject now = eventJson();
    QJsonObject diff;
    auto changed = [this, &now](const char *key) {
        return now.value(QLatin1String(key)) != m_original.value(QLatin1String(key));
    };
    if (m_attendee) {
        // An attendee only keeps their own reminder
        for (const char *key : {"isReminderOn", "reminderMinutesBeforeStart"}) {
            if (changed(key)) {
                diff.insert(QLatin1String(key), now.value(QLatin1String(key)));
            }
        }
        return diff;
    }
    for (const char *key : {"subject", "isAllDay", "isOnlineMeeting", "showAs", "isReminderOn", "reminderMinutesBeforeStart", "sensitivity"}) {
        if (changed(key)) {
            diff.insert(QLatin1String(key), now.value(QLatin1String(key)));
        }
    }
    // Graph's defaults when the cache has none: proposals allowed, attendees shown
    if (now.value(QLatin1String("allowNewTimeProposals")).toBool() != m_original.value(QLatin1String("allowNewTimeProposals")).toBool(true)) {
        diff.insert(QStringLiteral("allowNewTimeProposals"), now.value(QLatin1String("allowNewTimeProposals")));
    }
    if (now.value(QLatin1String("hideAttendees")).toBool() != m_original.value(QLatin1String("hideAttendees")).toBool(false)) {
        diff.insert(QStringLiteral("hideAttendees"), now.value(QLatin1String("hideAttendees")));
    }
    if (diff.contains(QLatin1String("isOnlineMeeting")) && now.contains(QLatin1String("onlineMeetingProvider"))) {
        diff.insert(QStringLiteral("onlineMeetingProvider"), now.value(QLatin1String("onlineMeetingProvider")));
    }
    // Times: compared as moments, Graph writes them in another zone than we send
    const DisplayEvent &e = *m_request.event;
    const qint64 originalStart = m_request.series ? timeOf(m_original.value(QLatin1String("start"))) : e.start.toMSecsSinceEpoch();
    const qint64 originalEnd = m_request.series ? timeOf(m_original.value(QLatin1String("end"))) : e.end.toMSecsSinceEpoch();
    if (timeOf(now.value(QLatin1String("start"))) != originalStart || timeOf(now.value(QLatin1String("end"))) != originalEnd || diff.contains(QLatin1String("isAllDay"))) {
        diff.insert(QStringLiteral("start"), now.value(QLatin1String("start")));
        diff.insert(QStringLiteral("end"), now.value(QLatin1String("end")));
    }
    if (now.value(QLatin1String("location")).toObject().value(QLatin1String("displayName")) != m_original.value(QLatin1String("location")).toObject().value(QLatin1String("displayName"))) {
        diff.insert(QStringLiteral("location"), now.value(QLatin1String("location")));
    }
    if (attendeeKeys(now.value(QLatin1String("attendees")).toArray()) != attendeeKeys(m_original.value(QLatin1String("attendees")).toArray())) {
        diff.insert(QStringLiteral("attendees"), now.value(QLatin1String("attendees")));
    }
    if (m_descriptionChanged) {
        diff.insert(QStringLiteral("body"), now.value(QLatin1String("body")));
    }
    if (m_request.series) {
        const QJsonObject before = m_original.value(QLatin1String("recurrence")).toObject();
        const QJsonObject after = now.value(QLatin1String("recurrence")).toObject();
        if (before.value(QLatin1String("pattern")) != after.value(QLatin1String("pattern"))
            || before.value(QLatin1String("range")).toObject().value(QLatin1String("type")) != after.value(QLatin1String("range")).toObject().value(QLatin1String("type"))
            || before.value(QLatin1String("range")).toObject().value(QLatin1String("endDate")) != after.value(QLatin1String("range")).toObject().value(QLatin1String("endDate"))
            || before.value(QLatin1String("range")).toObject().value(QLatin1String("numberOfOccurrences"))
                != after.value(QLatin1String("range")).toObject().value(QLatin1String("numberOfOccurrences"))) {
            diff.insert(QStringLiteral("recurrence"), after.isEmpty() ? QJsonValue() : QJsonValue(after));
        }
    }
    return diff;
}

std::optional<EventDialog::NewTime> EventDialog::askNewTime(QWidget *parent, const QDateTime &start, const QDateTime &end)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(i18nc("@title:window", "Propose New Time"));
    auto *from = new QDateTimeEdit(start, &dialog);
    auto *to = new QDateTimeEdit(end, &dialog);
    for (QDateTimeEdit *edit : {from, to}) {
        edit->setCalendarPopup(true);
        edit->setDisplayFormat(QLocale().dateTimeFormat(QLocale::ShortFormat));
    }
    // The length stays when the start moves
    const qint64 length = start.secsTo(end);
    connect(from, &QDateTimeEdit::dateTimeChanged, to, [to, length](const QDateTime &t) {
        to->setDateTime(t.addSecs(length));
    });
    auto *answer = new QComboBox(&dialog);
    answer->addItem(QIcon::fromTheme(QStringLiteral("dialog-question")), i18nc("@item:inlistbox", "Tentative and propose this time"), QStringLiteral("tentativelyAccept"));
    answer->addItem(QIcon::fromTheme(QStringLiteral("dialog-cancel")), i18nc("@item:inlistbox", "Decline and propose this time"), QStringLiteral("decline"));
    auto *comment = new QTextEdit(&dialog);
    comment->setAcceptRichText(false);
    comment->setPlaceholderText(i18nc("@info:placeholder", "Message to the organizer (optional)"));
    auto *form = new QFormLayout;
    form->addRow(i18nc("@label", "Start:"), from);
    form->addRow(i18nc("@label", "End:"), to);
    form->addRow(i18nc("@label:listbox", "Answer:"), answer);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Send Proposal"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(comment, 1);
    layout->addWidget(buttons);
    dialog.resize(440, 320);
    if (dialog.exec() != QDialog::Accepted || to->dateTime() <= from->dateTime()) {
        return std::nullopt;
    }
    return NewTime{from->dateTime(), to->dateTime(), answer->currentData().toString(), comment->toPlainText().trimmed()};
}

bool EventDialog::deleteRequestAfterResponding()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Calendar")).readEntry("DeleteRequestAfterResponding", true);
}

void EventDialog::respond(const QString &action)
{
    const DisplayEvent &e = *m_request.event;
    // As in Outlook: send now, edit the response first, or do not send one
    QMenu menu(this);
    menu.addAction(i18nc("@action:inmenu", "Send the Response Now"));
    QAction *edit = menu.addAction(i18nc("@action:inmenu", "Edit the Response Before Sending..."));
    QAction *silent = menu.addAction(i18nc("@action:inmenu", "Do Not Send a Response"));
    QAction *chosen = menu.exec(QCursor::pos());
    if (!chosen) {
        return;
    }
    QString comment;
    if (chosen == edit) {
        bool ok = false;
        comment = QInputDialog::getMultiLineText(this, i18nc("@title:window", "Response"), i18nc("@label:textbox", "Message to the organizer:"), {}, &ok);
        if (!ok) {
            return;
        }
    }
    m_daemon->respondEvent(e.mailboxKey.section(QLatin1Char('/'), 0, 0), e.mailboxKey.section(QLatin1Char('/'), 1), e.id, action, comment, chosen != silent);
    QDialog::accept();
}

void EventDialog::accept()
{
    if (m_readOnly && !m_attendee) {
        QDialog::reject();
        return;
    }
    if (!m_request.event) {
        const QString target = m_calendar->currentData().toString();
        const QString key = target.section(QLatin1Char('|'), 0, 0);
        if (const std::optional<CalendarInfo> chosen = m_data->calendar(key, target.section(QLatin1Char('|'), 1).toLongLong())) {
            m_data->setLastUsed(chosen->key());
        }
        m_daemon->createEvent(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), target.section(QLatin1Char('|'), 1).toLongLong(),
                              QString::fromUtf8(QJsonDocument(eventJson()).toJson(QJsonDocument::Compact)));
    } else {
        const QJsonObject diff = changes();
        // As Outlook warns: a new time or rule for the whole series makes Exchange drop what was
        // changed in single occurrences, and deleted occurrences come back
        const bool reshapes = diff.contains(QLatin1String("start")) || diff.contains(QLatin1String("end")) || diff.contains(QLatin1String("isAllDay"))
            || diff.contains(QLatin1String("recurrence"));
        if (m_request.series && reshapes && !m_testMode) {
            const Store *store = m_mailboxes->store(m_request.event->mailboxKey);
            const int changed = store ? store->exceptionCount(m_request.event->graphId) : 0;
            const QString text = changed > 0
                ? i18ncp("@info",
                         "This series has %1 occurrence that was changed on its own. Changing the time or the repeat of the whole series undoes that change, and occurrences you deleted come back.",
                         "This series has %1 occurrences that were changed on their own. Changing the time or the repeat of the whole series undoes those changes, and occurrences you deleted come back.",
                         changed)
                : i18nc("@info", "Changing the time or the repeat of the whole series undoes changes made to single occurrences, and occurrences you deleted come back.");
            if (KMessageBox::warningContinueCancel(this, text, i18nc("@title:window", "Change the Series")) != KMessageBox::Continue) {
                return;
            }
        } else if (m_request.series && reshapes) {
            qWarning() << "series test: would warn about dropping exceptions";
        }
        if (!diff.isEmpty()) {
            const DisplayEvent &e = *m_request.event;
            m_daemon->updateEvent(e.mailboxKey.section(QLatin1Char('/'), 0, 0), e.mailboxKey.section(QLatin1Char('/'), 1), e.id,
                                  QString::fromUtf8(QJsonDocument(diff).toJson(QJsonDocument::Compact)));
        }
    }
    QDialog::accept();
}

void EventDialog::setTestInput(const QString &subject, const QString &location, const QString &required, int reminder, bool teams)
{
    m_testMode = true;
    m_subject->setText(subject);
    m_location->setText(location);
    m_required->setRecipients(parseRecipients(required));
    m_reminder->setCurrentIndex(std::max(0, m_reminder->findData(reminder)));
    m_teams->setChecked(teams);
}

void EventDialog::setTestRecurrence(const QJsonObject &recurrence)
{
    m_recurrence = recurrence;
}

void EventDialog::applyTestEdit(const QString &title, int shiftMinutes)
{
    m_testMode = true;
    if (!title.isEmpty()) {
        m_subject->setText(title);
    }
    if (shiftMinutes != 0) {
        const QDateTime start = QDateTime(m_startDate->date(), m_startTime->time()).addSecs(qint64(shiftMinutes) * 60);
        const QDateTime end = QDateTime(m_endDate->date(), m_endTime->time()).addSecs(qint64(shiftMinutes) * 60);
        m_startDate->setDate(start.date());
        m_startTime->setTime(start.time());
        m_endDate->setDate(end.date());
        m_endTime->setTime(end.time());
    }
}

void EventDialog::setTestRoom(const QString &name, const QString &address)
{
    m_room = {{QStringLiteral("displayName"), name}, {QStringLiteral("emailAddress"), address}};
    m_location->setText(name);
}

void EventDialog::setTestAllDay(bool allDay)
{
    m_allDay->setChecked(allDay);
}

void EventDialog::selectCalendarForTests(const QString &mailboxKey)
{
    for (int i = 0; i < m_calendar->count(); ++i) {
        if (m_calendar->itemData(i).toString().startsWith(mailboxKey + QLatin1Char('|'))) {
            m_calendar->setCurrentIndex(i);
            return;
        }
    }
}

void EventDialog::saveNow()
{
    accept();
}

SchedulingStrip::SchedulingStrip(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize SchedulingStrip::sizeHint() const
{
    return {400, 22 + std::max<int>(1, int(m_schedules.size())) * 18};
}

void SchedulingStrip::setSchedules(QDate day, const QList<QPair<QString, QString>> &schedules)
{
    m_day = day;
    m_schedules = schedules;
    m_message.clear();
    updateGeometry();
    setFixedHeight(sizeHint().height());
    update();
}

void SchedulingStrip::setSlot(const QDateTime &start, const QDateTime &end)
{
    m_start = start;
    m_end = end;
    update();
}

void SchedulingStrip::setMessage(const QString &message)
{
    m_message = message;
    update();
}

void SchedulingStrip::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QPalette pal = palette();
    p.fillRect(rect(), pal.color(QPalette::Base));
    // 07:00 to 19:00, as the working day of Outlook's scheduling assistant
    constexpr int firstHalf = 14;
    constexpr int halves = 24;
    const int label = std::min(220, width() / 3);
    const qreal w = (width() - label) / qreal(halves);
    p.setPen(pal.color(QPalette::PlaceholderText));
    for (int h = 0; h <= halves; h += 2) {
        p.drawText(QRectF(label + h * w - 20, 0, 40, 18), Qt::AlignCenter, QStringLiteral("%1").arg((firstHalf + h) / 2, 2, 10, QLatin1Char('0')));
    }
    if (!m_message.isEmpty() && m_schedules.isEmpty()) {
        p.drawText(rect().adjusted(4, 18, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, m_message);
        return;
    }
    const QColor busy(0x3d, 0x6b, 0xb3);
    const QColor tentative(0x9c, 0xb8, 0xe2);
    const QColor away(0x8e, 0x44, 0xad);
    const QColor unknown = pal.color(QPalette::Midlight);
    for (int row = 0; row < m_schedules.size(); ++row) {
        const qreal y = 20 + row * 18;
        p.setPen(pal.color(QPalette::Text));
        p.drawText(QRectF(4, y, label - 8, 16), Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(m_schedules.at(row).first, Qt::ElideRight, label - 8));
        const QString view = m_schedules.at(row).second;
        for (int h = 0; h < halves; ++h) {
            const QChar c = firstHalf + h < view.size() ? view.at(firstHalf + h) : QLatin1Char('?');
            QColor fill;
            if (c == QLatin1Char('2')) {
                fill = busy;
            } else if (c == QLatin1Char('1')) {
                fill = tentative;
            } else if (c == QLatin1Char('3')) {
                fill = away;
            } else if (c == QLatin1Char('4')) {
                fill = tentative.darker(110);
            } else if (c == QLatin1Char('?')) {
                fill = unknown;
            }
            if (fill.isValid()) {
                p.fillRect(QRectF(label + h * w, y + 2, w, 12), fill);
            }
        }
        p.setPen(pal.color(QPalette::Mid));
        p.drawLine(QPointF(label, y + 16), QPointF(width(), y + 16));
    }
    // The event's own time
    if (m_start.isValid() && m_start.date() == m_day) {
        const qreal from = label + (QTime(0, 0).secsTo(m_start.time()) / 1800.0 - firstHalf) * w;
        const qreal to = m_end.date() == m_day ? label + (QTime(0, 0).secsTo(m_end.time()) / 1800.0 - firstHalf) * w : width();
        QColor mark = pal.color(QPalette::Highlight);
        p.setPen(QPen(mark, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(std::max<qreal>(label, from), 18, std::max<qreal>(2, std::min<qreal>(width(), to) - std::max<qreal>(label, from)), height() - 19));
    }
}

}
