// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

namespace GraphMail
{

// The sentences for graphmailcore's access codes (accessProblem(), docs/PHASE7.md): what went
// wrong with someone else's mailbox, said so the user knows what to do
QString accessProblemText(const QString &code, const QString &who);
// "Mail, calendar and contacts", "Calendar only (read only)", ...
QString accessSummary(bool mail, bool calendar, bool contacts, bool calendarEdit = true);

}
