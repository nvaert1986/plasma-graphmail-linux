// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "auth.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTest>
#include <QUrlQuery>

using namespace GraphCore;

namespace
{
QString makeJwt(const QJsonObject &claims)
{
    const QByteArray header = QByteArray(R"({"alg":"none"})").toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const QByteArray payload = QJsonDocument(claims).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return QString::fromLatin1(header + '.' + payload + ".sig");
}
}

class AuthTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void isGuid()
    {
        QVERIFY(Auth::isGuid(QStringLiteral("0a1b2c3d-4e5f-6789-abcd-ef0123456789")));
        QVERIFY(Auth::isGuid(QStringLiteral("0A1B2C3D-4E5F-6789-ABCD-EF0123456789")));
        QVERIFY(!Auth::isGuid(QStringLiteral("0a1b2c3d-4e5f-6789-abcd-ef012345678")));
        QVERIFY(!Auth::isGuid(QStringLiteral(" 0a1b2c3d-4e5f-6789-abcd-ef0123456789")));
        QVERIFY(!Auth::isGuid(QStringLiteral("contoso.onmicrosoft.com")));
    }

    void parseSetupText_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("clientId");
        QTest::addColumn<QString>("tenant");
        const QString client = QStringLiteral("0a1b2c3d-4e5f-6789-abcd-ef0123456789");
        const QString tenant = QStringLiteral("11111111-2222-3333-4444-555555555555");
        QTest::newRow("rc group") << QStringLiteral("[Auth]\nClientId=%1\nTenantId=%2\n").arg(client, tenant) << client << tenant;
        QTest::newRow("one line") << QStringLiteral("ClientId=%1 TenantId=%2").arg(client, tenant) << client << tenant;
        QTest::newRow("colons, domain") << QStringLiteral("clientid: %1 tenantid: contoso.com").arg(client) << client << QStringLiteral("contoso.com");
        QTest::newRow("client only") << QStringLiteral("ClientId = %1").arg(client) << client << QString();
        QTest::newRow("bare id is not setup text") << client << QString() << QString();
    }
    void parseSetupText()
    {
        QFETCH(QString, text);
        QFETCH(QString, clientId);
        QFETCH(QString, tenant);
        const Auth::SetupText setup = Auth::parseSetupText(text);
        QCOMPARE(setup.clientId, clientId);
        QCOMPARE(setup.tenant, tenant);
    }

    void resolveTenantWithoutNetwork()
    {
        bool offline = true;
        // A tenant ID needs no lookup; rubbish is refused before any request
        QCOMPARE(Auth::resolveTenant(QStringLiteral(" 11111111-2222-3333-4444-55555555555A "), &offline),
                 QStringLiteral("11111111-2222-3333-4444-55555555555a"));
        QVERIFY(!offline);
        QCOMPARE(Auth::resolveTenant(QStringLiteral("not a domain"), &offline), QString());
        QCOMPARE(Auth::resolveTenant(QStringLiteral("someone@"), &offline), QString());
        QVERIFY(!offline);
    }

    void parseIdToken()
    {
        const QString token = makeJwt({{QStringLiteral("oid"), QStringLiteral("user-1")},
                                       {QStringLiteral("tid"), QStringLiteral("tenant-1")},
                                       {QStringLiteral("preferred_username"), QStringLiteral("someone@example.com")},
                                       {QStringLiteral("name"), QStringLiteral("Some One")}});
        const SignedInUser user = Auth::parseIdToken(token);
        QCOMPARE(user.objectId, QStringLiteral("user-1"));
        QCOMPARE(user.tenantId, QStringLiteral("tenant-1"));
        QCOMPARE(user.userPrincipalName, QStringLiteral("someone@example.com"));
        QCOMPARE(user.displayName, QStringLiteral("Some One"));
        QVERIFY(Auth::parseIdToken(QStringLiteral("garbage")).objectId.isEmpty());
    }

    void adminConsentUrl()
    {
        const QUrl url = Auth::adminConsentUrl(QStringLiteral("tenant-1"), QStringLiteral("client-1"));
        QCOMPARE(url.host(), QStringLiteral("login.microsoftonline.com"));
        QCOMPARE(url.path(), QStringLiteral("/tenant-1/adminconsent"));
        QCOMPARE(QUrlQuery(url).queryItemValue(QStringLiteral("client_id")), QStringLiteral("client-1"));
        QCOMPARE(Auth::adminConsentUrl({}, QStringLiteral("c")).path(), QStringLiteral("/organizations/adminconsent"));
    }

    void settingsForAccount()
    {
        Accounts::setDefaultClientId(QStringLiteral("default-client"));
        Account account;
        account.tenantId = QStringLiteral("t");
        account.objectId = QStringLiteral("o");
        AuthSettings settings = Auth::settingsFor(account, QStringLiteral("svc"), {QStringLiteral("Mail.Read")});
        QCOMPARE(settings.clientId, QStringLiteral("default-client"));
        QCOMPARE(settings.tenant, QStringLiteral("t"));
        QCOMPARE(settings.objectId, QStringLiteral("o"));
        QCOMPARE(settings.keychainService, QStringLiteral("svc"));

        account.clientId = QStringLiteral("own-client");
        QCOMPARE(Auth::settingsFor(account, QStringLiteral("svc"), {}).clientId, QStringLiteral("own-client"));

        // Only a registration other than the default is remembered on the account
        const SignedInUser user{QStringLiteral("o"), QStringLiteral("u@example.com"), QStringLiteral("U"), QStringLiteral("t")};
        QVERIFY(Auth::accountFor(user, QStringLiteral("default-client")).clientId.isEmpty());
        QCOMPARE(Auth::accountFor(user, QStringLiteral("other")).clientId, QStringLiteral("other"));
    }

    void notSignedInWithoutObjectId()
    {
        // Before a sign-in there is no user, so nothing is read from the keychain
        Auth auth({QStringLiteral("graphcore-test"), QStringLiteral("c"), QStringLiteral("organizations"), {}, {}});
        QVERIFY(auth.isConfigured());
        QVERIFY(auth.cachedAccessToken().isEmpty());
        QString error;
        QVERIFY(auth.accessToken(&error).isEmpty());
        QCOMPARE(error, QStringLiteral("not signed in"));
    }
};

QTEST_GUILESS_MAIN(AuthTest)

#include "authtest.moc"
