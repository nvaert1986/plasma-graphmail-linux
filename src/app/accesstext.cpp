// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "accesstext.h"

#include <KLocalizedString>

#include <QStringList>

namespace GraphMail
{

QString accessProblemText(const QString &code, const QString &who)
{
    if (code == QLatin1String("noSuchMailbox")) {
        return i18nc("@info %1 an address or name", "There is no mailbox with the address %1 in your organisation.", who);
    }
    if (code == QLatin1String("noAccess") || code == QLatin1String("denied")) {
        return i18nc("@info %1 a mailbox", "You have no access to %1. Ask its owner or your administrator to share it with you.", who);
    }
    if (code == QLatin1String("sendAsDenied")) {
        return i18nc("@info %1 an address", "You may not send as %1. Ask your administrator for Send As or Send on Behalf permission.", who);
    }
    if (code == QLatin1String("notShareable")) {
        return i18nc("@info %1 an address", "A calendar cannot be shared with %1: Exchange shares calendars only with people, not with shared mailboxes or groups.", who);
    }
    if (code == QLatin1String("network")) {
        return i18nc("@info", "Microsoft 365 could not be reached. Check the connection and try again.");
    }
    if (code == QLatin1String("readOnly")) {
        return i18nc("@info %1 a calendar", "You can only view %1: its owner did not give you permission to change it.", who);
    }
    return i18nc("@info %1 a mailbox", "Microsoft 365 refused the request for %1.", who);
}

QString accessSummary(bool mail, bool calendar, bool contacts, bool calendarEdit)
{
    if (mail && calendar && contacts) {
        return i18nc("@item what a mailbox gives", "Mail, calendar and contacts");
    }
    if (!mail && calendar && !contacts) {
        return calendarEdit ? i18nc("@item what a mailbox gives", "Calendar only") : i18nc("@item what a mailbox gives", "Calendar only (read only)");
    }
    if (!mail && !calendar && !contacts) {
        return i18nc("@item what a mailbox gives", "Free/busy only (not shared with you)");
    }
    QStringList parts;
    if (mail) {
        parts << i18nc("@item part of a mailbox", "mail");
    }
    if (calendar) {
        parts << i18nc("@item part of a mailbox", "calendar");
    }
    if (contacts) {
        parts << i18nc("@item part of a mailbox", "contacts");
    }
    QString text = parts.join(i18nc("@item separator between parts of a mailbox", ", "));
    text[0] = text.at(0).toUpper();
    return text;
}

}
