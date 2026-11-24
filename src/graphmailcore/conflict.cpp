// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "conflict.h"
#include "graphmapper.h"

namespace GraphMail
{

namespace
{
// The same value, a time also when written in another zone: Graph answers in UTC what was sent
// in the local zone (0.14.5: a second drag of an event in the calendar was taken for a conflict)
bool same(const QJsonValue &a, const QJsonValue &b)
{
    if (a == b) {
        return true;
    }
    const QJsonObject x = a.toObject();
    const QJsonObject y = b.toObject();
    if (!x.contains(QLatin1String("dateTime")) || !y.contains(QLatin1String("dateTime"))) {
        return false;
    }
    const qint64 at = Mapper::parseDateTimeTimeZone(x);
    return at != 0 && at == Mapper::parseDateTimeTimeZone(y);
}
}

QStringList conflictingFields(const QJsonObject &changes, const QJsonObject &previous, const QJsonObject &server)
{
    QStringList both;
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
        const QJsonValue before = previous.value(it.key());
        const QJsonValue now = server.value(it.key());
        // Changed there (not what the change was made on) and not to what the user wants either
        if (!same(now, before) && !same(now, it.value())) {
            both.append(it.key());
        }
    }
    return both;
}

QByteArray ifMatch(const QJsonObject &previous)
{
    return previous.value(QLatin1String("@odata.etag")).toString().toUtf8();
}

}
