// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "serviceclient.h"
#include "mailboxworker.h"

#include <QDBusContext>
#include <QDBusMessage>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>

#include <memory>

class KNotification;
class QThread;

namespace GraphMail
{

// The sync daemon (docs/PHASE2.md section 5): one MailboxWorker thread per mailbox of every
// account in graphaccountsrc, the shared mailboxes from graphmailrc, and the D-Bus interface the UI and
// graphmailctl use. Changes go to the mailbox's worker; lists and counts the UI reads from the cache.
class Daemon : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.nvaert1986.GraphMail")

public:
    static QString serviceName();
    explicit Daemon(QObject *parent = nullptr);
    ~Daemon() override;

    bool start(QString *error);
    void shutdown();

public Q_SLOTS:
    // Every mailbox's status as a JSON array
    Q_SCRIPTABLE QString Status();
    // Every synced mailbox as a JSON array: account, mailbox, displayName, address, own, cache
    Q_SCRIPTABLE QString Mailboxes();
    Q_SCRIPTABLE QString CachePath(const QString &account, const QString &mailbox);
    Q_SCRIPTABLE void SyncNow();
    // graphmailrc [General] BodyWindowDays / AttachmentsOffline changed: every mailbox follows;
    // freeSpace also drops what is outside the window now
    Q_SCRIPTABLE void ApplyOfflineSettings(bool freeSpace);
    // The "Waiting to be sent" window
    Q_SCRIPTABLE void RetryNow(const QString &account, const QString &mailbox);
    // A conflict's "Apply Mine" (also from the notification): kind event, contact or task
    Q_SCRIPTABLE void ApplyMine(const QString &account, const QString &mailbox, const QString &kind, qlonglong row, const QString &changes);
    Q_SCRIPTABLE void DiscardOperation(const QString &account, const QString &mailbox, qlonglong operation);
    Q_SCRIPTABLE void ResyncFolder(const QString &account, const QString &mailbox, qlonglong folder);
    Q_SCRIPTABLE void SetFolderOpen(const QString &account, const QString &mailbox, qlonglong folder, bool open);
    Q_SCRIPTABLE void FetchBody(const QString &account, const QString &mailbox, qlonglong message);
    // The content of an attachment (row id in the cache), for opening or saving it
    Q_SCRIPTABLE void FetchAttachment(const QString &account, const QString &mailbox, qlonglong attachment);
    Q_SCRIPTABLE void MarkRead(const QString &account, const QString &mailbox, qlonglong message, bool read);
    // Read, and no read receipt even when the sender asked for one (docs/PHASE9B.md 9b.12)
    Q_SCRIPTABLE void MarkReadWithoutReceipt(const QString &account, const QString &mailbox, qlonglong message);
    Q_SCRIPTABLE void SetFlag(const QString &account, const QString &mailbox, qlonglong message, int flag);
    // Flagged with follow-up dates (milliseconds since the epoch)
    // Focused Inbox: Move to Focused (true) / Move to Other (false)
    Q_SCRIPTABLE void SetFocused(const QString &account, const QString &mailbox, qlonglong message, bool focused);
    Q_SCRIPTABLE void SetFlagDates(const QString &account, const QString &mailbox, qlonglong message, qlonglong start, qlonglong due);
    // categories: a JSON array of names
    Q_SCRIPTABLE void SetCategories(const QString &account, const QString &mailbox, qlonglong message, const QString &categories);
    Q_SCRIPTABLE void Move(const QString &account, const QString &mailbox, qlonglong message, qlonglong folder);
    Q_SCRIPTABLE void Delete(const QString &account, const QString &mailbox, qlonglong message);
    // Resolves the address to the mailbox's object id first (the SMTP address is not always the
    // UPN, phase 0) and starts syncing it. Returns the mailbox key, or "error: ..." text.
    // Adds another mailbox after checking what it gives; answers the check as JSON (MailboxAccess)
    Q_SCRIPTABLE QString AddSharedMailbox(const QString &account, const QString &address);
    // The same check without adding anything
    Q_SCRIPTABLE QString CheckMailbox(const QString &account, const QString &address);
    // A person's manager and direct reports (docs/PHASE9B.md 9b.30), as JSON {"ok", "problem",
    // "manager", "reports"}; problem "notGranted" when the tenant gave no consent for User.Read.All
    Q_SCRIPTABLE QString OrgChart(const QString &account, const QString &user);
    // A message being written (Outgoing JSON, docs/PHASE4.md): stored in the mailbox's cache and
    // brought to Exchange as a draft; sent when its "send" is true
    Q_SCRIPTABLE void SaveOutgoing(const QString &account, const QString &mailbox, const QString &json);
    // Deletes it, and its draft in Exchange
    Q_SCRIPTABLE void DiscardOutgoing(const QString &account, const QString &mailbox, const QString &uuid);
    // A message read whole from Graph (Bcc, body, attachments): JSON, empty when that failed
    Q_SCRIPTABLE QString LoadMessage(const QString &account, const QString &mailbox, qlonglong message);
    // Calendar (docs/PHASE5.md): event JSON in Graph's shape; ids are cache row ids
    Q_SCRIPTABLE void CreateEvent(const QString &account, const QString &mailbox, qlonglong calendar, const QString &json);
    Q_SCRIPTABLE void UpdateEvent(const QString &account, const QString &mailbox, qlonglong event, const QString &json);
    // cancel: as the organizer of a meeting; the attendees get `comment`
    Q_SCRIPTABLE void DeleteEvent(const QString &account, const QString &mailbox, qlonglong event, bool cancel, const QString &comment);
    // action: accept, tentativelyAccept, decline
    Q_SCRIPTABLE void RespondEvent(const QString &account, const QString &mailbox, qlonglong event, const QString &action, const QString &comment, bool sendResponse);
    // A meeting request's event, by its graph id (it may be outside the synced window)
    Q_SCRIPTABLE void RespondMeeting(const QString &account, const QString &mailbox, const QString &eventGraphId, const QString &action, const QString &comment, bool sendResponse);
    // Tentative or Decline with another time for the meeting (milliseconds since the epoch)
    Q_SCRIPTABLE void ProposeNewTime(const QString &account, const QString &mailbox, const QString &eventGraphId, const QString &action, const QString &comment, qlonglong start,
                                     qlonglong end);
    // A reminder's Dismiss, or Snooze for `minutes` (what the notification's buttons do)
    Q_SCRIPTABLE void ReminderAction(const QString &account, const QString &mailbox, qlonglong event, bool dismiss, int minutes);
    // Contacts and To Do (docs/PHASE6.md): kind contact or task, rows of the cache
    Q_SCRIPTABLE void CreateItem(const QString &account, const QString &mailbox, const QString &kind, qlonglong parent, const QString &json);
    Q_SCRIPTABLE void UpdateItem(const QString &account, const QString &mailbox, const QString &kind, qlonglong item, const QString &json);
    Q_SCRIPTABLE void DeleteItem(const QString &account, const QString &mailbox, const QString &kind, qlonglong item);
    // action: create (name), rename (list, name), delete (list)
    Q_SCRIPTABLE void TodoList(const QString &account, const QString &mailbox, const QString &action, qlonglong list, const QString &name);
    // action: create, update, delete
    Q_SCRIPTABLE void TaskStep(const QString &account, const QString &mailbox, qlonglong task, const QString &action, const QString &stepId, const QString &json);
    Q_SCRIPTABLE void TaskReminderAction(const QString &account, const QString &mailbox, qlonglong task, bool dismiss, int minutes);
    // Outlook's Dismiss All for the reminders of events that are over
    Q_SCRIPTABLE void DismissOverdueReminders(const QString &account, const QString &mailbox);
    // Read-only lookups: kind schedule, rooms or meetingEvent; JSON in and out, empty on failure
    Q_SCRIPTABLE QString Query(const QString &account, const QString &mailbox, const QString &kind, const QString &json);
    // Stops syncing it and deletes its cache
    Q_SCRIPTABLE QString RemoveMailbox(const QString &account, const QString &mailbox);

Q_SIGNALS:
    Q_SCRIPTABLE void CollectionChanged(const QString &account, const QString &mailbox, const QString &collection, int changes);
    Q_SCRIPTABLE void StatusChanged(const QString &account, const QString &mailbox, const QString &json);
    Q_SCRIPTABLE void BodyReady(const QString &account, const QString &mailbox, qlonglong message);
    Q_SCRIPTABLE void AttachmentReady(const QString &account, const QString &mailbox, qlonglong message, qlonglong attachment, bool ok);
    // state: saved, sent, failed, discarded
    Q_SCRIPTABLE void OutgoingChanged(const QString &account, const QString &mailbox, const QString &uuid, const QString &state, const QString &error);
    // Learned when sending from another mailbox (as, behalf, denied); also kept in graphmailrc
    Q_SCRIPTABLE void SendRightLearned(const QString &account, const QString &mailbox, const QString &right);
    Q_SCRIPTABLE void OperationFailed(const QString &account, const QString &mailbox, const QString &message, const QString &error);

private:
    // LoadMessage calls waiting for their worker
    QHash<quint64, QDBusMessage> m_loads;
    QHash<quint64, QDBusMessage> m_queries;
    // The overdue reminders' notification, replaced when it changes
    QPointer<KNotification> m_overdue;
    quint64 m_nextLoad = 1;
    struct Mailbox {
        QThread *thread = nullptr;
        GraphMail::MailboxWorker *worker = nullptr;
        QString status;
    };

    // The account's own and shared mailboxes, once the keychain holds its sign-in (checked off the
    // main thread); without one it waits and is checked again every few minutes
    void startAccount(const QString &account);
    // Accounts in graphaccountsrc this daemon does not know yet (signed in through the service)
    void addNewAccounts();
    void startMailbox(const QString &account, const QString &mailbox, const QString &root, bool own);
    // Display name and address of a shared mailbox, looked up once and kept in graphmailrc
    void resolveMailboxName(const QString &account, const QString &mailbox);
    QString runAccessCheck(const QString &account, const QString &address, bool add);
    void notifyNewMail(const QString &account, const QString &mailbox, qint64 message, const QString &sender, const QString &subject, int count);
    void stopMailbox(const QString &key);
    void outgoingChanged(const QString &account, const QString &mailbox, const QString &uuid, const QString &state, const QString &error);
    void sendRightLearned(const QString &account, const QString &fromMailbox, const QString &right);
    void notifyTaskReminder(const QString &account, const QString &mailbox, qint64 task, const QString &title, qint64 due);
    void notifyConflict(const QString &account, const QString &mailbox, const QString &kind, qint64 row, const QString &title, const QStringList &fields,
                        const QString &changes);
    void notifyOverdue(const QString &account, const QString &mailbox, int count, bool more, qint64 oldest, const QStringList &subjects);
    void notifyReminder(const QString &account, const QString &mailbox, qint64 event, const QString &subject, qint64 start, const QString &location);
    GraphMail::MailboxWorker *worker(const QString &account, const QString &mailbox) const;
    GraphMail::MailboxConfig baseConfig() const;
    static QString key(const QString &account, const QString &mailbox);

    // Access tokens of each account, from the suite's account service (graphaccountsd)
    QHash<QString, std::shared_ptr<GraphCore::ServiceTokenProvider>> m_auth;
    // Per account, only for OrgChart: the "directory" permission set (needs admin consent)
    QHash<QString, std::shared_ptr<GraphCore::ServiceTokenProvider>> m_directoryAuth;
    QHash<QString, Mailbox> m_mailboxes;
    // Accounts in graphaccountsrc without a Graph Mail sign-in (for instance one only another app
    // of the suite signed in): no mailboxes until they sign in
    QSet<QString> m_notSignedIn;
    QSet<QString> m_checkingSignIn;
    QTimer *m_signInCheck = nullptr;
    // The account service's signals: accounts signed in or out
    GraphCore::AccountsClient *m_accountsClient = nullptr;
};

}
