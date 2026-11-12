// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "graphresponse.h"

#include <QJsonDocument>

namespace GraphCore
{

QJsonObject GraphResponse::json() const
{
    return QJsonDocument::fromJson(body).object();
}

QByteArray GraphResponse::header(const QByteArray &name) const
{
    for (const auto &[key, value] : headers) {
        if (key.compare(name, Qt::CaseInsensitive) == 0) {
            return value;
        }
    }
    return {};
}

QString GraphResponse::graphErrorCode() const
{
    return json().value(QLatin1String("error")).toObject().value(QLatin1String("code")).toString();
}

QString GraphResponse::describe() const
{
    const QJsonObject error = json().value(QLatin1String("error")).toObject();
    if (!error.isEmpty()) {
        return QStringLiteral("HTTP %1 %2: %3")
            .arg(status)
            .arg(error.value(QLatin1String("code")).toString(), error.value(QLatin1String("message")).toString());
    }
    if (status == 0) {
        return errorString;
    }
    return QStringLiteral("HTTP %1 %2").arg(status).arg(errorString);
}

}
