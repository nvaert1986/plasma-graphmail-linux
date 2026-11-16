// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mailrunner.h"
#include "mailboxworker.h"
#include "searchquery.h"
#include "store.h"

#include <KLocalizedString>

#include <QCoreApplication>
#include <QDBusMetaType>
#include <QDateTime>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

QDBusArgument &operator<<(QDBusArgument &argument, const GraphMail::RunnerMatch &match)
{
    argument.beginStructure();
    argument << match.id << match.text << match.iconName << match.type << match.relevance << match.properties;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, GraphMail::RunnerMatch &match)
{
    argument.beginStructure();
    argument >> match.id >> match.text >> match.iconName >> match.type >> match.relevance >> match.properties;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const GraphMail::RunnerAction &action)
{
    argument.beginStructure();
    argument << action.id << action.text << action.iconName;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, GraphMail::RunnerAction &action)
{
    argument.beginStructure();
    argument >> action.id >> action.text >> action.iconName;
    argument.endStructure();
    return argument;
}

namespace GraphMail
{

namespace
{
// A few results per mailbox: KRunner shows the best ones of all its runners
constexpr int perMailbox = 6;
}

MailRunner::MailRunner(std::function<QStringList()> mailboxes, QObject *parent)
    : QObject(parent)
    , m_mailboxes(std::move(mailboxes))
{
}

MailRunner::~MailRunner() = default;

void MailRunner::registerTypes()
{
    qDBusRegisterMetaType<RunnerMatch>();
    qDBusRegisterMetaType<QList<RunnerMatch>>();
    qDBusRegisterMetaType<RunnerAction>();
    qDBusRegisterMetaType<QList<RunnerAction>>();
}

Store *MailRunner::store(const QString &key)
{
    // Read only, beside the worker's own connection; opened once, as the app does
    if (const auto it = m_stores.constFind(key); it != m_stores.cend()) {
        return it->get();
    }
    const QString path = cacheDirectory(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1)) + QStringLiteral("/cache.db");
    auto store = std::make_shared<Store>();
    QString error;
    if (!QFileInfo::exists(path) || !store->open(path, Store::Mode::ReadOnly, &error)) {
        return nullptr;
    }
    m_stores.insert(key, store);
    return store.get();
}

QList<RunnerAction> MailRunner::Actions()
{
    return {};
}

QList<RunnerMatch> MailRunner::Match(const QString &query)
{
    const QString text = query.trimmed();
    if (text.size() < 3) {
        return {};
    }
    SearchQuery search = SearchQuery::parse(text);
    search.lastIsPrefix = true;
    if (search.fts().isEmpty()) {
        return {};
    }
    QList<RunnerMatch> matches;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QStringList keys = m_mailboxes();
    for (const QString &key : keys) {
        Store *s = store(key);
        if (!s) {
            continue;
        }
        const QList<ListRow> rows = s->search(search, {}, perMailbox);
        for (const ListRow &row : rows) {
            RunnerMatch match;
            match.id = key + QLatin1Char('/') + QString::number(row.id);
            match.text = row.subject.isEmpty() ? i18nc("@info a message without a subject", "(no subject)") : row.subject;
            match.iconName = row.isRead ? QStringLiteral("mail-read") : QStringLiteral("mail-unread");
            // Newer mail first: from 0.7 for today down to 0.3 after a year
            const double days = double(now - row.received) / (24.0 * 3600 * 1000);
            match.relevance = 0.3 + 0.4 * std::max(0.0, 1.0 - days / 365.0);
            const QString sender = row.senderName.isEmpty() ? row.senderAddress : row.senderName;
            match.properties.insert(QStringLiteral("subtext"),
                                    i18nc("@info KRunner result, %1 the sender, %2 the date", "%1, %2", sender,
                                          QLocale().toString(QDateTime::fromMSecsSinceEpoch(row.received), QLocale::ShortFormat)));
            match.properties.insert(QStringLiteral("category"), i18nc("@info KRunner category", "Graph Mail"));
            matches.append(match);
        }
    }
    return matches;
}

void MailRunner::Run(const QString &matchId, const QString &actionId)
{
    Q_UNUSED(actionId)
    // The app next to the daemon (a build folder), else the installed one; an app that already
    // runs gets the message from the new instance (KDBusService)
    QString app = QCoreApplication::applicationDirPath() + QStringLiteral("/plasma-graphmail-linux");
    if (!QFileInfo(app).isExecutable()) {
        app = QStandardPaths::findExecutable(QStringLiteral("plasma-graphmail-linux"));
    }
    if (app.isEmpty()) {
        return;
    }
    QProcess process;
    process.setProgram(app);
    process.setArguments({QStringLiteral("--message"), matchId});
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (!m_token.isEmpty()) {
        // Wayland: the activation token lets the window come to the front
        environment.insert(QStringLiteral("XDG_ACTIVATION_TOKEN"), m_token);
        m_token.clear();
    }
    process.setProcessEnvironment(environment);
    process.startDetached();
}

void MailRunner::SetActivationToken(const QString &token)
{
    m_token = token;
}

void MailRunner::Teardown()
{
    // KRunner closed: let the caches go until the next search
    m_stores.clear();
}

}
