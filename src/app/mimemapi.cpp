// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mimemapi.h"
#include "msg/msgfile.h"
#include "pst/mapigraph.h"

#include <KMime/Message>
#include <KMime/Util>

#include <QFile>
#include <QSaveFile>
#include <QTextDocumentFragment>
#include <QTimeZone>
#include <QtEndian>

namespace GraphMail::MimeMapi
{

namespace
{
// [MS-OXPROPS]
constexpr quint16 propImportance = 0x0017;
constexpr quint16 propMessageClass = 0x001A;
constexpr quint16 propSubject = 0x0037;
constexpr quint16 propClientSubmitTime = 0x0039;
constexpr quint16 propSentRepresentingName = 0x0042;
constexpr quint16 propSentRepresentingAddressType = 0x0064;
constexpr quint16 propSentRepresentingEmailAddress = 0x0065;
constexpr quint16 propConversationTopic = 0x0070;
constexpr quint16 propTransportMessageHeaders = 0x007D;
constexpr quint16 propSenderName = 0x0C1A;
constexpr quint16 propSenderAddressType = 0x0C1E;
constexpr quint16 propSenderEmailAddress = 0x0C1F;
constexpr quint16 propDisplayBcc = 0x0E02;
constexpr quint16 propDisplayCc = 0x0E03;
constexpr quint16 propDisplayTo = 0x0E04;
constexpr quint16 propMessageDeliveryTime = 0x0E06;
constexpr quint16 propMessageFlags = 0x0E07;
constexpr quint16 propAttachSize = 0x0E20;
constexpr quint16 propBody = 0x1000;
constexpr quint16 propHtml = 0x1013;
constexpr quint16 propInternetMessageId = 0x1035;
constexpr quint16 propInternetReferences = 0x1039;
constexpr quint16 propInReplyToId = 0x1042;
constexpr quint16 propFlagStatus = 0x1090;
constexpr quint16 propDisplayName = 0x3001;
constexpr quint16 propCreationTime = 0x3007;
constexpr quint16 propLastModificationTime = 0x3008;
constexpr quint16 propAttachDataBinary = 0x3701;
constexpr quint16 propAttachFilename = 0x3704;
constexpr quint16 propAttachMethod = 0x3705;
constexpr quint16 propAttachLongFilename = 0x3707;
constexpr quint16 propRenderingPosition = 0x370B;
constexpr quint16 propAttachMimeTag = 0x370E;
constexpr quint16 propAttachContentId = 0x3712;
constexpr quint16 propAttachFlags = 0x3714;
constexpr quint16 propInternetCodepage = 0x3FDE;
constexpr quint16 propSenderSmtpAddress = 0x5D01;
constexpr quint16 propSentRepresentingSmtpAddress = 0x5D02;
constexpr quint16 propAttachmentHidden = 0x7FFE;
// Recipient table columns (tags)
constexpr quint32 tagRecipientType = 0x0C150003;
constexpr quint32 tagObjectType = 0x0FFE0003;
constexpr quint32 tagDisplayName = 0x3001001F;
constexpr quint32 tagAddressType = 0x3002001F;
constexpr quint32 tagEmailAddress = 0x3003001F;
constexpr quint32 tagDisplayType = 0x39000003;
constexpr quint32 tagSmtpAddress = 0x39FE001F;
// [MS-OXCMSG] / [MS-OXOFLAG] / [MS-OXCDATA]
constexpr quint32 mfRead = 0x01;
constexpr quint32 afByValue = 1;
constexpr quint32 afEmbeddedMessage = 5;
constexpr quint32 attRenderedInBody = 0x04;
constexpr quint32 followupComplete = 1;
constexpr quint32 followupFlagged = 2;
constexpr quint32 mapiMailUser = 6;
constexpr int maxDepth = 8;

Pst::PropValue string(const QString &s)
{
    return {Pst::PtypString, QByteArray(reinterpret_cast<const char *>(s.utf16()), s.size() * 2)};
}
Pst::PropValue int32(qint64 v)
{
    QByteArray b(4, 0);
    qToLittleEndian(quint32(v), b.data());
    return {Pst::PtypInteger32, b};
}
Pst::PropValue boolean(bool v)
{
    return {Pst::PtypBoolean, QByteArray(1, v ? 1 : 0)};
}
Pst::PropValue time(const QDateTime &t)
{
    // FILETIME: 100 ns since 1601
    QByteArray b(8, 0);
    qToLittleEndian(quint64((t.toMSecsSinceEpoch() + qint64(11644473600) * 1000) * 10000), b.data());
    return {Pst::PtypTime, b};
}
Pst::PropValue multiString(const QStringList &values)
{
    // ulCount, the offsets from the start of the value, the UTF-16 items
    QByteArray items;
    QList<quint32> offsets;
    const quint32 start = 4 + 4 * quint32(values.size());
    for (const QString &v : values) {
        offsets.append(start + quint32(items.size()));
        items += string(v).data;
    }
    QByteArray out(4, 0);
    qToLittleEndian(quint32(values.size()), out.data());
    for (const quint32 o : std::as_const(offsets)) {
        QByteArray b(4, 0);
        qToLittleEndian(o, b.data());
        out += b;
    }
    return {Pst::PtypString | Pst::PtypMultiple, out + items};
}

// The leaves of the MIME tree that are neither of the two bodies
void collectParts(KMime::Content *c, const KMime::Content *html, const KMime::Content *text, QList<KMime::Content *> &out)
{
    if (c->bodyIsMessage()) {
        out.append(c);
        return;
    }
    const QList<KMime::Content *> children = c->contents();
    if (children.isEmpty()) {
        if (c != html && c != text) {
            out.append(c);
        }
        return;
    }
    for (KMime::Content *child : children) {
        collectParts(child, html, text, out);
    }
}

void addRecipients(Pst::Message &m, const QList<KMime::Types::Mailbox> &list, int type)
{
    for (const KMime::Types::Mailbox &mb : list) {
        const QString address = QString::fromUtf8(mb.address());
        QHash<quint32, Pst::PropValue> row;
        row.insert(tagRecipientType, int32(type));
        row.insert(tagDisplayName, string(mb.name().isEmpty() ? address : mb.name()));
        row.insert(tagAddressType, string(QStringLiteral("SMTP")));
        row.insert(tagEmailAddress, string(address));
        row.insert(tagSmtpAddress, string(address));
        row.insert(tagObjectType, int32(mapiMailUser));
        row.insert(tagDisplayType, int32(0));
        m.recipients.append(row);
    }
}

QString displayList(const QList<KMime::Types::Mailbox> &list)
{
    QStringList names;
    for (const KMime::Types::Mailbox &mb : list) {
        names.append(mb.name().isEmpty() ? QString::fromUtf8(mb.address()) : mb.name());
    }
    return names.join(QStringLiteral("; "));
}

Pst::Message convert(KMime::Message &msg, const Extras &extras, quint16 keywords, int depth)
{
    Pst::Message m;
    Pst::PropertyMap &p = m.properties;
    const QString subject = msg.subject(KMime::CreatePolicy::DontCreate) ? msg.subject(KMime::CreatePolicy::DontCreate)->asUnicodeString() : QString();
    p.insert(propSubject, string(subject));
    p.insert(propConversationTopic, string(subject));
    p.insert(propMessageClass, string(QStringLiteral("IPM.Note")));
    p.insert(propInternetCodepage, int32(65001));
    p.insert(propTransportMessageHeaders, string(QString::fromUtf8(KMime::LFtoCRLF(msg.head()))));

    // Sender: From as the represented sender, Sender (else From) as the one who sent it
    const KMime::Headers::From *fromHeader = msg.from(KMime::CreatePolicy::DontCreate);
    const QList<KMime::Types::Mailbox> from = fromHeader ? fromHeader->mailboxes() : QList<KMime::Types::Mailbox>();
    if (!from.isEmpty()) {
        const QString address = QString::fromUtf8(from.first().address());
        p.insert(propSentRepresentingName, string(from.first().name().isEmpty() ? address : from.first().name()));
        p.insert(propSentRepresentingAddressType, string(QStringLiteral("SMTP")));
        p.insert(propSentRepresentingEmailAddress, string(address));
        p.insert(propSentRepresentingSmtpAddress, string(address));
    }
    const KMime::Headers::Sender *senderHeader = msg.sender(KMime::CreatePolicy::DontCreate);
    const QList<KMime::Types::Mailbox> sender = senderHeader && !senderHeader->mailbox().address().isEmpty() ? QList<KMime::Types::Mailbox>{senderHeader->mailbox()} : from;
    if (!sender.isEmpty()) {
        const QString address = QString::fromUtf8(sender.first().address());
        p.insert(propSenderName, string(sender.first().name().isEmpty() ? address : sender.first().name()));
        p.insert(propSenderAddressType, string(QStringLiteral("SMTP")));
        p.insert(propSenderEmailAddress, string(address));
        p.insert(propSenderSmtpAddress, string(address));
    }

    // Recipients (1 To, 2 Cc, 3 Bcc) and the display lists Outlook's columns show
    const QList<KMime::Types::Mailbox> to = msg.to(KMime::CreatePolicy::DontCreate) ? msg.to(KMime::CreatePolicy::DontCreate)->mailboxes() : QList<KMime::Types::Mailbox>();
    const QList<KMime::Types::Mailbox> cc = msg.cc(KMime::CreatePolicy::DontCreate) ? msg.cc(KMime::CreatePolicy::DontCreate)->mailboxes() : QList<KMime::Types::Mailbox>();
    const QList<KMime::Types::Mailbox> bcc = msg.bcc(KMime::CreatePolicy::DontCreate) ? msg.bcc(KMime::CreatePolicy::DontCreate)->mailboxes() : QList<KMime::Types::Mailbox>();
    addRecipients(m, to, 1);
    addRecipients(m, cc, 2);
    addRecipients(m, bcc, 3);
    p.insert(propDisplayTo, string(displayList(to)));
    p.insert(propDisplayCc, string(displayList(cc)));
    p.insert(propDisplayBcc, string(displayList(bcc)));

    // Dates: sent from the Date header, delivered as the mailbox has it
    const QDateTime sent = msg.date(KMime::CreatePolicy::DontCreate) ? msg.date(KMime::CreatePolicy::DontCreate)->dateTime() : QDateTime();
    const QDateTime delivered = extras.received > 0 ? QDateTime::fromMSecsSinceEpoch(extras.received, QTimeZone::UTC) : sent;
    if (sent.isValid()) {
        p.insert(propClientSubmitTime, time(sent));
    }
    if (delivered.isValid()) {
        p.insert(propMessageDeliveryTime, time(delivered));
        p.insert(propCreationTime, time(delivered));
        p.insert(propLastModificationTime, time(delivered));
    }
    if (const KMime::Headers::MessageID *id = msg.messageID(KMime::CreatePolicy::DontCreate)) {
        p.insert(propInternetMessageId, string(id->asUnicodeString()));
    }
    if (const KMime::Headers::InReplyTo *h = msg.inReplyTo(KMime::CreatePolicy::DontCreate)) {
        p.insert(propInReplyToId, string(h->asUnicodeString()));
    }
    if (const KMime::Headers::References *h = msg.references(KMime::CreatePolicy::DontCreate)) {
        p.insert(propInternetReferences, string(h->asUnicodeString()));
    }

    // What the cache keeps: read state, flag, importance, categories
    p.insert(propMessageFlags, int32(extras.read ? mfRead : 0));
    p.insert(propImportance, int32(extras.importance));
    if (extras.flag == 1 || extras.flag == 2) {
        p.insert(propFlagStatus, int32(extras.flag == 1 ? followupFlagged : followupComplete));
    }
    if (keywords != 0 && !extras.categories.isEmpty()) {
        p.insert(keywords, multiString(extras.categories));
    }

    // Bodies: HTML as UTF-8 bytes, the text as well (Outlook previews and searches it)
    KMime::Content *html = msg.mainBodyPart("text/html");
    KMime::Content *text = msg.mainBodyPart("text/plain");
    if (html) {
        const QString h = html->decodedText();
        p.insert(propHtml, {Pst::PtypBinary, h.toUtf8()});
        p.insert(propBody, string(text ? text->decodedText() : QTextDocumentFragment::fromHtml(h).toPlainText()));
    } else if (text) {
        p.insert(propBody, string(text->decodedText()));
    }

    // Everything else is an attachment: files, inline images, attached messages
    QList<KMime::Content *> parts;
    if (msg.contents().isEmpty() && !html && !text) {
        // The whole message is one non-text part (S/MIME encrypted, a lone file)
        parts.append(&msg);
        if (msg.contentType(KMime::CreatePolicy::DontCreate) && msg.contentType(KMime::CreatePolicy::DontCreate)->mimeType() == "application/pkcs7-mime") {
            p.insert(propMessageClass, string(QStringLiteral("IPM.Note.SMIME")));
        }
    } else {
        collectParts(&msg, html, text, parts);
    }
    for (KMime::Content *c : std::as_const(parts)) {
        Pst::Attachment a;
        Pst::PropertyMap &ap = a.properties;
        const KMime::Headers::ContentType *type = c->contentType(KMime::CreatePolicy::DontCreate);
        const KMime::Headers::ContentDisposition *disposition = c->contentDisposition(KMime::CreatePolicy::DontCreate);
        QString name = disposition ? disposition->filename() : QString();
        if (name.isEmpty() && type) {
            name = type->name();
        }
        if (c->bodyIsMessage() && depth < maxDepth) {
            const std::shared_ptr<KMime::Message> inner = c->bodyAsMessage();
            Extras innerExtras;
            a.embedded = std::make_shared<Pst::Message>(convert(*inner, innerExtras, keywords, depth + 1));
            const QString innerSubject = inner->subject(KMime::CreatePolicy::DontCreate) ? inner->subject(KMime::CreatePolicy::DontCreate)->asUnicodeString() : QString();
            ap.insert(propAttachMethod, int32(afEmbeddedMessage));
            ap.insert(propDisplayName, string(name.isEmpty() ? innerSubject : name));
            ap.insert(propAttachMimeTag, string(QStringLiteral("message/rfc822")));
            ap.insert(propRenderingPosition, int32(-1));
            m.attachments.append(a);
            continue;
        }
        const QByteArray data = c->decodedBody();
        const QString mimeType = type ? QString::fromLatin1(type->mimeType()) : QStringLiteral("application/octet-stream");
        if (name.isEmpty()) {
            name = QStringLiteral("attachment");
        }
        ap.insert(propAttachMethod, int32(afByValue));
        ap.insert(propAttachLongFilename, string(name));
        ap.insert(propAttachFilename, string(name));
        ap.insert(propDisplayName, string(name));
        ap.insert(propAttachMimeTag, string(mimeType));
        ap.insert(propAttachDataBinary, {Pst::PtypBinary, data});
        ap.insert(propAttachSize, int32(data.size()));
        ap.insert(propRenderingPosition, int32(-1));
        // An inline image the HTML refers to by its Content-ID
        const KMime::Headers::ContentID *cid = c->contentID(KMime::CreatePolicy::DontCreate);
        const bool attachment = disposition && disposition->disposition() == KMime::Headers::CDattachment;
        if (cid && !cid->identifier().isEmpty()) {
            ap.insert(propAttachContentId, string(QString::fromUtf8(cid->identifier())));
            if (!attachment) {
                ap.insert(propAttachFlags, int32(attRenderedInBody));
                ap.insert(propAttachmentHidden, boolean(true));
            }
        }
        m.attachments.append(a);
    }
    return m;
}
}

namespace
{
std::unique_ptr<KMime::Content> leaf(const QByteArray &type, const QByteArray &data, KMime::Headers::contentEncoding encoding)
{
    auto c = std::make_unique<KMime::Content>();
    c->contentType()->setMimeType(type);
    c->setBody(data);
    c->contentTransferEncoding()->setEncoding(encoding);
    return c;
}

std::unique_ptr<KMime::Content> multipart(const QByteArray &type, std::vector<std::unique_ptr<KMime::Content>> parts)
{
    auto c = std::make_unique<KMime::Content>();
    c->contentType()->setMimeType(type);
    c->contentType()->setBoundary(KMime::multiPartBoundary());
    for (auto &p : parts) {
        c->appendContent(std::move(p));
    }
    return c;
}

QByteArray buildMime(const Pst::Message &m, int depth)
{
    const Pst::PropertyMap &p = m.properties;
    KMime::Message msg;
    msg.contentType()->setMimeType("multipart/mixed");
    const QString subject = m.string(propSubject);
    msg.subject()->setRFC2047Charset("utf-8");
    msg.subject()->fromUnicodeString(subject);
    const QString from = MapiGraph::smtpAddress(m.string(propSentRepresentingSmtpAddress), m.string(propSentRepresentingEmailAddress), m.string(propSentRepresentingAddressType));
    if (!from.isEmpty() || p.contains(propSentRepresentingName)) {
        msg.from()->addAddress(from.toUtf8(), m.string(propSentRepresentingName));
    }
    for (const QHash<quint32, Pst::PropValue> &r : m.recipients) {
        QString name, address, type, smtp;
        qint64 kind = 1;
        for (auto it = r.constBegin(); it != r.constEnd(); ++it) {
            switch (it.key() >> 16) {
            case 0x3001:
                name = it->toString(m.codepage());
                break;
            case 0x3003:
                address = it->toString(m.codepage());
                break;
            case 0x3002:
                type = it->toString(m.codepage());
                break;
            case 0x39FE:
                smtp = it->toString(m.codepage());
                break;
            case 0x0C15:
                kind = it->toInt() & 0x0F;
                break;
            default:
                break;
            }
        }
        const QByteArray a = MapiGraph::smtpAddress(smtp, address, type).toUtf8();
        if (kind == 2) {
            msg.cc()->addAddress(a, name);
        } else if (kind == 3) {
            msg.bcc()->addAddress(a, name);
        } else {
            msg.to()->addAddress(a, name);
        }
    }
    const QDateTime sent = p.contains(propClientSubmitTime) ? p.value(propClientSubmitTime).toTime() : p.value(propMessageDeliveryTime).toTime();
    if (sent.isValid()) {
        msg.date()->setDateTime(sent);
    }
    if (const QString id = m.string(propInternetMessageId); !id.isEmpty()) {
        msg.messageID()->fromUnicodeString(id);
    }
    if (const QString id = m.string(propInReplyToId); !id.isEmpty()) {
        msg.inReplyTo()->fromUnicodeString(id);
    }
    if (const QString refs = m.string(propInternetReferences); !refs.isEmpty()) {
        msg.references()->fromUnicodeString(refs);
    }

    // The bodies: text and HTML as alternatives, inline images related to the HTML
    // The text body and the best body (HTML, or text from RTF when that is all there is)
    std::vector<std::unique_ptr<KMime::Content>> bodies;
    const MapiGraph::Body best = MapiGraph::bestBody(m);
    if (p.contains(propBody) && best.html) {
        auto text = leaf("text/plain", m.string(propBody).toUtf8(), KMime::Headers::CEquPr);
        text->contentType()->setCharset("utf-8");
        bodies.push_back(std::move(text));
    }
    if (best.html || !best.content.isEmpty() || p.contains(propBody)) {
        auto part = leaf(best.html ? QByteArray("text/html") : QByteArray("text/plain"), best.content.toUtf8(), KMime::Headers::CEquPr);
        part->contentType()->setCharset("utf-8");
        bodies.push_back(std::move(part));
    }
    std::unique_ptr<KMime::Content> body = bodies.size() == 1 ? std::move(bodies.front())
        : bodies.empty() ? leaf("text/plain", QByteArray(), KMime::Headers::CE7Bit)
                         : multipart("multipart/alternative", std::move(bodies));

    std::vector<std::unique_ptr<KMime::Content>> related;
    std::vector<std::unique_ptr<KMime::Content>> attached;
    for (const Pst::Attachment &a : m.attachments) {
        const Pst::PropertyMap &ap = a.properties;
        QString name = ap.value(propAttachLongFilename).toString(m.codepage());
        if (name.isEmpty()) {
            name = ap.value(propAttachFilename).toString(m.codepage());
        }
        if (name.isEmpty()) {
            name = ap.value(propDisplayName).toString(m.codepage());
        }
        if (a.embedded && depth < maxDepth) {
            auto part = leaf("message/rfc822", buildMime(*a.embedded, depth + 1), KMime::Headers::CE8Bit);
            auto d = std::make_unique<KMime::Headers::ContentDisposition>();
            d->setDisposition(KMime::Headers::CDattachment);
            if (!name.isEmpty()) {
                d->setFilename(name + QStringLiteral(".eml"));
            }
            part->setHeader(std::move(d));
            attached.push_back(std::move(part));
            continue;
        }
        if (!ap.contains(propAttachDataBinary)) {
            continue;
        }
        QString type = ap.value(propAttachMimeTag).toString(m.codepage());
        auto part = leaf(type.isEmpty() ? QByteArray("application/octet-stream") : type.toLatin1(), ap.value(propAttachDataBinary).data, KMime::Headers::CEbase64);
        part->contentType()->setName(name);
        const QString cid = ap.value(propAttachContentId).toString(m.codepage());
        const bool inlined = !cid.isEmpty() && ((ap.value(propAttachFlags).toInt() & attRenderedInBody) || ap.value(propAttachmentHidden).toBool());
        auto d = std::make_unique<KMime::Headers::ContentDisposition>();
        d->setDisposition(inlined ? KMime::Headers::CDinline : KMime::Headers::CDattachment);
        d->setFilename(name);
        part->setHeader(std::move(d));
        if (!cid.isEmpty()) {
            part->contentID()->setIdentifier(cid.toUtf8());
        }
        (inlined ? related : attached).push_back(std::move(part));
    }
    if (!related.empty()) {
        related.insert(related.begin(), std::move(body));
        body = multipart("multipart/related", std::move(related));
    }
    if (attached.empty()) {
        // The body alone is the message: move its headers up
        msg.contentType()->from7BitString(body->contentType()->as7BitString());
        if (const auto *cte = body->contentTransferEncoding(KMime::CreatePolicy::DontCreate)) {
            msg.contentTransferEncoding()->setEncoding(cte->encoding());
        }
        if (body->contents().isEmpty()) {
            msg.setBody(body->decodedBody());
        } else {
            const QList<KMime::Content *> parts = body->contents();
            for (KMime::Content *c : parts) {
                msg.appendContent(body->takeContent(c));
            }
        }
    } else {
        msg.contentType()->setBoundary(KMime::multiPartBoundary());
        msg.appendContent(std::move(body));
        for (auto &part : attached) {
            msg.appendContent(std::move(part));
        }
    }
    msg.assemble();
    // CRLF once, for the whole message: an attached message goes in with LF like the rest
    return msg.encodedContent(depth == 0 ? KMime::NewlineType::CRLF : KMime::NewlineType::LF);
}
}

QByteArray toMime(const Pst::Message &message)
{
    return buildMime(message, 0);
}

Pst::Message fromMime(const QByteArray &mime, const Extras &extras, quint16 keywords)
{
    KMime::Message msg;
    msg.setContent(KMime::CRLFtoLF(mime));
    msg.parse();
    return convert(msg, extras, keywords, 0);
}

bool msgToEml(const QString &msgPath, const QString &emlPath, QString *error)
{
    QFile in(msgPath);
    if (!in.open(QIODevice::ReadOnly)) {
        *error = in.errorString();
        return false;
    }
    const std::optional<Msg::File> msg = Msg::read(in.readAll(), error);
    if (!msg) {
        return false;
    }
    QSaveFile out(emlPath);
    const QByteArray mime = toMime(msg->message);
    if (!out.open(QIODevice::WriteOnly) || out.write(mime) != mime.size() || !out.commit()) {
        *error = out.errorString();
        return false;
    }
    return true;
}

QByteArray mimeToMsg(const QByteArray &mime, const Extras &extras)
{
    Msg::File file;
    Pst::NamedProperty keywords;
    keywords.guid = Pst::psPublicStrings;
    keywords.isString = true;
    keywords.name = QStringLiteral("Keywords");
    file.names.insert(0x8000, keywords);
    file.message = fromMime(mime, extras, 0x8000);
    return Msg::write(file);
}

}
