// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "graphresponse.h"

#include <QByteArray>
#include <QString>
#include <QTimeZone>
#include <QUrl>

namespace GraphCore
{

// Values for the Prefer header. Graph takes several, comma-separated, in one header (header()).
namespace Prefer
{
// Item ids that survive moves between folders. Every request of the mail client sends it: a
// cache keyed by the regular ids breaks as soon as a message is moved.
QByteArray immutableId();
// Start and end times of events in this zone (sent as its Windows name, e.g. "W. Europe
// Standard Time"); UTC when Qt knows no Windows name for it
QByteArray timeZone(const QTimeZone &zone = QTimeZone::systemTimeZone());
QByteArray textBody();
QByteArray htmlBody();
// Delta and list pages of at most this many items
QByteArray maxPageSize(int items);
QPair<QByteArray, QByteArray> header(const QList<QByteArray> &preferences);
}

// "/me" for the signed-in user's own mailbox, "/users/<address or id>" for another one (a shared
// mailbox, a delegate's calendar)
QString mailboxRoot(const QString &mailbox = {});

// The key throttling is kept under. Exchange Online throttles per app and mailbox (and allows four
// concurrent requests per mailbox), so Outlook resources (messages, events, contacts, To Do, ...)
// get "<scope>|<mailbox>"; everything else (OneDrive, SharePoint, the directory) and other hosts
// get "<scope>", the account. "/users/<own id or address>" counts as "/me" once the scope's own
// ids are known (setOwnMailboxAliases).
QString throttleKeyFor(const QString &scope, const QUrl &url, const QString &graphHost);
// The signed-in user's object id and addresses for a throttle scope (the account): requests to
// /users/<one of them> share the "/me" mailbox's key. Process-wide and thread-safe
void setOwnMailboxAliases(const QString &scope, const QStringList &aliases);
bool isMailboxKey(const QString &key);

}
