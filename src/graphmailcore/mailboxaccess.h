// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace GraphCore
{
class GraphClient;
struct GraphResponse;
}

namespace GraphMail
{

// What another mailbox gives the signed-in user (docs/PHASE7.md): Graph answers 404
// ErrorItemNotFound where the user has no access and 404 ErrorInvalidUser for an unknown address.
struct MailboxAccess {
    QString id;
    QString name;
    QString address;
    bool mail = false;
    bool calendar = false;
    bool calendarEdit = false;
    bool contacts = false;
    // Empty when the address was found; else a reason code, see accessProblem()
    QString problem;
    QString detail;

    bool any() const
    {
        return mail || calendar || contacts;
    }
    // "mail", "calendar", "contacts": what is stored per mailbox and synced
    QStringList parts() const;
    QJsonObject toJson() const;
    static MailboxAccess fromJson(const QJsonObject &o);
};

// Why Graph refused something of another mailbox, as a code the app turns into a sentence:
// "noAccess", "noSuchMailbox", "denied" (403), "sendAsDenied", "notShareable" (a calendar share
// for a shared mailbox or group), "network", "error"; empty when ok
QString accessProblem(const GraphCore::GraphResponse &response);

// Resolves the address in the directory, then tries the mailbox's inbox, calendar and contacts.
// Blocking: run it off the GUI and worker threads.
MailboxAccess checkMailboxAccess(GraphCore::GraphClient &client, const QString &address);

}
