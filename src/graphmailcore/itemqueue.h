// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "collectionsync.h"

#include <QObject>
#include <QSet>

namespace GraphMail
{

// Changes to contacts, To Do tasks, To Do lists and task steps (docs/PHASE6.md), in the ops table
// as kinds item*. Contacts and tasks show in the cache at once and come back as they were when
// Graph refuses; lists appear once Graph made them; steps are not cached and are just sent.
class ItemQueue : public QObject
{
    Q_OBJECT

public:
    explicit ItemQueue(SyncContext *context, QObject *parent = nullptr);

    // A contact (kind Contact, parent = contact folder row) or a task (kind Task, parent = list row)
    bool create(ItemKind kind, qint64 parent, const QJsonObject &json);
    bool update(ItemKind kind, qint64 item, const QJsonObject &changes);
    bool remove(ItemKind kind, qint64 item);

    bool createList(const QString &name);
    bool renameList(qint64 list, const QString &name);
    bool deleteList(qint64 list);
    // action: create (json), update (step id, json), delete (step id)
    bool step(qint64 task, const QString &action, const QString &stepId, const QJsonObject &json);

    void process(int slots);
    int inFlight() const;

    // Discard in the "Waiting to be sent" window: the change is undone locally and dropped;
    // false while it is on its way
    bool discard(const OperationRow &op);
    bool isInFlight(qint64 op) const;

Q_SIGNALS:
    // Changed elsewhere in the same fields (docs/PHASE8.md): Microsoft 365's version is kept;
    // kind "contact" or "task", row the cache row, changes what the user wanted
    void conflict(const QString &kind, qint64 row, const QString &title, const QStringList &fields, const QJsonObject &changes);
    void completed(const QString &kind);
    void failed(const QString &graphId, const QString &kind, const QString &error);
    void networkError();

private:
    // The collection path of a contact folder or a To Do list
    QString collectionPath(ItemKind kind, qint64 parent) const;
    bool enqueue(const QString &kind, const QString &target, const QJsonObject &payload);
    void finished(const OperationRow &op, const GraphCore::GraphResponse &response);
    // 412: read the item again; merge, or keep Microsoft 365's version and say so
    void resolveConflict(const OperationRow &op);
    void rollback(const OperationRow &op);

    SyncContext *m_ctx;
    QSet<qint64> m_inFlight;
};

}
