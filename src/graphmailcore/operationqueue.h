// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "collectionsync.h"

#include <QObject>
#include <QSet>

namespace GraphMail
{

// Changes the user makes to messages (docs/PHASE2.md section 6). Optimistic: the cache changes at
// once and the change is stored in the ops table; process() sends it to Graph. Success clears it;
// a permanent failure (4xx) restores the previous value and reports it; network trouble and
// throttling retry later, also after a restart. Two changes of the same kind to one message are
// merged; a change back to the original value cancels the queued one.
class OperationQueue : public QObject
{
    Q_OBJECT

public:
    explicit OperationQueue(SyncContext *context, QObject *parent = nullptr);

    bool markRead(qint64 message, bool read);
    // 0 not flagged, 1 flagged, 2 complete
    // With dates (milliseconds, 0 = none): Outlook's Today, Tomorrow, This Week, Next Week, Custom
    bool setFlag(qint64 message, int flag, qint64 start = 0, qint64 due = 0);
    bool setCategories(qint64 message, const QStringList &categories);
    // Focused Inbox: Move to Focused / Move to Other
    bool setFocused(qint64 message, bool focused);
    bool move(qint64 message, qint64 folder);
    // To Deleted Items; permanently when it is already there
    bool remove(qint64 message);

    // After Graph updated a message: queued local changes win until they are sent
    void reapply(const QString &graphId);
    // Sends due operations, at most `slots` at a time
    void process(int slots);
    int inFlight() const;

    // Discard in the "Waiting to be sent" window: the change is undone locally and dropped;
    // false while it is on its way
    bool discard(const OperationRow &op);
    bool isInFlight(qint64 op) const;

Q_SIGNALS:
    void completed(const QString &graphId, const QString &kind);
    void failed(const QString &graphId, const QString &kind, const QString &error);
    // An operation could not reach Graph (the scheduler pauses the mailbox)
    void networkError();

private:
    // extra: more in the payload (a flag's dates); it always reaches Graph, also when the value
    // goes back to what Graph has
    bool enqueue(qint64 message, const QString &kind, const QJsonValue &value, const QJsonValue &previous, const QJsonObject &extra = {});
    bool apply(const OperationRow &op, bool previous);
    void finished(const OperationRow &op, const GraphResponse &response);
    void clearPendingIfDone(const QString &graphId);

    SyncContext *m_ctx;
    QSet<qint64> m_inFlight;
};

}
