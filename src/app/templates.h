// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

namespace GraphMail
{

// Message templates (docs/PHASE9.md), as Outlook's "My Templates": a name, recipients, subject,
// the text and the message options; no attachments. One JSON file each, on this computer.
namespace Templates
{
// Every template, by name; each has "id" and "name"
QList<QJsonObject> all();
// Saves it (a new id when it has none); false when it could not be written
bool save(QJsonObject *templateJson);
void remove(const QString &id);
QString directory();
}

}
