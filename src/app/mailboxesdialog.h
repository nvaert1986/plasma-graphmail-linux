// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QJsonObject>

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace GraphMail
{

class AddressBook;
class DaemonClient;
class MailboxSet;
class RecipientEdit;

// Settings > Mailboxes (docs/PHASE7.md): the other mailboxes per account with what each gives,
// Remove, and Add after a check of what an address gives the user.
class MailboxesDialog : public QDialog
{
    Q_OBJECT

public:
    MailboxesDialog(MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent = nullptr);

    // Tests: the same paths as the buttons, without the confirmation; log the outcome
    void checkForTests(const QString &address, bool add);
    void removeForTests(const QString &address);

Q_SIGNALS:
    // The set of mailboxes changed: the windows reload
    void mailboxesChanged();

private:
    void fill();
    QString account() const;
    void check(bool add);
    void showCheck(const QJsonObject &access, bool added);
    void remove();
    void removeMailbox(const QString &account, const QString &mailbox, const QString &label);
    // The account's own calendars already show this person's calendar (shared with the user)
    bool inOwnCalendars(const QString &account, const QString &address) const;

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
    AddressBook *m_book;
    QTreeWidget *m_list;
    QPushButton *m_remove;
    QComboBox *m_account;
    RecipientEdit *m_address;
    QPushButton *m_check;
    QPushButton *m_add;
    QLabel *m_result;
    QJsonObject m_lastCheck;
};

}
