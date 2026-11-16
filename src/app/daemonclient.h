// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonArray>
#include <QObject>
#include <QVariantList>

#include <functional>

class QDBusServiceWatcher;

namespace GraphMail
{

// The app's line to graphmaild (io.github.nvaert1986.GraphMail on /GraphMail). Calls never block:
// a change the user makes is sent and the daemon's signals report the outcome. When the daemon is
// installed, the first call starts it through D-Bus activation.
class DaemonClient : public QObject
{
    Q_OBJECT

public:
    explicit DaemonClient(QObject *parent = nullptr);

    static QString service();
    bool isRunning() const;
    // The last status of every mailbox (StatusChanged / Status())
    QJsonArray status() const;
    void refreshStatus();

    void syncNow();
    void applyOfflineSettings(bool freeSpace);
    void retryNow(const QString &account, const QString &mailbox);
    void discardOperation(const QString &account, const QString &mailbox, qint64 operation);
    void setFolderOpen(const QString &account, const QString &mailbox, qint64 folder, bool open);
    void fetchBody(const QString &account, const QString &mailbox, qint64 message);
    void fetchAttachment(const QString &account, const QString &mailbox, qint64 attachment);
    // receipt false: read without sending a requested read receipt
    void markRead(const QString &account, const QString &mailbox, qint64 message, bool read, bool receipt = true);
    void setFlag(const QString &account, const QString &mailbox, qint64 message, int flag);
    void setFocused(const QString &account, const QString &mailbox, qint64 message, bool focused);
    void setFlagDates(const QString &account, const QString &mailbox, qint64 message, qint64 start, qint64 due);
    void setCategories(const QString &account, const QString &mailbox, qint64 message, const QStringList &categories);
    void move(const QString &account, const QString &mailbox, qint64 message, qint64 folder);
    void remove(const QString &account, const QString &mailbox, qint64 message);
    // A message being written (Outgoing JSON): stored by the daemon, kept in Exchange as a draft,
    // sent when its "send" is true
    void saveOutgoing(const QString &account, const QString &mailbox, const QString &json);
    void discardOutgoing(const QString &account, const QString &mailbox, const QString &uuid);
    // Calendar (phase 5): event JSON in Graph's shape, ids are cache row ids
    void createEvent(const QString &account, const QString &mailbox, qint64 calendar, const QString &json);
    void updateEvent(const QString &account, const QString &mailbox, qint64 event, const QString &json);
    void deleteEvent(const QString &account, const QString &mailbox, qint64 event, bool cancel, const QString &comment);
    void respondEvent(const QString &account, const QString &mailbox, qint64 event, const QString &action, const QString &comment, bool sendResponse);
    void proposeNewTime(const QString &account, const QString &mailbox, const QString &eventGraphId, const QString &action, const QString &comment, qint64 start, qint64 end);
    void respondMeeting(const QString &account, const QString &mailbox, const QString &eventGraphId, const QString &action, const QString &comment, bool sendResponse);
    // Contacts and To Do: kind "contact" or "task", rows of the cache
    void createItem(const QString &account, const QString &mailbox, const QString &kind, qint64 parent, const QString &json);
    void updateItem(const QString &account, const QString &mailbox, const QString &kind, qint64 item, const QString &json);
    void deleteItem(const QString &account, const QString &mailbox, const QString &kind, qint64 item);
    void todoList(const QString &account, const QString &mailbox, const QString &action, qint64 list, const QString &name);
    void taskStep(const QString &account, const QString &mailbox, qint64 task, const QString &action, const QString &stepId, const QString &json);
    // schedule, rooms or meetingEvent; `done` gets the JSON, empty when it failed
    void query(const QString &account, const QString &mailbox, const QString &kind, const QString &json, QObject *context, const std::function<void(const QString &)> &done);
    // A message read whole from Graph; `done` gets its JSON, empty when that failed or timed out
    // Another mailbox (docs/PHASE7.md): done gets the daemon's JSON answer (MailboxAccess), or
    // an empty string when the daemon did not answer
    void checkMailbox(const QString &account, const QString &address, bool add, QObject *context, const std::function<void(const QString &)> &done);
    // A person's manager and direct reports (the daemon's OrgChart), JSON; empty when no answer
    void orgChart(const QString &account, const QString &user, QObject *context, const std::function<void(const QString &)> &done);
    void removeMailbox(const QString &account, const QString &mailbox, QObject *context, const std::function<void(const QString &)> &done);
    void loadMessage(const QString &account, const QString &mailbox, qint64 message, QObject *context, const std::function<void(const QString &)> &done);

Q_SIGNALS:
    void runningChanged(bool running);
    void statusChanged();
    void collectionChanged(const QString &account, const QString &mailbox, const QString &collection, int changes);
    void bodyReady(const QString &account, const QString &mailbox, qint64 message);
    void attachmentReady(const QString &account, const QString &mailbox, qint64 message, qint64 attachment, bool ok);
    void operationFailed(const QString &account, const QString &mailbox, const QString &message, const QString &error);
    void outgoingChanged(const QString &account, const QString &mailbox, const QString &uuid, const QString &state, const QString &error);
    void sendRightLearned(const QString &account, const QString &mailbox, const QString &right);

private Q_SLOTS:
    // Targets of the D-Bus signal connections (string-based: QDBusConnection::connect)
    void onCollectionChanged(const QString &account, const QString &mailbox, const QString &collection, int changes);
    void onStatusChanged(const QString &account, const QString &mailbox, const QString &json);
    void onBodyReady(const QString &account, const QString &mailbox, qlonglong message);
    void onAttachmentReady(const QString &account, const QString &mailbox, qlonglong message, qlonglong attachment, bool ok);
    void onOperationFailed(const QString &account, const QString &mailbox, const QString &message, const QString &error);
    void onOutgoingChanged(const QString &account, const QString &mailbox, const QString &uuid, const QString &state, const QString &error);
    void onSendRightLearned(const QString &account, const QString &mailbox, const QString &right);

private:
    void call(const QString &method, const QVariantList &arguments);

    QDBusServiceWatcher *m_watcher;
    bool m_running = false;
    QJsonArray m_status;
};

}
