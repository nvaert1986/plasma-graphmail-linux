// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "conflict.h"

namespace GraphMail
{

QStringList conflictingFields(const QJsonObject &changes, const QJsonObject &previous, const QJsonObject &server)
{
    QStringList both;
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
        const QJsonValue before = previous.value(it.key());
        const QJsonValue now = server.value(it.key());
        // Changed there (not what the change was made on) and not to what the user wants either
        if (now != before && now != it.value()) {
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
