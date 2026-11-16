// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "outgoing.h"

#include <QTimeZone>

#include <QDateTime>

#include <QJsonArray>
#include <QRegularExpression>

namespace GraphMail
{

namespace
{
QJsonArray recipientsToJson(const QList<OutgoingRecipient> &list)
{
    QJsonArray array;
    for (const OutgoingRecipient &r : list) {
        array.append(QJsonObject{{QStringLiteral("name"), r.name}, {QStringLiteral("address"), r.address}});
    }
    return array;
}

QList<OutgoingRecipient> recipientsFromJson(const QJsonValue &value)
{
    QList<OutgoingRecipient> list;
    const QJsonArray array = value.toArray();
    for (const QJsonValue &v : array) {
        const QJsonObject o = v.toObject();
        list.append({o.value(QLatin1String("name")).toString(), o.value(QLatin1String("address")).toString()});
    }
    return list;
}

QJsonArray graphRecipients(const QList<OutgoingRecipient> &list)
{
    QJsonArray array;
    for (const OutgoingRecipient &r : list) {
        array.append(graphRecipient(r));
    }
    return array;
}

QString text(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString();
}
}

QJsonObject graphRecipient(const OutgoingRecipient &recipient)
{
    QJsonObject address{{QStringLiteral("address"), recipient.address}};
    if (!recipient.name.isEmpty()) {
        address.insert(QStringLiteral("name"), recipient.name);
    }
    return {{QStringLiteral("emailAddress"), address}};
}

QStringList splitRecipients(const QString &text)
{
    QStringList parts;
    QString current;
    bool inAngle = false;
    bool inQuote = false;
    for (const QChar c : text) {
        if (c == QLatin1Char('"')) {
            inQuote = !inQuote;
        } else if (c == QLatin1Char('<')) {
            inAngle = true;
        } else if (c == QLatin1Char('>')) {
            inAngle = false;
        }
        const bool separator = !inAngle && !inQuote && (c == QLatin1Char(';') || (c == QLatin1Char(',') && current.contains(QLatin1Char('@'))));
        if (separator) {
            parts.append(current);
            current.clear();
        } else {
            current.append(c);
        }
    }
    parts.append(current);
    return parts;
}

QList<OutgoingRecipient> parseRecipients(const QString &text, QStringList *invalid)
{
    QList<OutgoingRecipient> result;
    const QStringList parts = splitRecipients(text);
    static const QRegularExpression named(QStringLiteral(R"(^(.*)<\s*([^<>\s]+@[^<>\s]+)\s*>$)"));
    static const QRegularExpression bare(QStringLiteral(R"(^[^\s<>"@]+@[^\s<>"@]+\.[^\s<>"@]+$)"));
    for (const QString &raw : std::as_const(parts)) {
        const QString part = raw.trimmed();
        if (part.isEmpty()) {
            continue;
        }
        const QRegularExpressionMatch match = named.match(part);
        if (match.hasMatch()) {
            QString name = match.captured(1).trimmed();
            if (name.size() >= 2 && name.startsWith(QLatin1Char('"')) && name.endsWith(QLatin1Char('"'))) {
                name = name.mid(1, name.size() - 2);
            }
            result.append({name, match.captured(2)});
        } else if (bare.match(part).hasMatch()) {
            result.append({QString(), part});
        } else if (invalid) {
            invalid->append(part);
        }
    }
    return result;
}

QString formatRecipient(const OutgoingRecipient &recipient)
{
    if (recipient.name.isEmpty() || recipient.name.compare(recipient.address, Qt::CaseInsensitive) == 0) {
        return recipient.address;
    }
    return recipient.name + QStringLiteral(" <") + recipient.address + QLatin1Char('>');
}

QJsonObject Outgoing::toJson() const
{
    QJsonArray files;
    for (const OutgoingAttachment &a : attachments) {
        files.append(QJsonObject{{QStringLiteral("key"), a.key},
                                 {QStringLiteral("name"), a.name},
                                 {QStringLiteral("contentType"), a.contentType},
                                 {QStringLiteral("size"), a.size},
                                 {QStringLiteral("blob"), a.blob},
                                 {QStringLiteral("path"), a.path},
                                 {QStringLiteral("isInline"), a.isInline},
                                 {QStringLiteral("contentId"), a.contentId},
                                 {QStringLiteral("graphId"), a.graphId},
                                 {QStringLiteral("removed"), a.removed}});
    }
    return {{QStringLiteral("uuid"), uuid},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("source"), source},
            {QStringLiteral("draftId"), draftId},
            {QStringLiteral("internetMessageId"), internetMessageId},
            {QStringLiteral("draftEtag"), draftEtag},
            {QStringLiteral("fromAddress"), fromAddress},
            {QStringLiteral("fromName"), fromName},
            {QStringLiteral("fromMailbox"), fromMailbox},
            {QStringLiteral("sendMode"), sendMode},
            {QStringLiteral("to"), recipientsToJson(to)},
            {QStringLiteral("cc"), recipientsToJson(cc)},
            {QStringLiteral("bcc"), recipientsToJson(bcc)},
            {QStringLiteral("replyTo"), recipientsToJson(replyTo)},
            {QStringLiteral("mentions"), recipientsToJson(mentions)},
            {QStringLiteral("subject"), subject},
            {QStringLiteral("body"), body},
            {QStringLiteral("html"), html},
            {QStringLiteral("importance"), importance},
            {QStringLiteral("readReceipt"), readReceipt},
            {QStringLiteral("deliveryReceipt"), deliveryReceipt},
            {QStringLiteral("sensitivity"), sensitivity},
            {QStringLiteral("deferUntil"), deferUntil},
            {QStringLiteral("sendAfter"), sendAfter},
            {QStringLiteral("attachments"), files},
            {QStringLiteral("dropSourceAttachments"), QJsonArray::fromStringList(dropSourceAttachments)},
            {QStringLiteral("ui"), ui},
            {QStringLiteral("send"), send},
            {QStringLiteral("revision"), revision},
            {QStringLiteral("state"), int(state)},
            {QStringLiteral("attempts"), attempts},
            {QStringLiteral("nextAttempt"), nextAttempt},
            {QStringLiteral("updated"), updated},
            {QStringLiteral("lastError"), lastError}};
}

Outgoing Outgoing::fromJson(const QJsonObject &json)
{
    Outgoing o;
    o.uuid = text(json, "uuid");
    o.kind = json.value(QLatin1String("kind")).toString(QStringLiteral("new"));
    o.source = text(json, "source");
    o.draftId = text(json, "draftId");
    o.internetMessageId = text(json, "internetMessageId");
    o.draftEtag = text(json, "draftEtag");
    o.fromAddress = text(json, "fromAddress");
    o.fromName = text(json, "fromName");
    o.fromMailbox = text(json, "fromMailbox");
    o.sendMode = text(json, "sendMode");
    o.to = recipientsFromJson(json.value(QLatin1String("to")));
    o.cc = recipientsFromJson(json.value(QLatin1String("cc")));
    o.bcc = recipientsFromJson(json.value(QLatin1String("bcc")));
    o.replyTo = recipientsFromJson(json.value(QLatin1String("replyTo")));
    o.mentions = recipientsFromJson(json.value(QLatin1String("mentions")));
    o.subject = text(json, "subject");
    o.body = text(json, "body");
    o.html = json.value(QLatin1String("html")).toBool(true);
    o.importance = json.value(QLatin1String("importance")).toInt(1);
    o.readReceipt = json.value(QLatin1String("readReceipt")).toBool();
    o.deliveryReceipt = json.value(QLatin1String("deliveryReceipt")).toBool();
    o.sensitivity = json.value(QLatin1String("sensitivity")).toInt();
    o.deferUntil = json.value(QLatin1String("deferUntil")).toInteger();
    o.sendAfter = json.value(QLatin1String("sendAfter")).toInteger();
    const QJsonArray files = json.value(QLatin1String("attachments")).toArray();
    for (const QJsonValue &v : files) {
        const QJsonObject f = v.toObject();
        OutgoingAttachment a;
        a.key = text(f, "key");
        a.name = text(f, "name");
        a.contentType = text(f, "contentType");
        a.size = f.value(QLatin1String("size")).toInteger();
        a.blob = text(f, "blob");
        a.path = text(f, "path");
        a.isInline = f.value(QLatin1String("isInline")).toBool();
        a.contentId = text(f, "contentId");
        a.graphId = text(f, "graphId");
        a.removed = f.value(QLatin1String("removed")).toBool();
        o.attachments.append(a);
    }
    const QJsonArray drop = json.value(QLatin1String("dropSourceAttachments")).toArray();
    for (const QJsonValue &v : drop) {
        o.dropSourceAttachments.append(v.toString());
    }
    o.ui = json.value(QLatin1String("ui")).toObject();
    o.send = json.value(QLatin1String("send")).toBool();
    o.revision = json.value(QLatin1String("revision")).toInt();
    o.state = State(json.value(QLatin1String("state")).toInt());
    o.attempts = json.value(QLatin1String("attempts")).toInt();
    o.nextAttempt = json.value(QLatin1String("nextAttempt")).toInteger();
    o.updated = json.value(QLatin1String("updated")).toInteger();
    o.lastError = text(json, "lastError");
    return o;
}

QJsonObject Outgoing::graphMessage() const
{
    static const char *const importanceNames[] = {"low", "normal", "high"};
    QJsonObject m{{QStringLiteral("subject"), subject},
                  {QStringLiteral("body"),
                   QJsonObject{{QStringLiteral("contentType"), html ? QStringLiteral("HTML") : QStringLiteral("Text")}, {QStringLiteral("content"), body}}},
                  {QStringLiteral("toRecipients"), graphRecipients(to)},
                  {QStringLiteral("ccRecipients"), graphRecipients(cc)},
                  {QStringLiteral("bccRecipients"), graphRecipients(bcc)},
                  {QStringLiteral("importance"), QString::fromLatin1(importanceNames[qBound(0, importance, 2)])}};
    if (!replyTo.isEmpty()) {
        m.insert(QStringLiteral("replyTo"), graphRecipients(replyTo));
    }
    m.insert(QStringLiteral("isReadReceiptRequested"), readReceipt);
    m.insert(QStringLiteral("isDeliveryReceiptRequested"), deliveryReceipt);
    // MAPI properties Graph has no field for: sensitivity always (Outlook writes it too), the
    // deferred send time when set. A property cannot be removed through Graph: a delay taken away
    // again becomes a time in the past, which Exchange sends at once
    QJsonArray extended{QJsonObject{{QStringLiteral("id"), QStringLiteral("Integer 0x0036")}, {QStringLiteral("value"), QString::number(qBound(0, sensitivity, 3))}}};
    if (deferUntil != 0) {
        const QDateTime when = deferUntil > 0 ? QDateTime::fromMSecsSinceEpoch(deferUntil, QTimeZone::UTC) : QDateTime(QDate(2000, 1, 1), QTime(0, 0), QTimeZone::UTC);
        extended.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("SystemTime 0x3FEF")}, {QStringLiteral("value"), when.toString(Qt::ISODate)}});
    }
    m.insert(QStringLiteral("singleValueExtendedProperties"), extended);
    // Only for another mailbox: Exchange then sends as or on behalf of it, by the user's rights
    if (!fromAddress.isEmpty()) {
        m.insert(QStringLiteral("from"), graphRecipient({fromName, fromAddress}));
    }
    return m;
}

Outgoing Outgoing::merged(const Outgoing &stored, Outgoing incoming)
{
    incoming.draftId = stored.draftId;
    incoming.internetMessageId = stored.internetMessageId;
    incoming.draftEtag = stored.draftEtag;
    incoming.revision = std::max(incoming.revision, stored.revision + 1);
    QList<OutgoingAttachment> gone;
    for (const OutgoingAttachment &old : stored.attachments) {
        bool kept = false;
        for (OutgoingAttachment &a : incoming.attachments) {
            if (a.key == old.key) {
                a.graphId = old.graphId;
                a.removed = old.removed;
                if (a.blob.isEmpty() && a.path.isEmpty()) {
                    a.blob = old.blob;
                }
                kept = true;
                break;
            }
        }
        // Still on the draft: delete it there first
        if (!kept && !old.graphId.isEmpty()) {
            OutgoingAttachment removed = old;
            removed.removed = true;
            gone.append(removed);
        }
    }
    incoming.attachments += gone;
    return incoming;
}

}
