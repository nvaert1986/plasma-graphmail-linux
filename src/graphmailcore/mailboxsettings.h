// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDate>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QTime>

namespace GraphMail
{

// The mailbox's automatic replies (mailboxSettings.automaticRepliesSetting, docs/PHASE9.md).
// Only the user's own mailbox: Graph refuses another mailbox's settings (403).
struct AutomaticReplies {
    enum class Status { Disabled, AlwaysEnabled, Scheduled };
    enum class External { None, ContactsOnly, All };

    Status status = Status::Disabled;
    External external = External::All;
    // Milliseconds since the epoch; only used when scheduled
    qint64 start = 0;
    qint64 end = 0;
    // HTML, as Outlook stores them
    QString internalMessage;
    QString externalMessage;

    static AutomaticReplies fromJson(const QJsonObject &setting);
    // The automaticRepliesSetting object for a PATCH of /mailboxSettings (times in UTC)
    QJsonObject toJson() const;
    // Whether Exchange sends replies at this moment (Outlook's yellow bar)
    bool activeAt(qint64 now) const;
};

// The mailbox's working hours (mailboxSettings.workingHours): the days, the hours and the time
// zone they are in, which need not be the computer's (the user's: Central Europe while the mailbox
// says W. Europe)
struct WorkingHours {
    // Qt's day numbers, 1 = Monday .. 7 = Sunday
    QList<int> days{1, 2, 3, 4, 5};
    QTime start{8, 0};
    QTime end{17, 0};
    // A Windows time zone name as Graph gives it; empty = the computer's
    QString timeZone;

    static WorkingHours fromJson(const QJsonObject &workingHours);
    QJsonObject toJson() const;
    // Start and end on this day in hours of the computer's time zone (8.5 = 08:30), for the
    // calendar's shading; {0, 0} when the day is not a working day
    QPair<double, double> localHours(QDate day) const;
};

}
