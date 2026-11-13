// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pst/mapiitems.h"
#include "pst/pstwriter.h"

#include <QTemporaryDir>

#include <QJsonArray>
#include <QTest>
#include <QtEndian>

using namespace GraphMail;

namespace
{
const QUuid psetidAddress(0x00062004, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
const QUuid psetidAppointment(0x00062002, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
const QUuid psetidCommon(0x00062008, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
const QUuid psetidTask(0x00062003, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);

QByteArray le32(quint32 v)
{
    QByteArray b(4, 0);
    qToLittleEndian(v, b.data());
    return b;
}
Pst::PropValue str(const QString &s)
{
    return {Pst::PtypString, QByteArray(reinterpret_cast<const char *>(s.utf16()), s.size() * 2)};
}
Pst::PropValue int32(quint32 v)
{
    return {Pst::PtypInteger32, le32(v)};
}
Pst::PropValue boolean(bool v)
{
    return {Pst::PtypBoolean, QByteArray(1, v ? 1 : 0)};
}
Pst::PropValue time(const QDateTime &t)
{
    QByteArray b(8, 0);
    qToLittleEndian(quint64((t.toMSecsSinceEpoch() + qint64(11644473600) * 1000) * 10000), b.data());
    return {Pst::PtypTime, b};
}
QDateTime utc(int y, int mo, int d, int h = 0, int mi = 0)
{
    return QDateTime(QDate(y, mo, d), QTime(h, mi), QTimeZone::UTC);
}

// Named properties as a file maps them: each (set, LID) gets an ID from 0x8000
struct Names {
    QHash<quint16, Pst::NamedProperty> map;
    quint16 id(const QUuid &set, quint32 lid)
    {
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            if (it->guid == set && it->id == lid) {
                return it.key();
            }
        }
        Pst::NamedProperty n;
        n.guid = set;
        n.id = lid;
        const quint16 id = quint16(0x8000 + map.size());
        map.insert(id, n);
        return id;
    }
};

// A TZDEFINITION with just the key name (enough for the name)
QByteArray tzDefinition(const QString &name)
{
    QByteArray d(8, 0);
    d[0] = 2;
    d[1] = 1;
    qToLittleEndian(quint16(name.size()), d.data() + 6);
    d += QByteArray(reinterpret_cast<const char *>(name.utf16()), name.size() * 2);
    return d;
}

// [MS-OXOCAL] 4.1.1: the spec's recurrence BLOB examples
const QByteArray weeklySample = QByteArray::fromHex("043004300B2001000000C0210000010000000000000032000000222000000C0000000000000000000000000000008"
                                                    "020BC0C20ADBC0C0630000009300000580200007602000000000000000000000000");
const QByteArray dailySample = QByteArray::fromHex("043004300A2000000000A0050000E010000000000000212000000A0000000000000002000000A0C1DC0C80D2DC0C0"
                                                   "0000000207EDC0C0016DD0C0630000009300000E0010000FE01000000000000000000000000");
const QByteArray monthNthSample = QByteArray::fromHex("043004300C200300000060AE000003000000000000004100000003000000222000000A00000000000000020000006"
                                                      "028C50C4028C70C02000000002EC50C4028C70C8028C30C6027D50C063000000930000048030000FC030000");
}

class MapiItemsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void kinds()
    {
        QCOMPARE(MapiItems::kindOf(QStringLiteral("IPM.Contact")), MapiItems::Kind::Contact);
        QCOMPARE(MapiItems::kindOf(QStringLiteral("IPM.Appointment")), MapiItems::Kind::Event);
        QCOMPARE(MapiItems::kindOf(QStringLiteral("IPM.Task")), MapiItems::Kind::Task);
        QCOMPARE(MapiItems::kindOf(QStringLiteral("IPM.Note")), MapiItems::Kind::Mail);
        QCOMPARE(MapiItems::kindOf(QStringLiteral("IPM.DistList")), MapiItems::Kind::Other);
        QCOMPARE(MapiItems::kindOf(QStringLiteral("IPM.StickyNote")), MapiItems::Kind::Other);
        QCOMPARE(MapiItems::kindOf(QStringLiteral("IPM.TaskRequest")), MapiItems::Kind::Other);
        QCOMPARE(MapiItems::folderKind(QStringLiteral("IPF.Contact")), MapiItems::Kind::Contact);
        QCOMPARE(MapiItems::folderKind(QStringLiteral("IPF.Appointment")), MapiItems::Kind::Event);
        QCOMPARE(MapiItems::folderKind(QStringLiteral("IPF.Task")), MapiItems::Kind::Task);
        QCOMPARE(MapiItems::folderKind(QString()), MapiItems::Kind::Mail);
        QCOMPARE(MapiItems::folderKind(QStringLiteral("IPF.StickyNote")), MapiItems::Kind::Other);
    }

    // The spec's examples, through to Graph's recurrence
    void recurrenceSamples()
    {
        QStringList notes;
        std::optional<QJsonObject> r = MapiItems::recurrence(weeklySample, QStringLiteral("W. Europe Standard Time"), &notes);
        QVERIFY(r);
        QJsonObject p = r->value(QLatin1String("pattern")).toObject();
        QJsonObject range = r->value(QLatin1String("range")).toObject();
        QCOMPARE(p.value(QLatin1String("type")).toString(), QStringLiteral("weekly"));
        QCOMPARE(p.value(QLatin1String("interval")).toInt(), 1);
        QCOMPARE(p.value(QLatin1String("daysOfWeek")).toArray(), (QJsonArray{QStringLiteral("monday"), QStringLiteral("thursday"), QStringLiteral("friday")}));
        QCOMPARE(p.value(QLatin1String("firstDayOfWeek")).toString(), QStringLiteral("sunday"));
        QCOMPARE(range.value(QLatin1String("type")).toString(), QStringLiteral("numbered"));
        QCOMPARE(range.value(QLatin1String("numberOfOccurrences")).toInt(), 12);
        QCOMPARE(range.value(QLatin1String("startDate")).toString(), QStringLiteral("2007-03-26"));
        QCOMPARE(range.value(QLatin1String("recurrenceTimeZone")).toString(), QStringLiteral("W. Europe Standard Time"));
        QVERIFY(notes.isEmpty());

        // Every three days until 4 May 2011, two occurrences deleted
        r = MapiItems::recurrence(dailySample, QString(), &notes);
        QVERIFY(r);
        p = r->value(QLatin1String("pattern")).toObject();
        range = r->value(QLatin1String("range")).toObject();
        QCOMPARE(p.value(QLatin1String("type")).toString(), QStringLiteral("daily"));
        QCOMPARE(p.value(QLatin1String("interval")).toInt(), 3);
        QCOMPARE(range.value(QLatin1String("type")).toString(), QStringLiteral("endDate"));
        QCOMPARE(range.value(QLatin1String("startDate")).toString(), QStringLiteral("2011-04-07"));
        QCOMPARE(range.value(QLatin1String("endDate")).toString(), QStringLiteral("2011-05-04"));
        QCOMPARE(notes.size(), 1);
        QVERIFY(notes.first().startsWith(QLatin1String("2 changed or cancelled")));

        // The third weekend every three months, ten times
        notes.clear();
        r = MapiItems::recurrence(monthNthSample, QString(), &notes);
        QVERIFY(r);
        p = r->value(QLatin1String("pattern")).toObject();
        range = r->value(QLatin1String("range")).toObject();
        QCOMPARE(p.value(QLatin1String("type")).toString(), QStringLiteral("relativeMonthly"));
        QCOMPARE(p.value(QLatin1String("interval")).toInt(), 3);
        QCOMPARE(p.value(QLatin1String("index")).toString(), QStringLiteral("third"));
        QCOMPARE(p.value(QLatin1String("daysOfWeek")).toArray(), (QJsonArray{QStringLiteral("sunday"), QStringLiteral("saturday")}));
        QCOMPARE(range.value(QLatin1String("numberOfOccurrences")).toInt(), 10);
        QCOMPARE(range.value(QLatin1String("startDate")).toString(), QStringLiteral("2008-02-09"));

        // Damage and other calendars are refused with a note
        notes.clear();
        QVERIFY(!MapiItems::recurrence(weeklySample.left(20), QString(), &notes));
        QCOMPARE(notes.size(), 1);
        QByteArray hijri = weeklySample;
        qToLittleEndian(quint16(0x000A), hijri.data() + 6);
        QVERIFY(!MapiItems::recurrence(hijri, QString(), &notes));
    }

    void timeZones()
    {
        QCOMPARE(MapiItems::timeZoneName(tzDefinition(QStringLiteral("W. Europe Standard Time"))), QStringLiteral("W. Europe Standard Time"));
        QCOMPARE(MapiItems::timeZoneName(QByteArray(5, 0)), QString());
        QByteArray wrong = tzDefinition(QStringLiteral("X"));
        qToLittleEndian(quint16(500), wrong.data() + 6);
        QCOMPARE(MapiItems::timeZoneName(wrong), QString());
    }

    void contact()
    {
        Names names;
        Pst::Message m;
        m.properties.insert(0x001A, str(QStringLiteral("IPM.Contact")));
        m.properties.insert(0x3001, str(QStringLiteral("Alice Example")));
        m.properties.insert(0x3A06, str(QStringLiteral("Alice")));
        m.properties.insert(0x3A11, str(QStringLiteral("Example")));
        m.properties.insert(0x3A16, str(QStringLiteral("Acme")));
        m.properties.insert(0x3A17, str(QStringLiteral("Engineer")));
        m.properties.insert(0x3A08, str(QStringLiteral("+31 20 000 0000")));
        m.properties.insert(0x3A1C, str(QStringLiteral("+31 6 0000 0000")));
        m.properties.insert(0x3A5D, str(QStringLiteral("Example Street 1")));
        m.properties.insert(0x3A59, str(QStringLiteral("Exampletown")));
        m.properties.insert(0x3A42, time(utc(1990, 5, 14, 22)));
        m.properties.insert(0x1000, str(QStringLiteral("Example notes")));
        m.properties.insert(names.id(psetidAddress, 0x8083), str(QStringLiteral("alice@example.com")));
        m.properties.insert(names.id(psetidAddress, 0x8082), str(QStringLiteral("SMTP")));
        m.properties.insert(names.id(psetidAddress, 0x8080), str(QStringLiteral("Alice Example (alice@example.com)")));
        m.properties.insert(names.id(psetidAddress, 0x8093), str(QStringLiteral("/o=Example/cn=Alice")));
        m.properties.insert(names.id(psetidAddress, 0x8092), str(QStringLiteral("EX")));
        m.properties.insert(names.id(psetidAddress, 0x8045), str(QStringLiteral("Business Road 2")));
        m.properties.insert(names.id(psetidAddress, 0x8046), str(QStringLiteral("Worktown")));
        m.properties.insert(names.id(psetidAddress, 0x8005), str(QStringLiteral("Example, Alice")));

        const MapiItems::Item item = MapiItems::contact(m, names.map);
        const QJsonObject &j = item.json;
        QCOMPARE(j.value(QLatin1String("givenName")).toString(), QStringLiteral("Alice"));
        QCOMPARE(j.value(QLatin1String("surname")).toString(), QStringLiteral("Example"));
        QCOMPARE(j.value(QLatin1String("companyName")).toString(), QStringLiteral("Acme"));
        QCOMPARE(j.value(QLatin1String("jobTitle")).toString(), QStringLiteral("Engineer"));
        QCOMPARE(j.value(QLatin1String("fileAs")).toString(), QStringLiteral("Example, Alice"));
        QCOMPARE(j.value(QLatin1String("businessPhones")).toArray(), QJsonArray{QStringLiteral("+31 20 000 0000")});
        QCOMPARE(j.value(QLatin1String("mobilePhone")).toString(), QStringLiteral("+31 6 0000 0000"));
        // Stored as local midnight (22:00 UTC the day before): the date is the 15th
        QCOMPARE(j.value(QLatin1String("birthday")).toString(), QStringLiteral("1990-05-15T00:00:00Z"));
        QCOMPARE(j.value(QLatin1String("personalNotes")).toString(), QStringLiteral("Example notes"));
        const QJsonArray emails = j.value(QLatin1String("emailAddresses")).toArray();
        QCOMPARE(emails.size(), 1);
        QCOMPARE(emails.first().toObject().value(QLatin1String("address")).toString(), QStringLiteral("alice@example.com"));
        QCOMPARE(j.value(QLatin1String("homeAddress")).toObject().value(QLatin1String("city")).toString(), QStringLiteral("Exampletown"));
        QCOMPARE(j.value(QLatin1String("businessAddress")).toObject().value(QLatin1String("street")).toString(), QStringLiteral("Business Road 2"));
        QVERIFY(!j.contains(QLatin1String("otherAddress")));
        QCOMPARE(item.skipped.size(), 1);
        QVERIFY(item.skipped.first().contains(QLatin1String("/o=Example/cn=Alice")));
    }

    void event()
    {
        Names names;
        Pst::Message m;
        m.properties.insert(0x0037, str(QStringLiteral("Example meeting")));
        m.properties.insert(0x1000, str(QStringLiteral("Agenda")));
        m.properties.insert(0x0036, int32(2));
        m.properties.insert(names.id(psetidAppointment, 0x8208), str(QStringLiteral("Room 1")));
        // 10:00 in Amsterdam on 26 March 2007 (summer time): 08:00 UTC
        m.properties.insert(names.id(psetidAppointment, 0x820D), time(utc(2007, 3, 26, 8)));
        m.properties.insert(names.id(psetidAppointment, 0x820E), time(utc(2007, 3, 26, 8, 30)));
        m.properties.insert(names.id(psetidAppointment, 0x8205), int32(3));
        m.properties.insert(names.id(psetidAppointment, 0x825E), {Pst::PtypBinary, tzDefinition(QStringLiteral("W. Europe Standard Time"))});
        m.properties.insert(names.id(psetidAppointment, 0x8223), boolean(true));
        m.properties.insert(names.id(psetidAppointment, 0x8216), {Pst::PtypBinary, weeklySample});
        m.properties.insert(names.id(psetidCommon, 0x8503), boolean(true));
        m.properties.insert(names.id(psetidCommon, 0x8501), int32(15));
        QHash<quint32, Pst::PropValue> attendee;
        attendee.insert(0x3001001F, str(QStringLiteral("Bob Example")));
        m.recipients.append(attendee);

        const MapiItems::Item item = MapiItems::event(m, names.map);
        const QJsonObject &j = item.json;
        QCOMPARE(j.value(QLatin1String("subject")).toString(), QStringLiteral("Example meeting"));
        QCOMPARE(j.value(QLatin1String("location")).toObject().value(QLatin1String("displayName")).toString(), QStringLiteral("Room 1"));
        QCOMPARE(j.value(QLatin1String("start")).toObject().value(QLatin1String("dateTime")).toString(), QStringLiteral("2007-03-26T10:00:00"));
        QCOMPARE(j.value(QLatin1String("start")).toObject().value(QLatin1String("timeZone")).toString(), QStringLiteral("W. Europe Standard Time"));
        QCOMPARE(j.value(QLatin1String("end")).toObject().value(QLatin1String("dateTime")).toString(), QStringLiteral("2007-03-26T10:30:00"));
        QCOMPARE(j.value(QLatin1String("showAs")).toString(), QStringLiteral("oof"));
        QCOMPARE(j.value(QLatin1String("sensitivity")).toString(), QStringLiteral("private"));
        QVERIFY(j.value(QLatin1String("isReminderOn")).toBool());
        QCOMPARE(j.value(QLatin1String("reminderMinutesBeforeStart")).toInt(), 15);
        QCOMPARE(j.value(QLatin1String("recurrence")).toObject().value(QLatin1String("pattern")).toObject().value(QLatin1String("type")).toString(), QStringLiteral("weekly"));
        // Never any attendees: Graph would invite them again
        QVERIFY(!j.contains(QLatin1String("attendees")));
        QCOMPARE(item.skipped.size(), 1);
        QVERIFY(item.skipped.first().contains(QLatin1String("Bob Example")));

        // An all-day event: dates at midnight
        Pst::Message day;
        day.properties.insert(0x0037, str(QStringLiteral("Example holiday")));
        day.properties.insert(names.id(psetidAppointment, 0x8215), boolean(true));
        day.properties.insert(names.id(psetidAppointment, 0x820D), time(utc(2026, 4, 26, 22)));
        day.properties.insert(names.id(psetidAppointment, 0x820E), time(utc(2026, 4, 27, 22)));
        const QJsonObject d = MapiItems::event(day, names.map).json;
        QVERIFY(d.value(QLatin1String("isAllDay")).toBool());
        QCOMPARE(d.value(QLatin1String("start")).toObject().value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-04-27T00:00:00"));
        QCOMPARE(d.value(QLatin1String("end")).toObject().value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-04-28T00:00:00"));
    }

    // Through a real PST: folders by container class, named properties by the file's own map
    void throughPst()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("items.pst"));
        QString error;
        Pst::PstWriter w;
        QVERIFY(w.create(path, QStringLiteral("Example"), &error));
        const quint32 contacts = w.addFolder(Pst::PstWriter::nidIpmSubtree, QStringLiteral("Contacts"), QStringLiteral("IPF.Contact"));
        const quint32 calendar = w.addFolder(Pst::PstWriter::nidIpmSubtree, QStringLiteral("Calendar"), QStringLiteral("IPF.Appointment"));
        const quint32 tasks = w.addFolder(Pst::PstWriter::nidIpmSubtree, QStringLiteral("Tasks"), QStringLiteral("IPF.Task"));
        Pst::Message c;
        c.properties.insert(0x001A, str(QStringLiteral("IPM.Contact")));
        c.properties.insert(0x3A06, str(QStringLiteral("Alice")));
        c.properties.insert(w.namedProperty(psetidAddress, 0x8083), str(QStringLiteral("alice@example.com")));
        c.properties.insert(w.namedProperty(psetidAddress, 0x8082), str(QStringLiteral("SMTP")));
        const quint32 cNid = w.addMessage(contacts, c);
        Pst::Message e;
        e.properties.insert(0x001A, str(QStringLiteral("IPM.Appointment")));
        e.properties.insert(0x0037, str(QStringLiteral("Example meeting")));
        e.properties.insert(w.namedProperty(psetidAppointment, 0x820D), time(utc(2026, 11, 20, 9)));
        e.properties.insert(w.namedProperty(psetidAppointment, 0x820E), time(utc(2026, 11, 20, 10)));
        const quint32 eNid = w.addMessage(calendar, e);
        Pst::Message t;
        t.properties.insert(0x001A, str(QStringLiteral("IPM.Task")));
        t.properties.insert(0x0037, str(QStringLiteral("Example task")));
        t.properties.insert(w.namedProperty(psetidTask, 0x8101), int32(2));
        const quint32 tNid = w.addMessage(tasks, t);
        QVERIFY2(w.finish(&error), qPrintable(error));

        Pst::PstFile r;
        QVERIFY2(r.open(path, &error), qPrintable(error));
        const auto folders = r.subfolders(r.ipmSubtree(), &error);
        QVERIFY(folders);
        QHash<QString, MapiItems::Kind> kinds;
        for (const Pst::FolderInfo &f : *folders) {
            kinds.insert(f.name, MapiItems::folderKind(f.containerClass));
        }
        QCOMPARE(kinds.value(QStringLiteral("Contacts")), MapiItems::Kind::Contact);
        QCOMPARE(kinds.value(QStringLiteral("Calendar")), MapiItems::Kind::Event);
        QCOMPARE(kinds.value(QStringLiteral("Tasks")), MapiItems::Kind::Task);
        QCOMPARE(kinds.value(QStringLiteral("Deleted Items")), MapiItems::Kind::Mail);

        const auto cm = r.message(cNid, &error);
        QCOMPARE(MapiItems::kindOf(cm->string(0x001A)), MapiItems::Kind::Contact);
        QCOMPARE(MapiItems::contact(*cm, r.namedProperties()).json.value(QLatin1String("emailAddresses")).toArray().first().toObject().value(QLatin1String("address")).toString(),
                 QStringLiteral("alice@example.com"));
        const QJsonObject ev = MapiItems::event(*r.message(eNid, &error), r.namedProperties()).json;
        QCOMPARE(ev.value(QLatin1String("start")).toObject().value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-11-20T09:00:00"));
        QCOMPARE(MapiItems::task(*r.message(tNid, &error), r.namedProperties()).json.value(QLatin1String("status")).toString(), QStringLiteral("completed"));
    }

    // The export side writes the spec's own BLOBs back: the weekly and the daily example
    void recurrenceWriter()
    {
        QStringList notes;
        const QJsonObject weekly = *MapiItems::recurrence(weeklySample, QString(), &notes);
        QCOMPARE(MapiItems::recurrenceBlob(weekly, QDateTime(QDate(2007, 3, 26), QTime(10, 0)), 30, {}, &notes), weeklySample);
        const QJsonObject daily = *MapiItems::recurrence(dailySample, QString(), &notes);
        QCOMPARE(MapiItems::recurrenceBlob(daily, QDateTime(QDate(2011, 4, 7), QTime(8, 0)), 30, {QDate(2011, 4, 22), QDate(2011, 4, 19)}, &notes), dailySample);

        // Occurrences: the third Saturday or Sunday every three months, ten times
        const QJsonObject nth = *MapiItems::recurrence(monthNthSample, QString(), &notes);
        const QList<QDate> dates = MapiItems::occurrences(nth, 10);
        QCOMPARE(dates.size(), 10);
        QCOMPARE(dates.first(), QDate(2008, 2, 9));
        QCOMPARE(dates.at(1), QDate(2008, 5, 10));
        // Every weekday from a Wednesday, until the Tuesday after
        const QJsonObject weekdays{{QStringLiteral("pattern"),
                                    QJsonObject{{QStringLiteral("type"), QStringLiteral("weekly")},
                                                {QStringLiteral("interval"), 1},
                                                {QStringLiteral("daysOfWeek"), QJsonArray{QStringLiteral("monday"), QStringLiteral("tuesday"), QStringLiteral("wednesday"),
                                                                                          QStringLiteral("thursday"), QStringLiteral("friday")}}}},
                                   {QStringLiteral("range"),
                                    QJsonObject{{QStringLiteral("type"), QStringLiteral("endDate")}, {QStringLiteral("startDate"), QStringLiteral("2026-11-11")},
                                                {QStringLiteral("endDate"), QStringLiteral("2026-11-17")}}}};
        QCOMPARE(MapiItems::occurrences(weekdays, 100, QDate(2026, 11, 17)).size(), 5);
    }

    // [MS-OXOCAL] 4.1.4: Pacific Standard Time with its 2006 and 2007 rules
    void timeZoneWriter()
    {
        const QByteArray pacific = QByteArray::fromHex("0201300002001500500061006300690066006900630020005300740061006E0064006100720064002000540069006"
                                                       "D006500020002013E000000D6070000000000000000000000000000E001000000000000C4FFFFFF00000A00000005"
                                                       "0002000000000000000000040000000100020000000000000002013E000200D707000000000000000000000000000"
                                                       "0E001000000000000C4FFFFFF00000B0000000100020000000000000000000300000002000200000000000000");
        const QTimeZone zone = MapiItems::zoneFor(QStringLiteral("Pacific Standard Time"));
        QVERIFY(zone.isValid());
        QCOMPARE(MapiItems::timeZoneDefinition(zone, QStringLiteral("Pacific Standard Time"), {2006, 2007}, 2007), pacific);
        QCOMPARE(MapiItems::timeZoneName(pacific), QStringLiteral("Pacific Standard Time"));
        // The struct: the 2007 rule's biases and dates
        const QByteArray tz = MapiItems::timeZoneStruct(zone, 2007);
        QCOMPARE(tz.size(), 48);
        QCOMPARE(tz.mid(0, 12), QByteArray::fromHex("E001000000000000C4FFFFFF"));
        // A zone without daylight saving (UTC+9: bias -540): no dates
        const QByteArray tokyo = MapiItems::timeZoneStruct(MapiItems::zoneFor(QStringLiteral("Tokyo Standard Time")), 2026);
        QCOMPARE(tokyo.mid(0, 12), QByteArray::fromHex("E4FDFFFF0000000000000000"));
        QCOMPARE(tokyo.mid(12), QByteArray(36, 0));
        QCOMPARE(MapiItems::zoneFor(QStringLiteral("UTC")), QTimeZone(QTimeZone::UTC));
    }

    // Out to Outlook's form and back: what Graph had comes back
    void exportRoundTrips()
    {
        Names names;
        const MapiItems::NameId id = [&names](const QUuid &set, quint32 lid) {
            return names.id(set, lid);
        };
        Pst::NamedProperty keywords;
        keywords.guid = Pst::psPublicStrings;
        keywords.isString = true;
        keywords.name = QStringLiteral("Keywords");
        names.map.insert(0x8FFF, keywords);

        const QJsonObject contact{{QStringLiteral("displayName"), QStringLiteral("Alice Example")},
                                  {QStringLiteral("givenName"), QStringLiteral("Alice")},
                                  {QStringLiteral("surname"), QStringLiteral("Example")},
                                  {QStringLiteral("companyName"), QStringLiteral("Acme")},
                                  {QStringLiteral("birthday"), QStringLiteral("1990-05-15T00:00:00Z")},
                                  {QStringLiteral("businessPhones"), QJsonArray{QStringLiteral("+31 20 000 0000")}},
                                  {QStringLiteral("emailAddresses"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("Alice Example")},
                                                                                            {QStringLiteral("address"), QStringLiteral("alice@example.com")}}}},
                                  {QStringLiteral("businessAddress"), QJsonObject{{QStringLiteral("city"), QStringLiteral("Worktown")}}},
                                  {QStringLiteral("personalNotes"), QStringLiteral("Example notes")},
                                  {QStringLiteral("categories"), QJsonArray{QStringLiteral("Acme")}}};
        const Pst::Message c = MapiItems::contactToMapi(contact, id, 0x8FFF);
        QCOMPARE(MapiItems::kindOf(c.string(0x001A)), MapiItems::Kind::Contact);
        const QJsonObject back = MapiItems::contact(c, names.map).json;
        for (const char *key : {"displayName", "givenName", "surname", "companyName", "birthday", "personalNotes"}) {
            QCOMPARE(back.value(QLatin1String(key)), contact.value(QLatin1String(key)));
        }
        QCOMPARE(back.value(QLatin1String("businessPhones")), contact.value(QLatin1String("businessPhones")));
        QCOMPARE(back.value(QLatin1String("emailAddresses")).toArray().first().toObject().value(QLatin1String("address")).toString(), QStringLiteral("alice@example.com"));
        QCOMPARE(back.value(QLatin1String("businessAddress")).toObject().value(QLatin1String("city")).toString(), QStringLiteral("Worktown"));
        QCOMPARE(back.value(QLatin1String("categories")), contact.value(QLatin1String("categories")));
        // The RTF body says the same
        QVERIFY(c.properties.contains(0x1009));

        const QJsonObject event{{QStringLiteral("subject"), QStringLiteral("Example series")},
                                {QStringLiteral("body"), QJsonObject{{QStringLiteral("contentType"), QStringLiteral("html")}, {QStringLiteral("content"), QStringLiteral("<p>Agenda</p>")}}},
                                {QStringLiteral("start"), QJsonObject{{QStringLiteral("dateTime"), QStringLiteral("2026-03-02T08:00:00.0000000")}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}}},
                                {QStringLiteral("end"), QJsonObject{{QStringLiteral("dateTime"), QStringLiteral("2026-03-02T08:30:00.0000000")}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}}},
                                {QStringLiteral("originalStartTimeZone"), QStringLiteral("W. Europe Standard Time")},
                                {QStringLiteral("showAs"), QStringLiteral("tentative")},
                                {QStringLiteral("isReminderOn"), true},
                                {QStringLiteral("reminderMinutesBeforeStart"), 10},
                                {QStringLiteral("attendees"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("optional")},
                                                                                     {QStringLiteral("emailAddress"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Bob Example")},
                                                                                                                                  {QStringLiteral("address"), QStringLiteral("bob@example.com")}}}}}},
                                {QStringLiteral("recurrence"),
                                 QJsonObject{{QStringLiteral("pattern"), QJsonObject{{QStringLiteral("type"), QStringLiteral("weekly")}, {QStringLiteral("interval"), 1},
                                                                                     {QStringLiteral("daysOfWeek"), QJsonArray{QStringLiteral("monday")}},
                                                                                     {QStringLiteral("firstDayOfWeek"), QStringLiteral("monday")}}},
                                             {QStringLiteral("range"), QJsonObject{{QStringLiteral("type"), QStringLiteral("numbered")}, {QStringLiteral("startDate"), QStringLiteral("2026-03-02")},
                                                                                   {QStringLiteral("numberOfOccurrences"), 8},
                                                                                   {QStringLiteral("recurrenceTimeZone"), QStringLiteral("W. Europe Standard Time")}}}}}};
        const Pst::Message e = MapiItems::eventToMapi(event, id, 0x8FFF, {QDate(2026, 3, 16)});
        QCOMPARE(e.recipients.size(), 1);
        const MapiItems::Item ev = MapiItems::event(e, names.map);
        // 08:00 UTC in March is 09:00 in Amsterdam (winter time)
        QCOMPARE(ev.json.value(QLatin1String("start")).toObject().value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-03-02T09:00:00"));
        QCOMPARE(ev.json.value(QLatin1String("start")).toObject().value(QLatin1String("timeZone")).toString(), QStringLiteral("W. Europe Standard Time"));
        QCOMPARE(ev.json.value(QLatin1String("showAs")).toString(), QStringLiteral("tentative"));
        QCOMPARE(ev.json.value(QLatin1String("reminderMinutesBeforeStart")).toInt(), 10);
        QVERIFY(ev.json.value(QLatin1String("body")).toObject().value(QLatin1String("content")).toString().contains(QLatin1String("<p>Agenda</p>")));
        const QJsonObject r = ev.json.value(QLatin1String("recurrence")).toObject();
        QCOMPARE(r.value(QLatin1String("pattern")).toObject().value(QLatin1String("daysOfWeek")).toArray(), QJsonArray{QStringLiteral("monday")});
        QCOMPARE(r.value(QLatin1String("range")).toObject().value(QLatin1String("numberOfOccurrences")).toInt(), 8);
        QCOMPARE(r.value(QLatin1String("range")).toObject().value(QLatin1String("startDate")).toString(), QStringLiteral("2026-03-02"));
        // The occurrence exported apart is marked deleted in the series, and the log says so
        QVERIFY(ev.skipped.join(QLatin1Char(' ')).contains(QLatin1String("1 changed or cancelled")));

        const QJsonObject task{{QStringLiteral("title"), QStringLiteral("Example task")},
                               {QStringLiteral("status"), QStringLiteral("inProgress")},
                               {QStringLiteral("importance"), QStringLiteral("high")},
                               {QStringLiteral("dueDateTime"), QJsonObject{{QStringLiteral("dateTime"), QStringLiteral("2026-11-20T00:00:00.0000000")}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}}},
                               {QStringLiteral("body"), QJsonObject{{QStringLiteral("contentType"), QStringLiteral("text")}, {QStringLiteral("content"), QStringLiteral("Details")}}}};
        const QJsonObject tb = MapiItems::task(MapiItems::taskToMapi(task, id, 0x8FFF), names.map).json;
        QCOMPARE(tb.value(QLatin1String("title")).toString(), QStringLiteral("Example task"));
        QCOMPARE(tb.value(QLatin1String("status")).toString(), QStringLiteral("inProgress"));
        QCOMPARE(tb.value(QLatin1String("importance")).toString(), QStringLiteral("high"));
        QCOMPARE(tb.value(QLatin1String("body")).toObject().value(QLatin1String("content")).toString(), QStringLiteral("Details"));
        QCOMPARE(tb.value(QLatin1String("dueDateTime")).toObject().value(QLatin1String("dateTime")).toString().left(10), QStringLiteral("2026-11-20"));
    }

    void task()
    {
        Names names;
        Pst::Message m;
        m.properties.insert(0x0037, str(QStringLiteral("Example task")));
        m.properties.insert(0x1000, str(QStringLiteral("Details")));
        m.properties.insert(0x0017, int32(2));
        m.properties.insert(names.id(psetidTask, 0x8101), int32(3));
        m.properties.insert(names.id(psetidTask, 0x8105), time(utc(2026, 11, 20)));
        m.properties.insert(names.id(psetidTask, 0x8104), time(utc(2026, 11, 13)));
        m.properties.insert(names.id(psetidCommon, 0x8503), boolean(true));
        m.properties.insert(names.id(psetidCommon, 0x8502), time(utc(2026, 11, 19, 8)));
        m.properties.insert(names.id(psetidTask, 0x8116), {Pst::PtypBinary, QByteArray(10, 0)});
        const MapiItems::Item item = MapiItems::task(m, names.map);
        const QJsonObject &j = item.json;
        QCOMPARE(j.value(QLatin1String("title")).toString(), QStringLiteral("Example task"));
        QCOMPARE(j.value(QLatin1String("status")).toString(), QStringLiteral("waitingOnOthers"));
        QCOMPARE(j.value(QLatin1String("importance")).toString(), QStringLiteral("high"));
        QCOMPARE(j.value(QLatin1String("dueDateTime")).toObject().value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-11-20T00:00:00"));
        QCOMPARE(j.value(QLatin1String("startDateTime")).toObject().value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-11-13T00:00:00"));
        QVERIFY(j.value(QLatin1String("isReminderOn")).toBool());
        QCOMPARE(j.value(QLatin1String("reminderDateTime")).toObject().value(QLatin1String("dateTime")).toString(), QStringLiteral("2026-11-19T08:00:00"));
        QCOMPARE(j.value(QLatin1String("body")).toObject().value(QLatin1String("content")).toString(), QStringLiteral("Details"));
        QCOMPARE(item.skipped, QStringList{QStringLiteral("the task's recurrence")});
    }
};

QTEST_GUILESS_MAIN(MapiItemsTest)
#include "mapiitemstest.moc"
