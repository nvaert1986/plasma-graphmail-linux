// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pstmessaging.h"

#include <QDate>
#include <QJsonObject>
#include <QStringList>
#include <QTimeZone>

#include <functional>

// Contacts, calendar items and tasks from MAPI properties (a PST or .msg) to what Graph takes to
// create them (docs/PHASE9B.md): contacts ([MS-OXOCNTC]) for /contactFolders/{id}/contacts,
// appointments ([MS-OXOCAL]) for /calendars/{id}/events, tasks ([MS-OXOTASK]) for
// /todo/lists/{id}/tasks. Property IDs and LIDs from [MS-OXPROPS].
namespace GraphMail::MapiItems
{

enum class Kind {
    Mail,
    Contact,
    Event,
    Task,
    // Distribution lists, notes, journal entries and the like: left out
    Other,
};

// What a message class is
Kind kindOf(const QString &messageClass);
// What a folder holds by its container class (PidTagContainerClass); none is mail
Kind folderKind(const QString &containerClass);

struct Item {
    QJsonObject json;
    // What did not come along, for the import's log
    QStringList skipped;
};

Item contact(const Pst::Message &message, const QHash<quint16, Pst::NamedProperty> &names);
// Attendees are left out (Graph would invite them again) and listed in skipped
Item event(const Pst::Message &message, const QHash<quint16, Pst::NamedProperty> &names);
Item task(const Pst::Message &message, const QHash<quint16, Pst::NamedProperty> &names);

// The Windows time zone name in a TZDEFINITION (PidLidAppointmentTimeZoneDefinition*, [MS-OXOCAL]
// 2.2.1.41); empty when there is none
QString timeZoneName(const QByteArray &definition);
// A series' Graph recurrence from PidLidAppointmentRecur ([MS-OXOCAL] 2.2.1.44); nullopt with a
// note when Graph cannot hold it (other calendars). notes also gets what is lost on the way.
std::optional<QJsonObject> recurrence(const QByteArray &blob, const QString &timeZone, QStringList *notes);

// The other way, for PST export (docs/PHASE9B.md): Graph's contacts, events and To Do tasks as
// Outlook items, bodies also as compressed RTF ([MS-OXRTFCP]) the way Outlook keeps them.

// The property ID of a named property (set, LID) in the target file
using NameId = std::function<quint16(const QUuid &set, quint32 lid)>;

Pst::Message contactToMapi(const QJsonObject &contact, const NameId &name, quint16 keywords);
// An event (single, series master or a changed occurrence on its own). deleted: the original
// dates of a series' occurrences that are exported apart (or were cancelled), local dates.
Pst::Message eventToMapi(const QJsonObject &event, const NameId &name, quint16 keywords, const QList<QDate> &deleted = {});
Pst::Message taskToMapi(const QJsonObject &task, const NameId &name, quint16 keywords);

// PidLidAppointmentRecur ([MS-OXOCAL] 2.2.1.44) for Graph's recurrence; start: the first
// occurrence's local start; empty with a note when Graph's pattern has no Outlook form
QByteArray recurrenceBlob(const QJsonObject &recurrence, const QDateTime &start, int durationMinutes, const QList<QDate> &deleted, QStringList *notes);
// The dates a Graph recurrence gives from its start, at most count, until (inclusive) when valid
QList<QDate> occurrences(const QJsonObject &recurrence, int count, QDate until = {});
// TZDEFINITION ([MS-OXOCAL] 2.2.1.41): a rule per year, effectiveYear's marked effective
QByteArray timeZoneDefinition(const QTimeZone &zone, const QString &keyName, const QList<int> &years, int effectiveYear);
// PidLidTimeZoneStruct ([MS-OXOCAL] 2.2.1.39) for a year
QByteArray timeZoneStruct(const QTimeZone &zone, int year);
// A Graph time zone name (Windows, IANA or "UTC") as a zone; the system's when unknown
QTimeZone zoneFor(const QString &name);

}
