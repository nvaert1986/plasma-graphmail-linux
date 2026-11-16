// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <KXmlGuiWindow>

namespace GraphMail
{

class DaemonClient;
class FolderModel;
class MailboxSet;
class ReadingPane;

// One message in its own window (double-click or Enter in the list, as in Outlook): the reading
// pane with the message actions. Opening it counts as reading it. The window closes after the
// message was deleted, moved or archived, or when it disappears from the cache.
class MessageWindow : public KXmlGuiWindow
{
    Q_OBJECT

public:
    // markRead false: tests (--screenshot never marks anything read)
    MessageWindow(MailboxSet *mailboxes, DaemonClient *daemon, FolderModel *folders, const QString &mailboxKey, qint64 message, bool markRead = true,
                  QWidget *parent = nullptr);
    ~MessageWindow() override;

    qint64 message() const;
    QString mailboxKey() const;

Q_SIGNALS:
    // Reply with Meeting: the main window opens the meeting (it owns the calendar)
    void replyWithMeetingRequested(const QString &mailboxKey, qint64 message);
    // Unsubscribe by mail: the main window opens the composer
    void mailtoRequested(const QString &link);

private:
    void setupActions();
    void refresh();
    QString account() const;
    QString mailbox() const;
    void moveTo(qint64 folder);

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
    FolderModel *m_folders;
    QString m_mailboxKey;
    qint64 m_message;
    ReadingPane *m_reading;
};

}
