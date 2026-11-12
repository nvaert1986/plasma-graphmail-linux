// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "store.h"

#include <QDate>
#include <QJsonObject>
#include <QString>

// Microsoft Graph JSON to cache rows. Pure functions, no network or database.
namespace GraphMail::Mapper
{

// The message fields the cache keeps for every message (headers; bodies come on demand). Same
// list as the phase 0 probe measured.
QString messageSelect();
QString folderSelect();

// ISO 8601 ("2026-11-11T07:38:45Z") to UTC milliseconds; 0 when empty or invalid
qint64 parseTime(const QString &iso);
// Graph's dateTimeTimeZone ({"dateTime": "2026-11-11T14:00:00.0000000", "timeZone": "UTC"}): the
// zone is UTC when we asked for it (Prefer: outlook.timezone="UTC"), else a Windows or IANA name
qint64 parseDateTimeTimeZone(const QJsonObject &value);
// A To Do due date. Graph turns the midnight it was set at into UTC (Amsterdam's 20 November comes
// back as 19 November 23:00 UTC): a time of midnight is the date itself, any other time is that
// moment's local date. dueDateTime() writes it back as midnight UTC, which reads the same anywhere.
QDate dueDate(const QJsonObject &dueDateTime);
QJsonObject dueDateTime(const QDate &date);

bool isRemoved(const QJsonObject &item);

MessageRow message(const QJsonObject &item, qint64 folder);
// 0 mail, 1 meeting request, 2 meeting response, 3 other meeting message, from @odata.type
int messageItemType(const QJsonObject &item);
CalendarRow calendar(const QJsonObject &item, const QString &ownAddress);
// Incremental delta rounds may send only the changed properties (a read change comes as id,
// parentFolderId and isRead): such an item has no changeKey and must be merged into the cached row
bool isSparse(const QJsonObject &item);
MessageRow mergeMessage(MessageRow base, const QJsonObject &item, qint64 folder);
FolderRow folder(const QJsonObject &item);
ItemRow event(const QJsonObject &item);
ItemRow contact(const QJsonObject &item);
ItemRow task(const QJsonObject &item);
AttachmentRow attachment(const QJsonObject &item);

}
