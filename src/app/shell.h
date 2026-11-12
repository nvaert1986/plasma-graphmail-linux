// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWidget>

class QButtonGroup;
class QStackedWidget;
class QToolButton;

namespace GraphMail
{

class DaemonClient;
class MailboxSet;
class MainWindow;

// The one application window, as Outlook's: a navigation bar on the left (Mail, Calendar,
// Contacts, Tasks) and the module next to it with its own menus, toolbar and status bar. The
// modules are the windows of phases 1 to 8, embedded; one can still be opened in a window of its
// own (the navigation bar's right-click).
class Shell : public QWidget
{
    Q_OBJECT

public:
    Shell(MailboxSet *mailboxes, DaemonClient *daemon);
    ~Shell() override;

    MainWindow *mail() const
    {
        return m_mail;
    }
    // A module window shown in the shell (1 mail, 2 calendar, 3 contacts, 4 tasks)
    void embed(int module, QWidget *page);
    void showModule(int module);
    int module() const;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void updateTitle();
    // The account button on top of the navigation bar: the primary mailbox's photo
    void updateAccount();

    MainWindow *m_mail;
    QStackedWidget *m_stack;
    QButtonGroup *m_buttons;
    QToolButton *m_account = nullptr;
    MailboxSet *m_mailboxes;
    QWidget *m_pages[4] = {};
};

}
