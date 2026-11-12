// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "graphmapper.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimeZone>

namespace GraphMail::Mapper
{

namespace
{
QString str(const QJsonObject &o, QLatin1StringView key)
{
    return o.value(key).toString();
}

// "Name <address>; Name <address>", or just the address when there is no name
QString recipients(const QJsonArray &list)
{
    QStringList parts;
    for (const QJsonValue &value : list) {
        const QJsonObject address = value.toObject().value(QLatin1String("emailAddress")).toObject();
        const QString name = str(address, QLatin1String("name"));
        const QString mail = str(address, QLatin1String("address"));
        parts.append(name.isEmpty() || name == mail ? mail : name + QStringLiteral(" <") + mail + QLatin1Char('>'));
    }
    return parts.join(QStringLiteral("; "));
}

int enumIndex(const QString &value, std::initializer_list<QLatin1StringView> names, int fallback)
{
    int index = 0;
    for (QLatin1StringView name : names) {
        if (value.compare(name, Qt::CaseInsensitive) == 0) {
            return index;
        }
        ++index;
    }
    return fallback;
}

QString compact(const QJsonObject &item)
{
    return QString::fromUtf8(QJsonDocument(item).toJson(QJsonDocument::Compact));
}
}

QString messageSelect()
{
    return QStringLiteral(
        "subject,from,toRecipients,ccRecipients,receivedDateTime,sentDateTime,isRead,flag,categories,hasAttachments,importance,"
        "conversationId,internetMessageId,isDraft,bodyPreview,parentFolderId,changeKey,inferenceClassification");
}

QString folderSelect()
{
    return QStringLiteral("displayName,parentFolderId,totalItemCount,unreadItemCount");
}

qint64 parseTime(const QString &iso)
{
    if (iso.isEmpty()) {
        return 0;
    }
    QDateTime time = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!time.isValid()) {
        time = QDateTime::fromString(iso, Qt::ISODate);
    }
    return time.isValid() ? time.toMSecsSinceEpoch() : 0;
}

qint64 parseDateTimeTimeZone(const QJsonObject &value)
{
    QString text = str(value, QLatin1String("dateTime"));
    if (text.isEmpty()) {
        return 0;
    }
    // Graph sends seven fractional digits; Qt parses at most three
    if (const qsizetype dot = text.indexOf(QLatin1Char('.')); dot > 0) {
        text.truncate(std::min<qsizetype>(text.size(), dot + 4));
    }
    QDateTime local = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!local.isValid()) {
        local = QDateTime::fromString(text, Qt::ISODate);
    }
    if (!local.isValid()) {
        return 0;
    }
    const QString zoneName = str(value, QLatin1String("timeZone"));
    QTimeZone zone = QTimeZone::UTC;
    if (!zoneName.isEmpty() && zoneName.compare(QLatin1String("UTC"), Qt::CaseInsensitive) != 0) {
        QTimeZone named(zoneName.toUtf8());
        if (!named.isValid()) {
            named = QTimeZone(QTimeZone::windowsIdToDefaultIanaId(zoneName.toUtf8()));
        }
        if (named.isValid()) {
            zone = named;
        }
    }
    return QDateTime(local.date(), local.time(), zone).toMSecsSinceEpoch();
}

QDate dueDate(const QJsonObject &dueDateTime)
{
    const QString text = str(dueDateTime, QLatin1String("dateTime"));
    if (text.isEmpty()) {
        return {};
    }
    const QDateTime stated = QDateTime::fromString(text.left(19), Qt::ISODate);
    if (!stated.isValid()) {
        return {};
    }
    if (stated.time() == QTime(0, 0)) {
        return stated.date();
    }
    return QDateTime::fromMSecsSinceEpoch(parseDateTimeTimeZone(dueDateTime)).toLocalTime().date();
}

QJsonObject dueDateTime(const QDate &date)
{
    return {{QStringLiteral("dateTime"), QString(date.toString(Qt::ISODate) + QStringLiteral("T00:00:00"))}, {QStringLiteral("timeZone"), QStringLiteral("UTC")}};
}

bool isRemoved(const QJsonObject &item)
{
    return item.contains(QLatin1String("@removed"));
}

int messageItemType(const QJsonObject &item)
{
    const QString type = str(item, QLatin1String("@odata.type"));
    if (type.endsWith(QLatin1String("eventMessageRequest"))) {
        return 1;
    }
    if (type.endsWith(QLatin1String("eventMessageResponse"))) {
        return 2;
    }
    // A plain eventMessage: an update or a cancellation
    return type.endsWith(QLatin1String("eventMessage")) ? 3 : 0;
}

MessageRow message(const QJsonObject &item, qint64 folder)
{
    MessageRow m;
    m.graphId = str(item, QLatin1String("id"));
    m.folder = folder;
    m.itemType = messageItemType(item);
    m.focusKnown = item.contains(QLatin1String("inferenceClassification"));
    m.focused = item.value(QLatin1String("inferenceClassification")).toString() != QLatin1String("other");
    m.conversation = str(item, QLatin1String("conversationId"));
    m.received = parseTime(str(item, QLatin1String("receivedDateTime")));
    m.sent = parseTime(str(item, QLatin1String("sentDateTime")));
    // Drafts have no from yet
    const QJsonObject from = item.value(QLatin1String("from")).toObject().value(QLatin1String("emailAddress")).toObject();
    m.senderName = str(from, QLatin1String("name"));
    m.senderAddress = str(from, QLatin1String("address"));
    m.subject = str(item, QLatin1String("subject"));
    m.preview = str(item, QLatin1String("bodyPreview"));
    m.toList = recipients(item.value(QLatin1String("toRecipients")).toArray());
    m.ccList = recipients(item.value(QLatin1String("ccRecipients")).toArray());
    m.isRead = item.value(QLatin1String("isRead")).toBool();
    m.flag = enumIndex(item.value(QLatin1String("flag")).toObject().value(QLatin1String("flagStatus")).toString(),
                       {QLatin1String("notFlagged"), QLatin1String("flagged"), QLatin1String("complete")},
                       0);
    m.importance = enumIndex(str(item, QLatin1String("importance")), {QLatin1String("low"), QLatin1String("normal"), QLatin1String("high")}, 1);
    m.hasAttachments = item.value(QLatin1String("hasAttachments")).toBool();
    const QJsonArray categories = item.value(QLatin1String("categories")).toArray();
    for (const QJsonValue &category : categories) {
        m.categories.append(category.toString());
    }
    m.internetMessageId = str(item, QLatin1String("internetMessageId"));
    m.changeKey = str(item, QLatin1String("changeKey"));
    return m;
}

bool isSparse(const QJsonObject &item)
{
    return !item.contains(QLatin1String("changeKey"));
}

MessageRow mergeMessage(MessageRow base, const QJsonObject &item, qint64 folder)
{
    // Only what the item carries; the rest stays as cached
    const MessageRow full = message(item, folder);
    auto has = [&item](const char *key) {
        return item.contains(QLatin1String(key));
    };
    base.folder = folder;
    if (has("conversationId")) {
        base.conversation = full.conversation;
    }
    if (has("receivedDateTime")) {
        base.received = full.received;
    }
    if (has("sentDateTime")) {
        base.sent = full.sent;
    }
    if (has("from")) {
        base.senderName = full.senderName;
        base.senderAddress = full.senderAddress;
    }
    if (has("subject")) {
        base.subject = full.subject;
    }
    if (has("bodyPreview")) {
        base.preview = full.preview;
    }
    if (has("toRecipients")) {
        base.toList = full.toList;
    }
    if (has("ccRecipients")) {
        base.ccList = full.ccList;
    }
    if (has("isRead")) {
        base.isRead = full.isRead;
    }
    if (has("flag")) {
        base.flag = full.flag;
    }
    if (has("importance")) {
        base.importance = full.importance;
    }
    if (has("hasAttachments")) {
        base.hasAttachments = full.hasAttachments;
    }
    if (has("categories")) {
        base.categories = full.categories;
    }
    if (has("internetMessageId")) {
        base.internetMessageId = full.internetMessageId;
    }
    if (has("@odata.type")) {
        base.itemType = full.itemType;
    }
    // The cached change key no longer describes the row; the next full item replaces it
    base.changeKey.clear();
    return base;
}

CalendarRow calendar(const QJsonObject &item, const QString &ownAddress)
{
    CalendarRow c;
    c.graphId = str(item, QLatin1String("id"));
    c.name = str(item, QLatin1String("name")).trimmed();
    c.owner = item.value(QLatin1String("owner")).toObject().value(QLatin1String("address")).toString();
    if (c.owner.isEmpty()) {
        c.owner = ownAddress;
    }
    const QString color = str(item, QLatin1String("hexColor"));
    c.color = color.startsWith(QLatin1Char('#')) ? color : QString();
    c.canEdit = item.value(QLatin1String("canEdit")).toBool();
    c.isDefault = item.value(QLatin1String("isDefaultCalendar")).toBool();
    return c;
}

FolderRow folder(const QJsonObject &item)
{
    FolderRow f;
    f.graphId = str(item, QLatin1String("id"));
    f.parentGraphId = str(item, QLatin1String("parentFolderId"));
    f.name = str(item, QLatin1String("displayName"));
    f.total = item.value(QLatin1String("totalItemCount")).toInt();
    f.unread = item.value(QLatin1String("unreadItemCount")).toInt();
    return f;
}

ItemRow event(const QJsonObject &item)
{
    ItemRow row;
    row.graphId = str(item, QLatin1String("id"));
    row.changeKey = str(item, QLatin1String("changeKey"));
    row.json = compact(item);
    const QJsonObject organizer = item.value(QLatin1String("organizer")).toObject().value(QLatin1String("emailAddress")).toObject();
    row.columns = {
        {QStringLiteral("series_master"), str(item, QLatin1String("seriesMasterId"))},
        {QStringLiteral("start_time"), parseDateTimeTimeZone(item.value(QLatin1String("start")).toObject())},
        {QStringLiteral("end_time"), parseDateTimeTimeZone(item.value(QLatin1String("end")).toObject())},
        {QStringLiteral("all_day"), item.value(QLatin1String("isAllDay")).toBool() ? 1 : 0},
        {QStringLiteral("subject"), str(item, QLatin1String("subject"))},
        {QStringLiteral("location"), item.value(QLatin1String("location")).toObject().value(QLatin1String("displayName")).toString()},
        {QStringLiteral("organizer"), str(organizer, QLatin1String("name")).isEmpty() ? str(organizer, QLatin1String("address")) : str(organizer, QLatin1String("name"))},
        {QStringLiteral("show_as"),
         enumIndex(str(item, QLatin1String("showAs")),
                   {QLatin1String("free"), QLatin1String("tentative"), QLatin1String("busy"), QLatin1String("oof"), QLatin1String("workingElsewhere")},
                   -1)},
        {QStringLiteral("response"),
         enumIndex(item.value(QLatin1String("responseStatus")).toObject().value(QLatin1String("response")).toString(),
                   {QLatin1String("none"), QLatin1String("organizer"), QLatin1String("tentativelyAccepted"), QLatin1String("accepted"),
                    QLatin1String("declined"), QLatin1String("notResponded")},
                   0)},
        {QStringLiteral("is_online_meeting"), item.value(QLatin1String("isOnlineMeeting")).toBool() ? 1 : 0},
        {QStringLiteral("type"),
         enumIndex(str(item, QLatin1String("type")),
                   {QLatin1String("singleInstance"), QLatin1String("occurrence"), QLatin1String("exception"), QLatin1String("seriesMaster")},
                   0)},
    };
    return row;
}

ItemRow contact(const QJsonObject &item)
{
    ItemRow row;
    row.graphId = str(item, QLatin1String("id"));
    row.changeKey = str(item, QLatin1String("changeKey"));
    row.json = compact(item);
    QStringList emails;
    const QJsonArray addresses = item.value(QLatin1String("emailAddresses")).toArray();
    for (const QJsonValue &value : addresses) {
        emails.append(value.toObject().value(QLatin1String("address")).toString());
    }
    row.columns = {{QStringLiteral("display_name"), str(item, QLatin1String("displayName"))}, {QStringLiteral("emails"), emails.join(QStringLiteral("; "))}};
    return row;
}

ItemRow task(const QJsonObject &item)
{
    ItemRow row;
    row.graphId = str(item, QLatin1String("id"));
    // To Do tasks have no changeKey; the last modification time serves the same purpose
    row.changeKey = str(item, QLatin1String("lastModifiedDateTime"));
    row.json = compact(item);
    row.columns = {
        {QStringLiteral("title"), str(item, QLatin1String("title"))},
        {QStringLiteral("status"),
         enumIndex(str(item, QLatin1String("status")),
                   {QLatin1String("notStarted"), QLatin1String("inProgress"), QLatin1String("completed"), QLatin1String("waitingOnOthers"),
                    QLatin1String("deferred")},
                   0)},
        {QStringLiteral("due"), parseDateTimeTimeZone(item.value(QLatin1String("dueDateTime")).toObject())},
        {QStringLiteral("reminder"), parseDateTimeTimeZone(item.value(QLatin1String("reminderDateTime")).toObject())},
    };
    return row;
}

AttachmentRow attachment(const QJsonObject &item)
{
    AttachmentRow a;
    a.graphId = str(item, QLatin1String("id"));
    a.name = str(item, QLatin1String("name"));
    a.contentType = str(item, QLatin1String("contentType"));
    // An attached mail or Outlook item has no content type; its $value is MIME (an .eml)
    if (a.contentType.isEmpty() && str(item, QLatin1String("@odata.type")).endsWith(QLatin1String("itemAttachment"))) {
        a.contentType = QStringLiteral("message/rfc822");
    }
    a.size = item.value(QLatin1String("size")).toInteger();
    a.isInline = item.value(QLatin1String("isInline")).toBool();
    a.contentId = str(item, QLatin1String("contentId"));
    return a;
}

}
