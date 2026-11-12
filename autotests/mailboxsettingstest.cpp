// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mailboxsettings.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>
#include <QTimeZone>

using namespace GraphMail;

class MailboxSettingsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // What Graph answers for a scheduled reply: times in the mailbox's Windows time zone
    void scheduledFromGraph()
    {
        const QJsonObject setting = QJsonDocument::fromJson(R"({
            "status": "scheduled", "externalAudience": "contactsOnly",
            "scheduledStartDateTime": {"dateTime": "2026-12-24T17:00:00.0000000", "timeZone": "W. Europe Standard Time"},
            "scheduledEndDateTime": {"dateTime": "2027-01-04T08:00:00.0000000", "timeZone": "W. Europe Standard Time"},
            "internalReplyMessage": "<p>Away</p>", "externalReplyMessage": ""})")
                                        .object();
        const AutomaticReplies r = AutomaticReplies::fromJson(setting);
        QCOMPARE(r.status, AutomaticReplies::Status::Scheduled);
        QCOMPARE(r.external, AutomaticReplies::External::ContactsOnly);
        QCOMPARE(r.start, QDateTime(QDate(2026, 12, 24), QTime(16, 0), QTimeZone::UTC).toMSecsSinceEpoch());
        QCOMPARE(r.end, QDateTime(QDate(2027, 1, 4), QTime(7, 0), QTimeZone::UTC).toMSecsSinceEpoch());
        QCOMPARE(r.internalMessage, QStringLiteral("<p>Away</p>"));

        // Active only inside the window
        QVERIFY(!r.activeAt(r.start - 1));
        QVERIFY(r.activeAt(r.start));
        QVERIFY(r.activeAt(r.end - 1));
        QVERIFY(!r.activeAt(r.end));
    }

    void alwaysAndDisabled()
    {
        AutomaticReplies r = AutomaticReplies::fromJson(QJsonObject{{QStringLiteral("status"), QStringLiteral("alwaysEnabled")}});
        QVERIFY(r.activeAt(0));
        QCOMPARE(r.external, AutomaticReplies::External::All);
        r = AutomaticReplies::fromJson(QJsonObject{{QStringLiteral("status"), QStringLiteral("disabled")}, {QStringLiteral("externalAudience"), QStringLiteral("none")}});
        QVERIFY(!r.activeAt(0));
        QCOMPARE(r.external, AutomaticReplies::External::None);
    }

    // The user's own settings: Monday-Friday 08-17 in Central Europe Standard Time
    void workingHours()
    {
        const QJsonObject graph = QJsonDocument::fromJson(R"({"daysOfWeek": ["monday", "tuesday", "wednesday", "thursday", "friday"],
            "startTime": "08:00:00.0000000", "endTime": "17:00:00.0000000", "timeZone": {"name": "Central Europe Standard Time"}})")
                                      .object();
        const WorkingHours w = WorkingHours::fromJson(graph);
        QCOMPARE(w.days, QList<int>({1, 2, 3, 4, 5}));
        QCOMPARE(w.start, QTime(8, 0));
        QCOMPARE(w.end, QTime(17, 0));
        QCOMPARE(w.toJson(), graph);
        // Saturday is not a working day
        QCOMPARE(w.localHours(QDate(2026, 11, 14)), qMakePair(0.0, 0.0));
    }

    // Hours in another zone than the computer's are shown in the computer's time
    void workingHoursInAnotherZone()
    {
        qputenv("TZ", "Europe/Amsterdam");
        WorkingHours w;
        w.days = {1, 2, 3, 4, 5, 6};
        w.start = QTime(8, 30);
        w.end = QTime(17, 0);
        w.timeZone = QStringLiteral("W. Europe Standard Time");
        QCOMPARE(w.localHours(QDate(2026, 11, 16)), qMakePair(8.5, 17.0));
        // New York 08:30-17:00 is 14:30-23:00 in Amsterdam in November
        w.timeZone = QStringLiteral("Eastern Standard Time");
        QCOMPARE(w.localHours(QDate(2026, 11, 16)), qMakePair(14.5, 23.0));
        // Tokyo 08:30-17:00 is 00:30-09:00 in Amsterdam
        w.timeZone = QStringLiteral("Tokyo Standard Time");
        QCOMPARE(w.localHours(QDate(2026, 11, 16)), qMakePair(0.5, 9.0));
        // An empty set of days stays empty, not Graph Mail's default
        QVERIFY(WorkingHours::fromJson(QJsonObject{{QStringLiteral("daysOfWeek"), QJsonArray()}, {QStringLiteral("startTime"), QStringLiteral("09:00:00")}}).days.isEmpty());
    }

    // What is sent back: UTC times, and none when the replies are not scheduled
    void toGraph()
    {
        AutomaticReplies r;
        r.status = AutomaticReplies::Status::Scheduled;
        r.external = AutomaticReplies::External::None;
        r.start = QDateTime(QDate(2030, 1, 1), QTime(9, 30), QTimeZone::UTC).toMSecsSinceEpoch();
        r.end = r.start + 3600 * 1000;
        r.internalMessage = QStringLiteral("<p>Test</p>");
        QJsonObject o = r.toJson();
        QCOMPARE(o.value(QLatin1String("status")).toString(), QStringLiteral("scheduled"));
        QCOMPARE(o.value(QLatin1String("externalAudience")).toString(), QStringLiteral("none"));
        QCOMPARE(o.value(QLatin1String("scheduledStartDateTime")).toObject().value(QLatin1String("dateTime")).toString(), QStringLiteral("2030-01-01T09:30:00"));
        QCOMPARE(o.value(QLatin1String("scheduledEndDateTime")).toObject().value(QLatin1String("timeZone")).toString(), QStringLiteral("UTC"));
        QCOMPARE(AutomaticReplies::fromJson(o).end, r.end);

        r.status = AutomaticReplies::Status::Disabled;
        o = r.toJson();
        QCOMPARE(o.value(QLatin1String("status")).toString(), QStringLiteral("disabled"));
        QVERIFY(!o.contains(QLatin1String("scheduledStartDateTime")));
    }
};

QTEST_GUILESS_MAIN(MailboxSettingsTest)

#include "mailboxsettingstest.moc"
