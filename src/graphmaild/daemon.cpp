// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "daemon.h"
#include "accounts.h"
#include "graphclient.h"
#include "graphmail_debug.h"
#include "mailboxaccess.h"
#include "mailrunner.h"
#include "scopes.h"
#include "store.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KNotification>
#include <KSharedConfig>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDateTime>
#include <QDir>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QProcess>
#include <QThread>
#include <QUrl>
#include <QtConcurrentRun>

namespace GraphMail
{

namespace
{
QString keychainService()
{
    return QStringLiteral("plasma-graphmail-linux");
}

KSharedConfig::Ptr settings()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals);
    config->reparseConfiguration();
    return config;
}

KConfigGroup accountGroup(const KSharedConfig::Ptr &config, const QString &account)
{
    return config->group(QStringLiteral("Account ") + account);
}

KConfigGroup mailboxGroup(const KSharedConfig::Ptr &config, const QString &account, const QString &mailbox)
{
    return accountGroup(config, account).group(QStringLiteral("Mailbox ") + mailbox);
}
}

QString Daemon::serviceName()
{
    return QStringLiteral("io.github.nvaert1986.GraphMail");
}

Daemon::Daemon(QObject *parent)
    : QObject(parent)
{
}

Daemon::~Daemon()
{
    shutdown();
}

QString Daemon::key(const QString &account, const QString &mailbox)
{
    return account + QLatin1Char('/') + mailbox;
}

GraphMail::MailboxConfig Daemon::baseConfig() const
{
    const KConfigGroup general = settings()->group(QStringLiteral("General"));
    GraphMail::MailboxConfig config;
    config.bodyWindowDays = general.readEntry("BodyWindowDays", 90);
    config.attachmentsOffline = general.readEntry("AttachmentsOffline", false);
    config.hotSeconds = std::max(15, general.readEntry("HotSeconds", 60));
    config.warmSeconds = std::max(60, general.readEntry("WarmSeconds", 300));
    config.coldSeconds = std::max(300, general.readEntry("ColdSeconds", 1800));
    config.settings.recentWindowDays = std::max(7, general.readEntry("RecentWindowDays", 90));
    return config;
}

bool Daemon::start(QString *error)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerService(serviceName())) {
        *error = QStringLiteral("cannot register %1 on the session bus (already running?)").arg(serviceName());
        return false;
    }
    bus.registerObject(QStringLiteral("/GraphMail"), this, QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals);
    // Mail search from KRunner (data/io.github.nvaert1986.graphmail-runner.desktop)
    MailRunner::registerTypes();
    bus.registerObject(QStringLiteral("/runner"), new MailRunner([this] {
                           return m_mailboxes.keys();
                       }, this),
                       QDBusConnection::ExportScriptableSlots);

    const QList<GraphCore::Account> accounts = GraphCore::Accounts::all();
    if (accounts.isEmpty()) {
        qCWarning(GRAPHMAIL) << "No accounts in" << GraphCore::Accounts::configFileName() << "- sign in first (graphmailctl login)";
    }
    const KSharedConfig::Ptr config = settings();
    for (const GraphCore::Account &account : accounts) {
        m_auth.insert(account.id, std::make_shared<GraphCore::Auth>(GraphCore::Auth::settingsFor(account, keychainService(), GraphCore::Scopes::suite())));
        startMailbox(account.id, QStringLiteral("me"), QStringLiteral("/me"), true);
        const QStringList shared = accountGroup(config, account.id).readEntry("SharedMailboxes", QStringList());
        for (const QString &objectId : shared) {
            startMailbox(account.id, objectId, QStringLiteral("/users/") + objectId, false);
            if (mailboxGroup(config, account.id, objectId).readEntry("Address").isEmpty()) {
                resolveMailboxName(account.id, objectId);
            }
        }
    }
    return true;
}

void Daemon::startMailbox(const QString &account, const QString &mailbox, const QString &root, bool own)
{
    const QString k = key(account, mailbox);
    if (m_mailboxes.contains(k) || !m_auth.contains(account)) {
        return;
    }
    GraphMail::MailboxConfig config = baseConfig();
    config.account = account;
    config.mailbox = mailbox;
    config.root = root;
    config.cacheDir = GraphMail::cacheDirectory(account, mailbox);
    // To Do lists of shared mailboxes are not reachable this way
    config.todo = own;
    if (!own) {
        // Stored by AddSharedMailbox; mailboxes added before phase 7 have no entry: everything
        const QStringList access = mailboxGroup(settings(), account, mailbox).readEntry("Access", QStringList());
        if (!access.isEmpty()) {
            config.mail = access.contains(QLatin1String("mail"));
            config.calendar = access.contains(QLatin1String("calendar"));
            config.contacts = access.contains(QLatin1String("contacts"));
        }
    }

    auto *thread = new QThread(this);
    thread->setObjectName(QStringLiteral("mailbox ") + mailbox);
    auto *worker = new GraphMail::MailboxWorker(config, m_auth.value(account).get());
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &GraphMail::MailboxWorker::start);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &GraphMail::MailboxWorker::statusChanged, this, [this](const QString &account, const QString &mailbox, const QString &json) {
        const auto it = m_mailboxes.find(key(account, mailbox));
        if (it != m_mailboxes.end()) {
            it->status = json;
        }
        Q_EMIT StatusChanged(account, mailbox, json);
    });
    connect(worker, &GraphMail::MailboxWorker::collectionChanged, this, &Daemon::CollectionChanged);
    connect(worker, &GraphMail::MailboxWorker::bodyReady, this, [this](const QString &account, const QString &mailbox, qint64 message) {
        Q_EMIT BodyReady(account, mailbox, message);
    });
    connect(worker, &GraphMail::MailboxWorker::operationFailed, this, &Daemon::OperationFailed);
    connect(worker, &GraphMail::MailboxWorker::attachmentReady, this,
            [this](const QString &account, const QString &mailbox, qint64 message, qint64 attachment, bool ok) {
                Q_EMIT AttachmentReady(account, mailbox, message, attachment, ok);
            });
    connect(worker, &GraphMail::MailboxWorker::newMail, this, &Daemon::notifyNewMail);
    connect(worker, &GraphMail::MailboxWorker::outgoingChanged, this, &Daemon::outgoingChanged);
    connect(worker, &GraphMail::MailboxWorker::reminderDue, this, &Daemon::notifyReminder);
    connect(worker, &GraphMail::MailboxWorker::overdueReminders, this, &Daemon::notifyOverdue);
    connect(worker, &GraphMail::MailboxWorker::taskReminderDue, this, &Daemon::notifyTaskReminder);
    connect(worker, &GraphMail::MailboxWorker::conflictFound, this, &Daemon::notifyConflict);
    connect(worker, &GraphMail::MailboxWorker::queryAnswered, this, [this](quint64 request, const QString &json) {
        const QDBusMessage call = m_queries.take(request);
        if (call.type() == QDBusMessage::MethodCallMessage) {
            QDBusConnection::sessionBus().send(call.createReply(json));
        }
    });
    connect(worker, &GraphMail::MailboxWorker::sendRightLearned, this, &Daemon::sendRightLearned);
    connect(worker, &GraphMail::MailboxWorker::messageLoaded, this, [this](quint64 request, const QString &json) {
        const QDBusMessage call = m_loads.take(request);
        if (call.type() == QDBusMessage::MethodCallMessage) {
            QDBusConnection::sessionBus().send(call.createReply(json));
        }
    });
    connect(worker, &GraphMail::MailboxWorker::started, this, [mailbox](bool ok, const QString &error) {
        if (!ok) {
            qCWarning(GRAPHMAIL).noquote() << "Mailbox" << mailbox << "did not start:" << error;
        }
    });
    m_mailboxes.insert(k, {thread, worker, {}});
    thread->start();
    qCInfo(GRAPHMAIL).noquote() << "Syncing mailbox" << mailbox << "of account" << account;
}

void Daemon::stopMailbox(const QString &k)
{
    const auto it = m_mailboxes.find(k);
    if (it == m_mailboxes.end()) {
        return;
    }
    // stop() closes the cache in the worker's own thread, then the thread ends and deletes it
    QMetaObject::invokeMethod(it->worker, &GraphMail::MailboxWorker::stop, Qt::BlockingQueuedConnection);
    it->thread->quit();
    it->thread->wait();
    delete it->thread;
    m_mailboxes.erase(it);
}

void Daemon::shutdown()
{
    const QStringList keys = m_mailboxes.keys();
    for (const QString &k : keys) {
        stopMailbox(k);
    }
}

GraphMail::MailboxWorker *Daemon::worker(const QString &account, const QString &mailbox) const
{
    // An empty account means the only one there is (graphmailctl)
    if (account.isEmpty() && m_auth.size() == 1) {
        return worker(m_auth.cbegin().key(), mailbox);
    }
    const auto it = m_mailboxes.constFind(key(account, mailbox));
    return it == m_mailboxes.cend() ? nullptr : it->worker;
}

QString Daemon::Status()
{
    QJsonArray all;
    for (const Mailbox &mailbox : std::as_const(m_mailboxes)) {
        all.append(QJsonDocument::fromJson(mailbox.status.toUtf8()).object());
    }
    return QString::fromUtf8(QJsonDocument(all).toJson(QJsonDocument::Compact));
}

QString Daemon::Mailboxes()
{
    const KSharedConfig::Ptr config = settings();
    QJsonArray all;
    for (auto it = m_mailboxes.cbegin(); it != m_mailboxes.cend(); ++it) {
        const QString account = it.key().section(QLatin1Char('/'), 0, 0);
        const QString mailbox = it.key().section(QLatin1Char('/'), 1);
        const bool own = mailbox == QLatin1String("me");
        QString name;
        QString address;
        QStringList access{QStringLiteral("mail"), QStringLiteral("calendar"), QStringLiteral("contacts")};
        if (own) {
            const GraphCore::Account a = GraphCore::Accounts::find(account);
            name = a.displayName;
            address = a.userPrincipalName;
        } else {
            const KConfigGroup group = mailboxGroup(config, account, mailbox);
            name = group.readEntry("Name");
            address = group.readEntry("Address");
            access = group.readEntry("Access", access);
        }
        all.append(QJsonObject{{QStringLiteral("account"), account},
                               {QStringLiteral("mailbox"), mailbox},
                               {QStringLiteral("displayName"), name},
                               {QStringLiteral("address"), address},
                               {QStringLiteral("own"), own},
                               {QStringLiteral("access"), QJsonArray::fromStringList(access)},
                               {QStringLiteral("cache"), it->worker->cachePath()}});
    }
    return QString::fromUtf8(QJsonDocument(all).toJson(QJsonDocument::Compact));
}

void Daemon::resolveMailboxName(const QString &account, const QString &mailbox)
{
    std::shared_ptr<GraphCore::Auth> auth = m_auth.value(account);
    if (!auth) {
        return;
    }
    auto *watcher = new QFutureWatcher<QJsonObject>(this);
    connect(watcher, &QFutureWatcher<QJsonObject>::finished, this, [watcher, account, mailbox] {
        watcher->deleteLater();
        const QJsonObject user = watcher->result();
        if (user.isEmpty()) {
            return; // tried again at the next start
        }
        const KSharedConfig::Ptr config = settings();
        KConfigGroup group = mailboxGroup(config, account, mailbox);
        group.writeEntry("Name", user.value(QLatin1String("displayName")).toString());
        group.writeEntry("Address", user.value(QLatin1String("mail")).toString());
        config->sync();
    });
    watcher->setFuture(QtConcurrent::run([auth, mailbox]() -> QJsonObject {
        GraphCore::GraphClient client(auth.get());
        const GraphCore::GraphResponse response = client.get(QStringLiteral("/users/") + mailbox + QStringLiteral("?$select=displayName,mail"));
        return response.ok() ? response.json() : QJsonObject();
    }));
}

void Daemon::notifyNewMail(const QString &account, const QString &mailbox, qint64 message, const QString &sender, const QString &subject, int count)
{
    const KConfigGroup notifications = settings()->group(QStringLiteral("Notifications"));
    const bool own = mailbox == QLatin1String("me");
    const bool wanted = own ? notifications.readEntry("OwnInbox", true)
                            : notifications.readEntry("SharedMailboxes", QStringList()).contains(mailbox);
    if (!wanted) {
        return;
    }
    auto *notification = new KNotification(QStringLiteral("newMail"));
    notification->setComponentName(QStringLiteral("plasma-graphmail-linux"));
    if (count == 1) {
        notification->setTitle(sender);
        notification->setText(subject.toHtmlEscaped());
    } else {
        notification->setTitle(i18np("%1 new message", "%1 new messages", count));
        notification->setText(i18nc("@info newest of several new messages: sender, subject", "%1: %2", sender, subject).toHtmlEscaped());
    }
    notification->setIconName(QStringLiteral("mail-unread-new"));
    KNotificationAction *open = notification->addDefaultAction(i18nc("@action open the new message", "Open"));
    connect(open, &KNotificationAction::activated, this, [account, mailbox, message] {
        QProcess::startDetached(QStringLiteral(GRAPHMAIL_BINDIR "/plasma-graphmail-linux"),
                                {QStringLiteral("--message"), account + QLatin1Char('/') + mailbox + QLatin1Char('/') + QString::number(message)});
    });
    notification->sendEvent();
}

void Daemon::outgoingChanged(const QString &account, const QString &mailbox, const QString &uuid, const QString &state, const QString &error)
{
    Q_EMIT OutgoingChanged(account, mailbox, uuid, state, error);
    if (state != QLatin1String("failed")) {
        return;
    }
    // The composer is usually closed by now: tell the user, the draft stays in Drafts
    auto *notification = new KNotification(QStringLiteral("sendFailed"));
    notification->setComponentName(QStringLiteral("plasma-graphmail-linux"));
    notification->setTitle(i18nc("@title notification", "Message not sent"));
    const QString reason = error.startsWith(QLatin1String("sendAsDenied:"))
        ? i18nc("@info notification, %1 an address", "You may not send as %1. Ask your administrator for Send As or Send on Behalf permission.", error.mid(13))
        : error;
    notification->setText(i18nc("@info notification, %1 the reason", "It was kept in Drafts. %1", reason).toHtmlEscaped());
    notification->setIconName(QStringLiteral("mail-unread"));
    KNotificationAction *open = notification->addDefaultAction(i18nc("@action open the draft", "Open Draft"));
    connect(open, &KNotificationAction::activated, this, [account, mailbox, uuid] {
        QProcess::startDetached(QStringLiteral(GRAPHMAIL_BINDIR "/plasma-graphmail-linux"),
                                {QStringLiteral("--outgoing"), account + QLatin1Char('/') + mailbox + QLatin1Char('/') + uuid});
    });
    notification->sendEvent();
}

void Daemon::sendRightLearned(const QString &account, const QString &fromMailbox, const QString &right)
{
    const KSharedConfig::Ptr config = settings();
    KConfigGroup group = mailboxGroup(config, account, fromMailbox);
    group.writeEntry("SendRight", right);
    group.writeEntry("SendRightDate", QDateTime::currentDateTimeUtc());
    config->sync();
    Q_EMIT SendRightLearned(account, fromMailbox, right);
}

void Daemon::notifyReminder(const QString &account, const QString &mailbox, qint64 event, const QString &subject, qint64 start, const QString &location)
{
    auto *notification = new KNotification(QStringLiteral("reminder"), KNotification::Persistent);
    notification->setComponentName(QStringLiteral("plasma-graphmail-linux"));
    notification->setTitle(subject.isEmpty() ? i18nc("@title notification an event without a subject", "(no subject)") : subject);
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(start).toLocalTime();
    const QString time = QLocale().toString(when, QLocale().name() == QLatin1String("C") ? QStringLiteral("dd-MM-yyyy HH:mm") : QLocale().dateTimeFormat(QLocale::ShortFormat));
    notification->setText((location.isEmpty() ? time : i18nc("@info reminder: time, place", "%1, %2", time, location)).toHtmlEscaped());
    notification->setIconName(QStringLiteral("view-calendar-day"));
    GraphMail::MailboxWorker *w = worker(account, mailbox);
    KNotificationAction *snooze = notification->addAction(i18nc("@action reminder", "Snooze 5 Minutes"));
    connect(snooze, &KNotificationAction::activated, this, [w, event] {
        if (w) {
            const qint64 until = QDateTime::currentMSecsSinceEpoch() + 5 * 60 * 1000;
            QMetaObject::invokeMethod(w, [w, event, until] {
                w->reminderAction(event, false, until);
            });
        }
    });
    KNotificationAction *dismiss = notification->addAction(i18nc("@action reminder", "Dismiss"));
    connect(dismiss, &KNotificationAction::activated, this, [w, event] {
        if (w) {
            QMetaObject::invokeMethod(w, [w, event] {
                w->reminderAction(event, true, 0);
            });
        }
    });
    KNotificationAction *open = notification->addDefaultAction(i18nc("@action open the event", "Open"));
    connect(open, &KNotificationAction::activated, this, [account, mailbox, event] {
        QProcess::startDetached(QStringLiteral(GRAPHMAIL_BINDIR "/plasma-graphmail-linux"),
                                {QStringLiteral("--event"), account + QLatin1Char('/') + mailbox + QLatin1Char('/') + QString::number(event)});
    });
    notification->sendEvent();
}

void Daemon::notifyOverdue(const QString &account, const QString &mailbox, int count, bool more, qint64 oldest, const QStringList &subjects)
{
    if (m_overdue) {
        m_overdue->close();
    }
    if (count == 0) {
        return;
    }
    // One notification for all of them, as Outlook's reminder window lists them
    auto *notification = new KNotification(QStringLiteral("overdueReminders"), KNotification::Persistent);
    notification->setComponentName(QStringLiteral("plasma-graphmail-linux"));
    notification->setTitle(more ? i18nc("@title notification", "More than %1 overdue reminders", count) : i18ncp("@title notification", "%1 overdue reminder", "%1 overdue reminders", count));
    const QString since = QLocale().toString(QDateTime::fromMSecsSinceEpoch(oldest).toLocalTime().date(), QStringLiteral("d MMMM yyyy"));
    QString text = i18nc("@info notification", "Reminders of events that are over and were never dismissed, the oldest from %1.", since);
    if (!subjects.isEmpty()) {
        text += QLatin1Char('\n') + i18nc("@info notification, %1 a few subjects", "Latest: %1", subjects.join(QStringLiteral(", ")));
    }
    notification->setText(text.toHtmlEscaped());
    notification->setIconName(QStringLiteral("view-calendar-day"));
    KNotificationAction *dismissAll = notification->addAction(i18nc("@action reminders", "Dismiss All"));
    connect(dismissAll, &KNotificationAction::activated, this, [this, account, mailbox] {
        DismissOverdueReminders(account, mailbox);
    });
    m_overdue = notification;
    notification->sendEvent();
}

void Daemon::notifyTaskReminder(const QString &account, const QString &mailbox, qint64 task, const QString &title, qint64 due)
{
    auto *notification = new KNotification(QStringLiteral("taskReminder"), KNotification::Persistent);
    notification->setComponentName(QStringLiteral("plasma-graphmail-linux"));
    notification->setTitle(title.isEmpty() ? i18nc("@title notification a task without a title", "(no title)") : title);
    notification->setText(due > 0 ? i18nc("@info notification", "Task, due %1", QLocale().toString(QDateTime::fromMSecsSinceEpoch(due).toLocalTime().date(), QStringLiteral("dddd d MMMM")))
                                  : i18nc("@info notification", "Task"));
    notification->setIconName(QStringLiteral("view-task"));
    GraphMail::MailboxWorker *w = worker(account, mailbox);
    KNotificationAction *snooze = notification->addAction(i18nc("@action reminder", "Snooze 5 Minutes"));
    connect(snooze, &KNotificationAction::activated, this, [w, task] {
        const qint64 until = QDateTime::currentMSecsSinceEpoch() + 5 * 60 * 1000;
        QMetaObject::invokeMethod(w, [w, task, until] {
            w->taskReminderAction(task, false, until);
        });
    });
    KNotificationAction *dismiss = notification->addAction(i18nc("@action reminder", "Dismiss"));
    connect(dismiss, &KNotificationAction::activated, this, [w, task] {
        QMetaObject::invokeMethod(w, [w, task] {
            w->taskReminderAction(task, true, 0);
        });
    });
    KNotificationAction *open = notification->addDefaultAction(i18nc("@action open the task", "Open"));
    connect(open, &KNotificationAction::activated, this, [account, mailbox, task] {
        QProcess::startDetached(QStringLiteral(GRAPHMAIL_BINDIR "/plasma-graphmail-linux"),
                                {QStringLiteral("--task"), account + QLatin1Char('/') + mailbox + QLatin1Char('/') + QString::number(task)});
    });
    notification->sendEvent();
}

void Daemon::CreateItem(const QString &account, const QString &mailbox, const QString &kind, qlonglong parent, const QString &json)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, kind, parent, json] {
            w->createItem(kind, parent, json);
        });
    }
}

void Daemon::UpdateItem(const QString &account, const QString &mailbox, const QString &kind, qlonglong item, const QString &json)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, kind, item, json] {
            w->updateItem(kind, item, json);
        });
    }
}

void Daemon::DeleteItem(const QString &account, const QString &mailbox, const QString &kind, qlonglong item)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, kind, item] {
            w->deleteItem(kind, item);
        });
    }
}

void Daemon::TodoList(const QString &account, const QString &mailbox, const QString &action, qlonglong list, const QString &name)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, action, list, name] {
            w->todoList(action, list, name);
        });
    }
}

void Daemon::TaskStep(const QString &account, const QString &mailbox, qlonglong task, const QString &action, const QString &stepId, const QString &json)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, task, action, stepId, json] {
            w->taskStep(task, action, stepId, json);
        });
    }
}

void Daemon::TaskReminderAction(const QString &account, const QString &mailbox, qlonglong task, bool dismiss, int minutes)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        const qint64 until = QDateTime::currentMSecsSinceEpoch() + qint64(minutes) * 60 * 1000;
        QMetaObject::invokeMethod(w, [w, task, dismiss, until] {
            w->taskReminderAction(task, dismiss, until);
        });
    }
}

void Daemon::DismissOverdueReminders(const QString &account, const QString &mailbox)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, &GraphMail::MailboxWorker::dismissOverdueReminders);
    }
}

void Daemon::CreateEvent(const QString &account, const QString &mailbox, qlonglong calendar, const QString &json)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, calendar, json] {
            w->createEvent(calendar, json);
        });
    }
}

void Daemon::UpdateEvent(const QString &account, const QString &mailbox, qlonglong event, const QString &json)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, event, json] {
            w->updateEvent(event, json);
        });
    }
}

void Daemon::DeleteEvent(const QString &account, const QString &mailbox, qlonglong event, bool cancel, const QString &comment)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, event, cancel, comment] {
            w->deleteEvent(event, cancel, comment);
        });
    }
}

void Daemon::RespondEvent(const QString &account, const QString &mailbox, qlonglong event, const QString &action, const QString &comment, bool sendResponse)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, event, action, comment, sendResponse] {
            w->respondEvent(event, action, comment, sendResponse);
        });
    }
}

void Daemon::ProposeNewTime(const QString &account, const QString &mailbox, const QString &eventGraphId, const QString &action, const QString &comment, qlonglong start,
                            qlonglong end)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, eventGraphId, action, comment, start, end] {
            w->respondEventByGraphId(eventGraphId, action, comment, true, start, end);
        });
    }
}

void Daemon::RespondMeeting(const QString &account, const QString &mailbox, const QString &eventGraphId, const QString &action, const QString &comment, bool sendResponse)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, eventGraphId, action, comment, sendResponse] {
            w->respondEventByGraphId(eventGraphId, action, comment, sendResponse);
        });
    }
}

void Daemon::ReminderAction(const QString &account, const QString &mailbox, qlonglong event, bool dismiss, int minutes)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        const qint64 until = QDateTime::currentMSecsSinceEpoch() + qint64(minutes) * 60 * 1000;
        QMetaObject::invokeMethod(w, [w, event, dismiss, until] {
            w->reminderAction(event, dismiss, until);
        });
    }
}

QString Daemon::Query(const QString &account, const QString &mailbox, const QString &kind, const QString &json)
{
    GraphMail::MailboxWorker *w = worker(account, mailbox);
    if (!w) {
        return {};
    }
    setDelayedReply(true);
    const quint64 request = m_nextLoad++;
    m_queries.insert(request, this->message());
    QMetaObject::invokeMethod(w, [w, request, kind, json] {
        w->query(request, kind, json);
    });
    return {};
}

void Daemon::SaveOutgoing(const QString &account, const QString &mailbox, const QString &json)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, json] {
            w->saveOutgoing(json);
        });
    }
}

void Daemon::DiscardOutgoing(const QString &account, const QString &mailbox, const QString &uuid)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, uuid] {
            w->discardOutgoing(uuid);
        });
    }
}

QString Daemon::LoadMessage(const QString &account, const QString &mailbox, qlonglong message)
{
    GraphMail::MailboxWorker *w = worker(account, mailbox);
    if (!w) {
        return {};
    }
    // Answered when the worker has it (a network round trip)
    setDelayedReply(true);
    const quint64 request = m_nextLoad++;
    m_loads.insert(request, this->message());
    QMetaObject::invokeMethod(w, [w, request, message] {
        w->loadMessage(request, message);
    });
    return {};
}

QString Daemon::CachePath(const QString &account, const QString &mailbox)
{
    GraphMail::MailboxWorker *w = worker(account, mailbox);
    return w ? w->cachePath() : QString();
}

void Daemon::ApplyMine(const QString &account, const QString &mailbox, const QString &kind, qlonglong row, const QString &changes)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, kind, row, changes] {
            w->applyMine(kind, row, changes);
        });
    }
}

void Daemon::notifyConflict(const QString &account, const QString &mailbox, const QString &kind, qint64 row, const QString &title, const QStringList &fields,
                            const QString &changes)
{
    qCInfo(GRAPHMAIL).noquote() << "Conflict on" << kind << row << fields.join(QLatin1Char(','));
    auto *notification = new KNotification(QStringLiteral("conflict"));
    notification->setComponentName(QStringLiteral("plasma-graphmail-linux"));
    if (kind == QLatin1String("draft")) {
        // Both versions are kept: nothing to choose
        notification->setTitle(i18nc("@title notification", "Draft changed in two places"));
        notification->setText(i18nc("@info notification, %1 a subject", "The draft \"%1\" was also changed in Outlook. Both versions are in Drafts.", title).toHtmlEscaped());
        notification->setIconName(QStringLiteral("dialog-warning"));
        notification->sendEvent();
        return;
    }
    notification->setTitle(i18nc("@title notification", "Changed elsewhere too"));
    const QString what = kind == QLatin1String("event") ? i18nc("@item", "event") : kind == QLatin1String("contact") ? i18nc("@item", "contact") : i18nc("@item", "task");
    notification->setText(i18nc("@info notification, %1 event/contact/task, %2 its name, %3 the fields",
                                "Your change to the %1 \"%2\" (%3) was not applied: it was changed in the same place in Outlook or on another device.", what, title,
                                fields.join(QStringLiteral(", ")))
                              .toHtmlEscaped());
    notification->setIconName(QStringLiteral("dialog-warning"));
    KNotificationAction *mine = notification->addAction(i18nc("@action conflict", "Apply Mine"));
    connect(mine, &KNotificationAction::activated, this, [this, account, mailbox, kind, row, changes] {
        ApplyMine(account, mailbox, kind, row, changes);
    });
    notification->sendEvent();
}

void Daemon::RetryNow(const QString &account, const QString &mailbox)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, &GraphMail::MailboxWorker::retryNow);
    }
}

void Daemon::DiscardOperation(const QString &account, const QString &mailbox, qlonglong operation)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, operation] {
            w->discardOperation(operation);
        });
    }
}

void Daemon::ApplyOfflineSettings(bool freeSpace)
{
    // baseConfig() reads graphmailrc again (settings() reparses it)
    const GraphMail::MailboxConfig config = baseConfig();
    for (auto it = m_mailboxes.cbegin(); it != m_mailboxes.cend(); ++it) {
        GraphMail::MailboxWorker *worker = it->worker;
        QMetaObject::invokeMethod(worker, [worker, config, freeSpace] {
            worker->applyOffline(config.bodyWindowDays, config.attachmentsOffline, freeSpace);
        });
    }
}

void Daemon::SyncNow()
{
    for (const Mailbox &mailbox : std::as_const(m_mailboxes)) {
        QMetaObject::invokeMethod(mailbox.worker, &GraphMail::MailboxWorker::syncNow);
    }
}

void Daemon::ResyncFolder(const QString &account, const QString &mailbox, qlonglong folder)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, folder] {
            w->resyncFolder(folder);
        });
    }
}

void Daemon::SetFolderOpen(const QString &account, const QString &mailbox, qlonglong folder, bool open)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, folder, open] {
            w->setFolderOpen(folder, open);
        });
    }
}

void Daemon::FetchBody(const QString &account, const QString &mailbox, qlonglong message)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, message] {
            w->fetchBody(message);
        });
    }
}

void Daemon::FetchAttachment(const QString &account, const QString &mailbox, qlonglong attachment)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, attachment] {
            w->fetchAttachment(attachment);
        });
    }
}

void Daemon::MarkReadWithoutReceipt(const QString &account, const QString &mailbox, qlonglong message)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, message] {
            w->markRead(message, true, false);
        });
    }
}

void Daemon::MarkRead(const QString &account, const QString &mailbox, qlonglong message, bool read)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, message, read] {
            w->markRead(message, read);
        });
    }
}

void Daemon::SetCategories(const QString &account, const QString &mailbox, qlonglong message, const QString &categories)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        const QStringList names = GraphMail::categoriesFromJson(categories);
        QMetaObject::invokeMethod(w, [w, message, names] {
            w->setCategories(message, names);
        });
    }
}

void Daemon::SetFocused(const QString &account, const QString &mailbox, qlonglong message, bool focused)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, message, focused] {
            w->setFocused(message, focused);
        });
    }
}

void Daemon::SetFlagDates(const QString &account, const QString &mailbox, qlonglong message, qlonglong start, qlonglong due)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, message, start, due] {
            w->setFlag(message, 1, start, due);
        });
    }
}

void Daemon::SetFlag(const QString &account, const QString &mailbox, qlonglong message, int flag)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, message, flag] {
            w->setFlag(message, flag);
        });
    }
}

void Daemon::Move(const QString &account, const QString &mailbox, qlonglong message, qlonglong folder)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, message, folder] {
            w->move(message, folder);
        });
    }
}

void Daemon::Delete(const QString &account, const QString &mailbox, qlonglong message)
{
    if (GraphMail::MailboxWorker *w = worker(account, mailbox)) {
        QMetaObject::invokeMethod(w, [w, message] {
            w->remove(message);
        });
    }
}

QString Daemon::CheckMailbox(const QString &accountId, const QString &address)
{
    return runAccessCheck(accountId, address, false);
}

QString Daemon::AddSharedMailbox(const QString &accountId, const QString &address)
{
    return runAccessCheck(accountId, address, true);
}

QString Daemon::runAccessCheck(const QString &accountId, const QString &address, bool add)
{
    const QString account = accountId.isEmpty() && m_auth.size() == 1 ? m_auth.cbegin().key() : accountId;
    if (!m_auth.contains(account)) {
        return QStringLiteral("error: unknown account");
    }
    // Answered later: the check is several network round trips, and D-Bus must not wait on them here
    setDelayedReply(true);
    const QDBusMessage request = message();
    std::shared_ptr<GraphCore::Auth> auth = m_auth.value(account);
    auto *watcher = new QFutureWatcher<GraphMail::MailboxAccess>(this);
    connect(watcher, &QFutureWatcher<GraphMail::MailboxAccess>::finished, this, [this, watcher, request, account, add] {
        watcher->deleteLater();
        GraphMail::MailboxAccess access = watcher->result();
        if (add && access.problem.isEmpty() && !access.any()) {
            access.problem = QStringLiteral("noAccess");
        }
        if (add && access.problem.isEmpty()) {
            const KSharedConfig::Ptr config = settings();
            KConfigGroup names = mailboxGroup(config, account, access.id);
            names.writeEntry("Name", access.name);
            names.writeEntry("Address", access.address);
            names.writeEntry("Access", access.parts());
            KConfigGroup group = accountGroup(config, account);
            QStringList shared = group.readEntry("SharedMailboxes", QStringList());
            if (!shared.contains(access.id)) {
                shared.append(access.id);
                group.writeEntry("SharedMailboxes", shared);
            }
            config->sync();
            // Access changed for a mailbox already open: start it again with the new parts
            stopMailbox(key(account, access.id));
            startMailbox(account, access.id, QStringLiteral("/users/") + access.id, false);
        }
        QDBusConnection::sessionBus().send(request.createReply(QString::fromUtf8(QJsonDocument(access.toJson()).toJson(QJsonDocument::Compact))));
    });
    watcher->setFuture(QtConcurrent::run([auth, address]() {
        GraphCore::GraphClient client(auth.get());
        return GraphMail::checkMailboxAccess(client, address);
    }));
    return {};
}

QString Daemon::OrgChart(const QString &accountId, const QString &user)
{
    const QString account = accountId.isEmpty() && m_auth.size() == 1 ? m_auth.cbegin().key() : accountId;
    const GraphCore::Account a = GraphCore::Accounts::find(account);
    if (!a.isValid()) {
        return QStringLiteral("{\"ok\":false,\"problem\":\"unknownAccount\"}");
    }
    if (!m_directoryAuth.contains(account)) {
        m_directoryAuth.insert(account, std::make_shared<GraphCore::Auth>(GraphCore::Auth::settingsFor(a, keychainService(), GraphCore::Scopes::directory())));
    }
    setDelayedReply(true);
    const QDBusMessage request = message();
    std::shared_ptr<GraphCore::Auth> auth = m_directoryAuth.value(account);
    auto *watcher = new QFutureWatcher<QJsonObject>(this);
    connect(watcher, &QFutureWatcher<QJsonObject>::finished, this, [this, watcher, request, account] {
        watcher->deleteLater();
        const QJsonObject answer = watcher->result();
        // Asked again next time: an administrator may grant the consent meanwhile
        if (answer.value(QLatin1String("problem")).toString() == QLatin1String("notGranted")) {
            m_directoryAuth.remove(account);
        }
        QDBusConnection::sessionBus().send(request.createReply(QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact))));
    });
    watcher->setFuture(QtConcurrent::run([auth, user]() {
        QString error;
        if (auth->accessToken(&error).isEmpty()) {
            // No consent for User.Read.All in this tenant (AADSTS65001), or offline
            const bool consent = error.contains(QLatin1String("AADSTS65001")) || error.contains(QLatin1String("consent"), Qt::CaseInsensitive);
            return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("problem"), consent ? QStringLiteral("notGranted") : QStringLiteral("offline")}};
        }
        GraphCore::GraphClient client(auth.get());
        const QString base = QStringLiteral("/users/") + QString::fromUtf8(QUrl::toPercentEncoding(user));
        const QString fields = QStringLiteral("$select=id,displayName,mail,userPrincipalName,jobTitle");
        const GraphCore::GraphResponse manager = client.get(base + QStringLiteral("/manager?") + fields);
        const GraphCore::GraphResponse reports = client.get(base + QStringLiteral("/directReports?$top=50&") + fields);
        if (manager.status == 403 || reports.status == 403) {
            return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("problem"), QStringLiteral("notGranted")}};
        }
        // 404 on the manager: none is set
        return QJsonObject{{QStringLiteral("ok"), reports.ok()},
                           {QStringLiteral("problem"), reports.ok() ? QString() : QStringLiteral("failed")},
                           {QStringLiteral("manager"), manager.ok() ? QJsonValue(manager.json()) : QJsonValue()},
                           {QStringLiteral("reports"), reports.json().value(QLatin1String("value")).toArray()}};
    }));
    return {};
}

QString Daemon::RemoveMailbox(const QString &accountId, const QString &mailbox)
{
    const QString account = accountId.isEmpty() && m_auth.size() == 1 ? m_auth.cbegin().key() : accountId;
    if (mailbox == QLatin1String("me")) {
        return QStringLiteral("error: the own mailbox cannot be removed this way");
    }
    stopMailbox(key(account, mailbox));
    const KSharedConfig::Ptr config = settings();
    KConfigGroup group = accountGroup(config, account);
    QStringList shared = group.readEntry("SharedMailboxes", QStringList());
    shared.removeAll(mailbox);
    group.writeEntry("SharedMailboxes", shared);
    mailboxGroup(config, account, mailbox).deleteGroup();
    config->sync();
    QDir(GraphMail::cacheDirectory(account, mailbox)).removeRecursively();
    return QStringLiteral("removed");
}

}
