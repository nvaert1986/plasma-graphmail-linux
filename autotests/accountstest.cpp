// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "accounts.h"

#include <QFile>
#include <QStandardPaths>
#include <QTest>

using namespace GraphCore;

class AccountsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void init()
    {
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QLatin1Char('/') + Accounts::configFileName());
        for (const Account &account : Accounts::all()) {
            Accounts::remove(account.id);
        }
        Accounts::setDefaultClientId(QStringLiteral("default-client"));
        Accounts::setApplication({});
    }

    void addFindRemove()
    {
        QVERIFY(Accounts::all().isEmpty());
        Account account;
        account.tenantId = QStringLiteral("tenant-a");
        account.objectId = QStringLiteral("user-1");
        account.userPrincipalName = QStringLiteral("one@a.example");
        account.displayName = QStringLiteral("One");
        account.organization = QStringLiteral("Org A");
        const Account stored = Accounts::add(account);
        QCOMPARE(stored.id.size(), 8);
        QVERIFY(stored.isValid());

        const Account found = Accounts::find(stored.id);
        QCOMPARE(found.userPrincipalName, QStringLiteral("one@a.example"));
        QCOMPARE(found.organization, QStringLiteral("Org A"));
        QVERIFY(found.clientId.isEmpty());
        QCOMPARE(Accounts::findUser(QStringLiteral("TENANT-A"), QStringLiteral("user-1")).id, stored.id);
        QVERIFY(!Accounts::find(QStringLiteral("nope")).isValid());

        QVERIFY(Accounts::remove(stored.id));
        QVERIFY(!Accounts::remove(stored.id));
        QVERIFY(Accounts::all().isEmpty());
    }

    void sameUserIsUpdatedNotDuplicated()
    {
        Account account;
        account.tenantId = QStringLiteral("tenant-a");
        account.objectId = QStringLiteral("user-1");
        account.displayName = QStringLiteral("Old");
        const QString id = Accounts::add(account).id;
        account.displayName = QStringLiteral("New");
        QCOMPARE(Accounts::add(account).id, id);
        QCOMPARE(Accounts::all().size(), 1);
        QCOMPARE(Accounts::find(id).displayName, QStringLiteral("New"));

        // Another tenant is another account, also for the same object id
        account.tenantId = QStringLiteral("tenant-b");
        QVERIFY(Accounts::add(account).id != id);
        QCOMPARE(Accounts::all().size(), 2);
    }

    void registrationAndKeychainKey()
    {
        Account account;
        account.tenantId = QStringLiteral("t");
        account.objectId = QStringLiteral("o");
        QCOMPARE(account.effectiveClientId(), QStringLiteral("default-client"));
        QCOMPARE(account.keychainKey(), QStringLiteral("t:default-client:o"));
        account.clientId = QStringLiteral("own");
        QCOMPARE(account.keychainKey(), QStringLiteral("t:own:o"));

        const QString id = Accounts::add(account).id;
        QCOMPARE(Accounts::find(id).clientId, QStringLiteral("own"));
        account.id = id;
        account.clientId.clear();
        Accounts::add(account);
        QVERIFY(Accounts::find(id).clientId.isEmpty());
    }

    void clientIdPerApplication()
    {
        Account account;
        account.tenantId = QStringLiteral("t");
        account.objectId = QStringLiteral("o");
        // Written before apps kept their own: shared by all
        account.clientId = QStringLiteral("shared");
        const QString id = Accounts::add(account).id;

        // An app without its own key reads the shared one
        Accounts::setApplication(QStringLiteral("app-a"));
        QCOMPARE(Accounts::find(id).clientId, QStringLiteral("shared"));
        // app-a signs the user in with the suite's default registration: its own key says so
        account.id = id;
        account.clientId.clear();
        Accounts::add(account);
        QCOMPARE(Accounts::find(id).effectiveClientId(), QStringLiteral("default-client"));

        // app-b signs in with its own registration; app-a keeps the default
        Accounts::setApplication(QStringLiteral("app-b"));
        QCOMPARE(Accounts::find(id).clientId, QStringLiteral("shared"));
        account.clientId = QStringLiteral("b-client");
        Accounts::add(account);
        QCOMPARE(Accounts::find(id).keychainKey(), QStringLiteral("t:b-client:o"));
        Accounts::setApplication(QStringLiteral("app-a"));
        QCOMPARE(Accounts::find(id).keychainKey(), QStringLiteral("t:default-client:o"));
        // The shared key is left alone
        Accounts::setApplication({});
        QCOMPARE(Accounts::find(id).clientId, QStringLiteral("shared"));
    }
};

QTEST_GUILESS_MAIN(AccountsTest)

#include "accountstest.moc"
