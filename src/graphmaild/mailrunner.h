// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDBusArgument>
#include <QHash>
#include <QObject>
#include <QVariantMap>

#include <functional>
#include <memory>

namespace GraphMail
{

class Store;

// KRunner's D-Bus types (org.kde.krunner1)
struct RunnerMatch {
    QString id;
    QString text;
    QString iconName;
    int type = 30; // KRunner::QueryMatch::PossibleMatch
    double relevance = 0;
    QVariantMap properties;
};
struct RunnerAction {
    QString id;
    QString text;
    QString iconName;
};

// Mail search from KRunner (docs/PHASE9B.md 9b.31): subject, sender and preview in the caches,
// read only; Run opens the message in the app
class MailRunner : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.krunner1")

public:
    // mailboxes: the "account/mailbox" keys the daemon runs
    explicit MailRunner(std::function<QStringList()> mailboxes, QObject *parent = nullptr);
    ~MailRunner() override;
    static void registerTypes();

public Q_SLOTS:
    Q_SCRIPTABLE QList<GraphMail::RunnerAction> Actions();
    Q_SCRIPTABLE QList<GraphMail::RunnerMatch> Match(const QString &query);
    Q_SCRIPTABLE void Run(const QString &matchId, const QString &actionId);
    Q_SCRIPTABLE void SetActivationToken(const QString &token);
    Q_SCRIPTABLE void Teardown();

private:
    Store *store(const QString &key);

    std::function<QStringList()> m_mailboxes;
    QHash<QString, std::shared_ptr<Store>> m_stores;
    QString m_token;
};

}

QDBusArgument &operator<<(QDBusArgument &argument, const GraphMail::RunnerMatch &match);
const QDBusArgument &operator>>(const QDBusArgument &argument, GraphMail::RunnerMatch &match);
QDBusArgument &operator<<(QDBusArgument &argument, const GraphMail::RunnerAction &action);
const QDBusArgument &operator>>(const QDBusArgument &argument, GraphMail::RunnerAction &action);

Q_DECLARE_METATYPE(GraphMail::RunnerMatch)
Q_DECLARE_METATYPE(GraphMail::RunnerAction)
