// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "daemonclient.h"

#include <QDBusConnection>
#include <QJsonArray>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QJsonDocument>
#include <QJsonObject>

namespace GraphMail
{

namespace
{
const char *const path = "/GraphMail";
}

QString DaemonClient::service()
{
    return QStringLiteral("io.github.nvaert1986.GraphMail");
}

DaemonClient::DaemonClient(QObject *parent)
    : QObject(parent)
    , m_watcher(new QDBusServiceWatcher(service(), QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForOwnerChange, this))
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    const QString iface = service();
    const QString object = QString::fromLatin1(path);
    bus.connect(service(), object, iface, QStringLiteral("CollectionChanged"), this, SLOT(onCollectionChanged(QString,QString,QString,int)));
    bus.connect(service(), object, iface, QStringLiteral("StatusChanged"), this, SLOT(onStatusChanged(QString,QString,QString)));
    bus.connect(service(), object, iface, QStringLiteral("BodyReady"), this, SLOT(onBodyReady(QString,QString,qlonglong)));
    bus.connect(service(), object, iface, QStringLiteral("AttachmentReady"), this, SLOT(onAttachmentReady(QString,QString,qlonglong,qlonglong,bool)));
    bus.connect(service(), object, iface, QStringLiteral("OperationFailed"), this, SLOT(onOperationFailed(QString,QString,QString,QString)));
    bus.connect(service(), object, iface, QStringLiteral("OutgoingChanged"), this, SLOT(onOutgoingChanged(QString,QString,QString,QString,QString)));
    bus.connect(service(), object, iface, QStringLiteral("SendRightLearned"), this, SLOT(onSendRightLearned(QString,QString,QString)));

    connect(m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this](const QString &, const QString &, const QString &owner) {
        const bool running = !owner.isEmpty();
        if (running != m_running) {
            m_running = running;
            Q_EMIT runningChanged(running);
            if (running) {
                refreshStatus();
            }
        }
    });
    m_running = bus.interface()->isServiceRegistered(service());
    if (m_running) {
        refreshStatus();
    }
}

bool DaemonClient::isRunning() const
{
    return m_running;
}

QJsonArray DaemonClient::status() const
{
    return m_status;
}

void DaemonClient::refreshStatus()
{
    QDBusMessage message = QDBusMessage::createMethodCall(service(), QString::fromLatin1(path), service(), QStringLiteral("Status"));
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QString> reply = *w;
        if (reply.isValid()) {
            m_status = QJsonDocument::fromJson(reply.value().toUtf8()).array();
            Q_EMIT statusChanged();
        }
    });
}

void DaemonClient::call(const QString &method, const QVariantList &arguments)
{
    QDBusMessage message = QDBusMessage::createMethodCall(service(), QString::fromLatin1(path), service(), method);
    message.setArguments(arguments);
    // Fire and forget: the outcome arrives as a signal (OperationFailed, CollectionChanged, ...)
    QDBusConnection::sessionBus().asyncCall(message);
}

void DaemonClient::syncNow()
{
    call(QStringLiteral("SyncNow"), {});
}

void DaemonClient::retryNow(const QString &account, const QString &mailbox)
{
    call(QStringLiteral("RetryNow"), {account, mailbox});
}

void DaemonClient::discardOperation(const QString &account, const QString &mailbox, qint64 operation)
{
    call(QStringLiteral("DiscardOperation"), {account, mailbox, operation});
}

void DaemonClient::applyOfflineSettings(bool freeSpace)
{
    call(QStringLiteral("ApplyOfflineSettings"), {freeSpace});
}

void DaemonClient::setFolderOpen(const QString &account, const QString &mailbox, qint64 folder, bool open)
{
    call(QStringLiteral("SetFolderOpen"), {account, mailbox, qlonglong(folder), open});
}

void DaemonClient::fetchBody(const QString &account, const QString &mailbox, qint64 message)
{
    call(QStringLiteral("FetchBody"), {account, mailbox, qlonglong(message)});
}

void DaemonClient::fetchAttachment(const QString &account, const QString &mailbox, qint64 attachment)
{
    call(QStringLiteral("FetchAttachment"), {account, mailbox, qlonglong(attachment)});
}

void DaemonClient::markRead(const QString &account, const QString &mailbox, qint64 message, bool read, bool receipt)
{
    if (read && !receipt) {
        call(QStringLiteral("MarkReadWithoutReceipt"), {account, mailbox, qlonglong(message)});
        return;
    }
    call(QStringLiteral("MarkRead"), {account, mailbox, qlonglong(message), read});
}

void DaemonClient::setCategories(const QString &account, const QString &mailbox, qint64 message, const QStringList &categories)
{
    call(QStringLiteral("SetCategories"), {account, mailbox, qlonglong(message), QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(categories)).toJson(QJsonDocument::Compact))});
}

void DaemonClient::setFocused(const QString &account, const QString &mailbox, qint64 message, bool focused)
{
    call(QStringLiteral("SetFocused"), {account, mailbox, qlonglong(message), focused});
}

void DaemonClient::setFlagDates(const QString &account, const QString &mailbox, qint64 message, qint64 start, qint64 due)
{
    call(QStringLiteral("SetFlagDates"), {account, mailbox, qlonglong(message), qlonglong(start), qlonglong(due)});
}

void DaemonClient::setFlag(const QString &account, const QString &mailbox, qint64 message, int flag)
{
    call(QStringLiteral("SetFlag"), {account, mailbox, qlonglong(message), flag});
}

void DaemonClient::move(const QString &account, const QString &mailbox, qint64 message, qint64 folder)
{
    call(QStringLiteral("Move"), {account, mailbox, qlonglong(message), qlonglong(folder)});
}

void DaemonClient::remove(const QString &account, const QString &mailbox, qint64 message)
{
    call(QStringLiteral("Delete"), {account, mailbox, qlonglong(message)});
}

void DaemonClient::saveOutgoing(const QString &account, const QString &mailbox, const QString &json)
{
    call(QStringLiteral("SaveOutgoing"), {account, mailbox, json});
}

void DaemonClient::discardOutgoing(const QString &account, const QString &mailbox, const QString &uuid)
{
    call(QStringLiteral("DiscardOutgoing"), {account, mailbox, uuid});
}

void DaemonClient::loadMessage(const QString &account, const QString &mailbox, qint64 message, QObject *context, const std::function<void(const QString &)> &done)
{
    QDBusMessage call = QDBusMessage::createMethodCall(service(), QString::fromLatin1(path), service(), QStringLiteral("LoadMessage"));
    call.setArguments({account, mailbox, qlonglong(message)});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 30000), context);
    connect(watcher, &QDBusPendingCallWatcher::finished, context, [done](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QString> reply = *w;
        done(reply.isValid() ? reply.value() : QString());
    });
}

void DaemonClient::createEvent(const QString &account, const QString &mailbox, qint64 calendar, const QString &json)
{
    call(QStringLiteral("CreateEvent"), {account, mailbox, qlonglong(calendar), json});
}

void DaemonClient::updateEvent(const QString &account, const QString &mailbox, qint64 event, const QString &json)
{
    call(QStringLiteral("UpdateEvent"), {account, mailbox, qlonglong(event), json});
}

void DaemonClient::deleteEvent(const QString &account, const QString &mailbox, qint64 event, bool cancel, const QString &comment)
{
    call(QStringLiteral("DeleteEvent"), {account, mailbox, qlonglong(event), cancel, comment});
}

void DaemonClient::respondEvent(const QString &account, const QString &mailbox, qint64 event, const QString &action, const QString &comment, bool sendResponse)
{
    call(QStringLiteral("RespondEvent"), {account, mailbox, qlonglong(event), action, comment, sendResponse});
}

void DaemonClient::proposeNewTime(const QString &account, const QString &mailbox, const QString &eventGraphId, const QString &action, const QString &comment, qint64 start,
                                  qint64 end)
{
    call(QStringLiteral("ProposeNewTime"), {account, mailbox, eventGraphId, action, comment, qlonglong(start), qlonglong(end)});
}

void DaemonClient::respondMeeting(const QString &account, const QString &mailbox, const QString &eventGraphId, const QString &action, const QString &comment, bool sendResponse)
{
    call(QStringLiteral("RespondMeeting"), {account, mailbox, eventGraphId, action, comment, sendResponse});
}

void DaemonClient::createItem(const QString &account, const QString &mailbox, const QString &kind, qint64 parent, const QString &json)
{
    call(QStringLiteral("CreateItem"), {account, mailbox, kind, qlonglong(parent), json});
}

void DaemonClient::updateItem(const QString &account, const QString &mailbox, const QString &kind, qint64 item, const QString &json)
{
    call(QStringLiteral("UpdateItem"), {account, mailbox, kind, qlonglong(item), json});
}

void DaemonClient::deleteItem(const QString &account, const QString &mailbox, const QString &kind, qint64 item)
{
    call(QStringLiteral("DeleteItem"), {account, mailbox, kind, qlonglong(item)});
}

void DaemonClient::todoList(const QString &account, const QString &mailbox, const QString &action, qint64 list, const QString &name)
{
    call(QStringLiteral("TodoList"), {account, mailbox, action, qlonglong(list), name});
}

void DaemonClient::taskStep(const QString &account, const QString &mailbox, qint64 task, const QString &action, const QString &stepId, const QString &json)
{
    call(QStringLiteral("TaskStep"), {account, mailbox, qlonglong(task), action, stepId, json});
}

void DaemonClient::query(const QString &account, const QString &mailbox, const QString &kind, const QString &json, QObject *context, const std::function<void(const QString &)> &done)
{
    QDBusMessage call = QDBusMessage::createMethodCall(service(), QString::fromLatin1(path), service(), QStringLiteral("Query"));
    call.setArguments({account, mailbox, kind, json});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 30000), context);
    connect(watcher, &QDBusPendingCallWatcher::finished, context, [done](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QString> reply = *w;
        done(reply.isValid() ? reply.value() : QString());
    });
}

void DaemonClient::checkMailbox(const QString &account, const QString &address, bool add, QObject *context, const std::function<void(const QString &)> &done)
{
    QDBusMessage call =
        QDBusMessage::createMethodCall(service(), QString::fromLatin1(path), service(), add ? QStringLiteral("AddSharedMailbox") : QStringLiteral("CheckMailbox"));
    call.setArguments({account, address});
    // A few round trips to Graph, each with its own retries
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 90000), context);
    connect(watcher, &QDBusPendingCallWatcher::finished, context, [done](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QString> reply = *w;
        done(reply.isValid() ? reply.value() : QString());
    });
}

void DaemonClient::orgChart(const QString &account, const QString &user, QObject *context, const std::function<void(const QString &)> &done)
{
    QDBusMessage call = QDBusMessage::createMethodCall(service(), QString::fromLatin1(path), service(), QStringLiteral("OrgChart"));
    call.setArguments({account, user});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 60000), context);
    connect(watcher, &QDBusPendingCallWatcher::finished, context, [done](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QString> reply = *w;
        done(reply.isValid() ? reply.value() : QString());
    });
}

void DaemonClient::removeMailbox(const QString &account, const QString &mailbox, QObject *context, const std::function<void(const QString &)> &done)
{
    QDBusMessage call = QDBusMessage::createMethodCall(service(), QString::fromLatin1(path), service(), QStringLiteral("RemoveMailbox"));
    call.setArguments({account, mailbox});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 30000), context);
    connect(watcher, &QDBusPendingCallWatcher::finished, context, [done](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QString> reply = *w;
        done(reply.isValid() ? reply.value() : QString());
    });
}

void DaemonClient::onOutgoingChanged(const QString &account, const QString &mailbox, const QString &uuid, const QString &state, const QString &error)
{
    Q_EMIT outgoingChanged(account, mailbox, uuid, state, error);
}

void DaemonClient::onSendRightLearned(const QString &account, const QString &mailbox, const QString &right)
{
    Q_EMIT sendRightLearned(account, mailbox, right);
}

void DaemonClient::onCollectionChanged(const QString &account, const QString &mailbox, const QString &collection, int changes)
{
    Q_EMIT collectionChanged(account, mailbox, collection, changes);
}

void DaemonClient::onStatusChanged(const QString &account, const QString &mailbox, const QString &json)
{
    const QJsonObject entry = QJsonDocument::fromJson(json.toUtf8()).object();
    bool replaced = false;
    for (qsizetype i = 0; i < m_status.size(); ++i) {
        const QJsonObject old = m_status.at(i).toObject();
        if (old.value(QLatin1String("account")).toString() == account && old.value(QLatin1String("mailbox")).toString() == mailbox) {
            m_status.replace(i, entry);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        m_status.append(entry);
    }
    Q_EMIT statusChanged();
}

void DaemonClient::onBodyReady(const QString &account, const QString &mailbox, qlonglong message)
{
    Q_EMIT bodyReady(account, mailbox, message);
}

void DaemonClient::onAttachmentReady(const QString &account, const QString &mailbox, qlonglong message, qlonglong attachment, bool ok)
{
    Q_EMIT attachmentReady(account, mailbox, message, attachment, ok);
}

void DaemonClient::onOperationFailed(const QString &account, const QString &mailbox, const QString &message, const QString &error)
{
    Q_EMIT operationFailed(account, mailbox, message, error);
}

}
