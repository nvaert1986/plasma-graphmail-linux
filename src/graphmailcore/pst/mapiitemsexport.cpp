// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mapiitems.h"
#include "rtf.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QtEndian>

#include <algorithm>
#include <tuple>

namespace GraphMail::MapiItems
{

namespace
{
const QUuid psetidAddress(0x00062004, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
const QUuid psetidAppointment(0x00062002, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
const QUuid psetidCommon(0x00062008, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
const QUuid psetidTask(0x00062003, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);

// [MS-OXOCAL] 2.2.1.44.1: recurrence fields
constexpr quint16 freqDaily = 0x200A;
constexpr quint16 freqWeekly = 0x200B;
constexpr quint16 freqMonthly = 0x200C;
constexpr quint16 freqYearly = 0x200D;
constexpr quint32 endAfterDate = 0x2021;
constexpr quint32 endAfterCount = 0x2022;
constexpr quint32 neverEnd = 0x2023;
constexpr quint32 noEndDate = 0x5AE980DF;
// The most occurrences counted for an end date or a count
constexpr int maxOccurrences = 5000;

void put(QByteArray &b, quint16 v)
{
    char d[2];
    qToLittleEndian(v, d);
    b.append(d, 2);
}
void put(QByteArray &b, quint32 v)
{
    char d[4];
    qToLittleEndian(v, d);
    b.append(d, 4);
}

Pst::PropValue string(const QString &s)
{
    return {Pst::PtypString, QByteArray(reinterpret_cast<const char *>(s.utf16()), s.size() * 2)};
}
Pst::PropValue int32(qint64 v)
{
    QByteArray b;
    put(b, quint32(v));
    return {Pst::PtypInteger32, b};
}
Pst::PropValue boolean(bool v)
{
    return {Pst::PtypBoolean, QByteArray(1, v ? 1 : 0)};
}
Pst::PropValue floating(double v)
{
    QByteArray b(8, 0);
    qToLittleEndian(v, b.data());
    return {Pst::PtypFloating64, b};
}
Pst::PropValue binary(const QByteArray &b)
{
    return {Pst::PtypBinary, b};
}
Pst::PropValue time(const QDateTime &t)
{
    QByteArray b(8, 0);
    qToLittleEndian(quint64((t.toMSecsSinceEpoch() + qint64(11644473600) * 1000) * 10000), b.data());
    return {Pst::PtypTime, b};
}
Pst::PropValue multiString(const QStringList &values)
{
    QByteArray items;
    QByteArray head;
    put(head, quint32(values.size()));
    const quint32 start = 4 + 4 * quint32(values.size());
    for (const QString &v : values) {
        put(head, quint32(start + quint32(items.size())));
        items += string(v).data;
    }
    return {Pst::PtypString | Pst::PtypMultiple, head + items};
}
Pst::PropValue multiInt(const QList<quint32> &values)
{
    QByteArray b;
    for (const quint32 v : values) {
        put(b, v);
    }
    return {Pst::PtypInteger32 | Pst::PtypMultiple, b};
}

QString str(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString();
}

// A Graph dateTimeTimeZone as a moment
QDateTime graphTime(const QJsonObject &dtz)
{
    const QString value = dtz.value(QLatin1String("dateTime")).toString().left(19);
    const QDateTime local = QDateTime::fromString(value, Qt::ISODate);
    if (!local.isValid()) {
        return {};
    }
    return QDateTime(local.date(), local.time(), zoneFor(dtz.value(QLatin1String("timeZone")).toString())).toUTC();
}

// Midnight of a date in a zone, as a moment (Outlook's all-day and date-only values)
QDateTime midnight(QDate date, const QTimeZone &zone)
{
    return QDateTime(date, QTime(0, 0), zone).toUTC();
}

// Minutes from midnight 1 January 1601 to midnight of a date
quint32 minutesSince1601(QDate date)
{
    return quint32(QDate(1601, 1, 1).daysTo(date) * 1440);
}

// Readable text from HTML for PidTagBody (Outlook previews and searches it)
QString plainFromHtml(const QString &html)
{
    QString t = html;
    static const QRegularExpression hidden(QStringLiteral(R"(<(script|style|head)\b.*?</\1\s*>)"), QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression breaks(QStringLiteral(R"(<(br|/p|/div|/li|/tr|/h[1-6])\b[^>]*>)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
    static const QRegularExpression blank(QStringLiteral("\\n{3,}"));
    t.remove(hidden);
    t.remove(QLatin1Char('\r'));
    t.replace(QLatin1Char('\n'), QLatin1Char(' '));
    t.replace(breaks, QStringLiteral("\n"));
    t.remove(tags);
    t.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    t.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    t.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    t.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    t.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    t.replace(blank, QStringLiteral("\n\n"));
    return t.trimmed();
}

// A Graph itemBody as PidTagBody, PidTagHtml and the compressed RTF Outlook keeps for items
void putBody(Pst::PropertyMap &p, const QJsonObject &body)
{
    const QString content = str(body, "content");
    if (content.isEmpty()) {
        return;
    }
    if (str(body, "contentType").compare(QLatin1String("html"), Qt::CaseInsensitive) == 0) {
        p.insert(0x1013, binary(content.toUtf8()));
        p.insert(0x3FDE, int32(65001));
        p.insert(0x1000, string(plainFromHtml(content)));
        p.insert(0x1009, binary(Rtf::compress(Rtf::encapsulateHtml(content))));
    } else {
        p.insert(0x1000, string(content));
        p.insert(0x1009, binary(Rtf::compress(Rtf::encapsulateText(content))));
    }
    // PidTagRtfInSync: the RTF says what the other bodies say
    p.insert(0x0E1F, boolean(true));
}

void putCategories(Pst::PropertyMap &p, const QJsonObject &o, quint16 keywords)
{
    QStringList categories;
    const QJsonArray list = o.value(QLatin1String("categories")).toArray();
    for (const QJsonValue &v : list) {
        categories.append(v.toString());
    }
    if (keywords != 0 && !categories.isEmpty()) {
        p.insert(keywords, multiString(categories));
    }
}

quint32 dayMask(const QJsonArray &days)
{
    static const QStringList names{QStringLiteral("sunday"),   QStringLiteral("monday"), QStringLiteral("tuesday"), QStringLiteral("wednesday"),
                                   QStringLiteral("thursday"), QStringLiteral("friday"), QStringLiteral("saturday")};
    quint32 mask = 0;
    for (const QJsonValue &d : days) {
        const qsizetype i = names.indexOf(d.toString().toLower());
        if (i >= 0) {
            mask |= 1u << i;
        }
    }
    return mask;
}

int dayOfWeekIndex(const QString &name)
{
    static const QStringList names{QStringLiteral("sunday"),   QStringLiteral("monday"), QStringLiteral("tuesday"), QStringLiteral("wednesday"),
                                   QStringLiteral("thursday"), QStringLiteral("friday"), QStringLiteral("saturday")};
    return std::max<int>(0, int(names.indexOf(name.toLower())));
}

// Qt's day of week (Monday 1 .. Sunday 7) as Outlook's bit (Sunday 0 .. Saturday 6)
int bitOf(QDate d)
{
    return d.dayOfWeek() % 7;
}

// The dates of a month matching a day mask, in order
QList<QDate> matching(int year, int month, quint32 mask)
{
    QList<QDate> out;
    const QDate first(year, month, 1);
    for (int d = 0; d < first.daysInMonth(); ++d) {
        const QDate day = first.addDays(d);
        if (mask & (1u << bitOf(day))) {
            out.append(day);
        }
    }
    return out;
}

// A SYSTEMTIME for a yearly rule: month, weekday, which occurrence (5 = last), local time
QByteArray systemTime(const QDateTime &localBefore)
{
    QByteArray b;
    if (!localBefore.isValid()) {
        return QByteArray(16, 0);
    }
    const QDate d = localBefore.date();
    const int week = d.day() + 7 > d.daysInMonth() ? 5 : (d.day() - 1) / 7 + 1;
    put(b, quint16(0));
    put(b, quint16(d.month()));
    put(b, quint16(bitOf(d)));
    put(b, quint16(week));
    put(b, quint16(localBefore.time().hour()));
    put(b, quint16(localBefore.time().minute()));
    put(b, quint16(0));
    put(b, quint16(0));
    return b;
}

// A year's rule: bias, standard and daylight biases, and when each begins (local time before it)
struct Rule {
    qint32 bias = 0;
    qint32 daylightBias = 0;
    QByteArray standardDate = QByteArray(16, 0);
    QByteArray daylightDate = QByteArray(16, 0);
};

Rule ruleFor(const QTimeZone &zone, int year)
{
    Rule r;
    const QDateTime from(QDate(year, 1, 1), QTime(0, 0), QTimeZone::UTC);
    const QDateTime to(QDate(year + 1, 1, 1), QTime(0, 0), QTimeZone::UTC);
    r.bias = -zone.standardTimeOffset(from) / 60;
    const QTimeZone::OffsetDataList transitions = zone.transitions(from, to);
    for (const QTimeZone::OffsetData &t : transitions) {
        // The wall clock at the change, in the offset that was in force until then
        // (kept in UTC so its fields are the wall clock's, whatever the system's zone)
        const QDateTime atUtc = t.atUtc.toUTC();
        const QDateTime shifted = atUtc.addSecs(zone.offsetFromUtc(atUtc.addSecs(-1)));
        const QDateTime wall(shifted.date(), shifted.time(), QTimeZone::UTC);
        if (t.daylightTimeOffset > 0) {
            r.daylightBias = -t.daylightTimeOffset / 60;
            r.daylightDate = systemTime(wall);
        } else {
            r.standardDate = systemTime(wall);
        }
    }
    if (r.daylightDate == QByteArray(16, 0) || r.standardDate == QByteArray(16, 0)) {
        // No daylight saving this year: both dates empty (wMonth 0)
        r.daylightBias = 0;
        r.standardDate = QByteArray(16, 0);
        r.daylightDate = QByteArray(16, 0);
    }
    return r;
}

void recipientRows(const QJsonObject &event, Pst::Message &m)
{
    const QJsonArray attendees = event.value(QLatin1String("attendees")).toArray();
    for (const QJsonValue &v : attendees) {
        const QJsonObject a = v.toObject();
        const QJsonObject e = a.value(QLatin1String("emailAddress")).toObject();
        const QString address = str(e, "address");
        const QString name = str(e, "name").isEmpty() ? address : str(e, "name");
        const QString type = str(a, "type");
        // [MS-OXOCAL]: required as To, optional as Cc, resources as Bcc
        const int recipientType = type == QLatin1String("optional") ? 2 : type == QLatin1String("resource") ? 3 : 1;
        QHash<quint32, Pst::PropValue> row;
        row.insert(0x0C150003, int32(recipientType));
        row.insert(0x3001001F, string(name));
        row.insert(0x3002001F, string(QStringLiteral("SMTP")));
        row.insert(0x3003001F, string(address));
        row.insert(0x39FE001F, string(address));
        row.insert(0x0FFE0003, int32(6));
        // recipSendable
        row.insert(0x5FFD0003, int32(1));
        m.recipients.append(row);
    }
}
}

QTimeZone zoneFor(const QString &name)
{
    if (name.isEmpty() || name == QLatin1String("UTC") || name.contains(QLatin1String("Microsoft/Utc"), Qt::CaseInsensitive)
        || name == QLatin1String("Coordinated Universal Time")) {
        return QTimeZone::UTC;
    }
    const QByteArray iana = QTimeZone::windowsIdToDefaultIanaId(name.toUtf8());
    if (!iana.isEmpty()) {
        const QTimeZone z(iana);
        if (z.isValid()) {
            return z;
        }
    }
    const QTimeZone direct(name.toUtf8());
    return direct.isValid() ? direct : QTimeZone::systemTimeZone();
}

QByteArray timeZoneStruct(const QTimeZone &zone, int year)
{
    const Rule r = ruleFor(zone, year);
    QByteArray b;
    put(b, quint32(r.bias));
    put(b, quint32(0));
    put(b, quint32(r.daylightBias));
    put(b, quint16(0));
    b += r.standardDate;
    put(b, quint16(0));
    b += r.daylightDate;
    return b;
}

QByteArray timeZoneDefinition(const QTimeZone &zone, const QString &keyName, const QList<int> &years, int effectiveYear)
{
    const QString key = keyName.left(260);
    QByteArray b;
    b += char(2);
    b += char(1);
    put(b, quint16(6 + 2 * key.size()));
    put(b, quint16(2));
    put(b, quint16(key.size()));
    b.append(reinterpret_cast<const char *>(key.utf16()), key.size() * 2);
    put(b, quint16(years.size()));
    for (const int year : years) {
        const Rule r = ruleFor(zone, year);
        b += char(2);
        b += char(1);
        put(b, quint16(0x003E));
        // TZRULE_FLAG_EFFECTIVE_TZREG on the rule in force
        put(b, quint16(year == effectiveYear ? 0x0002 : 0));
        put(b, quint16(year));
        b += QByteArray(14, 0);
        put(b, quint32(r.bias));
        put(b, quint32(0));
        put(b, quint32(r.daylightBias));
        b += r.standardDate;
        b += r.daylightDate;
    }
    return b;
}

QList<QDate> occurrences(const QJsonObject &recurrence, int count, QDate until)
{
    const QJsonObject p = recurrence.value(QLatin1String("pattern")).toObject();
    const QJsonObject range = recurrence.value(QLatin1String("range")).toObject();
    const QString type = str(p, "type");
    const int interval = std::max(1, p.value(QLatin1String("interval")).toInt(1));
    const QDate start = QDate::fromString(str(range, "startDate"), Qt::ISODate);
    QList<QDate> out;
    if (!start.isValid() || count <= 0) {
        return out;
    }
    const auto take = [&](QDate d) {
        if (d < start || (until.isValid() && d > until) || out.size() >= count) {
            return false;
        }
        out.append(d);
        return true;
    };
    // Enough periods to find count dates or pass until; a bounded loop either way
    const int periods = maxOccurrences * 2;
    const quint32 mask = dayMask(p.value(QLatin1String("daysOfWeek")).toArray());
    const QString index = str(p, "index").toLower();
    static const QStringList indexes{QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("third"), QStringLiteral("fourth"), QStringLiteral("last")};
    const auto nth = [&](int year, int month) -> QDate {
        const QList<QDate> days = matching(year, month, mask);
        if (days.isEmpty()) {
            return {};
        }
        const qsizetype i = indexes.indexOf(index);
        return i == 4 || i < 0 ? (i < 0 ? days.first() : days.last()) : days.value(i);
    };
    for (int k = 0; k < periods && out.size() < count; ++k) {
        if (type == QLatin1String("daily")) {
            const QDate d = start.addDays(qint64(k) * interval);
            if (until.isValid() && d > until) {
                break;
            }
            take(d);
        } else if (type == QLatin1String("weekly")) {
            const int firstDow = dayOfWeekIndex(str(p, "firstDayOfWeek").isEmpty() ? QStringLiteral("sunday") : str(p, "firstDayOfWeek"));
            const QDate weekStart = start.addDays(-((bitOf(start) - firstDow + 7) % 7)).addDays(qint64(k) * 7 * interval);
            if (until.isValid() && weekStart > until) {
                break;
            }
            for (int i = 0; i < 7; ++i) {
                const QDate d = weekStart.addDays(i);
                if (mask & (1u << bitOf(d))) {
                    take(d);
                }
            }
        } else if (type == QLatin1String("absoluteMonthly") || type == QLatin1String("relativeMonthly") || type == QLatin1String("absoluteYearly")
                   || type == QLatin1String("relativeYearly")) {
            const bool yearly = type.endsWith(QLatin1String("Yearly"));
            const QDate first(start.year(), yearly ? std::clamp(p.value(QLatin1String("month")).toInt(start.month()), 1, 12) : start.month(), 1);
            const QDate month = first.addMonths(qint64(k) * interval * (yearly ? 12 : 1));
            if (until.isValid() && month > until) {
                break;
            }
            QDate d;
            if (type.startsWith(QLatin1String("absolute"))) {
                d = QDate(month.year(), month.month(), std::min(p.value(QLatin1String("dayOfMonth")).toInt(1), month.daysInMonth()));
            } else {
                d = nth(month.year(), month.month());
            }
            if (d.isValid()) {
                take(d);
            }
        } else {
            break;
        }
    }
    return out;
}

QByteArray recurrenceBlob(const QJsonObject &recurrence, const QDateTime &start, int durationMinutes, const QList<QDate> &deleted, QStringList *notes)
{
    const QJsonObject p = recurrence.value(QLatin1String("pattern")).toObject();
    const QJsonObject range = recurrence.value(QLatin1String("range")).toObject();
    const QString type = str(p, "type");
    const int interval = std::max(1, p.value(QLatin1String("interval")).toInt(1));
    const quint32 mask = dayMask(p.value(QLatin1String("daysOfWeek")).toArray());
    const QString firstDayName = str(p, "firstDayOfWeek");
    const quint32 firstDow = quint32(dayOfWeekIndex(firstDayName.isEmpty() ? QStringLiteral("sunday") : firstDayName));
    static const QStringList indexes{QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("third"), QStringLiteral("fourth"), QStringLiteral("last")};

    quint16 frequency = 0;
    quint16 patternType = 0;
    quint32 period = 0;
    QByteArray specific;
    if (type == QLatin1String("daily")) {
        frequency = freqDaily;
        period = quint32(interval) * 1440;
    } else if (type == QLatin1String("weekly")) {
        frequency = freqWeekly;
        patternType = 1;
        period = quint32(interval);
        put(specific, mask);
    } else if (type == QLatin1String("absoluteMonthly") || type == QLatin1String("absoluteYearly")) {
        const bool yearly = type == QLatin1String("absoluteYearly");
        frequency = yearly ? freqYearly : freqMonthly;
        patternType = 2;
        period = quint32(interval) * (yearly ? 12 : 1);
        put(specific, quint32(std::clamp(p.value(QLatin1String("dayOfMonth")).toInt(1), 1, 31)));
    } else if (type == QLatin1String("relativeMonthly") || type == QLatin1String("relativeYearly")) {
        const bool yearly = type == QLatin1String("relativeYearly");
        frequency = yearly ? freqYearly : freqMonthly;
        patternType = 3;
        period = quint32(interval) * (yearly ? 12 : 1);
        put(specific, mask);
        put(specific, quint32(std::max<qsizetype>(0, indexes.indexOf(str(p, "index").toLower())) + 1));
    } else {
        if (notes) {
            notes->append(QStringLiteral("a series pattern Outlook cannot hold (%1)").arg(type));
        }
        return {};
    }

    // The first occurrence, the count and the end, from the pattern itself
    const QList<QDate> first = occurrences(recurrence, 1);
    if (first.isEmpty()) {
        if (notes) {
            notes->append(QStringLiteral("a series without occurrences"));
        }
        return {};
    }
    const QDate startDate = first.first();
    quint32 endType = neverEnd;
    quint32 count = 10;
    quint32 endDate = noEndDate;
    const QString rangeType = str(range, "type");
    if (rangeType == QLatin1String("endDate")) {
        const QDate until = QDate::fromString(str(range, "endDate"), Qt::ISODate);
        const QList<QDate> all = occurrences(recurrence, maxOccurrences, until);
        endType = endAfterDate;
        count = quint32(all.size());
        endDate = minutesSince1601(all.isEmpty() ? until : all.last());
    } else if (rangeType == QLatin1String("numbered")) {
        const int n = std::clamp(range.value(QLatin1String("numberOfOccurrences")).toInt(1), 1, maxOccurrences);
        const QList<QDate> all = occurrences(recurrence, n);
        endType = endAfterCount;
        count = quint32(n);
        endDate = minutesSince1601(all.isEmpty() ? startDate : all.last());
    }

    // FirstDateTime ([MS-OXOCAL] 2.2.1.44.1.1)
    const quint32 startMinutes = minutesSince1601(startDate);
    quint32 firstDateTime = 0;
    if (frequency == freqDaily) {
        firstDateTime = startMinutes % period;
    } else if (frequency == freqWeekly) {
        const QDate weekStart = startDate.addDays(-((bitOf(startDate) - int(firstDow) + 7) % 7));
        firstDateTime = minutesSince1601(weekStart) % (period * 10080);
    } else {
        const int months = (startDate.year() - 1601) * 12 + startDate.month() - 1;
        firstDateTime = minutesSince1601(QDate(1601, 1, 1).addMonths(months % int(period)));
    }

    QByteArray b;
    put(b, quint16(0x3004));
    put(b, quint16(0x3004));
    put(b, frequency);
    put(b, patternType);
    put(b, quint16(0));
    put(b, firstDateTime);
    put(b, period);
    put(b, quint32(0));
    b += specific;
    put(b, endType);
    put(b, count);
    put(b, firstDow);
    QList<QDate> gone = deleted;
    std::sort(gone.begin(), gone.end());
    gone.erase(std::unique(gone.begin(), gone.end()), gone.end());
    put(b, quint32(gone.size()));
    for (const QDate d : std::as_const(gone)) {
        put(b, minutesSince1601(d));
    }
    // Changed occurrences go along as appointments of their own, not as modified instances
    put(b, quint32(0));
    put(b, startMinutes);
    put(b, endDate);
    // AppointmentRecurrencePattern (2.2.1.44.5)
    const int startOffset = start.time().hour() * 60 + start.time().minute();
    put(b, quint32(0x3006));
    put(b, quint32(0x3009));
    put(b, quint32(startOffset));
    put(b, quint32(startOffset + std::max(0, durationMinutes)));
    put(b, quint16(0));
    put(b, quint32(0));
    put(b, quint32(0));
    return b;
}

Pst::Message contactToMapi(const QJsonObject &c, const NameId &name, quint16 keywords)
{
    Pst::Message m;
    Pst::PropertyMap &p = m.properties;
    const auto set = [&p](quint16 id, const QString &v) {
        if (!v.isEmpty()) {
            p.insert(id, string(v));
        }
    };
    p.insert(0x001A, string(QStringLiteral("IPM.Contact")));
    p.insert(0x0E07, int32(1));
    const QString display = str(c, "displayName");
    set(0x3001, display);
    set(0x0037, display);
    set(0x3A06, str(c, "givenName"));
    set(0x3A44, str(c, "middleName"));
    set(0x3A11, str(c, "surname"));
    set(0x3A45, str(c, "title"));
    set(0x3A05, str(c, "generation"));
    set(0x3A0A, str(c, "initials"));
    set(0x3A4F, str(c, "nickName"));
    set(0x3A17, str(c, "jobTitle"));
    set(0x3A16, str(c, "companyName"));
    set(0x3A18, str(c, "department"));
    set(0x3A19, str(c, "officeLocation"));
    set(0x3A46, str(c, "profession"));
    set(0x3A4E, str(c, "manager"));
    set(0x3A30, str(c, "assistantName"));
    set(0x3A48, str(c, "spouseName"));
    set(0x3A51, str(c, "businessHomePage"));
    set(0x3A1C, str(c, "mobilePhone"));
    const auto phones = [&](const char *key, quint16 first, quint16 second) {
        const QJsonArray list = c.value(QLatin1String(key)).toArray();
        set(first, list.at(0).toString());
        set(second, list.at(1).toString());
    };
    phones("businessPhones", 0x3A08, 0x3A1B);
    phones("homePhones", 0x3A09, 0x3A2F);
    if (const QString notes = str(c, "personalNotes"); !notes.isEmpty()) {
        putBody(p, QJsonObject{{QStringLiteral("contentType"), QStringLiteral("text")}, {QStringLiteral("content"), notes}});
    }
    if (const QDate birthday = QDate::fromString(str(c, "birthday").left(10), Qt::ISODate); birthday.isValid()) {
        // A date: local midnight, as Outlook keeps it
        p.insert(0x3A42, time(midnight(birthday, QTimeZone::systemTimeZone())));
    }
    if (const QString fileAs = str(c, "fileAs"); !fileAs.isEmpty()) {
        p.insert(name(psetidAddress, 0x8005), string(fileAs));
    }
    // Email1-3, and the list Outlook's address book reads them from
    static const quint32 lids[3][4] = {{0x8080, 0x8082, 0x8083, 0x8084}, {0x8090, 0x8092, 0x8093, 0x8094}, {0x80A0, 0x80A2, 0x80A3, 0x80A4}};
    const QJsonArray emails = c.value(QLatin1String("emailAddresses")).toArray();
    QList<quint32> present;
    for (int i = 0; i < 3 && i < emails.size(); ++i) {
        const QJsonObject e = emails.at(i).toObject();
        const QString address = str(e, "address");
        if (address.isEmpty()) {
            continue;
        }
        const QString shown = str(e, "name").isEmpty() ? address : str(e, "name");
        p.insert(name(psetidAddress, lids[i][0]), string(shown == address ? address : QStringLiteral("%1 (%2)").arg(shown, address)));
        p.insert(name(psetidAddress, lids[i][1]), string(QStringLiteral("SMTP")));
        p.insert(name(psetidAddress, lids[i][2]), string(address));
        p.insert(name(psetidAddress, lids[i][3]), string(address));
        present.append(quint32(i));
    }
    if (!present.isEmpty()) {
        quint32 bits = 0;
        for (const quint32 i : std::as_const(present)) {
            bits |= 1u << i;
        }
        p.insert(name(psetidAddress, 0x8029), int32(bits));
        p.insert(name(psetidAddress, 0x8028), multiInt(present));
    }
    if (const QJsonArray im = c.value(QLatin1String("imAddresses")).toArray(); !im.isEmpty()) {
        p.insert(name(psetidAddress, 0x8062), string(im.first().toString()));
    }
    const auto address = [&](const char *key, quint16 street, quint16 city, quint16 state, quint16 postal, quint16 country) {
        const QJsonObject a = c.value(QLatin1String(key)).toObject();
        set(street, str(a, "street"));
        set(city, str(a, "city"));
        set(state, str(a, "state"));
        set(postal, str(a, "postalCode"));
        set(country, str(a, "countryOrRegion"));
    };
    address("homeAddress", 0x3A5D, 0x3A59, 0x3A5C, 0x3A5B, 0x3A5A);
    address("otherAddress", 0x3A63, 0x3A5F, 0x3A62, 0x3A61, 0x3A60);
    const QJsonObject work = c.value(QLatin1String("businessAddress")).toObject();
    const auto workField = [&](const char *key, quint32 lid) {
        if (const QString v = str(work, key); !v.isEmpty()) {
            p.insert(name(psetidAddress, lid), string(v));
        }
    };
    workField("street", 0x8045);
    workField("city", 0x8046);
    workField("state", 0x8047);
    workField("postalCode", 0x8048);
    workField("countryOrRegion", 0x8049);
    putCategories(p, c, keywords);
    return m;
}

Pst::Message eventToMapi(const QJsonObject &e, const NameId &name, quint16 keywords, const QList<QDate> &deleted)
{
    Pst::Message m;
    Pst::PropertyMap &p = m.properties;
    p.insert(0x001A, string(QStringLiteral("IPM.Appointment")));
    p.insert(0x0E07, int32(1));
    p.insert(0x0037, string(str(e, "subject")));
    putBody(p, e.value(QLatin1String("body")).toObject());
    if (const QString where = str(e.value(QLatin1String("location")).toObject(), "displayName"); !where.isEmpty()) {
        p.insert(name(psetidAppointment, 0x8208), string(where));
    }

    // The zone it was made in keeps the series at its local time
    const QString zoneName = str(e, "originalStartTimeZone").isEmpty() ? str(e.value(QLatin1String("start")).toObject(), "timeZone") : str(e, "originalStartTimeZone");
    const QTimeZone zone = zoneFor(zoneName);
    const bool allDay = e.value(QLatin1String("isAllDay")).toBool();
    QDateTime start = graphTime(e.value(QLatin1String("start")).toObject());
    QDateTime end = graphTime(e.value(QLatin1String("end")).toObject());
    if (allDay) {
        // Whole days: midnight to midnight where the user is
        const QDate first = QDate::fromString(str(e.value(QLatin1String("start")).toObject(), "dateTime").left(10), Qt::ISODate);
        const QDate last = QDate::fromString(str(e.value(QLatin1String("end")).toObject(), "dateTime").left(10), Qt::ISODate);
        start = midnight(first, QTimeZone::systemTimeZone());
        end = midnight(last.isValid() && last > first ? last : first.addDays(1), QTimeZone::systemTimeZone());
        p.insert(name(psetidAppointment, 0x8215), boolean(true));
    }
    if (start.isValid()) {
        if (!end.isValid() || end < start) {
            end = start;
        }
        for (const auto &[set, lid, value] : {std::tuple{psetidAppointment, 0x820Du, start}, std::tuple{psetidAppointment, 0x820Eu, end},
                                               std::tuple{psetidCommon, 0x8516u, start}, std::tuple{psetidCommon, 0x8517u, end}}) {
            p.insert(name(set, lid), time(value));
        }
        p.insert(0x0060, time(start));
        p.insert(0x0061, time(end));
        p.insert(name(psetidAppointment, 0x8213), int32(start.secsTo(end) / 60));
        p.insert(name(psetidAppointment, 0x8235), time(start));
        p.insert(name(psetidAppointment, 0x8236), time(end));
    }

    static const QStringList showAs{QStringLiteral("free"), QStringLiteral("tentative"), QStringLiteral("busy"), QStringLiteral("oof"), QStringLiteral("workingElsewhere")};
    const qsizetype busy = showAs.indexOf(str(e, "showAs"));
    p.insert(name(psetidAppointment, 0x8205), int32(busy < 0 ? 2 : busy));
    static const QStringList sensitivity{QStringLiteral("normal"), QStringLiteral("personal"), QStringLiteral("private"), QStringLiteral("confidential")};
    p.insert(0x0036, int32(std::max<qsizetype>(0, sensitivity.indexOf(str(e, "sensitivity")))));
    static const QStringList importance{QStringLiteral("low"), QStringLiteral("normal"), QStringLiteral("high")};
    const qsizetype imp = importance.indexOf(str(e, "importance"));
    p.insert(0x0017, int32(imp < 0 ? 1 : imp));
    const bool reminder = e.value(QLatin1String("isReminderOn")).toBool();
    p.insert(name(psetidCommon, 0x8503), boolean(reminder));
    if (reminder && start.isValid()) {
        const int minutes = e.value(QLatin1String("reminderMinutesBeforeStart")).toInt(15);
        p.insert(name(psetidCommon, 0x8501), int32(minutes));
        p.insert(name(psetidCommon, 0x8502), time(start));
        p.insert(name(psetidCommon, 0x8560), time(start.addSecs(-60 * qint64(minutes))));
    }
    putCategories(p, e, keywords);

    // A meeting: its attendees as recipients, its organizer as the sender
    if (!e.value(QLatin1String("attendees")).toArray().isEmpty()) {
        recipientRows(e, m);
        const QJsonObject organizer = e.value(QLatin1String("organizer")).toObject().value(QLatin1String("emailAddress")).toObject();
        if (const QString address = str(organizer, "address"); !address.isEmpty()) {
            p.insert(0x0042, string(str(organizer, "name").isEmpty() ? address : str(organizer, "name")));
            p.insert(0x0064, string(QStringLiteral("SMTP")));
            p.insert(0x0065, string(address));
            p.insert(0x5D02, string(address));
        }
        // asfMeeting, and asfReceived when someone else organised it
        p.insert(name(psetidAppointment, 0x8217), int32(e.value(QLatin1String("isOrganizer")).toBool(true) ? 1 : 3));
    }

    // Time zones: the definition the start and end were made in, and for a series its rule
    if (start.isValid() && zone != QTimeZone(QTimeZone::UTC)) {
        const QDateTime local = start.toTimeZone(zone);
        const int now = QDate::currentDate().year();
        const QList<int> years = local.date().year() < now ? QList<int>{local.date().year(), now} : QList<int>{local.date().year()};
        const QByteArray definition = timeZoneDefinition(zone, zoneName, years, years.last());
        p.insert(name(psetidAppointment, 0x825E), binary(definition));
        p.insert(name(psetidAppointment, 0x825F), binary(definition));
    }
    const QJsonObject recurrence = e.value(QLatin1String("recurrence")).toObject();
    if (!recurrence.isEmpty() && start.isValid()) {
        const QString seriesZoneName = str(recurrence.value(QLatin1String("range")).toObject(), "recurrenceTimeZone").isEmpty()
            ? zoneName
            : str(recurrence.value(QLatin1String("range")).toObject(), "recurrenceTimeZone");
        const QTimeZone seriesZone = zoneFor(seriesZoneName);
        const QDateTime local = start.toTimeZone(seriesZone);
        QStringList notes;
        const QByteArray blob = recurrenceBlob(recurrence, QDateTime(local.date(), local.time()), int(start.secsTo(end) / 60), deleted, &notes);
        if (!blob.isEmpty()) {
            p.insert(name(psetidAppointment, 0x8223), boolean(true));
            p.insert(name(psetidAppointment, 0x8216), binary(blob));
            const QString type = str(recurrence.value(QLatin1String("pattern")).toObject(), "type");
            p.insert(name(psetidAppointment, 0x8231),
                     int32(type == QLatin1String("daily") ? 1 : type == QLatin1String("weekly") ? 2 : type.endsWith(QLatin1String("Monthly")) ? 3 : 4));
            const int year = local.date().year();
            p.insert(name(psetidAppointment, 0x8260), binary(timeZoneDefinition(seriesZone, seriesZoneName, {year}, year)));
            p.insert(name(psetidAppointment, 0x8233), binary(timeZoneStruct(seriesZone, year)));
            p.insert(name(psetidAppointment, 0x8234), string(seriesZoneName));
            // ClipStart and ClipEnd: midnights of the first and last day, or the end of time
            const QList<QDate> first = occurrences(recurrence, 1);
            if (!first.isEmpty()) {
                p.insert(name(psetidAppointment, 0x8235), time(midnight(first.first(), seriesZone)));
            }
            const QString rangeType = str(recurrence.value(QLatin1String("range")).toObject(), "type");
            if (rangeType == QLatin1String("noEnd")) {
                p.insert(name(psetidAppointment, 0x8236), time(QDateTime(QDate(4500, 8, 31), QTime(23, 59), QTimeZone::UTC)));
            } else {
                const QList<QDate> all = occurrences(recurrence, maxOccurrences,
                                                     rangeType == QLatin1String("endDate")
                                                         ? QDate::fromString(str(recurrence.value(QLatin1String("range")).toObject(), "endDate"), Qt::ISODate)
                                                         : QDate());
                const int n = rangeType == QLatin1String("numbered") ? recurrence.value(QLatin1String("range")).toObject().value(QLatin1String("numberOfOccurrences")).toInt(1)
                                                                       : int(all.size());
                if (!all.isEmpty()) {
                    p.insert(name(psetidAppointment, 0x8236), time(midnight(all.at(std::clamp(n, 1, int(all.size())) - 1), seriesZone)));
                }
            }
        }
    }
    return m;
}

Pst::Message taskToMapi(const QJsonObject &t, const NameId &name, quint16 keywords)
{
    Pst::Message m;
    Pst::PropertyMap &p = m.properties;
    p.insert(0x001A, string(QStringLiteral("IPM.Task")));
    p.insert(0x0E07, int32(1));
    p.insert(0x0037, string(str(t, "title")));
    putBody(p, t.value(QLatin1String("body")).toObject());
    static const QStringList statuses{QStringLiteral("notStarted"), QStringLiteral("inProgress"), QStringLiteral("completed"), QStringLiteral("waitingOnOthers"),
                                      QStringLiteral("deferred")};
    const qsizetype status = std::max<qsizetype>(0, statuses.indexOf(str(t, "status")));
    p.insert(name(psetidTask, 0x8101), int32(status));
    // [MS-OXOTASK]: 0 when not started, between 0 and 1 in progress, 1 when complete
    p.insert(name(psetidTask, 0x8102), floating(status == 2 ? 1.0 : status == 1 ? 0.5 : 0.0));
    p.insert(name(psetidTask, 0x811C), boolean(status == 2));
    static const QStringList importance{QStringLiteral("low"), QStringLiteral("normal"), QStringLiteral("high")};
    const qsizetype imp = importance.indexOf(str(t, "importance"));
    p.insert(0x0017, int32(imp < 0 ? 1 : imp));
    // Dates as local midnights ([MS-OXOTASK] 2.2.2.2.4), their moments for the common start and end
    const auto date = [&](const char *key, quint32 lid, quint32 common) {
        // To Do keeps a date as midnight of that date: the date as written, not moved by zones
        const QDate day = QDate::fromString(str(t.value(QLatin1String(key)).toObject(), "dateTime").left(10), Qt::ISODate);
        if (!day.isValid()) {
            return;
        }
        p.insert(name(psetidTask, lid), time(QDateTime(day, QTime(0, 0), QTimeZone::UTC)));
        if (common != 0) {
            p.insert(name(psetidCommon, common), time(midnight(day, QTimeZone::systemTimeZone())));
        }
    };
    date("startDateTime", 0x8104, 0x8516);
    date("dueDateTime", 0x8105, 0x8517);
    date("completedDateTime", 0x810F, 0);
    const bool reminder = t.value(QLatin1String("isReminderOn")).toBool();
    p.insert(name(psetidCommon, 0x8503), boolean(reminder));
    if (const QDateTime at = graphTime(t.value(QLatin1String("reminderDateTime")).toObject()); reminder && at.isValid()) {
        p.insert(name(psetidCommon, 0x8502), time(at));
        p.insert(name(psetidCommon, 0x8560), time(at));
    }
    putCategories(p, t, keywords);
    return m;
}

}
