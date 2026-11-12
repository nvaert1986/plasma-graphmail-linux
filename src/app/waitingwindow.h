// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QTimer>

class QLabel;
class QPushButton;
class QTreeWidget;

namespace GraphMail
{

class DaemonClient;
class MailboxSet;

// View > Waiting to Be Sent (docs/PHASE8.md): the messages not sent yet and the changes not yet
// in Microsoft 365, from every mailbox, with what went wrong. Send Again, Open, Delete for a
// message; Retry Now and Discard (the change is undone here) for a change.
class WaitingWindow : public QDialog
{
    Q_OBJECT

public:
    WaitingWindow(MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent = nullptr);

    // What a waiting change is, in a few words ("Mark as read", "Move", "Change event")
    static QString describeKind(const QString &kind);

    // Tests: the counts shown
    int messageCount() const;
    int changeCount() const;

Q_SIGNALS:
    void openOutgoing(const QString &mailboxKey, const QString &uuid);

private:
    void fill();
    void sendAgain();
    void deleteMessage();
    void retry();
    void discard();

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
    QLabel *m_state;
    QTreeWidget *m_messages;
    QTreeWidget *m_changes;
    QPushButton *m_open;
    QPushButton *m_send;
    QPushButton *m_delete;
    QPushButton *m_retry;
    QPushButton *m_discard;
    QTimer m_refresh;
};

}
