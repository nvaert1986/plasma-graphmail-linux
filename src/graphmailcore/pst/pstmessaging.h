// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pstltp.h"

#include <QUuid>

#include <memory>

// The messaging layer of a PST file ([MS-PST] 2.4, docs/PST-NOTES.md): the message store, folders
// with their hierarchy and contents tables, messages with recipients and attachments, and the
// named property map.
namespace GraphMail::Pst
{

// Special internal NIDs (2.4.1)
constexpr quint32 nidMessageStore = 0x21;
constexpr quint32 nidNameToIdMap = 0x61;
constexpr quint32 nidRootFolder = 0x122;

// NID types used by the messaging layer (2.2.2.1)
enum NidType : quint8 {
    NidTypeNormalFolder = 0x02,
    NidTypeSearchFolder = 0x03,
    NidTypeNormalMessage = 0x04,
    NidTypeAttachment = 0x05,
    NidTypeAssocMessage = 0x08,
    NidTypeHierarchyTable = 0x0D,
    NidTypeContentsTable = 0x0E,
    NidTypeAssocContentsTable = 0x0F,
    NidTypeSearchContentsTable = 0x10,
    NidTypeAttachmentTable = 0x11,
    NidTypeRecipientTable = 0x12,
};

// The NID of a folder's other parts: same index, another type
inline quint32 withNidType(quint32 nid, quint8 type)
{
    return (nid & ~quint32(0x1F)) | type;
}

// A named property: a property set GUID with a number or a name (2.4.7)
struct NamedProperty {
    QUuid guid;
    bool isString = false;
    quint32 id = 0;
    QString name;
};
// PS_MAPI and PS_PUBLIC_STRINGS, the two property sets a NAMEID can name without the GUID stream
extern const QUuid psMapi;
extern const QUuid psPublicStrings;
// The map from the PC at NID_NAME_TO_ID_MAP: property ID (0x8000 and up) -> name
QHash<quint16, NamedProperty> parseNameMap(const PropertyMap &map);

// A subject without its stored prefix length (2.5.3.1.1): "\x01\x05RE: x" -> "RE: x"
QString decodeSubject(const QString &stored);
// The NID in an ENTRYID (rgbFlags, the store's uid, nid); 0 when it is no PST entry ID
quint32 entryIdNid(const QByteArray &entryId);

struct Message;

struct Attachment {
    PropertyMap properties;
    // An attached message (PidTagAttachDataObject, attach method afEmbeddedMessage), read with the
    // attachment; none when it is damaged (the rest of the message still reads)
    std::shared_ptr<Message> embedded;
    // An OLE object (attach method afStorage): the compound file in PidTagAttachDataObject
    QByteArray storage;
};

struct Message {
    quint32 nid = 0;
    quint32 parent = 0;
    PropertyMap properties;
    // Rows of the recipient table (property tag -> value)
    QList<QHash<quint32, PropValue>> recipients;
    QList<Attachment> attachments;

    // The code page of PtypString8 values (ANSI files)
    int codepage() const;
    QString string(quint16 propId) const;
};

struct FolderInfo {
    quint32 nid = 0;
    QString name;
    qint64 count = 0;
    qint64 unread = 0;
    bool hasSubfolders = false;
    QString containerClass;
    // PidTagAttributeHidden: Outlook's own folders (Recipient Cache, Yammer Root, Quick Step Settings)
    bool hidden = false;
};

class PstFile
{
public:
    // Opens the file and reads the message store and the named property map
    bool open(const QString &path, QString *error);
    void close();

    const Header &header() const
    {
        return m_ndb.header();
    }
    Ndb &ndb()
    {
        return m_ndb;
    }
    const PropertyMap &store() const
    {
        return m_store;
    }
    QString displayName() const;
    // The CRC of a PST password (PidTagPstPassword); 0 when there is none
    quint32 passwordCrc() const;
    // The top of the visible folders (PidTagIpmSubTreeEntryId), else the root folder
    quint32 ipmSubtree() const;
    const QHash<quint16, NamedProperty> &namedProperties() const
    {
        return m_names;
    }

    std::optional<PropertyMap> folder(quint32 nid, QString *error);
    std::optional<QList<FolderInfo>> subfolders(quint32 nid, QString *error);
    // The NIDs of a folder's messages (or of its associated, FAI, messages)
    std::optional<QList<quint32>> messageIds(quint32 folderNid, bool associated, QString *error);
    std::optional<Message> message(quint32 nid, QString *error);

private:
    std::optional<Message> readMessage(quint64 bidData, quint64 bidSub, int depth, QString *error);
    std::optional<Table> table(quint32 nid, QString *error);

    Ndb m_ndb;
    PropertyMap m_store;
    QHash<quint16, NamedProperty> m_names;
};

}
