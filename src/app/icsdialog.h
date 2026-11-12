// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QJsonObject>

class QComboBox;
class QLabel;
class QListWidget;

namespace GraphMail
{

class CalendarData;
class DaemonClient;
class MailboxSet;

// A .ics file (an attachment): its events, and "Add to Calendar" into one of the user's calendars.
// Simple repeat rules (daily, weekly on days, monthly on a day, yearly) come along.
class IcsDialog : public QDialog
{
    Q_OBJECT

public:
    // nullptr when the file holds no events
    static IcsDialog *open(const QString &path, MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent);

    // Adds the ticked events to the chosen calendar (the Add to Calendar button)
    void add();

private:
    IcsDialog(const QList<QJsonObject> &events, const QStringList &notes, MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent);

    QList<QJsonObject> m_events;
    DaemonClient *m_daemon;
    CalendarData *m_data;
    QListWidget *m_list;
    QComboBox *m_calendar;
    QLabel *m_status;
};

}
