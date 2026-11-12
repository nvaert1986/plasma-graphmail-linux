// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonArray>
#include <QList>
#include <QString>

namespace GraphMail
{

// A reminder from Exchange's list (/me/reminderView): Outlook's reminder window
struct OverdueReminder {
    QString eventId;
    QString subject;
    qint64 start = 0;
    qint64 end = 0;
};

namespace Reminders
{
// Exchange returns at most this many reminders per call, oldest first, and no next page
constexpr int viewLimit = 200;
// The reminders of events that are over, from a reminderView answer (times in UTC)
QList<OverdueReminder> overdue(const QJsonArray &view, qint64 now);
}

}
