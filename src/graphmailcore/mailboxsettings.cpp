// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mailboxsettings.h"
#include "graphmapper.h"

#include <QDateTime>
#include <QJsonArray>
#include <QTimeZone>

#include <algorithm>

namespace GraphMail
{

AutomaticReplies AutomaticReplies::fromJson(const QJsonObject &setting)
{
    AutomaticReplies r;
    const QString status = setting.value(QLatin1String("status")).toString();
    if (status == QLatin1String("alwaysEnabled")) {
        r.status = Status::AlwaysEnabled;
    } else if (status == QLatin1String("scheduled")) {
        r.status = Status::Scheduled;
    }
    const QString external = setting.value(QLatin1String("externalAudience")).toString();
    if (external == QLatin1String("none")) {
        r.external = External::None;
    } else if (external == QLatin1String("contactsOnly")) {
        r.external = External::ContactsOnly;
    }
    r.start = Mapper::parseDateTimeTimeZone(setting.value(QLatin1String("scheduledStartDateTime")).toObject());
    r.end = Mapper::parseDateTimeTimeZone(setting.value(QLatin1String("scheduledEndDateTime")).toObject());
    r.internalMessage = setting.value(QLatin1String("internalReplyMessage")).toString();
    r.externalMessage = setting.value(QLatin1String("externalReplyMessage")).toString();
    return r;
}

QJsonObject AutomaticReplies::toJson() const
{
    auto utc = [](qint64 ms) {
        return QJsonObject{{QStringLiteral("dateTime"), QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))},
                           {QStringLiteral("timeZone"), QStringLiteral("UTC")}};
    };
    static const char *const statuses[] = {"disabled", "alwaysEnabled", "scheduled"};
    static const char *const audiences[] = {"none", "contactsOnly", "all"};
    QJsonObject o{{QStringLiteral("status"), QLatin1String(statuses[int(status)])},
                  {QStringLiteral("externalAudience"), QLatin1String(audiences[int(external)])},
                  {QStringLiteral("internalReplyMessage"), internalMessage},
                  {QStringLiteral("externalReplyMessage"), externalMessage}};
    if (status == Status::Scheduled) {
        o.insert(QStringLiteral("scheduledStartDateTime"), utc(start));
        o.insert(QStringLiteral("scheduledEndDateTime"), utc(end));
    }
    return o;
}

bool AutomaticReplies::activeAt(qint64 now) const
{
    switch (status) {
    case Status::AlwaysEnabled:
        return true;
    case Status::Scheduled:
        return start <= now && now < end;
    case Status::Disabled:
        break;
    }
    return false;
}

namespace
{
const char *const dayNames[] = {"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"};

QTimeZone zoneNamed(const QString &name)
{
    if (name.isEmpty()) {
        return QTimeZone::systemTimeZone();
    }
    QTimeZone zone(name.toUtf8());
    if (!zone.isValid()) {
        zone = QTimeZone(QTimeZone::windowsIdToDefaultIanaId(name.toUtf8()));
    }
    return zone.isValid() ? zone : QTimeZone::systemTimeZone();
}
}

WorkingHours WorkingHours::fromJson(const QJsonObject &workingHours)
{
    WorkingHours w;
    if (workingHours.isEmpty()) {
        return w;
    }
    w.days.clear();
    const QJsonArray days = workingHours.value(QLatin1String("daysOfWeek")).toArray();
    for (const QJsonValue &d : days) {
        for (int i = 0; i < 7; ++i) {
            if (d.toString() == QLatin1String(dayNames[i])) {
                w.days.append(i + 1);
            }
        }
    }
    std::sort(w.days.begin(), w.days.end());
    // "08:00:00.0000000"
    auto time = [&workingHours](const char *key, QTime fallback) {
        const QTime t = QTime::fromString(workingHours.value(QLatin1String(key)).toString().left(8), QStringLiteral("HH:mm:ss"));
        return t.isValid() ? t : fallback;
    };
    w.start = time("startTime", w.start);
    w.end = time("endTime", w.end);
    w.timeZone = workingHours.value(QLatin1String("timeZone")).toObject().value(QLatin1String("name")).toString();
    return w;
}

QJsonObject WorkingHours::toJson() const
{
    QJsonArray daysOfWeek;
    for (int d : days) {
        if (d >= 1 && d <= 7) {
            daysOfWeek.append(QLatin1String(dayNames[d - 1]));
        }
    }
    return {{QStringLiteral("daysOfWeek"), daysOfWeek},
            {QStringLiteral("startTime"), start.toString(QStringLiteral("HH:mm:ss.0000000"))},
            {QStringLiteral("endTime"), end.toString(QStringLiteral("HH:mm:ss.0000000"))},
            {QStringLiteral("timeZone"), QJsonObject{{QStringLiteral("name"), timeZone}}}};
}

QPair<double, double> WorkingHours::localHours(QDate day) const
{
    if (!days.contains(day.dayOfWeek())) {
        return {0, 0};
    }
    const QTimeZone zone = zoneNamed(timeZone);
    const QTimeZone local = QTimeZone::systemTimeZone();
    auto hours = [&](QTime t) {
        const QDateTime there(day, t, zone);
        const QDateTime here = there.toTimeZone(local);
        // A day boundary crossed by the conversion: clamp to this day
        if (here.date() < day) {
            return 0.0;
        }
        if (here.date() > day) {
            return 24.0;
        }
        return QTime(0, 0).secsTo(here.time()) / 3600.0;
    };
    return {hours(start), hours(end)};
}

}
