// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mapiitems.h"
#include "mapigraph.h"

#include <QJsonArray>
#include <QTimeZone>
#include <QtEndian>

namespace GraphMail::MapiItems
{

namespace
{
// Property sets ([MS-OXPROPS] 1.3.2)
const QUuid psetidAddress(0x00062004, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
const QUuid psetidAppointment(0x00062002, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
const QUuid psetidCommon(0x00062008, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
const QUuid psetidTask(0x00062003, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);

// Tags
constexpr quint16 propImportance = 0x0017;
constexpr quint16 propSensitivity = 0x0036;
constexpr quint16 propSubject = 0x0037;
constexpr quint16 propDisplayName = 0x3001;
constexpr quint16 propGeneration = 0x3A05;
constexpr quint16 propGivenName = 0x3A06;
constexpr quint16 propBusinessTelephone = 0x3A08;
constexpr quint16 propHomeTelephone = 0x3A09;
constexpr quint16 propInitials = 0x3A0A;
constexpr quint16 propSurname = 0x3A11;
constexpr quint16 propCompanyName = 0x3A16;
constexpr quint16 propTitle = 0x3A17;
constexpr quint16 propDepartmentName = 0x3A18;
constexpr quint16 propOfficeLocation = 0x3A19;
constexpr quint16 propBusiness2Telephone = 0x3A1B;
constexpr quint16 propMobileTelephone = 0x3A1C;
constexpr quint16 propHome2Telephone = 0x3A2F;
constexpr quint16 propAssistant = 0x3A30;
constexpr quint16 propBirthday = 0x3A42;
constexpr quint16 propMiddleName = 0x3A44;
constexpr quint16 propDisplayNamePrefix = 0x3A45;
constexpr quint16 propProfession = 0x3A46;
constexpr quint16 propSpouseName = 0x3A48;
constexpr quint16 propManagerName = 0x3A4E;
constexpr quint16 propNickname = 0x3A4F;
constexpr quint16 propPersonalHomePage = 0x3A50;
constexpr quint16 propBusinessHomePage = 0x3A51;
constexpr quint16 propHomeAddressCity = 0x3A59;
constexpr quint16 propHomeAddressCountry = 0x3A5A;
constexpr quint16 propHomeAddressPostalCode = 0x3A5B;
constexpr quint16 propHomeAddressState = 0x3A5C;
constexpr quint16 propHomeAddressStreet = 0x3A5D;
constexpr quint16 propOtherAddressCity = 0x3A5F;
constexpr quint16 propOtherAddressCountry = 0x3A60;
constexpr quint16 propOtherAddressPostalCode = 0x3A61;
constexpr quint16 propOtherAddressState = 0x3A62;
constexpr quint16 propOtherAddressStreet = 0x3A63;
constexpr quint16 propBody = 0x1000;

// LIDs
constexpr quint32 lidFileUnder = 0x8005;
constexpr quint32 lidWorkAddressStreet = 0x8045;
constexpr quint32 lidWorkAddressCity = 0x8046;
constexpr quint32 lidWorkAddressState = 0x8047;
constexpr quint32 lidWorkAddressPostalCode = 0x8048;
constexpr quint32 lidWorkAddressCountry = 0x8049;
constexpr quint32 lidInstantMessagingAddress = 0x8062;
// Email1, Email2, Email3: display name, address type, address
constexpr quint32 lidEmail[3][3] = {{0x8080, 0x8082, 0x8083}, {0x8090, 0x8092, 0x8093}, {0x80A0, 0x80A2, 0x80A3}};
constexpr quint32 lidBusyStatus = 0x8205;
constexpr quint32 lidLocation = 0x8208;
constexpr quint32 lidAppointmentStartWhole = 0x820D;
constexpr quint32 lidAppointmentEndWhole = 0x820E;
constexpr quint32 lidAppointmentSubType = 0x8215;
constexpr quint32 lidAppointmentRecur = 0x8216;
constexpr quint32 lidRecurring = 0x8223;
constexpr quint32 lidTimeZoneStartDisplay = 0x825E;
constexpr quint32 lidTimeZoneRecur = 0x8260;
constexpr quint32 lidReminderDelta = 0x8501;
constexpr quint32 lidReminderTime = 0x8502;
constexpr quint32 lidReminderSet = 0x8503;
constexpr quint32 lidTaskStatus = 0x8101;
constexpr quint32 lidTaskStartDate = 0x8104;
constexpr quint32 lidTaskDueDate = 0x8105;
constexpr quint32 lidTaskDateCompleted = 0x810F;
constexpr quint32 lidTaskRecurrence = 0x8116;

// [MS-OXOCAL] 2.2.1.44.1
constexpr quint16 freqDaily = 0x200A;
constexpr quint16 freqWeekly = 0x200B;
constexpr quint16 freqMonthly = 0x200C;
constexpr quint16 freqYearly = 0x200D;
constexpr quint16 patternDay = 0x0000;
constexpr quint16 patternWeek = 0x0001;
constexpr quint16 patternMonth = 0x0002;
constexpr quint16 patternMonthNth = 0x0003;
constexpr quint16 patternMonthEnd = 0x0004;
constexpr quint32 endAfterDate = 0x2021;
constexpr quint32 endAfterCount = 0x2022;

// A named property's value; nullptr when the message lacks it
const Pst::PropValue *named(const Pst::Message &m, const QHash<quint16, Pst::NamedProperty> &names, const QUuid &set, quint32 lid)
{
    for (auto it = names.constBegin(); it != names.constEnd(); ++it) {
        if (!it->isString && it->id == lid && it->guid == set) {
            const auto v = m.properties.constFind(it.key());
            return v == m.properties.constEnd() ? nullptr : &*v;
        }
    }
    return nullptr;
}

QString namedString(const Pst::Message &m, const QHash<quint16, Pst::NamedProperty> &names, const QUuid &set, quint32 lid)
{
    const Pst::PropValue *v = named(m, names, set, lid);
    return v ? v->toString(m.codepage()) : QString();
}

void putString(QJsonObject &json, const QString &key, const QString &value)
{
    if (!value.isEmpty()) {
        json.insert(key, value);
    }
}

QJsonArray categoriesOf(const Pst::Message &m, const QHash<quint16, Pst::NamedProperty> &names)
{
    QJsonArray out;
    for (auto it = names.constBegin(); it != names.constEnd(); ++it) {
        if (it->isString && it->guid == Pst::psPublicStrings && it->name == QLatin1String("Keywords") && m.properties.contains(it.key())) {
            const Pst::PropValue v = m.properties.value(it.key());
            const QList<QByteArray> items = v.items();
            for (const QByteArray &item : items) {
                out.append(Pst::PropValue{quint16(v.type & ~Pst::PtypMultiple), item}.toString(m.codepage()));
            }
        }
    }
    return out;
}

QString importance(qint64 v)
{
    return v == 0 ? QStringLiteral("low") : v == 2 ? QStringLiteral("high") : QStringLiteral("normal");
}

// A Graph dateTimeTimeZone: the moment in the named Windows zone when Qt knows it, else UTC
QJsonObject dateTimeIn(const QDateTime &utc, const QString &windowsZone)
{
    const QByteArray iana = windowsZone.isEmpty() ? QByteArray() : QTimeZone::windowsIdToDefaultIanaId(windowsZone.toUtf8());
    const QTimeZone zone(iana);
    if (!iana.isEmpty() && zone.isValid()) {
        return {{QStringLiteral("dateTime"), utc.toTimeZone(zone).toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))}, {QStringLiteral("timeZone"), windowsZone}};
    }
    return {{QStringLiteral("dateTime"), utc.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}};
}

// A date stored as local midnight in a PtypTime (task dates, birthdays): the date it names
QString dateOnly(const QDateTime &t)
{
    // Half a day either way covers every zone's offset from the stored value
    return t.toUTC().addSecs(12 * 3600).date().toString(Qt::ISODate);
}

QDate dateFromMinutes(quint32 minutes)
{
    return QDate(1601, 1, 1).addDays(qint64(minutes) / 1440);
}

QJsonArray daysOfWeek(quint32 mask)
{
    static const char *const days[] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
    QJsonArray out;
    for (int i = 0; i < 7; ++i) {
        if (mask & (1u << i)) {
            out.append(QString::fromLatin1(days[i]));
        }
    }
    return out;
}

QJsonObject address(const QString &street, const QString &city, const QString &state, const QString &postalCode, const QString &country)
{
    QJsonObject a;
    putString(a, QStringLiteral("street"), street);
    putString(a, QStringLiteral("city"), city);
    putString(a, QStringLiteral("state"), state);
    putString(a, QStringLiteral("postalCode"), postalCode);
    putString(a, QStringLiteral("countryOrRegion"), country);
    return a;
}
}

Kind kindOf(const QString &messageClass)
{
    const QString c = messageClass.toLower();
    const auto is = [&](const char *prefix) {
        const QLatin1String p(prefix);
        return c == p || c.startsWith(p + QLatin1Char('.'));
    };
    if (is("ipm.contact")) {
        return Kind::Contact;
    }
    if (is("ipm.appointment")) {
        return Kind::Event;
    }
    if (is("ipm.task")) {
        return Kind::Task;
    }
    return MapiGraph::isMail(messageClass) ? Kind::Mail : Kind::Other;
}

Kind folderKind(const QString &containerClass)
{
    const QString c = containerClass.toLower();
    if (c.startsWith(QLatin1String("ipf.contact"))) {
        return Kind::Contact;
    }
    if (c.startsWith(QLatin1String("ipf.appointment"))) {
        return Kind::Event;
    }
    if (c.startsWith(QLatin1String("ipf.task"))) {
        return Kind::Task;
    }
    if (c.isEmpty() || c.startsWith(QLatin1String("ipf.note"))) {
        return Kind::Mail;
    }
    return Kind::Other;
}

QString timeZoneName(const QByteArray &d)
{
    // Major, Minor, cbHeader, Reserved, cchKeyName, then KeyName (UTF-16, no null)
    if (d.size() < 8) {
        return {};
    }
    const int chars = qFromLittleEndian<quint16>(d.constData() + 6);
    if (chars <= 0 || chars > 260 || 8 + chars * 2 > d.size()) {
        return {};
    }
    return QString::fromUtf16(reinterpret_cast<const char16_t *>(d.constData() + 8), chars);
}

std::optional<QJsonObject> recurrence(const QByteArray &blob, const QString &timeZone, QStringList *notes)
{
    qsizetype at = 0;
    bool ok = true;
    const auto u16 = [&]() -> quint16 {
        if (at + 2 > blob.size()) {
            ok = false;
            return 0;
        }
        const quint16 v = qFromLittleEndian<quint16>(blob.constData() + at);
        at += 2;
        return v;
    };
    const auto u32 = [&]() -> quint32 {
        if (at + 4 > blob.size()) {
            ok = false;
            return 0;
        }
        const quint32 v = qFromLittleEndian<quint32>(blob.constData() + at);
        at += 4;
        return v;
    };
    u16();
    u16();
    const quint16 frequency = u16();
    const quint16 patternType = u16();
    const quint16 calendarType = u16();
    u32(); // FirstDateTime: Graph works from the start date instead
    const quint32 period = u32();
    u32(); // SlidingFlag
    quint32 days = 0;
    quint32 dayOfMonth = 0;
    quint32 nth = 0;
    if (patternType == patternWeek) {
        days = u32();
    } else if (patternType == patternMonth || patternType == patternMonthEnd) {
        dayOfMonth = u32();
    } else if (patternType == patternMonthNth) {
        days = u32();
        nth = u32();
    } else if (patternType != patternDay) {
        if (notes) {
            notes->append(QStringLiteral("a series in another calendar (Hijri)"));
        }
        return std::nullopt;
    }
    const quint32 endType = u32();
    const quint32 occurrences = u32();
    const quint32 firstDow = u32();
    const quint32 deleted = u32();
    at += qsizetype(std::min<quint32>(deleted, 0x100000)) * 4;
    const quint32 modified = u32();
    at += qsizetype(std::min<quint32>(modified, 0x100000)) * 4;
    const quint32 startDate = u32();
    const quint32 endDate = u32();
    if (!ok || period == 0) {
        if (notes) {
            notes->append(QStringLiteral("a damaged series pattern"));
        }
        return std::nullopt;
    }
    if (calendarType > 2) {
        if (notes) {
            notes->append(QStringLiteral("a series in another calendar"));
        }
        return std::nullopt;
    }

    static const char *const weekdays[] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
    static const char *const index[] = {"first", "first", "second", "third", "fourth", "last"};
    QJsonObject pattern;
    const QDate start = dateFromMinutes(startDate);
    if (frequency == freqDaily && patternType == patternDay) {
        pattern = {{QStringLiteral("type"), QStringLiteral("daily")}, {QStringLiteral("interval"), qint64(std::max<quint32>(1, period / 1440))}};
    } else if (frequency == freqWeekly || (frequency == freqDaily && patternType == patternWeek)) {
        // "Every weekday" is stored as a weekly pattern under the daily frequency
        pattern = {{QStringLiteral("type"), QStringLiteral("weekly")},
                   {QStringLiteral("interval"), qint64(frequency == freqWeekly ? period : 1)},
                   {QStringLiteral("daysOfWeek"), daysOfWeek(days)},
                   {QStringLiteral("firstDayOfWeek"), QString::fromLatin1(weekdays[firstDow % 7])}};
    } else if ((frequency == freqMonthly || frequency == freqYearly) && (patternType == patternMonth || patternType == patternMonthEnd)) {
        const bool yearly = frequency == freqYearly;
        pattern = {{QStringLiteral("type"), yearly ? QStringLiteral("absoluteYearly") : QStringLiteral("absoluteMonthly")},
                   {QStringLiteral("interval"), qint64(yearly ? std::max<quint32>(1, period / 12) : period)},
                   {QStringLiteral("dayOfMonth"), qint64(patternType == patternMonthEnd ? 31 : dayOfMonth)}};
        if (yearly) {
            pattern.insert(QStringLiteral("month"), start.month());
        }
        if (patternType == patternMonthEnd && notes) {
            notes->append(QStringLiteral("the series' last day of the month (now day 31)"));
        }
    } else if ((frequency == freqMonthly || frequency == freqYearly) && patternType == patternMonthNth) {
        const bool yearly = frequency == freqYearly;
        pattern = {{QStringLiteral("type"), yearly ? QStringLiteral("relativeYearly") : QStringLiteral("relativeMonthly")},
                   {QStringLiteral("interval"), qint64(yearly ? std::max<quint32>(1, period / 12) : period)},
                   {QStringLiteral("daysOfWeek"), daysOfWeek(days)},
                   {QStringLiteral("index"), QString::fromLatin1(index[std::min<quint32>(nth, 5)])}};
        if (yearly) {
            pattern.insert(QStringLiteral("month"), start.month());
        }
    } else {
        if (notes) {
            notes->append(QStringLiteral("a series pattern Graph cannot hold"));
        }
        return std::nullopt;
    }

    QJsonObject range{{QStringLiteral("startDate"), start.toString(Qt::ISODate)},
                      {QStringLiteral("recurrenceTimeZone"), timeZone.isEmpty() ? QStringLiteral("UTC") : timeZone}};
    if (endType == endAfterDate) {
        range.insert(QStringLiteral("type"), QStringLiteral("endDate"));
        range.insert(QStringLiteral("endDate"), dateFromMinutes(endDate).toString(Qt::ISODate));
    } else if (endType == endAfterCount) {
        range.insert(QStringLiteral("type"), QStringLiteral("numbered"));
        range.insert(QStringLiteral("numberOfOccurrences"), qint64(occurrences));
    } else {
        range.insert(QStringLiteral("type"), QStringLiteral("noEnd"));
    }
    if (deleted > 0 && notes) {
        notes->append(QStringLiteral("%1 changed or cancelled occurrences of the series").arg(deleted));
    }
    return QJsonObject{{QStringLiteral("pattern"), pattern}, {QStringLiteral("range"), range}};
}

Item contact(const Pst::Message &m, const QHash<quint16, Pst::NamedProperty> &names)
{
    Item out;
    QJsonObject &j = out.json;
    putString(j, QStringLiteral("displayName"), m.string(propDisplayName));
    putString(j, QStringLiteral("givenName"), m.string(propGivenName));
    putString(j, QStringLiteral("middleName"), m.string(propMiddleName));
    putString(j, QStringLiteral("surname"), m.string(propSurname));
    putString(j, QStringLiteral("title"), m.string(propDisplayNamePrefix));
    putString(j, QStringLiteral("generation"), m.string(propGeneration));
    putString(j, QStringLiteral("initials"), m.string(propInitials));
    putString(j, QStringLiteral("nickName"), m.string(propNickname));
    putString(j, QStringLiteral("jobTitle"), m.string(propTitle));
    putString(j, QStringLiteral("companyName"), m.string(propCompanyName));
    putString(j, QStringLiteral("department"), m.string(propDepartmentName));
    putString(j, QStringLiteral("officeLocation"), m.string(propOfficeLocation));
    putString(j, QStringLiteral("profession"), m.string(propProfession));
    putString(j, QStringLiteral("manager"), m.string(propManagerName));
    putString(j, QStringLiteral("assistantName"), m.string(propAssistant));
    putString(j, QStringLiteral("spouseName"), m.string(propSpouseName));
    putString(j, QStringLiteral("businessHomePage"), m.string(propBusinessHomePage));
    putString(j, QStringLiteral("personalNotes"), m.string(propBody));
    putString(j, QStringLiteral("fileAs"), namedString(m, names, psetidAddress, lidFileUnder));
    putString(j, QStringLiteral("mobilePhone"), m.string(propMobileTelephone));
    if (m.properties.contains(propBirthday)) {
        const QDateTime birthday = m.properties.value(propBirthday).toTime();
        if (birthday.isValid()) {
            j.insert(QStringLiteral("birthday"), QString(dateOnly(birthday) + QStringLiteral("T00:00:00Z")));
        }
    }
    const auto phones = [&](quint16 a, quint16 b) {
        QJsonArray list;
        for (const quint16 id : {a, b}) {
            if (const QString n = m.string(id); !n.isEmpty()) {
                list.append(n);
            }
        }
        return list;
    };
    j.insert(QStringLiteral("businessPhones"), phones(propBusinessTelephone, propBusiness2Telephone));
    j.insert(QStringLiteral("homePhones"), phones(propHomeTelephone, propHome2Telephone));

    // Email1-3: an SMTP address each; Exchange-only addresses are listed as left out
    QJsonArray emails;
    for (const auto &lids : lidEmail) {
        const QString addr = namedString(m, names, psetidAddress, lids[2]);
        if (addr.isEmpty()) {
            continue;
        }
        const QString smtp = MapiGraph::smtpAddress(QString(), addr, namedString(m, names, psetidAddress, lids[1]));
        if (smtp.isEmpty()) {
            out.skipped.append(QStringLiteral("an Exchange-only e-mail address: %1").arg(addr));
            continue;
        }
        QJsonObject e{{QStringLiteral("address"), smtp}};
        putString(e, QStringLiteral("name"), namedString(m, names, psetidAddress, lids[0]));
        emails.append(e);
    }
    j.insert(QStringLiteral("emailAddresses"), emails);
    if (const QString im = namedString(m, names, psetidAddress, lidInstantMessagingAddress); !im.isEmpty()) {
        j.insert(QStringLiteral("imAddresses"), QJsonArray{im});
    }

    const QJsonObject home = address(m.string(propHomeAddressStreet), m.string(propHomeAddressCity), m.string(propHomeAddressState), m.string(propHomeAddressPostalCode),
                                     m.string(propHomeAddressCountry));
    const QJsonObject business = address(namedString(m, names, psetidAddress, lidWorkAddressStreet), namedString(m, names, psetidAddress, lidWorkAddressCity),
                                         namedString(m, names, psetidAddress, lidWorkAddressState), namedString(m, names, psetidAddress, lidWorkAddressPostalCode),
                                         namedString(m, names, psetidAddress, lidWorkAddressCountry));
    const QJsonObject other = address(m.string(propOtherAddressStreet), m.string(propOtherAddressCity), m.string(propOtherAddressState), m.string(propOtherAddressPostalCode),
                                      m.string(propOtherAddressCountry));
    if (!home.isEmpty()) {
        j.insert(QStringLiteral("homeAddress"), home);
    }
    if (!business.isEmpty()) {
        j.insert(QStringLiteral("businessAddress"), business);
    }
    if (!other.isEmpty()) {
        j.insert(QStringLiteral("otherAddress"), other);
    }
    if (const QJsonArray c = categoriesOf(m, names); !c.isEmpty()) {
        j.insert(QStringLiteral("categories"), c);
    }
    if (const QString page = m.string(propPersonalHomePage); !page.isEmpty()) {
        out.skipped.append(QStringLiteral("the personal web page %1").arg(page));
    }
    if (!m.attachments.isEmpty()) {
        out.skipped.append(QStringLiteral("the contact's photo or attachments"));
    }
    return out;
}

Item event(const Pst::Message &m, const QHash<quint16, Pst::NamedProperty> &names)
{
    Item out;
    QJsonObject &j = out.json;
    j.insert(QStringLiteral("subject"), m.string(propSubject));
    const MapiGraph::Body body = MapiGraph::bestBody(m);
    j.insert(QStringLiteral("body"),
             QJsonObject{{QStringLiteral("contentType"), body.html ? QStringLiteral("html") : QStringLiteral("text")}, {QStringLiteral("content"), body.content}});
    if (!body.note.isEmpty()) {
        out.skipped.append(body.note);
    }
    if (const QString where = namedString(m, names, psetidAppointment, lidLocation); !where.isEmpty()) {
        j.insert(QStringLiteral("location"), QJsonObject{{QStringLiteral("displayName"), where}});
    }

    // The zone the user saw it in, so the series keeps its local time across daylight saving
    QString zone;
    if (const Pst::PropValue *v = named(m, names, psetidAppointment, lidTimeZoneStartDisplay)) {
        zone = timeZoneName(v->data);
    }
    if (zone.isEmpty()) {
        if (const Pst::PropValue *v = named(m, names, psetidAppointment, lidTimeZoneRecur)) {
            zone = timeZoneName(v->data);
        }
    }
    const Pst::PropValue *startValue = named(m, names, psetidAppointment, lidAppointmentStartWhole);
    const Pst::PropValue *endValue = named(m, names, psetidAppointment, lidAppointmentEndWhole);
    const QDateTime start = startValue ? startValue->toTime() : QDateTime();
    const QDateTime end = endValue ? endValue->toTime() : start;
    const Pst::PropValue *allDayValue = named(m, names, psetidAppointment, lidAppointmentSubType);
    const bool allDay = allDayValue && allDayValue->toBool();
    if (allDay) {
        // Midnight to midnight, as dates: Graph wants them at midnight in the zone given
        j.insert(QStringLiteral("isAllDay"), true);
        j.insert(QStringLiteral("start"), QJsonObject{{QStringLiteral("dateTime"), QString(dateOnly(start) + QStringLiteral("T00:00:00"))}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}});
        j.insert(QStringLiteral("end"), QJsonObject{{QStringLiteral("dateTime"), QString(dateOnly(end.isValid() ? end : start.addDays(1)) + QStringLiteral("T00:00:00"))},
                                                    {QStringLiteral("timeZone"), QStringLiteral("UTC")}});
    } else if (start.isValid()) {
        j.insert(QStringLiteral("start"), dateTimeIn(start, zone));
        j.insert(QStringLiteral("end"), dateTimeIn(end.isValid() ? end : start, zone));
    }

    static const char *const showAs[] = {"free", "tentative", "busy", "oof", "workingElsewhere"};
    if (const Pst::PropValue *v = named(m, names, psetidAppointment, lidBusyStatus)) {
        j.insert(QStringLiteral("showAs"), QString::fromLatin1(showAs[std::clamp<qint64>(v->toInt(), 0, 4)]));
    }
    static const char *const sensitivity[] = {"normal", "personal", "private", "confidential"};
    j.insert(QStringLiteral("sensitivity"), QString::fromLatin1(sensitivity[std::clamp<qint64>(m.properties.value(propSensitivity).toInt(), 0, 3)]));
    j.insert(QStringLiteral("importance"), importance(m.properties.contains(propImportance) ? m.properties.value(propImportance).toInt() : 1));
    const Pst::PropValue *reminder = named(m, names, psetidCommon, lidReminderSet);
    j.insert(QStringLiteral("isReminderOn"), reminder && reminder->toBool());
    if (const Pst::PropValue *v = named(m, names, psetidCommon, lidReminderDelta)) {
        j.insert(QStringLiteral("reminderMinutesBeforeStart"), v->toInt());
    }
    if (const QJsonArray c = categoriesOf(m, names); !c.isEmpty()) {
        j.insert(QStringLiteral("categories"), c);
    }

    const Pst::PropValue *recurring = named(m, names, psetidAppointment, lidRecurring);
    const Pst::PropValue *pattern = named(m, names, psetidAppointment, lidAppointmentRecur);
    if (recurring && recurring->toBool() && pattern) {
        if (const std::optional<QJsonObject> r = recurrence(pattern->data, zone, &out.skipped)) {
            j.insert(QStringLiteral("recurrence"), *r);
        } else {
            out.skipped.append(QStringLiteral("the series (imported as its first occurrence)"));
        }
    }

    // Graph would send invitations to attendees: they are named in the log instead
    QStringList attendees;
    for (const QHash<quint32, Pst::PropValue> &r : m.recipients) {
        for (auto it = r.constBegin(); it != r.constEnd(); ++it) {
            if (it.key() >> 16 == propDisplayName) {
                attendees.append(it->toString(m.codepage()));
            }
        }
    }
    if (!attendees.isEmpty()) {
        out.skipped.append(QStringLiteral("attendees, not invited again: %1").arg(attendees.join(QStringLiteral(", "))));
    }
    if (!m.attachments.isEmpty()) {
        out.skipped.append(QStringLiteral("%1 attachments").arg(m.attachments.size()));
    }
    return out;
}

Item task(const Pst::Message &m, const QHash<quint16, Pst::NamedProperty> &names)
{
    Item out;
    QJsonObject &j = out.json;
    j.insert(QStringLiteral("title"), m.string(propSubject));
    if (const QString text = m.string(propBody); !text.isEmpty()) {
        j.insert(QStringLiteral("body"), QJsonObject{{QStringLiteral("contentType"), QStringLiteral("text")}, {QStringLiteral("content"), text}});
    }
    static const char *const status[] = {"notStarted", "inProgress", "completed", "waitingOnOthers", "deferred"};
    if (const Pst::PropValue *v = named(m, names, psetidTask, lidTaskStatus)) {
        j.insert(QStringLiteral("status"), QString::fromLatin1(status[std::clamp<qint64>(v->toInt(), 0, 4)]));
    }
    j.insert(QStringLiteral("importance"), importance(m.properties.contains(propImportance) ? m.properties.value(propImportance).toInt() : 1));
    // Task dates are local midnights ([MS-OXOTASK] 2.2.2.2.4): the dates they name
    const auto date = [&](quint32 lid, const QString &key) {
        if (const Pst::PropValue *v = named(m, names, psetidTask, lid)) {
            if (const QDateTime t = v->toTime(); t.isValid()) {
                j.insert(key, QJsonObject{{QStringLiteral("dateTime"), QString(dateOnly(t) + QStringLiteral("T00:00:00"))}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}});
            }
        }
    };
    date(lidTaskStartDate, QStringLiteral("startDateTime"));
    date(lidTaskDueDate, QStringLiteral("dueDateTime"));
    date(lidTaskDateCompleted, QStringLiteral("completedDateTime"));
    const Pst::PropValue *reminder = named(m, names, psetidCommon, lidReminderSet);
    if (reminder && reminder->toBool()) {
        if (const Pst::PropValue *v = named(m, names, psetidCommon, lidReminderTime); v && v->toTime().isValid()) {
            j.insert(QStringLiteral("isReminderOn"), true);
            j.insert(QStringLiteral("reminderDateTime"), dateTimeIn(v->toTime(), QString()));
        }
    }
    if (const QJsonArray c = categoriesOf(m, names); !c.isEmpty()) {
        j.insert(QStringLiteral("categories"), c);
    }
    if (named(m, names, psetidTask, lidTaskRecurrence)) {
        out.skipped.append(QStringLiteral("the task's recurrence"));
    }
    if (!m.attachments.isEmpty()) {
        out.skipped.append(QStringLiteral("%1 attachments").arg(m.attachments.size()));
    }
    return out;
}

}
