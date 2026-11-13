// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pstwriter.h"
#include "pstalgorithms.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QtEndian>

namespace GraphMail::Pst
{

namespace
{
constexpr quint32 tagRowId = 0x67F20003;
constexpr quint32 tagRowVer = 0x67F30003;
// Internal nodes of the minimum PST (2.7.1)
constexpr quint32 nidSearchManagementQueue = 0x1E1;
constexpr quint32 nidSearchActivityList = 0x201;
constexpr quint32 nidHierarchyTemplate = 0x60D;
constexpr quint32 nidContentsTemplate = 0x60E;
constexpr quint32 nidAssocContentsTemplate = 0x60F;
constexpr quint32 nidSearchContentsTemplate = 0x610;
constexpr quint32 nidAttachmentTable = 0x671;
constexpr quint32 nidRecipientTable = 0x692;
// The receive folder table, and the spam search folder's update queue and criteria object, as
// Outlook makes them (scanpst wants them)
constexpr quint32 nidReceiveFolderTable = 0x62B;
constexpr quint32 nidSpamSearchUpdateQueue = 0x2226;
constexpr quint32 nidSpamSearchCriteria = 0x2227;
// More of what Outlook puts in every new PST and scanpst asks for: the outgoing queue (type 0x0C),
// three empty templates (types 0x16, 0x17, 0x18), the search domain object (0x261) and three
// empty internal nodes (0xEC1 is the update queue scanpst looks for)
constexpr quint32 nidOutgoingQueue = 0x64C;
constexpr quint32 nidSearchDomainObject = 0x261;
QList<std::pair<quint32, QList<quint32>>> moreTemplates()
{
    return {
        {0x6B6, {0x0E330014, 0x0E370102, 0x0E380003}},
        {0x6D7, {0x001A001F, 0x0E300102, 0x0E310102, 0x0E330014, 0x0E340102, 0x0E380003, 0x0E3E0102}},
        {0x6F8, {0x0E330014, 0x30070040}},
    };
}
constexpr quint32 emptyInternalNodes[] = {0xE41, 0xEC1, 0xF21};
// The "HMP" scanpst looks for (not in [MS-PST]): a heap of client signature 0x9C whose user root
// item holds the HID of a BTH from 16-byte keys to 4-byte values. scanpst fills it with each
// object's PidTagReplItemid (0x0E30) GUID -> NID; Outlook writes it empty, and so does this writer.
constexpr quint32 nidHmp = 0xC01;
constexpr quint8 hmpClientSig = 0x9C;
constexpr int maxEmbedDepth = 8;
constexpr int nameBuckets = 251;

// Template columns: the folder tables as in the spec's sample folder (3.12), the others from
// their template tables (2.4.5.3.1, 2.4.6.1.1, 2.4.8.6.2.1). Outlook and scanpst want 0x0E30
// (PidTagReplItemid) as binary, not the spec's integer, and 0x3013 in contents tables.
QList<quint32> hierarchyColumns()
{
    return {
        0x0E300102, 0x0E330014, 0x0E340102, 0x0E380003, 0x3001001F, 0x36020003, 0x36030003,
        0x360A000B, 0x3613001F, 0x66350003, 0x66360003, 0x67F20003, 0x67F30003
    };
}
QList<quint32> contentsColumns()
{
    return {
        0x00170003, 0x001A001F, 0x00360003, 0x0037001F, 0x00390040, 0x0042001F, 0x0057000B,
        0x0058000B, 0x0070001F, 0x00710102, 0x0E03001F, 0x0E04001F, 0x0E060040, 0x0E070003,
        0x0E080003, 0x0E170003, 0x0E300102, 0x0E330014, 0x0E340102, 0x0E380003, 0x0E3C0102,
        0x0E3D0102, 0x10970003, 0x30080040, 0x30130102, 0x65C60003, 0x67F20003, 0x67F30003
    };
}
QList<quint32> assocContentsColumns()
{
    return {
        0x001A001F, 0x003A001F, 0x0070001F, 0x0E070003, 0x0E170003, 0x3001001F, 0x67F20003,
        0x67F30003, 0x6800001F, 0x6803000B, 0x68051003, 0x682F001F, 0x70030003, 0x70040102,
        0x70050102, 0x7006001F, 0x70070003
    };
}
QList<quint32> searchContentsColumns()
{
    return {
        0x00170003, 0x001A001F, 0x00360003, 0x0E070003, 0x0E170003, 0x0037001F, 0x0042001F,
        0x0057000B, 0x0058000B, 0x0E03001F, 0x0E04001F, 0x0E05001F, 0x0E060040, 0x0E080003,
        0x0E2A000B, 0x30080040, 0x67F10003, 0x67F20003, 0x67F30003
    };
}
QList<quint32> recipientColumns()
{
    return {
        0x0C150003, 0x0E0F000B, 0x0FF90102, 0x0FFE0003, 0x0FFF0102, 0x3001001F, 0x3002001F,
        0x3003001F, 0x300B0102, 0x39000003, 0x39FF001F, 0x3A40000B, 0x67F20003, 0x67F30003
    };
}
QList<quint32> attachmentColumns()
{
    return {
        0x0E200003, 0x3704001F, 0x37050003, 0x370B0003, 0x67F20003, 0x67F30003
    };
}

// Property IDs this writer sets
constexpr quint16 propMessageClass = 0x001A;
constexpr quint16 propMessageFlags = 0x0E07;
constexpr quint16 propMessageSize = 0x0E08;
constexpr quint16 propMessageStatus = 0x0E17;
constexpr quint16 propAttachSize = 0x0E20;
constexpr quint16 propRecordKey = 0x0FF9;
constexpr quint16 propDisplayName = 0x3001;
constexpr quint16 propCreationTime = 0x3007;
constexpr quint16 propLastModificationTime = 0x3008;
constexpr quint16 propSearchKey = 0x300B;
constexpr quint16 propValidFolderMask = 0x35DF;
constexpr quint16 propIpmSubTreeEntryId = 0x35E0;
constexpr quint16 propIpmWastebasketEntryId = 0x35E3;
constexpr quint16 propFinderEntryId = 0x35E7;
constexpr quint16 propContentCount = 0x3602;
constexpr quint16 propContentUnreadCount = 0x3603;
constexpr quint16 propSubfolders = 0x360A;
constexpr quint16 propContainerClass = 0x3613;
constexpr quint16 propAttachDataBinary = 0x3701;
constexpr quint16 propAttachMethod = 0x3705;
constexpr quint16 propRenderingPosition = 0x370B;
constexpr quint16 propPstPassword = 0x67FF;
// Message flags ([MS-OXCMSG] 2.2.1.6)
constexpr quint32 mfRead = 0x01;
constexpr quint32 mfHasAttach = 0x10;
constexpr quint32 mfFai = 0x40;
// Attach methods ([MS-OXCMSG] 2.2.2.9)
constexpr quint32 afByValue = 1;
constexpr quint32 afEmbeddedMessage = 5;
constexpr quint32 afStorage = 6;
// The subnode type Outlook gives an OLE object's compound file (not in [MS-PST]'s list)
constexpr quint8 nidTypeStorage = 0x09;
// PidTagValidFolderMask: IPM subtree, wastebasket and finder valid
constexpr quint32 validFolderMask = 0x01 | 0x08 | 0x80;

QByteArray le32(quint32 v)
{
    QByteArray b(4, 0);
    qToLittleEndian(v, b.data());
    return b;
}
QByteArray le64(quint64 v)
{
    QByteArray b(8, 0);
    qToLittleEndian(v, b.data());
    return b;
}
PropValue int32(qint64 v)
{
    return {PtypInteger32, le32(quint32(v))};
}
PropValue boolean(bool v)
{
    return {PtypBoolean, QByteArray(1, v ? 1 : 0)};
}
PropValue string(const QString &s)
{
    return {PtypString, QByteArray(reinterpret_cast<const char *>(s.utf16()), s.size() * 2)};
}
PropValue binary(const QByteArray &b)
{
    return {PtypBinary, b};
}
PropValue now()
{
    // FILETIME: 100 ns since 1601
    const qint64 ms = QDateTime::currentMSecsSinceEpoch() + qint64(11644473600) * 1000;
    return {PtypTime, le64(quint64(ms) * 10000)};
}

// A table row from an object's properties: the columns it has, with matching types
QHash<quint32, PropValue> rowFrom(const PropertyMap &properties, const QList<quint32> &columns, quint32 rowId, quint32 rowVersion)
{
    QHash<quint32, PropValue> row;
    for (const quint32 tag : columns) {
        const auto it = properties.constFind(quint16(tag >> 16));
        if (it != properties.constEnd() && it->type == quint16(tag & 0xFFFF)) {
            row.insert(tag, *it);
        }
    }
    row.insert(tagRowId, int32(rowId));
    row.insert(tagRowVer, int32(rowVersion));
    return row;
}

// Columns of a table: the template's plus every tag the rows carry
QList<quint32> columnsWith(QList<quint32> columns, const QList<QHash<quint32, PropValue>> &rows)
{
    for (const QHash<quint32, PropValue> &row : rows) {
        for (auto it = row.constBegin(); it != row.constEnd(); ++it) {
            if (!columns.contains(it.key())) {
                columns.append(it.key());
            }
        }
    }
    return columns;
}

// PidTagAttachSize as Outlook counts it (checked on all attachments of an Outlook-made file):
// each property's size, fixed types their width (a boolean 1), others their bytes, an object
// the size in its value
qint64 propertyBytes(const PropertyMap &properties)
{
    qint64 total = 0;
    for (const PropValue &v : properties) {
        const int fixed = (v.type & PtypMultiple) ? 0 : fixedSize(v.type);
        if (fixed > 0) {
            total += fixed;
        } else if (v.type == PtypObject && v.data.size() >= 8) {
            total += qFromLittleEndian<quint32>(v.data.constData() + 4);
        } else {
            total += v.data.size();
        }
    }
    return total;
}
}

bool PstWriter::create(const QString &path, const QString &displayName, QString *error)
{
    if (!m_ndb.create(path, 1, error)) {
        return false;
    }
    m_displayName = displayName;
    m_recordKey = QByteArray(16, 0);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(m_recordKey.data()), 4);
    m_folders.clear();
    m_names.clear();
    m_rowVersion = 0;
    // The fixed folders take the first folder NIDs (0x8022, 0x8042, 0x8062) as in the spec
    const quint32 ipm = m_ndb.newNid(NidTypeNormalFolder);
    const quint32 searchRoot = m_ndb.newNid(NidTypeNormalFolder);
    const quint32 deleted = m_ndb.newNid(NidTypeNormalFolder);
    if (ipm != nidIpmSubtree || searchRoot != nidSearchRoot || deleted != nidDeletedItems) {
        *error = QStringLiteral("unexpected folder numbering");
        return false;
    }
    folderNode(nidRootFolder, nidRootFolder, QString(), QString());
    folderNode(nidIpmSubtree, nidRootFolder, QStringLiteral("Top of Personal Folders"), QString());
    folderNode(nidSearchRoot, nidRootFolder, QStringLiteral("Search Root"), QString());
    folderNode(nidSpamSearchFolder, nidRootFolder, QStringLiteral("SPAM Search Folder 2"), QString());
    m_folders[nidSpamSearchFolder].search = true;
    folderNode(nidDeletedItems, nidIpmSubtree, QStringLiteral("Deleted Items"), QStringLiteral("IPF.Note"));
    return true;
}

quint32 PstWriter::folderNode(quint32 nid, quint32 parent, const QString &name, const QString &containerClass)
{
    Folder f;
    f.nid = nid;
    f.parent = parent;
    f.name = name;
    f.containerClass = containerClass;
    m_folders.insert(nid, f);
    if (parent != nid) {
        m_folders[parent].children.append(nid);
    }
    return nid;
}

quint32 PstWriter::addFolder(quint32 parent, const QString &name, const QString &containerClass)
{
    const auto p = m_folders.constFind(parent);
    if (p == m_folders.constEnd() || p->search || parent == nidRootFolder) {
        return 0;
    }
    return folderNode(m_ndb.newNid(NidTypeNormalFolder), parent, name, containerClass);
}

quint16 PstWriter::addName(const NamedProperty &name)
{
    for (int i = 0; i < m_names.size(); ++i) {
        const NamedProperty &n = m_names.at(i);
        if (n.guid == name.guid && n.isString == name.isString && (n.isString ? n.name == name.name : n.id == name.id)) {
            return quint16(0x8000 + i);
        }
    }
    m_names.append(name);
    return quint16(0x8000 + m_names.size() - 1);
}

quint16 PstWriter::namedProperty(const QUuid &guid, quint32 id)
{
    NamedProperty n;
    n.guid = guid;
    n.id = id;
    return addName(n);
}

quint16 PstWriter::namedProperty(const QUuid &guid, const QString &name)
{
    NamedProperty n;
    n.guid = guid;
    n.isString = true;
    n.name = name;
    return addName(n);
}

PstWriter::Written PstWriter::writeMessage(const Message &message, int depth, bool associated)
{
    if (depth > maxEmbedDepth) {
        m_error = QStringLiteral("attached messages nested too deeply");
        return {};
    }
    SubnodeSink subs(m_ndb);
    PropertyMap props = message.properties;

    // The recipient table: always there (2.4.5.3), template columns plus what the rows carry
    QList<QHash<quint32, PropValue>> recipients;
    for (int i = 0; i < message.recipients.size(); ++i) {
        QHash<quint32, PropValue> row = message.recipients.at(i);
        // Row IDs from 1: scanpst takes a key of 0 as overlapping its BTH's minimum
        row.insert(tagRowId, int32(i + 1));
        row.insert(tagRowVer, int32(++m_rowVersion));
        recipients.append(row);
    }
    {
        SubnodeSink tableSubs(m_ndb);
        const QList<QByteArray> blocks = buildTableContext(columnsWith(recipientColumns(), recipients), recipients, tableSubs);
        const quint64 data = m_ndb.writeBlocks(blocks);
        subs.add(nidRecipientTable, data, tableSubs.write(), tableSubs.bytes());
    }

    // Attachments: each a PC subnode; the table lists them by subnode NID
    QList<QHash<quint32, PropValue>> attachmentRows;
    for (int i = 0; i < message.attachments.size(); ++i) {
        const Attachment &a = message.attachments.at(i);
        PropertyMap ap = a.properties;
        SubnodeSink attachmentSubs(m_ndb);
        if (a.embedded) {
            // The object's size is the attached message's PidTagMessageSize, as in Outlook's files
            const Written inner = writeMessage(*a.embedded, depth + 1);
            const quint32 nid = attachmentSubs.add(NidTypeNormalMessage, inner.data, inner.sub, inner.bytes - m_ndb.dataBytes(inner.data));
            ap.insert(propAttachDataBinary, {PtypObject, le32(nid) + le32(quint32(std::min<qint64>(inner.bytes, 0x7FFFFFFF)))});
            ap.insert(propAttachMethod, int32(afEmbeddedMessage));
        } else if (!a.storage.isEmpty()) {
            const quint32 inner = attachmentSubs.add(nidTypeStorage, m_ndb.writeData(a.storage));
            ap.insert(propAttachDataBinary, {PtypObject, le32(inner) + le32(quint32(a.storage.size()))});
            ap.insert(propAttachMethod, int32(afStorage));
        } else if (!ap.contains(propAttachMethod)) {
            ap.insert(propAttachMethod, int32(afByValue));
        }
        if (!ap.contains(propRenderingPosition)) {
            ap.insert(propRenderingPosition, int32(-1));
        }
        // Counted with itself in, as Outlook does; scanpst checks it
        ap.insert(propAttachSize, int32(0));
        ap.insert(propAttachSize, int32(std::min<qint64>(propertyBytes(ap), 0x7FFFFFFF)));
        const QList<QByteArray> blocks = buildPropertyContext(ap, attachmentSubs);
        const quint64 data = m_ndb.writeBlocks(blocks);
        const quint32 nid = subs.add(NidTypeAttachment, data, attachmentSubs.write(), attachmentSubs.bytes());
        attachmentRows.append(rowFrom(ap, attachmentColumns(), nid, ++m_rowVersion));
    }
    if (!attachmentRows.isEmpty()) {
        SubnodeSink tableSubs(m_ndb);
        const QList<QByteArray> blocks = buildTableContext(attachmentColumns(), attachmentRows, tableSubs);
        const quint64 data = m_ndb.writeBlocks(blocks);
        subs.add(nidAttachmentTable, data, tableSubs.write(), tableSubs.bytes());
    }

    // The properties a message PC MUST have (2.4.5.1.1), and the attachment flag
    if (!props.contains(propMessageClass)) {
        props.insert(propMessageClass, string(QStringLiteral("IPM.Note")));
    }
    quint32 flags = quint32(props.value(propMessageFlags).toInt());
    flags = attachmentRows.isEmpty() ? flags & ~mfHasAttach : flags | mfHasAttach;
    // An associated (FAI) message says so in its flags (scanpst checks)
    flags = associated ? flags | mfFai : flags & ~mfFai;
    props.insert(propMessageFlags, int32(flags));
    if (!props.contains(propMessageStatus)) {
        props.insert(propMessageStatus, int32(0));
    }
    if (!props.contains(propCreationTime)) {
        props.insert(propCreationTime, now());
    }
    if (!props.contains(propLastModificationTime)) {
        props.insert(propLastModificationTime, props.value(propCreationTime));
    }
    if (!props.contains(propSearchKey)) {
        QByteArray key(16, 0);
        QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(key.data()), 4);
        props.insert(propSearchKey, binary(key));
    }
    // PidTagMessageSize: the data bytes of this node and all its subnodes (as in every message of
    // an Outlook-made file); set once the PC's own size is known
    props.insert(propMessageSize, int32(0));
    qint64 bytes = 0;
    const QList<QByteArray> blocks = buildPropertyContext(props, subs, [&](qint64 pcBytes) {
        bytes = pcBytes + subs.bytes();
        return quint32(std::min<qint64>(bytes, 0x7FFFFFFF));
    });
    const quint64 data = m_ndb.writeBlocks(blocks);
    props.insert(propMessageSize, int32(std::min<qint64>(bytes, 0x7FFFFFFF)));
    return {data, subs.write(), bytes, props};
}

quint32 PstWriter::addMessage(quint32 folder, const Message &message, bool associated)
{
    const auto f = m_folders.find(folder);
    if (f == m_folders.end() || f->search || folder == nidRootFolder) {
        return 0;
    }
    const quint32 nid = m_ndb.newNid(associated ? NidTypeAssocMessage : NidTypeNormalMessage);
    const Written written = writeMessage(message, 0, associated);
    if (written.data == 0 || !m_error.isEmpty() || m_ndb.failed()) {
        return 0;
    }
    m_ndb.addNode({nid, written.data, written.sub, folder});
    // The contents row shows the message as written: every column the PC has (scanpst checks)
    const PropertyMap &shown = written.properties;
    const quint32 flags = quint32(shown.value(propMessageFlags).toInt());
    if (associated) {
        f->associated.append(rowFrom(shown, assocContentsColumns(), nid, ++m_rowVersion));
    } else {
        f->contents.append(rowFrom(shown, contentsColumns(), nid, ++m_rowVersion));
        if (!(flags & mfRead)) {
            ++f->unread;
        }
    }
    return nid;
}

QByteArray PstWriter::entryId(quint32 nid) const
{
    return QByteArray(4, 0) + m_recordKey + le32(nid);
}

PropertyMap PstWriter::folderProperties(const Folder &folder) const
{
    PropertyMap p;
    p.insert(propDisplayName, string(folder.name));
    p.insert(propContentCount, int32(folder.contents.size()));
    p.insert(propContentUnreadCount, int32(folder.unread));
    p.insert(propSubfolders, boolean(!folder.children.isEmpty()));
    if (!folder.containerClass.isEmpty()) {
        p.insert(propContainerClass, string(folder.containerClass));
    }
    return p;
}

void PstWriter::writePc(quint32 nid, quint32 parent, const PropertyMap &properties)
{
    SubnodeSink subs(m_ndb);
    const QList<QByteArray> blocks = buildPropertyContext(properties, subs);
    const quint64 data = m_ndb.writeBlocks(blocks);
    m_ndb.addNode({nid, data, subs.write(), parent});
}

void PstWriter::writeTable(quint32 nid, const QList<quint32> &columns, const QList<QHash<quint32, PropValue>> &rows)
{
    SubnodeSink subs(m_ndb);
    const QList<QByteArray> blocks = buildTableContext(columns, rows, subs);
    const quint64 data = m_ndb.writeBlocks(blocks);
    m_ndb.addNode({nid, data, subs.write(), 0});
}

PropertyMap PstWriter::nameMap() const
{
    // 2.4.7: the GUID, entry and string streams, and the hash buckets
    QByteArray guids;
    QList<QUuid> guidList;
    QByteArray entries;
    QByteArray strings;
    QList<QByteArray> buckets(nameBuckets);
    for (int i = 0; i < m_names.size(); ++i) {
        const NamedProperty &n = m_names.at(i);
        quint16 guidIndex = 0;
        if (n.guid == psMapi) {
            guidIndex = 1;
        } else if (n.guid == psPublicStrings) {
            guidIndex = 2;
        } else if (!n.guid.isNull()) {
            qsizetype at = guidList.indexOf(n.guid);
            if (at < 0) {
                at = guidList.size();
                guidList.append(n.guid);
                // On disk a GUID is Data1, Data2, Data3 little-endian, then Data4
                guids += le32(n.guid.data1);
                QByteArray w(4, 0);
                qToLittleEndian(n.guid.data2, w.data());
                qToLittleEndian(n.guid.data3, w.data() + 2);
                guids += w + QByteArray(reinterpret_cast<const char *>(n.guid.data4), 8);
            }
            guidIndex = quint16(3 + at);
        }
        const quint16 kind = quint16(guidIndex << 1 | (n.isString ? 1 : 0));
        quint32 id = n.id;
        quint32 hashId = n.id;
        if (n.isString) {
            const QByteArray text(reinterpret_cast<const char *>(n.name.utf16()), n.name.size() * 2);
            id = quint32(strings.size());
            strings += le32(quint32(text.size())) + text;
            strings.resize((strings.size() + 3) / 4 * 4, 0);
            hashId = crc(text.constData(), text.size());
        }
        QByteArray tail(4, 0);
        qToLittleEndian(kind, tail.data());
        qToLittleEndian(quint16(i), tail.data() + 2);
        entries += le32(id) + tail;
        buckets[int((hashId ^ kind) % nameBuckets)] += le32(hashId) + tail;
    }
    PropertyMap p;
    p.insert(0x0001, int32(nameBuckets));
    p.insert(0x0002, binary(guids));
    p.insert(0x0003, binary(entries));
    p.insert(0x0004, binary(strings));
    for (int b = 0; b < nameBuckets; ++b) {
        if (!buckets.at(b).isEmpty()) {
            p.insert(quint16(0x1000 + b), binary(buckets.at(b)));
        }
    }
    return p;
}

bool PstWriter::finish(QString *error)
{
    if (!m_error.isEmpty()) {
        *error = m_error;
        return false;
    }
    // The message store (2.4.3.1)
    PropertyMap store;
    store.insert(propRecordKey, binary(m_recordKey));
    store.insert(propDisplayName, string(m_displayName));
    store.insert(propValidFolderMask, int32(validFolderMask));
    store.insert(propIpmSubTreeEntryId, binary(entryId(nidIpmSubtree)));
    store.insert(propIpmWastebasketEntryId, binary(entryId(nidDeletedItems)));
    store.insert(propFinderEntryId, binary(entryId(nidSearchRoot)));
    store.insert(propPstPassword, int32(0));
    writePc(nidMessageStore, 0, store);
    writePc(nidNameToIdMap, 0, nameMap());

    // Folders: the PC and, for normal folders, the hierarchy, contents and FAI tables
    for (const Folder &f : std::as_const(m_folders)) {
        writePc(f.nid, f.parent, folderProperties(f));
        if (f.search) {
            continue;
        }
        QList<QHash<quint32, PropValue>> children;
        for (const quint32 child : f.children) {
            children.append(rowFrom(folderProperties(m_folders.value(child)), hierarchyColumns(), child, ++m_rowVersion));
        }
        writeTable(withNidType(f.nid, NidTypeHierarchyTable), hierarchyColumns(), children);
        writeTable(withNidType(f.nid, NidTypeContentsTable), contentsColumns(), f.contents);
        writeTable(withNidType(f.nid, NidTypeAssocContentsTable), assocContentsColumns(), f.associated);
    }

    // Templates (no rows) and the empty search queues
    writeTable(nidHierarchyTemplate, hierarchyColumns(), {});
    writeTable(nidContentsTemplate, contentsColumns(), {});
    writeTable(nidAssocContentsTemplate, assocContentsColumns(), {});
    writeTable(nidSearchContentsTemplate, searchContentsColumns(), {});
    writeTable(nidAttachmentTable, attachmentColumns(), {});
    writeTable(nidRecipientTable, recipientColumns(), {});
    writeTable(nidOutgoingQueue, {0x00390040, 0x0E100003, 0x0E140003}, {});
    for (const auto &[nid, columns] : moreTemplates()) {
        writeTable(nid, columns, {});
    }
    m_ndb.addNode({nidSearchDomainObject, m_ndb.writeData(QByteArray()), 0, 0});
    {
        HeapWriter heap(hmpClientSig);
        const quint32 root = heap.allocate(QByteArray(4, 0));
        heap.replace(root, le32(writeBth(heap, 16, 4, {})));
        heap.setUserRoot(root);
        m_ndb.addNode({nidHmp, m_ndb.writeBlocks(heap.blocks()), 0, 0});
    }
    // The spam search folder's contents table, empty, as in Outlook's files
    writeTable(withNidType(nidSpamSearchFolder, NidTypeSearchContentsTable), searchContentsColumns(), {});
    for (const quint32 nid : emptyInternalNodes) {
        m_ndb.addNode({nid, m_ndb.writeData(QByteArray()), 0, 0});
    }
    m_ndb.addNode({nidSearchManagementQueue, m_ndb.writeData(QByteArray()), 0, 0});
    // The search activity list names the search folder (2.4.8.4.2); its update queue is empty and
    // its criteria object holds what Outlook writes there (0x660B = 0)
    m_ndb.addNode({nidSearchActivityList, m_ndb.writeData(le32(nidSpamSearchFolder)), 0, 0});
    m_ndb.addNode({nidSpamSearchUpdateQueue, m_ndb.writeData(QByteArray()), 0, 0});
    writePc(nidSpamSearchCriteria, 0, PropertyMap{{0x660B, int32(0)}});
    // The receive folder table (NID 0x62B; not in the minimum PST, but scanpst wants its default
    // entry): Outlook's one row maps the default message class to the root folder
    QHash<quint32, PropValue> receive{{0x001A001F, string(QString())}, {0x66050003, int32(nidRootFolder)}, {tagRowId, int32(1)}, {tagRowVer, int32(++m_rowVersion)}};
    writeTable(nidReceiveFolderTable, {0x001A001F, 0x66050003}, {receive});
    return m_ndb.finish(error);
}

}
