// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "calendardata.h"
#include "mailboxsettings.h"

#include <KXmlGuiWindow>

#include <QPointer>
#include <QSet>

#include <functional>
#include <QTimer>

class KConfigGroup;
class KDatePicker;
class QLabel;
class QStackedWidget;
class QTreeWidget;

namespace GraphMail
{

class AgendaView;
class CalendarSharingDialog;
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

private:
    void setupActions();
    void fillCalendarList();
    void refresh();
    void step(int direction);
    // The days the current mode shows around m_date
    QPair<QDate, QDate> range() const;
    CalendarView *currentView() const;

    void newEvent(const QDateTime &start, const QDateTime &end, bool allDay);
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
    KDatePicker *m_picker;
    QTreeWidget *m_calendarList;
    QLabel *m_title;
    QStackedWidget *m_stack;
    TimeGridView *m_grid;
    MonthView *m_month;
    AgendaView *m_agenda;
    QTimer m_reloadTimer;
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
