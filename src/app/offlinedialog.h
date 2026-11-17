// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QTreeWidget;

namespace GraphMail
{

class DaemonClient;
class MailboxSet;

// Settings > Offline (docs/PHASE8.md): how much mail is kept on this computer (graphmailrc
// [General] BodyWindowDays, AttachmentsOffline), the space each mailbox takes, Free Up Space.
// Applied at once: the daemon is told, no restart.
class OfflineDialog : public QDialog
{
    Q_OBJECT

public:
    OfflineDialog(MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent = nullptr);

    // Space a mailbox's copy takes: the cache and its files
    static qint64 diskUse(const QString &account, const QString &mailbox);

    // Tests: choose these settings and save (days -1 = all, 0 = none); freeSpace as the button
    void applyForTests(int days, bool attachments, bool freeSpace);

private:
    void fillSpace();
    void save();

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
    QComboBox *m_window;
    QCheckBox *m_attachments;
    QCheckBox *m_metered;
    QLabel *m_note;
    QTreeWidget *m_space;
};

}
