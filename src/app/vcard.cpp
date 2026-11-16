// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vcard.h"

#include <QJsonArray>
#include <QStringList>

namespace GraphMail::VCard
{

namespace
{
QString text(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString().trimmed();
}

// Backslash, comma, semicolon and line ends are escaped in a text value
QString escaped(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char(','), QStringLiteral("\\,"));
    value.replace(QLatin1Char(';'), QStringLiteral("\\;"));
    value.replace(QStringLiteral("\r\n"), QStringLiteral("\\n"));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    value.remove(QLatin1Char('\r'));
    return value;
}

QString parts(const QStringList &values)
{
    QStringList list;
    for (const QString &value : values) {
        list.append(escaped(value));
    }
    return list.join(QLatin1Char(';'));
}

// A line longer than 75 octets continues on the next with a space, never inside a character
void addLine(QByteArray &out, const QString &line)
{
    const QByteArray bytes = line.toUtf8();
    qsizetype start = 0;
    qsizetype limit = 75;
    while (bytes.size() - start > limit) {
        qsizetype end = start + limit;
        while (end > start && (quint8(bytes.at(end)) & 0xC0) == 0x80) {
            --end;
        }
        out += bytes.mid(start, end - start) + "\r\n ";
        start = end;
        limit = 74;
    }
    out += bytes.mid(start) + "\r\n";
}

void addAddress(QByteArray &out, const QJsonObject &contact, const char *key, const char *type)
{
    const QJsonObject a = contact.value(QLatin1String(key)).toObject();
    const QStringList fields{text(a, "street"), text(a, "city"), text(a, "state"), text(a, "postalCode"), text(a, "countryOrRegion")};
    if (fields.join(QString()).isEmpty()) {
        return;
    }
    // Post office box, extended address, street, locality, region, postal code, country
    addLine(out, QStringLiteral("ADR;TYPE=%1:;;").arg(QLatin1String(type)) + parts(fields));
}
}

QByteArray fromContact(const QJsonObject &contact)
{
    QByteArray out("BEGIN:VCARD\r\nVERSION:3.0\r\n");
    const QString given = text(contact, "givenName");
    const QString middle = text(contact, "middleName");
    const QString surname = text(contact, "surname");
    QString name = text(contact, "displayName");
    if (name.isEmpty()) {
        name = QStringList{given, middle, surname}.join(QLatin1Char(' ')).simplified();
    }
    addLine(out, QStringLiteral("FN:") + escaped(name));
    addLine(out, QStringLiteral("N:") + parts({surname, given, middle, text(contact, "title"), text(contact, "generation")}));
    if (const QString nick = text(contact, "nickName"); !nick.isEmpty()) {
        addLine(out, QStringLiteral("NICKNAME:") + escaped(nick));
    }
    const QString company = text(contact, "companyName");
    const QString department = text(contact, "department");
    if (!company.isEmpty() || !department.isEmpty()) {
        addLine(out, QStringLiteral("ORG:") + parts(department.isEmpty() ? QStringList{company} : QStringList{company, department}));
    }
    if (const QString job = text(contact, "jobTitle"); !job.isEmpty()) {
        addLine(out, QStringLiteral("TITLE:") + escaped(job));
    }
    // Contacts have emailAddresses, directory entries mail
    QStringList addresses;
    for (const QJsonValue &v : contact.value(QLatin1String("emailAddresses")).toArray()) {
        addresses.append(v.toObject().value(QLatin1String("address")).toString().trimmed());
    }
    if (addresses.isEmpty()) {
        addresses.append(text(contact, "mail"));
    }
    bool first = true;
    for (const QString &address : std::as_const(addresses)) {
        if (!address.isEmpty()) {
            addLine(out, (first ? QStringLiteral("EMAIL;TYPE=INTERNET,PREF:") : QStringLiteral("EMAIL;TYPE=INTERNET:")) + escaped(address));
            first = false;
        }
    }
    const struct {
        const char *key;
        const char *type;
    } phoneLists[] = {{"businessPhones", "WORK,VOICE"}, {"homePhones", "HOME,VOICE"}};
    for (const auto &list : phoneLists) {
        for (const QJsonValue &v : contact.value(QLatin1String(list.key)).toArray()) {
            if (const QString phone = v.toString().trimmed(); !phone.isEmpty()) {
                addLine(out, QStringLiteral("TEL;TYPE=%1:").arg(QLatin1String(list.type)) + escaped(phone));
            }
        }
    }
    if (const QString mobile = text(contact, "mobilePhone"); !mobile.isEmpty()) {
        addLine(out, QStringLiteral("TEL;TYPE=CELL,VOICE:") + escaped(mobile));
    }
    addAddress(out, contact, "businessAddress", "WORK");
    addAddress(out, contact, "homeAddress", "HOME");
    addAddress(out, contact, "otherAddress", "POSTAL");
    if (const QString page = text(contact, "businessHomePage"); !page.isEmpty()) {
        addLine(out, QStringLiteral("URL:") + escaped(page));
    }
    // Graph's birthday is a date and time in UTC; the date is what counts
    if (const QString birthday = text(contact, "birthday"); birthday.size() >= 10) {
        addLine(out, QStringLiteral("BDAY:") + birthday.left(10));
    }
    if (const QString notes = text(contact, "personalNotes"); !notes.isEmpty()) {
        addLine(out, QStringLiteral("NOTE:") + escaped(notes));
    }
    out += "END:VCARD\r\n";
    return out;
}

}
