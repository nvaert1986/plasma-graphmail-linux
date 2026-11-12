// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "templates.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace GraphMail
{

QString Templates::directory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/graphmail/templates");
}

QList<QJsonObject> Templates::all()
{
    QList<QJsonObject> result;
    const QDir dir(directory());
    const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString &name : files) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        QJsonObject t = QJsonDocument::fromJson(file.readAll()).object();
        if (!t.isEmpty()) {
            t.insert(QStringLiteral("id"), name.chopped(5));
            result.append(t);
        }
    }
    std::sort(result.begin(), result.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return QString::localeAwareCompare(a.value(QLatin1String("name")).toString(), b.value(QLatin1String("name")).toString()) < 0;
    });
    return result;
}

bool Templates::save(QJsonObject *templateJson)
{
    QString id = templateJson->value(QLatin1String("id")).toString();
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        templateJson->insert(QStringLiteral("id"), id);
    }
    QDir().mkpath(directory());
    QSaveFile file(directory() + QLatin1Char('/') + id + QStringLiteral(".json"));
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(*templateJson).toJson());
    return file.commit();
}

void Templates::remove(const QString &id)
{
    if (!id.isEmpty() && !id.contains(QLatin1Char('/'))) {
        QFile::remove(directory() + QLatin1Char('/') + id + QStringLiteral(".json"));
    }
}

}
