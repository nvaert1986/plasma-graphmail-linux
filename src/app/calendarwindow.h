// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "calendardata.h"
#include "mailboxsettings.h"
#include "outgoing.h"

#include <KXmlGuiWindow>

#include <QPointer>
#include <QSet>

#include <functional>
#include <QTimer>

class KConfigGroup;
class QCalendarWidget;
class QLabel;
class QStackedWidget;
class QTreeWidget;

namespace GraphMail
{

class AgendaView;
class CalendarSharingDialog;
class EventDialog;
class CategoryList;
class CalendarView;
class DaemonClient;
class MailboxSet;
class MonthView;
class TimeGridView;
struct EventEditRequest;

// The calendar (docs/PHASE5.md): the mini month and the list of calendars on the left, the day,
// work week, week, month or agenda view on the right. Ctrl+1 goes back to the mail.
class CalendarWindow : public KXmlGuiWindow
{
    Q_OBJECT

public:
    // File > Export Calendar; a file name skips the dialogs (tests). The file written, or empty
    QString exportCalendar(const QString &testFile = {});
    // Tests: select a time in the view shown, as a click and drag does
    void selectSlotForTests(const QDateTime &start, const QDateTime &end);
    // Tests: drag over a time in the week grid, then click or double click in it; logged
    void mouseForTests(const QString &script);
    // Drags in the date picker from one day of the month shown to another; gives the range shown
    QString pickerDragForTests(int fromDay, int toDay);
    enum class Mode { Day, WorkWeek, Week, Month, Agenda };

    CalendarWindow(MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent = nullptr);
    ~CalendarWindow() override;

    void setMode(Mode mode);
    void goTo(QDate date);
    // The own mailbox's working hours (docs/PHASE9.md): shading, Work Week's days, first hour shown
    void setWorkingHours(const WorkingHours &hours);
    void loadWorkingHours();
    // View > Second Time Zone (docs/PHASE9.md); tests: set without the dialog (empty = none)
    // File > Print: the view shown (docs/PHASE9B.md); tests: into a PDF
    void printView(const QString &pdfFile = {});
    void setSecondTimeZoneForTests(const QString &zone, const QString &label, const QString &ownLabel);
    // Opens an event (a reminder's Open, tests)
    void openEvent(const QString &mailboxKey, qint64 event);
    // Tests: a new event in the default calendar, filled in and saved
    QWidget *newEventForTests(const QDateTime &start, const QDateTime &end);
    // Reply with Meeting: a new meeting at the next half hour, in a calendar of mailboxKey (the
    // message's mailbox) when it has a writable one, else the first writable calendar
    QWidget *newMeeting(const QString &mailboxKey, const QString &subject, const QList<OutgoingRecipient> &required, const QList<OutgoingRecipient> &optional,
                        const QString &description);
    // Tests: answer every question the way given (series or occurrence, a message for a
    // cancellation or response, confirmations yes); only events marked [TEST Graph Mail]
    void setTestAnswers(bool series, const QString &comment);
    QWidget *editEventForTests(const QString &mailboxKey, qint64 event);
    bool deleteEventForTests(const QString &mailboxKey, qint64 event);
    bool respondForTests(const QString &mailboxKey, qint64 event, const QString &action);
    // Tests: Propose New Time without the dialog; spec "propose|start ISO|end ISO|tentativelyAccept or decline"
    bool proposeForTests(const QString &mailboxKey, qint64 event, const QString &spec);
    // Tests: the event dialog opened last
    QWidget *lastDialog() const;
    // Tests: Add a Colleague's Calendar / Remove for this address, without dialogs; logs the outcome
    void addColleagueForTests(const QString &address);
    void removeColleagueForTests(const QString &address);
    // Categories: events in their colours, Categorize in the event menu (docs/PHASE9.md)
    void setCategoryList(CategoryList *categories);
    // Tests: give this event the category, or take it away
    void categorizeEventForTests(const QString &mailboxKey, qint64 event, const QString &name, bool on);
    // Sharing of one of the user's calendars; the default calendar of the primary account when empty
    CalendarSharingDialog *openSharing(const QString &calendarKey = {});

Q_SIGNALS:
    void mailRequested();
    void navigate(int module);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupActions();
    void fillCalendarList();
    void refresh();
    void step(int direction);
    // The days the current mode shows around m_date (or the span picked in the date picker)
    QPair<QDate, QDate> range() const;
    // Outlook's date picker: days dragged over are shown together, up to 14 as day columns,
    // longer (up to six weeks) as weeks; a single day, Today or a view ends it
    void showSpan(QDate from, QDate to);
    void endSpan();
    void markSpan();
    // The day under a point of the date picker's grid (invalid on the header or outside)
    QDate pickerDateAt(const QPoint &pos) const;
    CalendarView *currentView() const;

    void newEvent(const QDateTime &start, const QDateTime &end, bool allDay, const QString &subject = {}, bool meeting = false);
    // The selected time of the view, else the next full hour of the day shown
    QPair<QDateTime, QDateTime> newEventTime(bool *allDay) const;
    void slotMenu(const QPoint &globalPos);
    // The dialog opened by typing on a selected time: more typing goes into its title
    QPointer<EventDialog> m_typedDialog;
    void activate(const DisplayEvent &event);
    void showMenu(const DisplayEvent &event, const QPoint &globalPos);
    // Asks "this occurrence or the series" for a recurring event; empty when cancelled
    std::optional<DisplayEvent> chooseOccurrenceOrSeries(const DisplayEvent &event, bool forDelete, bool *series);
    // The dialog it opened
    QWidget *editEvent(const DisplayEvent &event, bool series);
    std::optional<DisplayEvent> testEvent(const QString &mailboxKey, qint64 id) const;
    void deleteEvent(const DisplayEvent &event);
    void respond(const DisplayEvent &event, const QString &action, int sendMode);
    void setEventCategory(const DisplayEvent &event, const QString &name, bool on);
    // Colleagues' calendars (docs/PHASE7.md): shared ones are shown or added as a calendar-only
    // mailbox, the others as free/busy; done gets a sentence for the user
    void openColleagueCalendar();
    void addColleague(const QString &address, QObject *context, const std::function<void(const QString &, bool)> &done);
    void removeCalendarEntry(const CalendarInfo &calendar, bool ask);
    // A colleague's calendar the user accepted into their own list (read only)
    bool isSharedWithMe(const CalendarInfo &calendar) const;
    static KConfigGroup calendarGroup();
    void applySecondTimeZone();
    void askSecondTimeZone();
    // getSchedule for the free/busy people the visible days still need
    void fetchFreeBusy(QDate first, QDate last);

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
    CalendarData *m_data;
    Mode m_mode = Mode::WorkWeek;
    WorkingHours m_workingHours;
    bool m_scrolledToWork = false;
    QDate m_date;
    QCalendarWidget *m_picker;
    std::optional<QPair<QDate, QDate>> m_span;
    // A drag in the date picker: the day it started on
    bool m_pickerPressed = false;
    QDate m_pickAnchor;
    QList<QDate> m_marked;
    QTreeWidget *m_calendarList;
    QLabel *m_title;
    QStackedWidget *m_stack;
    TimeGridView *m_grid;
    MonthView *m_month;
    AgendaView *m_agenda;
    QTimer m_reloadTimer;
    // Colleagues' free/busy asked again while the window is open (0.14.4)
    QTimer m_freeBusyTimer;
    bool m_filling = false;
    bool m_testMode = false;
    bool m_testSeries = false;
    QString m_testComment;
    QPointer<QWidget> m_lastDialog;
    CategoryList *m_categoryList = nullptr;
    // Accounts with a getSchedule call on the way
    QSet<QString> m_freeBusyPending;
};

}
