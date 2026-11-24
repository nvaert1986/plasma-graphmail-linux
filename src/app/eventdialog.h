// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QJsonObject>

#include <optional>

#include "calendardata.h"
#include "outgoing.h"

#include <QDialog>
#include <QTimer>

class QCheckBox;
class QComboBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QTextEdit;
class QTimeEdit;
class QToolButton;

namespace GraphMail
{

class TimeGridView;

class AddressBook;
class DaemonClient;
class MailboxSet;
class RecipientEdit;
class SchedulingStrip;

struct EventEditRequest {
    // A new event: the calendar and the times picked in the view
    QString mailboxKey;
    qint64 calendar = 0;
    QDateTime start;
    QDateTime end;
    bool allDay = false;
    // A new meeting filled in from a message (Reply with Meeting): subject, people, the text
    QString subject;
    QList<OutgoingRecipient> required;
    QList<OutgoingRecipient> optional;
    QString description;
    // New Meeting: the attendees first, as Outlook opens a meeting request
    bool meeting = false;
    // An existing one: what is opened (an occurrence, or the series master for the whole series)
    std::optional<DisplayEvent> event;
    bool series = false;
};

// Graph's dateTimeTimeZone for a time of this computer (a drag in the calendar, 0.14.5)
QJsonObject graphLocalTime(const QDateTime &local);

// Creating, changing or looking at an event. The organizer (or anyone who may edit the calendar)
// edits; an attendee sees it and answers (Accept, Tentative, Decline); other people's calendars
// are read only. Saving sends only what changed, so a Teams meeting's text is not rewritten when
// only the time moved. With attendees Exchange sends the invitations or updates.
class EventDialog : public QDialog
{
    Q_OBJECT

public:
    EventDialog(MailboxSet *mailboxes, DaemonClient *daemon, CalendarData *data, const EventEditRequest &request, QWidget *parent = nullptr);

    void accept() override;

    // Propose New Time (docs/PHASE9.md), asked the same way from the event, the calendar's menu
    // and an invitation in Mail: another start and end, Tentative or Decline, a message
    struct NewTime {
        QDateTime start;
        QDateTime end;
        QString action;
        QString comment;
    };
    static std::optional<NewTime> askNewTime(QWidget *parent, const QDateTime &start, const QDateTime &end);
    // Graph Mail moves an invitation to Deleted Items after it is answered (Outlook's option)
    static bool deleteRequestAfterResponding();

    // Tests: fill in and save without dialogs
    void setTestInput(const QString &subject, const QString &location, const QString &required, int reminderMinutes, bool teams);
    void setTestRecurrence(const QJsonObject &recurrence);
    // Tests: change an opened event the way a user would (title, times moved by minutes)
    void applyTestEdit(const QString &title, int shiftMinutes);
    void setTestRoom(const QString &name, const QString &address);
    void setTestAllDay(bool allDay);
    // Typing in the calendar faster than this dialog takes the keyboard: the title gets the rest
    void typeIntoSubject(const QString &text);
    // Tests: Time Zones on, with these zones; the times typed stay as they are
    void setTestZones(const QByteArray &start, const QByteArray &end);
    // Tests: the calendar of this mailbox (its default one) for a new event
    void selectCalendarForTests(const QString &mailboxKey);
    void saveNow();

    // Showing a dialog gives its focus widget a Tab focus, which selects a line edit's text: a
    // title typed in the calendar goes on after its first letter instead (0.14.5)
    void setVisible(bool visible) override;

Q_SIGNALS:
    // The user chose Delete (the calendar window asks the questions that go with it)
    void deleteRequested();

private:
    void fillFromEvent(const DisplayEvent &event);
    void setReadOnly(bool readOnly);
    void allDayToggled(bool allDay);
    void repeatChosen(int index);
    void updateSchedule();
    void pickRoom();
    void respond(const QString &action);

    QJsonObject eventJson() const;
    QJsonObject changes() const;
    QStringList attendeeAddresses() const;

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
    CalendarData *m_data;
    // Outlook's day beside the form: that day's events, the event's time marked; a double click
    // there moves the event to that half hour
    TimeGridView *m_day = nullptr;
    // Teams: the event's own link, or its series' (an ordinary occurrence has none)
    QString m_joinUrl;
    void updateDay();
    EventEditRequest m_request;
    QJsonObject m_original;
    QJsonObject m_recurrence;
    QJsonObject m_room;
    bool m_readOnly = false;
    bool m_attendee = false;
    bool m_descriptionChanged = false;
    // Tests: never ask (an invisible dialog blocks an off-screen run)
    bool m_testMode = false;
    // A new event typed straight into the calendar, until the dialog is first shown
    bool m_goOnTyping = false;

    AddressBook *m_book;
    QComboBox *m_calendar;
    QLabel *m_calendarLabel;
    QLineEdit *m_subject;
    RecipientEdit *m_required;
    // Save, or Send while there are attendees (it is a meeting then: Exchange sends invitations)
    void updateSaveLabel();
    RecipientEdit *m_optional;
    QPushButton *m_rooms;
    QDateEdit *m_startDate;
    QTimeEdit *m_startTime;
    QDateEdit *m_endDate;
    QTimeEdit *m_endTime;
    // Outlook's Time Zones (docs/PHASE9B.md 9b.29): the start and end each in a zone of its own,
    // the local one unless the user picks another (a flight, a meeting abroad)
    QComboBox *m_startZone = nullptr;
    QComboBox *m_endZone = nullptr;
    QToolButton *m_zones = nullptr;
    QTimeZone startZone() const;
    QTimeZone endZone() const;
    void showZones(bool on);
    QCheckBox *m_allDay;
    QComboBox *m_repeat;
    QLineEdit *m_location;
    QCheckBox *m_teams;
    QComboBox *m_showAs;
    QComboBox *m_reminder;
    QCheckBox *m_private;
    // Meeting options of the organizer (docs/PHASE9.md)
    QCheckBox *m_allowProposals;
    QPushButton *m_propose = nullptr;
    QCheckBox *m_hideAttendees;
    QTextEdit *m_description;
    SchedulingStrip *m_schedule;
    QLabel *m_responseLabel;
    QWidget *m_responseRow;
    QPushButton *m_join;
    QPushButton *m_delete;
    QTimer m_scheduleTimer;
};

// The attendees' busy times on the event's day (getSchedule), with the event's time marked
class SchedulingStrip : public QWidget
{
    Q_OBJECT

public:
    explicit SchedulingStrip(QWidget *parent = nullptr);
    // availability: one character per half hour from midnight: 0 free, 1 tentative, 2 busy, 3 away, 4 elsewhere
    void setSchedules(QDate day, const QList<QPair<QString, QString>> &schedules);
    void setSlot(const QDateTime &start, const QDateTime &end);
    void setMessage(const QString &message);

protected:
    void paintEvent(QPaintEvent *event) override;
    QSize sizeHint() const override;

private:
    QDate m_day;
    QList<QPair<QString, QString>> m_schedules;
    QDateTime m_start;
    QDateTime m_end;
    QString m_message;
};

}
