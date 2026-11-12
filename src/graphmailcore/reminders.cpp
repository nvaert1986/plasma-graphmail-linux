// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "reminders.h"
#include "graphmapper.h"

#include <QJsonObject>

namespace GraphMail
{

QList<OverdueReminder> Reminders::overdue(const QJsonArray &view, qint64 now)
{
    QList<OverdueReminder> result;
    for (const QJsonValue &v : view) {
        const QJsonObject r = v.toObject();
        OverdueReminder o;
        o.eventId = r.value(QLatin1String("eventId")).toString();
        o.subject = r.value(QLatin1String("eventSubject")).toString();
        o.start = Mapper::parseDateTimeTimeZone(r.value(QLatin1String("eventStartTime")).toObject());
        o.end = Mapper::parseDateTimeTimeZone(r.value(QLatin1String("eventEndTime")).toObject());
        // Still running or to come: the reminders from the cache cover those
        if (!o.eventId.isEmpty() && o.end > 0 && o.end <= now) {
            result.append(o);
        }
    }
    return result;
}

}
