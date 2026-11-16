// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "calendardata.h"
#include "calendaroptions.h"
#include "categorylist.h"
#include "graphmapper.h"
#include "mailboxset.h"
#include "store.h"

#include <KConfigGroup>
#include <KHolidays/HolidayRegion>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QJsonArray>
#include <QJsonDocument>
#include <QTimeZone>

namespace GraphMail
{

namespace
{
KConfigGroup calendarGroup()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Calendar"));
}
}

QColor paletteColor(int index)
{
    static const QList<QColor> colors{QColor(0x3d, 0xae, 0xe9), QColor(0x27, 0xae, 0x60), QColor(0xf6, 0x74, 0x00), QColor(0x9b, 0x59, 0xb6),
                                      QColor(0xda, 0x44, 0x53), QColor(0x16, 0xa0, 0x85), QColor(0xc9, 0xce, 0x3b), QColor(0x8e, 0x44, 0xad),
                                      QColor(0x1d, 0x99, 0xf3), QColor(0xe6, 0x7e, 0x22), QColor(0x7f, 0x8c, 0x8d), QColor(0xd3, 0x54, 0x00)};
    return colors.at(index % colors.size());
}

CalendarData::CalendarData(MailboxSet *mailboxes, QObject *parent)
    : QObject(parent)
    , m_mailboxes(mailboxes)
{
    reload();
}

void CalendarData::reload()
{
    m_calendars.clear();
    const KConfigGroup group = calendarGroup();
    const QStringList shown = group.readEntry("Shown", QStringList());
    const QStringList hidden = group.readEntry("Hidden", QStringList());
    int colour = 0;
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    // With several accounts "My Calendars" would mix them: each account by its address then
    const bool severalAccounts = std::count_if(boxes.cbegin(), boxes.cend(), [](const MailboxInfo &box) {
                                     return box.own;
                                 })
        > 1;
    for (const MailboxInfo &box : boxes) {
        const Store *store = m_mailboxes->store(box.key());
        if (!store) {
            continue;
        }
        QString own;
        const QList<CalendarRow> rows = store->calendars();
        for (const CalendarRow &row : rows) {
            if (row.isDefault) {
                own = row.owner;
            }
        }
        for (const CalendarRow &row : rows) {
            CalendarInfo c;
            c.mailboxKey = box.key();
            c.id = row.id;
            c.graphId = row.graphId;
            c.name = row.name;
            c.owner = row.owner;
            c.canEdit = row.canEdit;
            c.isDefault = row.isDefault;
            c.color = row.color.isEmpty() ? paletteColor(colour++) : QColor(row.color);
            const bool mine = row.isDefault || own.isEmpty() || row.owner.compare(own, Qt::CaseInsensitive) == 0;
            if (box.own && severalAccounts) {
                c.group = mine ? box.address : i18nc("@title calendar group, %1 the account", "Shared with %1", box.address);
            } else if (box.own) {
                c.group = mine ? i18nc("@title calendar group", "My Calendars") : i18nc("@title calendar group", "Shared Calendars");
            } else {
                c.group = box.label();
            }
            // By default: own calendars and shared mailboxes' calendars, not colleagues'
            const bool byDefault = mine;
            c.shown = shown.contains(c.key()) || (byDefault && !hidden.contains(c.key()));
            if (row.isDefault && box.own && c.name == QLatin1String("Calendar")) {
                c.name = i18nc("@item the default calendar", "Calendar");
            }
            m_calendars.append(c);
        }
    }
    const QStringList people = group.readEntry("FreeBusy", QStringList());
    for (int i = 0; i < people.size(); ++i) {
        const KConfigGroup person = group.group(QStringLiteral("FreeBusy ") + people.at(i));
        CalendarInfo c;
        c.mailboxKey = person.readEntry("Account") + QStringLiteral("/freebusy");
        c.id = -(i + 1);
        c.graphId = people.at(i);
        c.name = person.readEntry("Name", people.at(i));
        c.owner = people.at(i);
        c.color = paletteColor(colour++);
        c.group = i18nc("@title calendar group", "Free/busy (not shared with you)");
        c.shown = !hidden.contains(c.key());
        m_calendars.append(c);
    }
    Q_EMIT changed();
}

bool CalendarData::isFreeBusy(const QString &mailboxKey)
{
    return mailboxKey.endsWith(QLatin1String("/freebusy"));
}

bool CalendarData::isHoliday(const QString &mailboxKey)
{
    return mailboxKey == QLatin1String("local/holidays");
}

void CalendarData::addFreeBusy(const QString &account, const QString &address, const QString &name)
{
    KConfigGroup group = calendarGroup();
    QStringList people = group.readEntry("FreeBusy", QStringList());
    if (!people.contains(address, Qt::CaseInsensitive)) {
        people.append(address);
    }
    group.writeEntry("FreeBusy", people);
    KConfigGroup person = group.group(QStringLiteral("FreeBusy ") + address);
    person.writeEntry("Account", account);
    person.writeEntry("Name", name);
    group.sync();
    reload();
}

void CalendarData::removeFreeBusy(const QString &address)
{
    KConfigGroup group = calendarGroup();
    QStringList people = group.readEntry("FreeBusy", QStringList());
    people.removeAll(address);
    if (people.isEmpty()) {
        group.deleteEntry("FreeBusy");
    } else {
        group.writeEntry("FreeBusy", people);
    }
    group.group(QStringLiteral("FreeBusy ") + address).deleteGroup();
    group.sync();
    m_busy.remove(address.toLower());
    reload();
}

QHash<QString, QStringList> CalendarData::freeBusyMissing(QDate from, QDate to) const
{
    QHash<QString, QStringList> missing;
    const qint64 stale = QDateTime::currentMSecsSinceEpoch() - 10 * 60 * 1000;
    for (const CalendarInfo &c : m_calendars) {
        if (!isFreeBusy(c.mailboxKey) || !c.shown) {
            continue;
        }
        const auto it = m_busy.constFind(c.graphId.toLower());
        if (it == m_busy.cend() || it->from > from || it->to < to || it->fetched < stale) {
            missing[c.mailboxKey.section(QLatin1Char('/'), 0, 0)].append(c.graphId);
        }
    }
    return missing;
}

void CalendarData::setFreeBusy(const QJsonObject &schedule, QDate from, QDate to)
{
    const QString address = schedule.value(QLatin1String("scheduleId")).toString().toLower();
    std::optional<CalendarInfo> calendar;
    for (const CalendarInfo &c : std::as_const(m_calendars)) {
        if (isFreeBusy(c.mailboxKey) && c.graphId.toLower() == address) {
            calendar = c;
        }
    }
    if (!calendar) {
        return;
    }
    Busy busy;
    busy.from = from;
    busy.to = to;
    busy.fetched = QDateTime::currentMSecsSinceEpoch();
    busy.error = schedule.value(QLatin1String("error")).toObject().value(QLatin1String("message")).toString();
    const QStringList statuses{QStringLiteral("free"), QStringLiteral("tentative"), QStringLiteral("busy"), QStringLiteral("oof"), QStringLiteral("workingElsewhere")};
    const QStringList labels{i18nc("@item free/busy", "Free"),
                             i18nc("@item free/busy", "Tentative"),
                             i18nc("@item free/busy", "Busy"),
                             i18nc("@item free/busy", "Away"),
                             i18nc("@item free/busy", "Working elsewhere")};
    const QJsonArray items = schedule.value(QLatin1String("scheduleItems")).toArray();
    for (const QJsonValue &v : items) {
        const QJsonObject item = v.toObject();
        DisplayEvent e;
        e.mailboxKey = calendar->mailboxKey;
        e.calendar = calendar->id;
        e.color = calendar->color;
        e.start = QDateTime::fromMSecsSinceEpoch(Mapper::parseDateTimeTimeZone(item.value(QLatin1String("start")).toObject())).toLocalTime();
        e.end = QDateTime::fromMSecsSinceEpoch(Mapper::parseDateTimeTimeZone(item.value(QLatin1String("end")).toObject())).toLocalTime();
        e.showAs = std::max<int>(0, int(statuses.indexOf(item.value(QLatin1String("status")).toString())));
        // The subject and location only when the colleague lets the organisation see them
        e.subject = item.value(QLatin1String("subject")).toString();
        if (e.subject.isEmpty()) {
            e.subject = labels.value(e.showAs);
        }
        e.location = item.value(QLatin1String("location")).toString();
        e.organizer = calendar->name;
        e.isPrivate = item.value(QLatin1String("isPrivate")).toBool();
        // A block from midnight to midnight is a whole day
        e.allDay = e.start.time() == QTime(0, 0) && e.end.time() == QTime(0, 0) && e.end > e.start;
        e.firstDay = e.start.date();
        e.lastDay = e.allDay ? e.end.date().addDays(-1) : e.end.addMSecs(-1).date();
        if (e.showAs != 0) {
            busy.blocks.append(e);
        }
    }
    m_busy.insert(address, busy);
    Q_EMIT changed();
}

QList<CalendarInfo> CalendarData::calendars() const
{
    return m_calendars;
}

std::optional<CalendarInfo> CalendarData::calendar(const QString &mailboxKey, qint64 id) const
{
    for (const CalendarInfo &c : m_calendars) {
        if (c.mailboxKey == mailboxKey && c.id == id) {
            return c;
        }
    }
    return std::nullopt;
}

void CalendarData::setShown(const QString &key, bool show)
{
    KConfigGroup group = calendarGroup();
    QStringList shown = group.readEntry("Shown", QStringList());
    QStringList hidden = group.readEntry("Hidden", QStringList());
    shown.removeAll(key);
    hidden.removeAll(key);
    (show ? shown : hidden).append(key);
    group.writeEntry("Shown", shown);
    group.writeEntry("Hidden", hidden);
    group.sync();
    for (CalendarInfo &c : m_calendars) {
        if (c.key() == key) {
            c.shown = show;
        }
    }
    Q_EMIT changed();
}

QList<CalendarInfo> CalendarData::writable() const
{
    QList<CalendarInfo> result;
    for (const CalendarInfo &c : m_calendars) {
        if (c.canEdit) {
            result.append(c);
        }
    }
    // The one used last first, then the own default calendars
    const QString last = calendarGroup().readEntry("LastUsed");
    std::stable_sort(result.begin(), result.end(), [this, &last](const CalendarInfo &a, const CalendarInfo &b) {
        const auto rank = [this, &last](const CalendarInfo &c) {
            if (c.key() == last) {
                return -1;
            }
            const std::optional<MailboxInfo> box = m_mailboxes->mailbox(c.mailboxKey);
            return (box && box->own && c.isDefault) ? 0 : (box && box->own ? 1 : 2);
        };
        return rank(a) < rank(b);
    });
    return result;
}

void CalendarData::setLastUsed(const QString &key)
{
    KConfigGroup group = calendarGroup();
    group.writeEntry("LastUsed", key);
    group.sync();
}

QStringList CalendarData::ownAddresses() const
{
    QStringList result;
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        if (!box.address.isEmpty()) {
            result.append(box.address.toLower());
        }
    }
    return result;
}

DisplayEvent CalendarData::display(const QString &mailboxKey, const EventRow &row) const
{
    DisplayEvent e;
    e.mailboxKey = mailboxKey;
    e.id = row.id;
    e.calendar = row.calendar;
    e.graphId = row.graphId;
    e.seriesMaster = row.seriesMaster;
    e.allDay = row.allDay;
    // The cache keeps UTC. All-day events are dates, kept at midnight UTC: read them as dates
    const QDateTime startUtc = QDateTime::fromMSecsSinceEpoch(row.start, QTimeZone::UTC);
    const QDateTime endUtc = QDateTime::fromMSecsSinceEpoch(row.end, QTimeZone::UTC);
    if (e.allDay) {
        e.firstDay = startUtc.date();
        e.lastDay = std::max(e.firstDay, endUtc.date().addDays(endUtc.time() == QTime(0, 0) ? -1 : 0));
        e.start = QDateTime(e.firstDay, QTime(0, 0));
        e.end = QDateTime(e.lastDay.addDays(1), QTime(0, 0));
    } else {
        e.start = startUtc.toLocalTime();
        e.end = endUtc.toLocalTime();
        e.firstDay = e.start.date();
        e.lastDay = e.end.addMSecs(-1).date();
    }
    e.subject = row.subject;
    e.location = row.location;
    e.organizer = row.organizer;
    e.showAs = row.showAs;
    e.response = row.response;
    e.type = row.type;
    e.online = row.isOnlineMeeting;
    e.json = QJsonDocument::fromJson(row.json.toUtf8()).object();
    e.cancelled = e.json.value(QLatin1String("isCancelled")).toBool();
    e.isOrganizer = e.json.contains(QLatin1String("isOrganizer")) ? e.json.value(QLatin1String("isOrganizer")).toBool() : row.response == 1;
    e.isPrivate = e.json.value(QLatin1String("sensitivity")).toString() == QLatin1String("private");
    if (const std::optional<CalendarInfo> c = calendar(mailboxKey, row.calendar)) {
        e.color = c->color;
        e.canEdit = c->canEdit;
    }
    // The first category's colour wins over the calendar's, as in Outlook
    if (m_categoryList) {
        const QStringList categories = categoriesOf(e);
        if (!categories.isEmpty() && m_categoryList->contains(mailboxKey, categories.constFirst())) {
            e.color = m_categoryList->color(mailboxKey, categories.constFirst());
        }
    }
    return e;
}

void CalendarData::setCategoryList(CategoryList *categories)
{
    m_categoryList = categories;
}

QString CalendarData::joinUrlOf(const DisplayEvent &event) const
{
    const auto joinUrl = [](const QJsonObject &json) {
        return json.value(QLatin1String("onlineMeeting")).toObject().value(QLatin1String("joinUrl")).toString();
    };
    QString url = joinUrl(event.json);
    if (url.isEmpty() && !event.seriesMaster.isEmpty()) {
        const Store *store = m_mailboxes->store(event.mailboxKey);
        if (const std::optional<EventRow> master = store ? store->eventByGraphId(event.seriesMaster) : std::nullopt) {
            url = joinUrl(QJsonDocument::fromJson(master->json.toUtf8()).object());
        }
    }
    return url;
}

QStringList CalendarData::categoriesOf(const DisplayEvent &event) const
{
    QJsonArray array = event.json.value(QLatin1String("categories")).toArray();
    if (array.isEmpty() && !event.seriesMaster.isEmpty()) {
        const Store *store = m_mailboxes->store(event.mailboxKey);
        if (const std::optional<EventRow> master = store ? store->eventByGraphId(event.seriesMaster) : std::nullopt) {
            array = QJsonDocument::fromJson(master->json.toUtf8()).object().value(QLatin1String("categories")).toArray();
        }
    }
    QStringList names;
    for (const QJsonValue &v : std::as_const(array)) {
        names.append(v.toString());
    }
    return names;
}

QList<DisplayEvent> CalendarData::events(QDate from, QDate to) const
{
    QList<DisplayEvent> result;
    // A day wider on both sides: all-day events sit at midnight UTC, local days do not
    const qint64 start = QDateTime(from.addDays(-1), QTime(0, 0)).toMSecsSinceEpoch();
    const qint64 end = QDateTime(to.addDays(2), QTime(0, 0)).toMSecsSinceEpoch();
    QHash<QString, QList<qint64>> perMailbox;
    for (const CalendarInfo &c : m_calendars) {
        if (c.shown) {
            perMailbox[c.mailboxKey].append(c.id);
        }
    }
    for (auto it = perMailbox.cbegin(); it != perMailbox.cend(); ++it) {
        const Store *store = m_mailboxes->store(it.key());
        if (!store) {
            continue;
        }
        const QList<EventRow> rows = store->eventsBetween(it.value(), start, end);
        for (const EventRow &row : rows) {
            const DisplayEvent e = display(it.key(), row);
            if (e.lastDay >= from && e.firstDay <= to) {
                result.append(e);
            }
        }
    }
    for (const CalendarInfo &c : m_calendars) {
        if (!isFreeBusy(c.mailboxKey) || !c.shown) {
            continue;
        }
        const auto it = m_busy.constFind(c.graphId.toLower());
        if (it == m_busy.cend()) {
            continue;
        }
        for (const DisplayEvent &e : it->blocks) {
            if (e.lastDay >= from && e.firstDay <= to) {
                result.append(e);
            }
        }
    }
    if (const QString region = CalendarOptions::holidayRegion(); !region.isEmpty()) {
        // Public holidays and days off of the region, as all-day items nobody can change
        const KHolidays::HolidayRegion holidays(region);
        const KHolidays::Holiday::List list = holidays.isValid() ? holidays.rawHolidays(from, to) : KHolidays::Holiday::List();
        for (const KHolidays::Holiday &h : list) {
            if (h.dayType() != KHolidays::Holiday::NonWorkday && !h.categoryList().contains(QLatin1String("public"))) {
                continue;
            }
            DisplayEvent e;
            e.mailboxKey = QStringLiteral("local/holidays");
            e.id = -h.observedStartDate().toJulianDay();
            e.color = QColor(0x8f, 0x9a, 0xa6);
            e.allDay = true;
            e.firstDay = h.observedStartDate();
            e.lastDay = h.observedEndDate();
            e.start = QDateTime(e.firstDay, QTime(0, 0));
            e.end = QDateTime(e.lastDay.addDays(1), QTime(0, 0));
            e.subject = h.name();
            e.showAs = 0;
            result.append(e);
        }
    }
    std::sort(result.begin(), result.end(), [](const DisplayEvent &a, const DisplayEvent &b) {
        if (a.allDay != b.allDay) {
            return a.allDay;
        }
        return a.start != b.start ? a.start < b.start : a.end > b.end;
    });
    return result;
}

QList<DisplayEvent> CalendarData::eventsOf(const QString &mailboxKey, qint64 calendar, QDate from, QDate to) const
{
    QList<DisplayEvent> result;
    const Store *store = m_mailboxes->store(mailboxKey);
    if (!store) {
        return result;
    }
    const qint64 start = QDateTime(from.addDays(-1), QTime(0, 0)).toMSecsSinceEpoch();
    const qint64 end = QDateTime(to.addDays(2), QTime(0, 0)).toMSecsSinceEpoch();
    const QList<EventRow> rows = store->eventsBetween({calendar}, start, end);
    for (const EventRow &row : rows) {
        const DisplayEvent e = display(mailboxKey, row);
        if (e.lastDay >= from && e.firstDay <= to) {
            result.append(e);
        }
    }
    return result;
}

std::optional<DisplayEvent> CalendarData::eventById(const QString &mailboxKey, qint64 id) const
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<EventRow> row = store ? store->event(id) : std::nullopt;
    if (!row) {
        return std::nullopt;
    }
    return display(mailboxKey, *row);
}

}
