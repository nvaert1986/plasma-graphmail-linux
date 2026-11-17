// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "graphsynclink.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>

namespace GraphMail::GraphSyncLink
{

namespace
{
const QString service = QStringLiteral("io.github.nvaert1986.GraphSync");
const QString path = QStringLiteral("/GraphSync");
const QString iface = QStringLiteral("io.github.nvaert1986.GraphSync");

QDBusMessage call(const QString &method, const QVariantList &arguments)
{
    QDBusMessage message = QDBusMessage::createMethodCall(service, path, iface, method);
    message.setArguments(arguments);
    return message;
}

// The answer of an asynchronous call, as JSON; {error} when D-Bus itself failed
void callAsync(const QString &method, const QVariantList &arguments, int timeoutMs, QObject *context, const Answer &done)
{
    QDBusPendingCall pending = QDBusConnection::sessionBus().asyncCall(call(method, arguments), timeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(pending, context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, [watcher, done] {
        watcher->deleteLater();
        const QDBusPendingReply<QString> reply = *watcher;
        if (reply.isError()) {
            done(QJsonObject{{QStringLiteral("error"), reply.error().message()}});
            return;
        }
        done(QJsonDocument::fromJson(reply.value().toUtf8()).object());
    });
}
}

bool available()
{
    QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    if (!bus) {
        return false;
    }
    if (bus->isServiceRegistered(service)) {
        return true;
    }
    const QDBusReply<QStringList> activatable = bus->activatableServiceNames();
    return activatable.isValid() && activatable.value().contains(service);
}

QList<Location> locations(const QString &accountId)
{
    if (!available()) {
        return {};
    }
    // Short: graphsyncd answers its status from memory (D-Bus activation may start it first)
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call(QStringLiteral("Status"), {}), QDBus::Block, 10000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return {};
    }
    QList<Location> result;
    const QJsonArray roots = QJsonDocument::fromJson(reply.arguments().constFirst().toString().toUtf8()).object().value(QLatin1String("roots")).toArray();
    for (const QJsonValue &value : roots) {
        const QJsonObject r = value.toObject();
        Location location{r.value(QLatin1String("id")).toString(),
                          r.value(QLatin1String("name")).toString(),
                          r.value(QLatin1String("localPath")).toString(),
                          r.value(QLatin1String("kind")).toString(),
                          r.value(QLatin1String("accountId")).toString()};
        // Backup locations (PC folders) are not places to share or save to from mail
        if (r.value(QLatin1String("backup")).toBool() || location.localPath.isEmpty()) {
            continue;
        }
        if (accountId.isEmpty() || location.accountId == accountId) {
            result.append(location);
        }
    }
    return result;
}

Location personalLocation(const QString &accountId)
{
    const QList<Location> all = locations(accountId);
    for (const Location &l : all) {
        if (l.kind == QLatin1String("personal")) {
            return l;
        }
    }
    return {};
}

Location locationOf(const QString &filePath, const QString &accountId)
{
    const QString clean = QDir::cleanPath(filePath);
    const QList<Location> all = locations(accountId);
    for (const Location &l : all) {
        const QString root = QDir::cleanPath(l.localPath);
        if (clean == root || clean.startsWith(root + QLatin1Char('/'))) {
            return l;
        }
    }
    return {};
}

bool isCloudUrl(const QString &url)
{
    const QUrl parsed(url);
    const QString host = parsed.host().toLower();
    return parsed.scheme() == QLatin1String("https")
        && (host.endsWith(QLatin1String(".sharepoint.com")) || host == QLatin1String("onedrive.live.com") || host == QLatin1String("1drv.ms"));
}

void fileLink(const QString &filePath, QObject *context, const Answer &done)
{
    callAsync(QStringLiteral("FileLink"), {filePath}, 60000, context, done);
}

void shareFile(const QString &filePath, const QStringList &emails, QObject *context, const Answer &done)
{
    callAsync(QStringLiteral("ShareFile"), {filePath, emails, QStringLiteral("write")}, 60000, context, done);
}

void localPathForUrl(const QString &url, QObject *context, const Answer &done)
{
    // A link click must not wait long: the browser is the fallback
    callAsync(QStringLiteral("LocalPathForUrl"), {url}, 10000, context, [done](const QJsonObject &answer) {
        done(answer.contains(QLatin1String("error")) ? QJsonObject() : answer);
    });
}

}
