// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mailboxaccess.h"

#include "graphclient.h"
#include "graphresponse.h"

#include <QJsonArray>
#include <QUrl>

namespace GraphMail
{

QStringList MailboxAccess::parts() const
{
    QStringList p;
    if (mail) {
        p << QStringLiteral("mail");
    }
    if (calendar) {
        p << QStringLiteral("calendar");
    }
    if (contacts) {
        p << QStringLiteral("contacts");
    }
    return p;
}

QJsonObject MailboxAccess::toJson() const
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("address"), address},
            {QStringLiteral("mail"), mail},
            {QStringLiteral("calendar"), calendar},
            {QStringLiteral("calendarEdit"), calendarEdit},
            {QStringLiteral("contacts"), contacts},
            {QStringLiteral("problem"), problem},
            {QStringLiteral("detail"), detail}};
}

MailboxAccess MailboxAccess::fromJson(const QJsonObject &o)
{
    MailboxAccess a;
    a.id = o.value(QLatin1String("id")).toString();
    a.name = o.value(QLatin1String("name")).toString();
    a.address = o.value(QLatin1String("address")).toString();
    a.mail = o.value(QLatin1String("mail")).toBool();
    a.calendar = o.value(QLatin1String("calendar")).toBool();
    a.calendarEdit = o.value(QLatin1String("calendarEdit")).toBool();
    a.contacts = o.value(QLatin1String("contacts")).toBool();
    a.problem = o.value(QLatin1String("problem")).toString();
    a.detail = o.value(QLatin1String("detail")).toString();
    return a;
}

QString accessProblem(const GraphCore::GraphResponse &response)
{
    if (response.ok()) {
        return {};
    }
    if (response.isNetworkError()) {
        return QStringLiteral("network");
    }
    const QString code = response.graphErrorCode();
    if (code == QLatin1String("ErrorInvalidUser") || code == QLatin1String("MailboxNotEnabledForRESTAPI")
        || code == QLatin1String("ResourceNotFound") || code == QLatin1String("Request_ResourceNotFound")) {
        return QStringLiteral("noSuchMailbox");
    }
    if (code == QLatin1String("ErrorSendAsDenied")) {
        return QStringLiteral("sendAsDenied");
    }
    // Sharing a calendar with a shared mailbox, a group or a room: Exchange answers 500 with this
    // inner message (phase 7 live test); only user mailboxes can receive a share
    const QString inner = response.json().value(QLatin1String("error")).toObject().value(QLatin1String("innererror")).toObject().value(QLatin1String("message")).toString();
    if (inner.startsWith(QLatin1String("No sharing handler"))) {
        return QStringLiteral("notShareable");
    }
    if (response.status == 404) {
        return QStringLiteral("noAccess");
    }
    if (response.status == 403 || response.status == 401) {
        return QStringLiteral("denied");
    }
    return QStringLiteral("error");
}

MailboxAccess checkMailboxAccess(GraphCore::GraphClient &client, const QString &address)
{
    MailboxAccess access;
    const QString quoted = QString(address.trimmed()).replace(QLatin1Char('\''), QStringLiteral("''"));
    const GraphCore::GraphResponse found = client.get(
        QStringLiteral("/users?$select=id,mail,userPrincipalName,displayName&$filter=")
        + QString::fromLatin1(QUrl::toPercentEncoding(QStringLiteral("mail eq '%1' or userPrincipalName eq '%1'").arg(quoted))));
    if (!found.ok()) {
        access.problem = accessProblem(found);
        access.detail = found.userMessage();
        return access;
    }
    const QJsonArray users = found.json().value(QLatin1String("value")).toArray();
    if (users.isEmpty()) {
        access.problem = QStringLiteral("noSuchMailbox");
        return access;
    }
    const QJsonObject user = users.first().toObject();
    access.id = user.value(QLatin1String("id")).toString();
    access.name = user.value(QLatin1String("displayName")).toString();
    access.address = user.value(QLatin1String("mail")).toString();
    if (access.address.isEmpty()) {
        access.address = user.value(QLatin1String("userPrincipalName")).toString();
    }
    const QString root = QStringLiteral("/users/") + access.id;

    // An unknown answer on one part (network, throttling) makes the whole check fail: a mailbox
    // must not be stored with a part missing only because of a hiccup
    auto probe = [&](const QString &path, bool &part) -> bool {
        const GraphCore::GraphResponse r = client.get(root + path);
        const QString problem = accessProblem(r);
        if (problem.isEmpty()) {
            part = true;
            return true;
        }
        if (problem == QLatin1String("noAccess") || problem == QLatin1String("denied")) {
            return true;
        }
        if (problem == QLatin1String("noSuchMailbox")) {
            // A user without a mailbox (a guest or an unlicensed account)
            access.problem = problem;
            return false;
        }
        access.problem = problem;
        access.detail = r.userMessage();
        return false;
    };
    if (!probe(QStringLiteral("/mailFolders/inbox?$select=id"), access.mail)) {
        return access;
    }
    bool calendar = false;
    const GraphCore::GraphResponse cal = client.get(root + QStringLiteral("/calendar?$select=id,canEdit"));
    const QString calendarProblem = accessProblem(cal);
    if (calendarProblem.isEmpty()) {
        calendar = true;
        access.calendarEdit = cal.json().value(QLatin1String("canEdit")).toBool();
    } else if (calendarProblem != QLatin1String("noAccess") && calendarProblem != QLatin1String("denied")) {
        access.problem = calendarProblem;
        access.detail = cal.userMessage();
        return access;
    }
    access.calendar = calendar;
    if (!probe(QStringLiteral("/contacts?$top=1&$select=id"), access.contacts)) {
        return access;
    }
    return access;
}

}
