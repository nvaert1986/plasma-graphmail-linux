// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mapigraph.h"
#include "rtf.h"

#include <QJsonArray>
#include <QTimeZone>

namespace GraphMail::MapiGraph
{

namespace
{
// [MS-OXPROPS]
constexpr quint16 propImportance = 0x0017;
constexpr quint16 propSensitivity = 0x0036;
constexpr quint16 propSubject = 0x0037;
constexpr quint16 propInternetMessageId = 0x1035;
constexpr quint16 propClientSubmitTime = 0x0039;
constexpr quint16 propSentRepresentingName = 0x0042;
constexpr quint16 propSentRepresentingAddressType = 0x0064;
constexpr quint16 propSentRepresentingEmailAddress = 0x0065;
constexpr quint16 propRecipientType = 0x0C15;
constexpr quint16 propSenderName = 0x0C1A;
constexpr quint16 propSenderAddressType = 0x0C1E;
constexpr quint16 propSenderEmailAddress = 0x0C1F;
constexpr quint16 propMessageDeliveryTime = 0x0E06;
constexpr quint16 propMessageFlags = 0x0E07;
constexpr quint16 propBody = 0x1000;
constexpr quint16 propRtfCompressed = 0x1009;
constexpr quint16 propHtml = 0x1013;
constexpr quint16 propFlagStatus = 0x1090;
constexpr quint16 propDisplayName = 0x3001;
constexpr quint16 propAddressType = 0x3002;
constexpr quint16 propEmailAddress = 0x3003;
constexpr quint16 propAttachDataBinary = 0x3701;
constexpr quint16 propAttachFilename = 0x3704;
constexpr quint16 propAttachMethod = 0x3705;
constexpr quint16 propAttachLongFilename = 0x3707;
constexpr quint16 propAttachMimeTag = 0x370E;
constexpr quint16 propAttachContentId = 0x3712;
constexpr quint16 propAttachFlags = 0x3714;
constexpr quint16 propSmtpAddress = 0x39FE;
constexpr quint16 propInternetCodepage = 0x3FDE;
constexpr quint16 propSenderSmtpAddress = 0x5D01;
constexpr quint16 propSentRepresentingSmtpAddress = 0x5D02;
constexpr quint16 propAttachmentHidden = 0x7FFE;
// [MS-OXCMSG] 2.2.1.6 and 2.2.2.9, 2.2.2.18; [MS-OXOFLAG] 2.2.1.1
constexpr qint64 mfRead = 0x01;
constexpr qint64 afByValue = 1;
constexpr qint64 afEmbeddedMessage = 5;
constexpr qint64 attRenderedInBody = 0x04;
constexpr qint64 followupComplete = 1;
constexpr qint64 followupFlagged = 2;

// A cell of a recipient row by property ID, whatever its string type
QString cellString(const QHash<quint32, Pst::PropValue> &row, quint16 propId, int codepage)
{
    for (auto it = row.constBegin(); it != row.constEnd(); ++it) {
        if (it.key() >> 16 == propId) {
            return it->toString(codepage);
        }
    }
    return {};
}
qint64 cellInt(const QHash<quint32, Pst::PropValue> &row, quint16 propId)
{
    for (auto it = row.constBegin(); it != row.constEnd(); ++it) {
        if (it.key() >> 16 == propId) {
            return it->toInt();
        }
    }
    return 0;
}

QJsonObject address(const QString &name, const QString &smtp)
{
    QJsonObject a{{QStringLiteral("address"), smtp}};
    if (!name.isEmpty()) {
        a.insert(QStringLiteral("name"), name);
    }
    return QJsonObject{{QStringLiteral("emailAddress"), a}};
}

QJsonObject extended(const QString &id, const QString &value)
{
    return QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("value"), value}};
}

QString isoTime(const Pst::PropValue &v)
{
    const QDateTime t = v.toTime();
    return t.isValid() ? t.toTimeZone(QTimeZone::UTC).toString(Qt::ISODate) : QString();
}

// The message part shared by mail and attached messages
QJsonObject messageJson(const Pst::Message &m, const QHash<quint16, Pst::NamedProperty> &names, QStringList &skipped)
{
    const Pst::PropertyMap &p = m.properties;
    const int codepage = m.codepage();
    QJsonObject json;
    json.insert(QStringLiteral("subject"), m.string(propSubject));

    const Body body = bestBody(m);
    json.insert(QStringLiteral("body"),
                QJsonObject{{QStringLiteral("contentType"), body.html ? QStringLiteral("html") : QStringLiteral("text")}, {QStringLiteral("content"), body.content}});
    if (!body.note.isEmpty()) {
        skipped.append(body.note);
    }

    const QString from = smtpAddress(m.string(propSentRepresentingSmtpAddress), m.string(propSentRepresentingEmailAddress), m.string(propSentRepresentingAddressType));
    if (!from.isEmpty()) {
        json.insert(QStringLiteral("from"), address(m.string(propSentRepresentingName), from));
    }
    const QString sender = smtpAddress(m.string(propSenderSmtpAddress), m.string(propSenderEmailAddress), m.string(propSenderAddressType));
    if (!sender.isEmpty()) {
        json.insert(QStringLiteral("sender"), address(m.string(propSenderName), sender));
    }

    // Recipients by PidTagRecipientType: 1 To, 2 Cc, 3 Bcc
    QJsonArray to, cc, bcc;
    for (const QHash<quint32, Pst::PropValue> &r : m.recipients) {
        const QString name = cellString(r, propDisplayName, codepage);
        const QString smtp = smtpAddress(cellString(r, propSmtpAddress, codepage), cellString(r, propEmailAddress, codepage), cellString(r, propAddressType, codepage));
        if (smtp.isEmpty()) {
            skipped.append(QStringLiteral("the recipient %1 (no SMTP address)").arg(name));
            continue;
        }
        const qint64 type = cellInt(r, propRecipientType) & 0x0F;
        (type == 2 ? cc : type == 3 ? bcc : to).append(address(name, smtp));
    }
    json.insert(QStringLiteral("toRecipients"), to);
    json.insert(QStringLiteral("ccRecipients"), cc);
    json.insert(QStringLiteral("bccRecipients"), bcc);

    const qint64 importance = p.contains(propImportance) ? p.value(propImportance).toInt() : 1;
    json.insert(QStringLiteral("importance"), importance == 0 ? QStringLiteral("low") : importance == 2 ? QStringLiteral("high") : QStringLiteral("normal"));
    json.insert(QStringLiteral("isRead"), bool(p.value(propMessageFlags).toInt() & mfRead));

    // Categories: the named property Keywords of PS_PUBLIC_STRINGS
    for (auto it = names.constBegin(); it != names.constEnd(); ++it) {
        if (it->isString && it->guid == Pst::psPublicStrings && it->name == QLatin1String("Keywords") && p.contains(it.key())) {
            QJsonArray categories;
            const Pst::PropValue v = p.value(it.key());
            const QList<QByteArray> items = v.items();
            for (const QByteArray &item : items) {
                categories.append(Pst::PropValue{quint16(v.type & ~Pst::PtypMultiple), item}.toString(codepage));
            }
            json.insert(QStringLiteral("categories"), categories);
        }
    }

    const qint64 flag = p.value(propFlagStatus).toInt();
    if (flag == followupFlagged || flag == followupComplete) {
        json.insert(QStringLiteral("flag"),
                    QJsonObject{{QStringLiteral("flagStatus"), flag == followupFlagged ? QStringLiteral("flagged") : QStringLiteral("complete")}});
    }

    // Received mail, not a draft: message flags without mfUnsent, and the original dates
    QJsonArray props{extended(QStringLiteral("Integer 0x0E07"), QString::number(p.value(propMessageFlags).toInt() & mfRead))};
    if (const QString t = isoTime(p.value(propMessageDeliveryTime)); !t.isEmpty()) {
        props.append(extended(QStringLiteral("SystemTime 0x0E06"), t));
    }
    if (const QString t = isoTime(p.value(propClientSubmitTime)); !t.isEmpty()) {
        props.append(extended(QStringLiteral("SystemTime 0x0039"), t));
    }
    if (p.contains(propSensitivity)) {
        props.append(extended(QStringLiteral("Integer 0x0036"), QString::number(p.value(propSensitivity).toInt())));
    }
    json.insert(QStringLiteral("singleValueExtendedProperties"), props);
    return json;
}
}

Body bestBody(const Pst::Message &m)
{
    const Pst::PropertyMap &p = m.properties;
    Body body;
    if (p.contains(propHtml)) {
        const int codepage = p.contains(propInternetCodepage) ? int(p.value(propInternetCodepage).toInt()) : 65001;
        body.html = true;
        body.content = Pst::decodeCodepage(p.value(propHtml).data, codepage);
        return body;
    }
    std::optional<Rtf::Body> rtf;
    if (p.contains(propRtfCompressed)) {
        QString error;
        if (const std::optional<QByteArray> raw = Rtf::decompress(p.value(propRtfCompressed).data, &error)) {
            rtf = Rtf::deencapsulate(*raw);
        } else if (!p.contains(propBody)) {
            body.note = QStringLiteral("body (damaged RTF: %1)").arg(error);
        }
    }
    if (rtf && rtf->kind == Rtf::Body::Kind::Html) {
        body.html = true;
        body.content = rtf->content;
        return body;
    }
    if (p.contains(propBody)) {
        body.content = m.string(propBody);
        return body;
    }
    // Outlook gives contacts and tasks without notes an RTF body of one empty paragraph
    if (rtf && !rtf->content.trimmed().isEmpty()) {
        body.content = rtf->content;
        if (rtf->kind == Rtf::Body::Kind::PlainFromRtf) {
            body.note = QStringLiteral("the body's RTF formatting (kept as text)");
        }
    }
    return body;
}

bool isMail(const QString &messageClass)
{
    const QString c = messageClass.toLower();
    const auto is = [&](const char *prefix) {
        const QLatin1String p(prefix);
        return c == p || c.startsWith(p + QLatin1Char('.'));
    };
    return c.isEmpty() || c == QLatin1String("ipm") || is("ipm.note") || is("ipm.schedule.meeting") || is("report") || is("ipm.post") || is("ipm.outlook.recall")
        || is("ipm.recall");
}

QString smtpAddress(const QString &smtp, const QString &address, const QString &addressType)
{
    if (!smtp.isEmpty()) {
        return smtp;
    }
    if (addressType.compare(QLatin1String("SMTP"), Qt::CaseInsensitive) == 0 || (addressType.isEmpty() && address.contains(QLatin1Char('@')))) {
        return address;
    }
    return {};
}

Import toGraph(const Pst::Message &message, const QHash<quint16, Pst::NamedProperty> &names)
{
    Import out;
    out.message = messageJson(message, names, out.skipped);
    // The original Message-ID, so replies still thread (Exchange takes it on create; tested live)
    if (const QString id = message.string(propInternetMessageId); !id.isEmpty()) {
        out.message.insert(QStringLiteral("internetMessageId"), id);
    }
    for (const Pst::Attachment &a : message.attachments) {
        const Pst::PropertyMap &p = a.properties;
        const int codepage = message.codepage();
        QString name = p.value(propAttachLongFilename).toString(codepage);
        if (name.isEmpty()) {
            name = p.value(propAttachFilename).toString(codepage);
        }
        if (name.isEmpty()) {
            name = p.value(propDisplayName).toString(codepage);
        }
        const qint64 method = p.value(propAttachMethod).toInt();
        if (method == afEmbeddedMessage && a.embedded) {
            QStringList inner;
            QJsonObject item = messageJson(*a.embedded, names, inner);
            item.insert(QStringLiteral("@odata.type"), QStringLiteral("#microsoft.graph.message"));
            item.remove(QStringLiteral("isRead"));
            if (!a.embedded->attachments.isEmpty()) {
                out.skipped.append(QStringLiteral("attachments inside the attached message %1").arg(name));
            }
            out.items.append(QJsonObject{{QStringLiteral("name"), name.isEmpty() ? a.embedded->string(propSubject) : name}, {QStringLiteral("item"), item}});
        } else if (method == afByValue && p.contains(propAttachDataBinary)) {
            File f;
            f.name = name.isEmpty() ? QStringLiteral("attachment") : name;
            f.contentType = p.value(propAttachMimeTag).toString(codepage);
            if (f.contentType.isEmpty()) {
                f.contentType = QStringLiteral("application/octet-stream");
            }
            f.contentId = p.value(propAttachContentId).toString(codepage);
            f.isInline = !f.contentId.isEmpty() && ((p.value(propAttachFlags).toInt() & attRenderedInBody) || p.value(propAttachmentHidden).toBool());
            f.data = p.value(propAttachDataBinary).data;
            out.files.append(f);
        } else {
            out.skipped.append(QStringLiteral("attachment %1 (attach method %2)").arg(name).arg(method));
        }
    }
    return out;
}

}
