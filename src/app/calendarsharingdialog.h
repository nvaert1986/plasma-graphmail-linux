// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "calendardata.h"

#include <QDialog>
#include <QJsonArray>

#include <functional>

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

// Calendar list > Sharing... (docs/PHASE7.md): who may see one of the user's calendars and how
// much, as Outlook's sharing page has it (calendarPermissions). Changes go out at once.
class CalendarSharingDialog : public QDialog
{
    Q_OBJECT

public:
    CalendarSharingDialog(MailboxSet *mailboxes, DaemonClient *daemon, const CalendarInfo &calendar, QWidget *parent = nullptr);

    // Outlook's words for a Graph role ("read" is "Can view all details")
    static QString roleText(const QString &role);

    // Tests: the same paths as the controls, once the list is loaded; log the outcome
    void addForTests(const QString &address, const QString &role);
    void setRoleForTests(const QString &address, const QString &role);
    void removeForTests(const QString &address);

private:
    void load();
    void fill();
    void add();
    void setRole(const QString &permission, const QString &role);
    void remove(const QString &permission, const QString &who);
    void call(const QString &kind, const QJsonObject &args, const std::function<void(const QJsonObject &)> &done);
    // Runs now when the list is loaded, else once it is
    void whenLoaded(const std::function<void()> &action);
    QString permissionOf(const QString &address) const;

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
    CalendarInfo m_calendar;
    AddressBook *m_book;
    QTreeWidget *m_list;
    QPushButton *m_remove;
    RecipientEdit *m_address;
    QComboBox *m_role;
    QPushButton *m_add;
    QLabel *m_status;
    QJsonArray m_permissions;
    bool m_loaded = false;
    QList<std::function<void()>> m_waiting;
};

}
