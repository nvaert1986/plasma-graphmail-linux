// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QObject>

namespace GraphMail
{

class MailboxSet;

struct CalendarInfo {
    QString mailboxKey;
    qint64 id = 0;
    QString graphId;
    QString name;
    QString owner;
    QColor color;
    bool canEdit = false;
    bool isDefault = false;
    bool shown = true;
    // Where the list shows it: "My Calendars", a shared mailbox's name, or "Shared Calendars"
    QString group;

    QString key() const
    {
        return mailboxKey + QLatin1Char('/') + graphId;
    }
};

// An event as the views draw it, in local time
struct DisplayEvent {
    QString mailboxKey;
    qint64 id = 0;
    qint64 calendar = 0;
    QString graphId;
    QString seriesMaster;
    QColor color;
    QDateTime start;
    QDateTime end;
    bool allDay = false;
    // All-day events by date: [firstDay, lastDay]
    QDate firstDay;
    QDate lastDay;
    QString subject;
    QString location;
    QString organizer;
    int showAs = 2;
    int response = 0;
    int type = 0;
    bool online = false;
    bool canEdit = false;
    bool cancelled = false;
    bool isOrganizer = false;
    bool isPrivate = false;
    QJsonObject json;

    bool recurring() const
    {
        return type == 1 || type == 2;
    }
};

// The calendars of every synced mailbox and their events, read from the caches. Which calendars
// are shown is kept in graphmailrc [Calendar] (Shown / Hidden keys); by default the user's own
// calendars and the shared mailboxes' are shown, colleagues' shared calendars not.
class CalendarData : public QObject
{
    Q_OBJECT

public:
    explicit CalendarData(MailboxSet *mailboxes, QObject *parent = nullptr);

    QList<CalendarInfo> calendars() const;
    std::optional<CalendarInfo> calendar(const QString &mailboxKey, qint64 id) const;
    void setShown(const QString &key, bool shown);
    // Shown calendars' events on these local days (inclusive), sorted by start
    QList<DisplayEvent> events(QDate from, QDate to) const;
    std::optional<DisplayEvent> eventById(const QString &mailboxKey, qint64 id) const;
    // Calendars the user can add events to (the default one first)
    QList<CalendarInfo> writable() const;
    // New events go there next time (the editor remembers the calendar chosen)
    void setLastUsed(const QString &key);
    // The user's own addresses (to tell organizer and attendee apart)
    QStringList ownAddresses() const;

    // Reads the caches again (a calendar sync, a change)
    void reload();
    // Events in the colour of their first category, as Outlook (docs/PHASE9.md)
    void setCategoryList(class CategoryList *categories);
    // An event's categories (an occurrence has its series master's)
    QStringList categoriesOf(const DisplayEvent &event) const;

    // Colleagues whose calendar is not shared with the user (docs/PHASE7.md): shown as their
    // free/busy times from getSchedule, kept in graphmailrc [Calendar][FreeBusy <address>]
    static bool isFreeBusy(const QString &mailboxKey);
    void addFreeBusy(const QString &account, const QString &address, const QString &name);
    void removeFreeBusy(const QString &address);
    // Shown free/busy people (by account) whose times for these days are not known (or older
    // than ten minutes): ask getSchedule and hand the answer to setFreeBusy()
    QHash<QString, QStringList> freeBusyMissing(QDate from, QDate to) const;
    void setFreeBusy(const QJsonObject &schedule, QDate from, QDate to);

Q_SIGNALS:
    void changed();

private:
    DisplayEvent display(const QString &mailboxKey, const struct EventRow &row) const;
    struct Busy {
        QDate from;
        QDate to;
        qint64 fetched = 0;
        QList<DisplayEvent> blocks;
        QString error;
    };

    MailboxSet *m_mailboxes;
    class CategoryList *m_categoryList = nullptr;
    QList<CalendarInfo> m_calendars;
    // By lower-case address; kept over reload(), which runs at every calendar sync
    QHash<QString, Busy> m_busy;
};

// Outlook-like colours for calendars without one of their own
QColor paletteColor(int index);

}
