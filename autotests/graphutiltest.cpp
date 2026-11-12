// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "graphutil.h"
#include "scopes.h"
#include "throttle.h"

#include <QTest>

using namespace GraphCore;

class GraphUtilTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void throttleKey_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<QString>("key");
        const QString host = QStringLiteral("https://graph.microsoft.com");
        QTest::newRow("own messages") << host + QStringLiteral("/v1.0/me/messages?$top=1") << QStringLiteral("acc|me");
        QTest::newRow("folder with key syntax") << host + QStringLiteral("/v1.0/me/mailFolders('inbox')/messages/delta") << QStringLiteral("acc|me");
        QTest::newRow("shared mailbox, case folded") << host + QStringLiteral("/v1.0/users/Shared@Example.com/mailFolders") << QStringLiteral("acc|shared@example.com");
        QTest::newRow("calendar view") << host + QStringLiteral("/beta/me/calendarView/delta") << QStringLiteral("acc|me");
        QTest::newRow("to do") << host + QStringLiteral("/v1.0/me/todo/lists") << QStringLiteral("acc|me");
        QTest::newRow("own OneDrive is not a mailbox") << host + QStringLiteral("/v1.0/me/drive/root") << QStringLiteral("acc");
        QTest::newRow("profile is not a mailbox") << host + QStringLiteral("/v1.0/me") << QStringLiteral("acc");
        QTest::newRow("drives") << host + QStringLiteral("/v1.0/drives/b!x/items/y") << QStringLiteral("acc");
        QTest::newRow("batch") << host + QStringLiteral("/v1.0/$batch") << QStringLiteral("acc");
        QTest::newRow("other host") << QStringLiteral("https://contoso.sharepoint.com/me/messages") << QStringLiteral("acc");
    }
    void throttleKey()
    {
        QFETCH(QString, url);
        QFETCH(QString, key);
        QCOMPARE(throttleKeyFor(QStringLiteral("acc"), QUrl(url), QStringLiteral("graph.microsoft.com")), key);
    }

    void prefer()
    {
        QCOMPARE(Prefer::header({Prefer::immutableId(), Prefer::textBody()}).second,
                 QByteArray("IdType=\"ImmutableId\", outlook.body-content-type=\"text\""));
        QCOMPARE(Prefer::timeZone(QTimeZone("Europe/Amsterdam")), QByteArray("outlook.timezone=\"W. Europe Standard Time\""));
        QCOMPARE(Prefer::timeZone(QTimeZone::UTC), QByteArray("outlook.timezone=\"UTC\""));
        QCOMPARE(Prefer::maxPageSize(50), QByteArray("odata.maxpagesize=50"));
    }

    void mailboxRoot()
    {
        QCOMPARE(GraphCore::mailboxRoot(), QStringLiteral("/me"));
        QCOMPARE(GraphCore::mailboxRoot(QStringLiteral("info@example.com")), QStringLiteral("/users/info@example.com"));
        QCOMPARE(GraphCore::mailboxRoot(QStringLiteral("a b@example.com")), QStringLiteral("/users/a%20b@example.com"));
    }

    void throttleState()
    {
        Throttle::resetForTests();
        const QString key = QStringLiteral("acc|me");
        QCOMPARE(Throttle::waitMs(key), 0);
        Throttle::note(key, 5000);
        QVERIFY(Throttle::waitMs(key) > 4000);
        // A shorter pause never shortens a longer one
        Throttle::note(key, 10);
        QVERIFY(Throttle::waitMs(key) > 4000);

        QVERIFY(Throttle::tryAcquire(key, 2));
        QVERIFY(Throttle::tryAcquire(key, 2));
        QVERIFY(!Throttle::tryAcquire(key, 2));
        Throttle::release(key);
        QCOMPARE(Throttle::running(key), 1);
        QVERIFY(Throttle::tryAcquire(key, 2));
        Throttle::resetForTests();
        QCOMPARE(Throttle::running(key), 0);

        // All or nothing for several slots at once
        QVERIFY(Throttle::tryAcquire(key, 4));
        QVERIFY(!Throttle::tryAcquire(key, 4, 4));
        QCOMPARE(Throttle::running(key), 1);
        Throttle::release(key);
        QVERIFY(Throttle::tryAcquire(key, 4, 4));
        QVERIFY(!Throttle::tryAcquire(key, 4));
        Throttle::release(key, 4);
        QCOMPARE(Throttle::running(key), 0);
    }

    void suiteScopes()
    {
        QStringList suite = Scopes::suite();
        QCOMPARE(suite.removeDuplicates(), 0);
        for (const QString &scope : {QStringLiteral("offline_access"), QStringLiteral("Files.ReadWrite.All"), QStringLiteral("Mail.Send.Shared"),
                                     QStringLiteral("Calendars.ReadWrite.Shared"), QStringLiteral("Tasks.ReadWrite"),
                                     QStringLiteral("Tasks.ReadWrite.Shared")}) {
            QVERIFY2(suite.contains(scope), qPrintable(scope));
        }
    }
};

QTEST_GUILESS_MAIN(GraphUtilTest)

#include "graphutiltest.moc"
