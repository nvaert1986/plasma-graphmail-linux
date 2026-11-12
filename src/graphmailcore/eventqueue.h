// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "collectionsync.h"

#include <QObject>
#include <QSet>

namespace GraphMail
{

// Changes to calendar events (docs/PHASE5.md), in the same ops table as the message changes
// (kinds event*). Optimistic like OperationQueue: the cache shows the change at once, Graph gets
// it in the background; network trouble retries, a refusal puts the old state back.
// The event JSON is Graph's (start/end as dateTimeTimeZone, recurrence, attendees, ...).
class EventQueue : public QObject
{
    Q_OBJECT

public:
    explicit EventQueue(SyncContext *context, QObject *parent = nullptr);

    // A new event in a calendar (row id); the cache shows it under a local id until Graph answers
    bool create(qint64 calendar, const QJsonObject &event);
    // Changes these properties of an event, an occurrence (only that one) or a series master
    bool update(qint64 event, const QJsonObject &changes);
    // cancel: the organizer of a meeting with attendees; they get a cancellation with `comment`
    bool remove(qint64 event, bool cancel, const QString &comment);
    // action: accept, tentativelyAccept or decline
    // proposedStart/End (milliseconds, 0 = none): Propose New Time with Tentative or Decline
    bool respond(qint64 event, const QString &action, const QString &comment, bool sendResponse, qint64 proposedStart = 0, qint64 proposedEnd = 0);
    // By graph id (a meeting request's event may not be in the window)
    bool respondByGraphId(const QString &graphId, const QString &calendarGraphId, const QString &action, const QString &comment, bool sendResponse, qint64 proposedStart = 0,
                          qint64 proposedEnd = 0);
    bool dismissReminder(qint64 event);
    // An event that may be outside the synced window (an overdue reminder from Exchange's list)
    bool dismissReminderByGraphId(const QString &graphId);
    bool snoozeReminder(qint64 event, qint64 until);

    void process(int slots);
    int inFlight() const;

    // Discard in the "Waiting to be sent" window: the change is undone locally and dropped;
    // false while it is on its way
    bool discard(const OperationRow &op);
    bool isInFlight(qint64 op) const;

Q_SIGNALS:
    // Changed elsewhere in the same fields (docs/PHASE8.md): Microsoft 365's version is kept
    void conflict(const QString &kind, qint64 row, const QString &title, const QStringList &fields, const QJsonObject &changes);
    void completed(const QString &kind);
    void failed(const QString &graphId, const QString &kind, const QString &error);
    void networkError();

private:
    bool enqueue(const QString &kind, const QString &target, const QJsonObject &payload);
    // The path of an event in its calendar
    QString eventPath(const QString &calendarGraphId, const QString &graphId) const;
    void finished(const OperationRow &op, const GraphCore::GraphResponse &response);
    // 412: read the event again; merge, or keep Microsoft 365's version and say so
    void resolveConflict(const OperationRow &op);
    void rollback(const OperationRow &op);

    SyncContext *m_ctx;
    QSet<qint64> m_inFlight;
};

}
