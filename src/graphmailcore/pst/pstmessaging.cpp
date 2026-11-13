// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pstmessaging.h"

#include <QtEndian>

namespace GraphMail::Pst
{

const QUuid psMapi(0x00020328, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
const QUuid psPublicStrings(0x00020329, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);

namespace
{
// Property IDs this layer reads
constexpr quint16 propSubject = 0x0037;
constexpr quint16 propDisplayName = 0x3001;
constexpr quint16 propIpmSubTreeEntryId = 0x35E0;
constexpr quint16 propContentCount = 0x3602;
constexpr quint16 propContentUnreadCount = 0x3603;
constexpr quint16 propSubfolders = 0x360A;
constexpr quint16 propContainerClass = 0x3613;
constexpr quint16 propAttributeHidden = 0x10F4;
constexpr quint16 propAttachDataObject = 0x3701;
constexpr quint16 propAttachMethod = 0x3705;
constexpr qint64 afEmbeddedMessage = 5;
constexpr qint64 afStorage = 6;
constexpr quint16 propInternetCodepage = 0x3FDE;
constexpr quint16 propMessageCodepage = 0x3FFD;
constexpr quint16 propPstPassword = 0x67FF;
constexpr quint32 tagLtpRowId = 0x67F20003;
// Named property map (2.4.7)
constexpr quint16 propNameidStreamGuid = 0x0002;
constexpr quint16 propNameidStreamEntry = 0x0003;
constexpr quint16 propNameidStreamString = 0x0004;
// Embedded messages inside embedded messages; more is a damaged or hostile file
constexpr int maxEmbedDepth = 8;

quint16 u16(const char *at)
{
    return qFromLittleEndian<quint16>(at);
}
quint32 u32(const char *at)
{
    return qFromLittleEndian<quint32>(at);
}

bool setError(QString *error, const QString &text)
{
    if (error) {
        *error = text;
    }
    return false;
}

// A table cell by property ID, whatever its type (PtypString in Unicode files, PtypString8 in ANSI)
const PropValue *cell(const QHash<quint32, PropValue> &row, quint16 propId)
{
    for (auto it = row.constBegin(); it != row.constEnd(); ++it) {
        if (it.key() >> 16 == propId) {
            return &it.value();
        }
    }
    return nullptr;
}

QUuid guidAt(const char *at)
{
    return QUuid(u32(at), u16(at + 4), u16(at + 6), quint8(at[8]), quint8(at[9]), quint8(at[10]), quint8(at[11]), quint8(at[12]), quint8(at[13]),
                 quint8(at[14]), quint8(at[15]));
}
}

QHash<quint16, NamedProperty> parseNameMap(const PropertyMap &map)
{
    QHash<quint16, NamedProperty> out;
    const QByteArray guids = map.value(propNameidStreamGuid).data;
    const QByteArray entries = map.value(propNameidStreamEntry).data;
    const QByteArray strings = map.value(propNameidStreamString).data;
    for (qsizetype at = 0; at + 8 <= entries.size(); at += 8) {
        const char *e = entries.constData() + at;
        const quint32 id = u32(e);
        const quint16 guidAndKind = u16(e + 4);
        const quint16 index = u16(e + 6);
        NamedProperty p;
        p.isString = guidAndKind & 1;
        const int guid = guidAndKind >> 1;
        if (guid == 1) {
            p.guid = psMapi;
        } else if (guid == 2) {
            p.guid = psPublicStrings;
        } else if (guid >= 3) {
            const qsizetype g = qsizetype(guid - 3) * 16;
            if (g + 16 > guids.size()) {
                continue;
            }
            p.guid = guidAt(guids.constData() + g);
        }
        if (p.isString) {
            // A length (bytes) and UTF-16 text, at the offset the record gives
            if (qsizetype(id) + 4 > strings.size()) {
                continue;
            }
            const quint32 length = u32(strings.constData() + id);
            if (qsizetype(id) + 4 + length > strings.size()) {
                continue;
            }
            p.name = QString::fromUtf16(reinterpret_cast<const char16_t *>(strings.constData() + id + 4), length / 2);
        } else {
            p.id = id;
        }
        out.insert(quint16(0x8000 + index), p);
    }
    return out;
}

QString decodeSubject(const QString &stored)
{
    if (stored.size() >= 2 && stored.at(0).unicode() == 1) {
        return stored.mid(2);
    }
    return stored;
}

quint32 entryIdNid(const QByteArray &entryId)
{
    if (entryId.size() != 24 || u32(entryId.constData()) != 0) {
        return 0;
    }
    return u32(entryId.constData() + 20);
}

int Message::codepage() const
{
    if (properties.contains(propMessageCodepage)) {
        return int(properties.value(propMessageCodepage).toInt());
    }
    return int(properties.value(propInternetCodepage).toInt());
}

QString Message::string(quint16 propId) const
{
    const QString s = properties.value(propId).toString(codepage());
    return propId == propSubject ? decodeSubject(s) : s;
}

bool PstFile::open(const QString &path, QString *error)
{
    close();
    if (!m_ndb.open(path, error)) {
        return false;
    }
    const std::optional<PropertyMap> store = folder(nidMessageStore, error);
    if (!store) {
        close();
        return false;
    }
    m_store = *store;
    // The map is required, but a file without it is still readable (names stay unknown)
    if (const std::optional<PropertyMap> names = folder(nidNameToIdMap, nullptr)) {
        m_names = parseNameMap(*names);
    }
    return true;
}

void PstFile::close()
{
    m_ndb.close();
    m_store.clear();
    m_names.clear();
}

QString PstFile::displayName() const
{
    return m_store.value(propDisplayName).toString();
}

quint32 PstFile::passwordCrc() const
{
    return quint32(m_store.value(propPstPassword).toInt());
}

quint32 PstFile::ipmSubtree() const
{
    const quint32 nid = entryIdNid(m_store.value(propIpmSubTreeEntryId).data);
    return nid != 0 ? nid : nidRootFolder;
}

std::optional<PropertyMap> PstFile::folder(quint32 nid, QString *error)
{
    const std::optional<NodeEntry> node = m_ndb.node(nid);
    if (!node) {
        setError(error, m_ndb.error().isEmpty() ? QStringLiteral("node %1 not found").arg(nid, 0, 16) : m_ndb.error());
        return std::nullopt;
    }
    return readNodeProperties(m_ndb, node->data, node->sub, error);
}

std::optional<Table> PstFile::table(quint32 nid, QString *error)
{
    const std::optional<NodeEntry> node = m_ndb.node(nid);
    if (!node) {
        setError(error, m_ndb.error().isEmpty() ? QStringLiteral("table %1 not found").arg(nid, 0, 16) : m_ndb.error());
        return std::nullopt;
    }
    return readNodeTable(m_ndb, node->data, node->sub, error);
}

std::optional<QList<FolderInfo>> PstFile::subfolders(quint32 nid, QString *error)
{
    const std::optional<Table> t = table(withNidType(nid, NidTypeHierarchyTable), error);
    if (!t) {
        return std::nullopt;
    }
    QList<FolderInfo> out;
    for (const QHash<quint32, PropValue> &row : t->rows) {
        FolderInfo f;
        f.nid = quint32(row.value(tagLtpRowId).toInt());
        if (const PropValue *v = cell(row, propDisplayName)) {
            f.name = v->toString();
        }
        if (const PropValue *v = cell(row, propContentCount)) {
            f.count = v->toInt();
        }
        if (const PropValue *v = cell(row, propContentUnreadCount)) {
            f.unread = v->toInt();
        }
        if (const PropValue *v = cell(row, propSubfolders)) {
            f.hasSubfolders = v->toBool();
        }
        if (const PropValue *v = cell(row, propContainerClass)) {
            f.containerClass = v->toString();
        }
        // In the folder's property context only ([MS-PST] 2.4.4.1)
        QString ignored;
        if (const std::optional<PropertyMap> props = folder(f.nid, &ignored)) {
            f.hidden = props->value(propAttributeHidden).toBool();
        }
        out.append(f);
    }
    return out;
}

std::optional<QList<quint32>> PstFile::messageIds(quint32 folderNid, bool associated, QString *error)
{
    const std::optional<Table> t = table(withNidType(folderNid, associated ? NidTypeAssocContentsTable : NidTypeContentsTable), error);
    if (!t) {
        return std::nullopt;
    }
    QList<quint32> out;
    out.reserve(t->rows.size());
    for (const QHash<quint32, PropValue> &row : t->rows) {
        out.append(quint32(row.value(tagLtpRowId).toInt()));
    }
    return out;
}

std::optional<Message> PstFile::message(quint32 nid, QString *error)
{
    const std::optional<NodeEntry> node = m_ndb.node(nid);
    if (!node) {
        setError(error, m_ndb.error().isEmpty() ? QStringLiteral("message %1 not found").arg(nid, 0, 16) : m_ndb.error());
        return std::nullopt;
    }
    std::optional<Message> m = readMessage(node->data, node->sub, 0, error);
    if (m) {
        m->nid = nid;
        m->parent = node->parent;
    }
    return m;
}

std::optional<Message> PstFile::readMessage(quint64 bidData, quint64 bidSub, int depth, QString *error)
{
    if (depth > maxEmbedDepth) {
        setError(error, QStringLiteral("attached messages nested too deeply"));
        return std::nullopt;
    }
    const auto node = openNode(m_ndb, bidData, bidSub, error);
    if (!node) {
        return std::nullopt;
    }
    const std::optional<PropertyMap> props = readPropertyContext(node->first, node->second, error);
    if (!props) {
        return std::nullopt;
    }
    Message m;
    m.properties = *props;
    const QHash<quint32, SubnodeEntry> &subs = node->second.subnodes();
    for (const SubnodeEntry &sub : subs) {
        if (nidType(sub.nid) != NidTypeRecipientTable) {
            continue;
        }
        const std::optional<Table> recipients = readNodeTable(m_ndb, sub.data, sub.sub, error);
        if (!recipients) {
            return std::nullopt;
        }
        m.recipients = recipients->rows;
    }
    for (const SubnodeEntry &sub : subs) {
        if (nidType(sub.nid) != NidTypeAttachmentTable) {
            continue;
        }
        const std::optional<Table> table = readNodeTable(m_ndb, sub.data, sub.sub, error);
        if (!table) {
            return std::nullopt;
        }
        // Each row's ID is the subnode of its attachment object
        for (const QHash<quint32, PropValue> &row : table->rows) {
            const auto a = subs.constFind(quint32(row.value(tagLtpRowId).toInt()));
            if (a == subs.constEnd()) {
                setError(error, QStringLiteral("attachment without its object"));
                return std::nullopt;
            }
            const auto attachmentNode = openNode(m_ndb, a->data, a->sub, error);
            if (!attachmentNode) {
                return std::nullopt;
            }
            const std::optional<PropertyMap> attachmentProps = readPropertyContext(attachmentNode->first, attachmentNode->second, error);
            if (!attachmentProps) {
                return std::nullopt;
            }
            Attachment attachment;
            attachment.properties = *attachmentProps;
            // {Nid, ulSize}, the NID a subnode of the attachment: a whole message for afEmbeddedMessage,
            // a compound file for afStorage (an OLE object; Outlook-made files have them)
            const PropValue object = attachment.properties.value(propAttachDataObject);
            const qint64 method = attachment.properties.value(propAttachMethod).toInt();
            if (object.type == PtypObject && object.data.size() >= 8) {
                const auto inner = attachmentNode->second.subnodes().constFind(u32(object.data.constData()));
                if (inner != attachmentNode->second.subnodes().constEnd() && method == afEmbeddedMessage) {
                    // A damaged attached message costs that attachment, not the message around it
                    QString ignored;
                    if (std::optional<Message> embedded = readMessage(inner->data, inner->sub, depth + 1, &ignored)) {
                        attachment.embedded = std::make_shared<Message>(std::move(*embedded));
                    }
                } else if (inner != attachmentNode->second.subnodes().constEnd() && method == afStorage) {
                    if (std::optional<QByteArray> data = m_ndb.treeData(inner->data)) {
                        attachment.storage = std::move(*data);
                    }
                }
            }
            m.attachments.append(attachment);
        }
    }
    return m;
}

}
