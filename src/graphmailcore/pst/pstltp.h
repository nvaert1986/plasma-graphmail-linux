// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pstndb.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>

#include <optional>

// The LTP layer of a PST file ([MS-PST] 2.3, docs/PST-NOTES.md): the heap on a node, the BTree
// on a heap, property contexts (one object's properties) and table contexts (rows of columns).
namespace GraphMail::Pst
{

// MAPI property types ([MS-OXCDATA] 2.11.1)
enum PropType : quint16 {
    PtypInteger16 = 0x0002,
    PtypInteger32 = 0x0003,
    PtypFloating32 = 0x0004,
    PtypFloating64 = 0x0005,
    PtypCurrency = 0x0006,
    PtypFloatingTime = 0x0007,
    PtypErrorCode = 0x000A,
    PtypBoolean = 0x000B,
    PtypObject = 0x000D,
    PtypInteger64 = 0x0014,
    PtypString8 = 0x001E,
    PtypString = 0x001F,
    PtypTime = 0x0040,
    PtypGuid = 0x0048,
    PtypBinary = 0x0102,
    PtypMultiple = 0x1000,
};

// The size of a fixed-size type, 0 for variable-size and multi-valued ones
int fixedSize(quint16 type);

// Text in a Windows code page (PidTagInternetCodepage, PidTagMessageCodepage); 0 is Windows-1252.
// Unknown code pages fall back to Latin-1.
QString decodeCodepage(const QByteArray &bytes, int codepage);

// One property value as stored (little-endian bytes)
struct PropValue {
    quint16 type = 0;
    QByteArray data;

    qint64 toInt() const;
    bool toBool() const;
    // PtypString (UTF-16LE) or PtypString8 in a Windows code page (0: Windows-1252)
    QString toString(int codepage = 0) const;
    // PtypTime: FILETIME, 100 ns since 1601
    QDateTime toTime() const;
    // A multi-valued property's values (a single value for others)
    QList<QByteArray> items() const;
};

// A heap on a node's data blocks
class Heap
{
public:
    static std::optional<Heap> fromBlocks(const QList<QByteArray> &blocks, QString *error);
    quint8 clientSignature() const
    {
        return m_clientSig;
    }
    quint32 userRoot() const
    {
        return m_userRoot;
    }
    // A heap item by its HID; nullopt for a bad one
    std::optional<QByteArray> item(quint32 hid) const;

private:
    QList<QByteArray> m_blocks;
    // Per block: the item offsets (cAlloc + 1 of them)
    QList<QList<quint16>> m_offsets;
    quint8 m_clientSig = 0;
    quint32 m_userRoot = 0;
};

// The records of a BTree on a heap (key, data), in key order
struct BthRecord {
    QByteArray key;
    QByteArray data;
};
std::optional<QList<BthRecord>> readBth(const Heap &heap, quint32 hidHeader, QString *error);

// Where a node's large values live: its subnodes
class NodeContext
{
public:
    NodeContext(Ndb *ndb, const QHash<quint32, SubnodeEntry> &subnodes)
        : m_ndb(ndb)
        , m_subnodes(subnodes)
    {
    }
    // An HNID: a heap item (HID) or a subnode's data (NID)
    std::optional<QByteArray> resolve(const Heap &heap, quint32 hnid) const;
    const QHash<quint32, SubnodeEntry> &subnodes() const
    {
        return m_subnodes;
    }
    Ndb *ndb() const
    {
        return m_ndb;
    }

private:
    Ndb *m_ndb;
    QHash<quint32, SubnodeEntry> m_subnodes;
};

// A property context: an object's properties by property ID
using PropertyMap = QHash<quint16, PropValue>;
std::optional<PropertyMap> readPropertyContext(const Heap &heap, const NodeContext &context, QString *error);

// A table context: columns and rows (property tag -> value; absent cells left out)
struct TableColumn {
    quint32 tag = 0;
    quint16 offset = 0;
    quint8 size = 0;
    quint8 bit = 0;
};
struct Table {
    QList<TableColumn> columns;
    QList<QHash<quint32, PropValue>> rows;
};
std::optional<Table> readTableContext(const Heap &heap, const NodeContext &context, bool unicode, QString *error);

// A node's heap with its subnodes, and its property context or table context in one call
std::optional<std::pair<Heap, NodeContext>> openNode(Ndb &ndb, quint64 bidData, quint64 bidSub, QString *error);
std::optional<PropertyMap> readNodeProperties(Ndb &ndb, quint64 bidData, quint64 bidSub, QString *error);
std::optional<Table> readNodeTable(Ndb &ndb, quint64 bidData, quint64 bidSub, QString *error);

}
