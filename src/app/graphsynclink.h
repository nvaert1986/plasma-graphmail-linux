// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class QObject;

// Graph Sync, the suite's OneDrive and SharePoint client (phase 10 step 7): when it is installed,
// Graph Mail opens SharePoint/OneDrive links in their synced copy, saves attachments into synced
// folders, attaches synced files as links and puts large attachments in OneDrive. Without it none
// of this is offered. Calls go over D-Bus to graphsyncd (io.github.nvaert1986.GraphSync), which
// answers in JSON; the asynchronous ones never block the window.
namespace GraphMail::GraphSyncLink
{
// A synced location (Graph Sync's "root")
struct Location {
    QString id;
    QString name;
    QString localPath;
    // personal (OneDrive), library, folder or shortcut
    QString kind;
    QString accountId;
};

// Graph Sync is installed: its service runs, or D-Bus can start it
bool available();
// Its synced locations (all accounts, or only accountId's); empty when it is not available
QList<Location> locations(const QString &accountId = {});
// The account's own OneDrive location; id empty when there is none
Location personalLocation(const QString &accountId);
// The location a local path lies in; id empty when none
Location locationOf(const QString &path, const QString &accountId = {});
// A SharePoint or OneDrive address (worth asking Graph Sync about)
bool isCloudUrl(const QString &url);

using Answer = std::function<void(const QJsonObject &answer)>;
// The online address of a synced file: {url, name, accountId, synced} or {error, notSynced}
void fileLink(const QString &path, QObject *context, const Answer &done);
// Gives these people access, no invitation mail (role write: can edit, Outlook's default):
// {url, name} or {error, notSynced}
void shareFile(const QString &path, const QStringList &emails, QObject *context, const Answer &done);
// The local copy of a SharePoint/OneDrive link: {path, placeholder} or {} (none, or no answer)
void localPathForUrl(const QString &url, QObject *context, const Answer &done);
}
