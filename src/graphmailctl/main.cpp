// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Command-line access to the graphcore library: sign in, list accounts, raw Graph requests.
// Used to test the library against a real tenant until the daemon and the app exist.

#include "accounts.h"
#include "auth.h"
#include "graphclient.h"
#include "graphutil.h"
#include "mailboxworker.h"
#include "scopes.h"

#include <QCommandLineParser>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDateTime>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <cstdio>

using namespace GraphCore;

namespace
{
QString keychainService()
{
    return QStringLiteral("plasma-graphmail-linux");
}

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

QTextStream &err()
{
    static QTextStream stream(stderr);
    return stream;
}

int fail(const QString &message)
{
    err() << "error: " << message << Qt::endl;
    return 1;
}

// The account given with --account, or the only one there is
Account pickAccount(const QString &id, QString *error)
{
    if (!id.isEmpty()) {
        const Account account = Accounts::find(id);
        if (!account.isValid()) {
            *error = QStringLiteral("no account with id %1 (see: graphmailctl accounts)").arg(id);
        }
        return account;
    }
    const QList<Account> accounts = Accounts::all();
    if (accounts.size() != 1) {
        *error = accounts.isEmpty() ? QStringLiteral("not signed in (graphmailctl login)") : QStringLiteral("several accounts: pass --account <id>");
        return {};
    }
    return accounts.first();
}

int login(const QCommandLineParser &parser)
{
    QString clientId = parser.value(QStringLiteral("client"));
    if (clientId.isEmpty()) {
        clientId = Accounts::defaultClientId();
    } else if (Accounts::defaultClientId().isEmpty()) {
        Accounts::setDefaultClientId(clientId);
    }
    if (clientId.isEmpty()) {
        return fail(QStringLiteral("no app registration known yet: pass --client <application id> once"));
    }
    QString tenant = QStringLiteral("organizations");
    if (const QString given = parser.value(QStringLiteral("tenant")); !given.isEmpty()) {
        bool offline = false;
        tenant = Auth::resolveTenant(given, &offline);
        if (tenant.isEmpty()) {
            return fail(offline ? QStringLiteral("no connection") : QStringLiteral("no Microsoft 365 tenant found for %1").arg(given));
        }
    }

    Auth auth({keychainService(), clientId, tenant, {}, Scopes::suite()});
    QString error;
    bool ok = false;
    if (parser.isSet(QStringLiteral("code")) || !Auth::browserLoginAvailable()) {
        ok = auth.deviceCodeLogin(
            [](const QString &code, const QString &address) {
                out() << "Open " << address << " and enter the code " << code << Qt::endl;
            },
            nullptr,
            &error);
    } else {
        out() << "Opening the browser for sign-in..." << Qt::endl;
        ok = auth.interactiveLogin(&error, parser.value(QStringLiteral("hint")));
    }
    if (!ok) {
        return fail(error);
    }

    const SignedInUser user = auth.signedInUser();
    Account account = Auth::accountFor(user, clientId);
    if (const Account previous = Accounts::findUser(user.tenantId, user.objectId); previous.isValid()) {
        account.organization = previous.organization;
    }
    GraphClient client(&auth);
    const GraphResponse organization = client.get(QStringLiteral("/organization?$select=displayName"));
    if (organization.ok()) {
        account.organization = organization.json().value(QLatin1String("value")).toArray().first().toObject().value(QLatin1String("displayName")).toString();
    }
    account = Accounts::add(account);
    out() << "Signed in as " << account.userPrincipalName << " (" << account.organization << "), account id " << account.id << Qt::endl;
    return 0;
}

int listAccounts()
{
    const QList<Account> accounts = Accounts::all();
    if (accounts.isEmpty()) {
        out() << "No accounts." << Qt::endl;
    }
    for (const Account &account : accounts) {
        out() << account.id << "  " << account.userPrincipalName << "  " << account.organization << "  tenant " << account.tenantId
              << (account.clientId.isEmpty() ? QString() : QStringLiteral("  own client ") + account.clientId) << Qt::endl;
    }
    return 0;
}

int logout(const QCommandLineParser &parser)
{
    QString error;
    const Account account = pickAccount(parser.value(QStringLiteral("account")), &error);
    if (!account.isValid()) {
        return fail(error);
    }
    Auth(Auth::settingsFor(account, keychainService(), Scopes::suite())).logout();
    // The account entry is shared by the apps of the suite (Graph Sync syncs with it too): only
    // Graph Mail's own sign-in goes, unless asked to remove the account for all of them
    if (parser.isSet(QStringLiteral("remove"))) {
        Accounts::remove(account.id);
        out() << "Signed out and removed " << account.userPrincipalName << " (for all apps of the suite)" << Qt::endl;
    } else {
        out() << "Signed out " << account.userPrincipalName << "; the account stays for the other apps (--remove removes it)" << Qt::endl;
    }
    return 0;
}

int get(const QCommandLineParser &parser, const QString &path, const QByteArray &body = {}, const QByteArray &verb = "GET")
{
    QString error;
    const Account account = pickAccount(parser.value(QStringLiteral("account")), &error);
    if (!account.isValid()) {
        return fail(error);
    }
    Auth auth(Auth::settingsFor(account, keychainService(), Scopes::suite()));
    GraphClient client(&auth);
    client.setThrottleScope(account.id);
    if (parser.isSet(QStringLiteral("beta"))) {
        client.setBaseUrl(QStringLiteral("https://graph.microsoft.com/beta"));
    }
    QList<QByteArray> preferences{Prefer::immutableId()};
    const QStringList extra = parser.values(QStringLiteral("prefer"));
    for (const QString &value : extra) {
        preferences.append(value.toUtf8());
    }
    // patch|post <path> <json>, delete <path>: a change, for probes and test clean-up (otherwise read only)
    GraphRequestSpec spec;
    spec.verb = verb;
    spec.pathOrUrl = path;
    spec.headers = {Prefer::header(preferences)};
    if (!body.isEmpty()) {
        spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
        spec.body = body;
    }
    const GraphResponse response = client.send(spec);
    err() << "HTTP " << response.status << (response.ok() ? QString() : QStringLiteral(" ") + response.describe()) << Qt::endl;
    const QJsonDocument json = QJsonDocument::fromJson(response.body);
    if (json.isNull()) {
        fwrite(response.body.constData(), 1, response.body.size(), stdout);
    } else {
        out() << json.toJson(QJsonDocument::Indented);
    }
    return response.ok() ? 0 : 1;
}

// --- daemon (D-Bus) and cache commands ---------------------------------------------------------

QDBusInterface daemonInterface()
{
    return QDBusInterface(QStringLiteral("io.github.nvaert1986.GraphMail"), QStringLiteral("/GraphMail"), QStringLiteral("io.github.nvaert1986.GraphMail"));
}

// Calls a daemon method; prints the error and returns false when the daemon does not answer
bool callDaemon(const QString &method, const QVariantList &arguments, QVariant *result = nullptr)
{
    QDBusInterface daemon = daemonInterface();
    if (!daemon.isValid()) {
        fail(QStringLiteral("the daemon (graphmaild) is not running"));
        return false;
    }
    const QDBusMessage reply = daemon.callWithArgumentList(QDBus::Block, method, arguments);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        fail(reply.errorMessage());
        return false;
    }
    if (result) {
        *result = reply.arguments().value(0);
    }
    return true;
}

int status()
{
    QVariant result;
    if (!callDaemon(QStringLiteral("Status"), {}, &result)) {
        return 1;
    }
    const QJsonArray mailboxes = QJsonDocument::fromJson(result.toString().toUtf8()).array();
    for (const QJsonValue &value : mailboxes) {
        const QJsonObject m = value.toObject();
        out() << m.value(QLatin1String("mailbox")).toString() << ": " << m.value(QLatin1String("state")).toString() << ", "
              << m.value(QLatin1String("folders")).toInt() << " folders (" << m.value(QLatin1String("foldersInInitialSync")).toInt() << " in initial sync), "
              << m.value(QLatin1String("messages")).toInteger() << " messages, " << m.value(QLatin1String("events")).toInteger() << " events, "
              << m.value(QLatin1String("contacts")).toInteger() << " contacts, " << m.value(QLatin1String("tasks")).toInteger() << " tasks, "
              << m.value(QLatin1String("pendingOperations")).toInt() << " pending changes";
        if (const QString error = m.value(QLatin1String("lastError")).toString(); !error.isEmpty()) {
            out() << "\n    last error: " << error;
        }
        out() << Qt::endl;
    }
    return 0;
}

// Opens a mailbox's cache read-only, like the UI will
bool openCache(GraphMail::Store &store, const QString &mailbox)
{
    QString error;
    const QList<Account> accounts = Accounts::all();
    if (accounts.size() != 1) {
        fail(QStringLiteral("exactly one account expected"));
        return false;
    }
    if (!store.open(GraphMail::cacheDirectory(accounts.first().id, mailbox) + QStringLiteral("/cache.db"), GraphMail::Store::Mode::ReadOnly, &error)) {
        fail(error);
        return false;
    }
    return true;
}

int folders(const QString &mailbox)
{
    GraphMail::Store store;
    if (!openCache(store, mailbox)) {
        return 1;
    }
    const QHash<qint64, int> unread = store.unreadCounts();
    const QList<GraphMail::FolderRow> rows = store.folders();
    for (const GraphMail::FolderRow &f : rows) {
        out() << qSetFieldWidth(6) << f.id << qSetFieldWidth(0) << "  " << qSetFieldWidth(7) << store.messageCount(f.id) << qSetFieldWidth(0)
              << " cached, " << unread.value(f.id) << " unread (Graph: " << f.total << "/" << f.unread << ")  " << f.name
              << (f.wellKnown.isEmpty() ? QString() : QStringLiteral("  [") + f.wellKnown + QLatin1Char(']')) << Qt::endl;
    }
    return 0;
}

// The message list as the UI shows it: sender, subject with the date, one preview line
int list(const QString &mailbox, qint64 folder, int limit)
{
    GraphMail::Store store;
    if (!openCache(store, mailbox)) {
        return 1;
    }
    const QList<GraphMail::ListRow> rows = store.listPage(folder, std::nullopt, limit);
    for (const GraphMail::ListRow &r : rows) {
        const QString date = QDateTime::fromMSecsSinceEpoch(r.received).toString(QStringLiteral("ddd dd-MM-yyyy HH:mm"));
        out() << "[" << r.id << "] " << (r.isRead ? "  " : "* ") << (r.senderName.isEmpty() ? r.senderAddress : r.senderName) << Qt::endl
              << "    " << r.subject.left(70) << "  " << date << Qt::endl
              << "    " << r.preview.simplified().left(90) << Qt::endl;
    }
    return 0;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("graphmailctl"));
    // Accounts are shared by the suite; the app registration per account is this app's own
    Accounts::setApplication(keychainService());
    QCoreApplication::setApplicationVersion(QStringLiteral(GRAPHMAIL_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Test tool for the plasma-graphmail-linux Graph library"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("command"),
                                 QStringLiteral("login | accounts | logout | get <path> | patch|post <path> <json> | delete <path> | status | sync | folders <mailbox> | list <mailbox> <folder> | "
                                                "mark-read <mailbox> <message> [--unread] | resync <mailbox> <folder> | add-shared <address> | check-mailbox <address>"));
    parser.addOptions({
        {QStringLiteral("client"), QStringLiteral("App registration (client) ID; remembered as the default"), QStringLiteral("id")},
        {QStringLiteral("tenant"), QStringLiteral("Tenant ID, domain or work e-mail address (login; default: any work account)"), QStringLiteral("tenant")},
        {QStringLiteral("hint"), QStringLiteral("Account to preselect in the browser (login)"), QStringLiteral("address")},
        {QStringLiteral("code"), QStringLiteral("Sign in with a code instead of the browser (login)")},
        {QStringLiteral("account"), QStringLiteral("Account id (see accounts)"), QStringLiteral("id")},
        {QStringLiteral("beta"), QStringLiteral("Use the Graph beta endpoint (get)")},
        {QStringLiteral("prefer"), QStringLiteral("Extra Prefer value, repeatable (get; immutable ids are always asked for)"), QStringLiteral("value")},
        {QStringLiteral("unread"), QStringLiteral("Mark as unread instead (mark-read)")},
        {QStringLiteral("remove"), QStringLiteral("Also remove the account for all apps of the suite (logout)")},
        {QStringLiteral("limit"), QStringLiteral("Rows to show (list, default 20)"), QStringLiteral("rows"), QStringLiteral("20")},
    });
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    const QString command = args.value(0);
    if (command == QLatin1String("login")) {
        return login(parser);
    }
    if (command == QLatin1String("accounts")) {
        return listAccounts();
    }
    if (command == QLatin1String("logout")) {
        return logout(parser);
    }
    if (command == QLatin1String("get") && args.size() == 2) {
        return get(parser, args.at(1));
    }
    if ((command == QLatin1String("patch") || command == QLatin1String("post")) && args.size() == 3) {
        return get(parser, args.at(1), args.at(2).toUtf8(), command.toUpper().toLatin1());
    }
    if (command == QLatin1String("delete") && args.size() == 2) {
        return get(parser, args.at(1), {}, QByteArrayLiteral("DELETE"));
    }
    if (command == QLatin1String("status")) {
        return status();
    }
    if (command == QLatin1String("sync")) {
        return callDaemon(QStringLiteral("SyncNow"), {}) ? 0 : 1;
    }
    if (command == QLatin1String("folders") && args.size() == 2) {
        return folders(args.at(1));
    }
    if (command == QLatin1String("list") && args.size() == 3) {
        return list(args.at(1), args.at(2).toLongLong(), parser.value(QStringLiteral("limit")).toInt());
    }
    if (command == QLatin1String("mark-read") && args.size() == 3) {
        return callDaemon(QStringLiteral("MarkRead"), {QString(), args.at(1), args.at(2).toLongLong(), !parser.isSet(QStringLiteral("unread"))}) ? 0 : 1;
    }
    if (command == QLatin1String("resync") && args.size() == 3) {
        // With more than one account signed in the daemon needs --account
        return callDaemon(QStringLiteral("ResyncFolder"), {parser.value(QStringLiteral("account")), args.at(1), args.at(2).toLongLong()}) ? 0 : 1;
    }
    if ((command == QLatin1String("add-shared") || command == QLatin1String("check-mailbox")) && args.size() == 2) {
        // Both answer the access check as JSON (docs/PHASE7.md); "problem" is empty when it worked
        QVariant result;
        // With more than one account signed in the daemon needs --account
        if (!callDaemon(command == QLatin1String("add-shared") ? QStringLiteral("AddSharedMailbox") : QStringLiteral("CheckMailbox"),
                        {parser.value(QStringLiteral("account")), args.at(1)},
                        &result)) {
            return 1;
        }
        out() << result.toString() << Qt::endl;
        const QString text = result.toString();
        return text.startsWith(QLatin1String("error")) || !QJsonDocument::fromJson(text.toUtf8()).object().value(QLatin1String("problem")).toString().isEmpty() ? 1 : 0;
    }
    parser.showHelp(1);
}
