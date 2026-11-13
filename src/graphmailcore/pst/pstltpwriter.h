// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pstltp.h"
#include "pstndbwriter.h"

#include <functional>

// Builds the LTP structures of a new PST ([MS-PST] 2.3 and 2.6.2, docs/PST-NOTES.md): heaps,
// BTrees on heaps, property contexts and table contexts, as the data blocks of a node plus the
// subnodes that hold their large values.
namespace GraphMail::Pst
{

// The subnodes of one node being written; values too large for its heap go here
class SubnodeSink
{
public:
    explicit SubnodeSink(NdbWriter &ndb)
        : m_ndb(ndb)
    {
    }
    NdbWriter &ndb()
    {
        return m_ndb;
    }
    // A subnode with a new local NID of a type; nestedBytes: the data under bidSub
    quint32 add(quint8 type, quint64 bidData, quint64 bidSub = 0, qint64 nestedBytes = 0);
    // A subnode with a fixed NID (the recipient and attachment tables)
    void add(quint32 nid, quint64 bidData, quint64 bidSub, qint64 nestedBytes = 0);
    // The data bytes of all subnodes, nested ones included
    qint64 bytes() const
    {
        return m_bytes;
    }
    // The subnode tree; 0 when there are none
    quint64 write();

private:
    NdbWriter &m_ndb;
    QList<SubnodeEntry> m_entries;
    // Local NIDs count from 0x400, clear of the fixed table NIDs (0x671, 0x692)
    quint32 m_next = 0x400;
    qint64 m_bytes = 0;
};

class HeapWriter
{
public:
    // The largest heap item; larger values go to a subnode
    static constexpr int maxItem = 3580;

    explicit HeapWriter(quint8 clientSig)
        : m_clientSig(clientSig)
    {
    }
    // An item (at most maxItem bytes); its HID
    quint32 allocate(const QByteArray &item);
    // Replaces an item by one of the same size
    void replace(quint32 hid, const QByteArray &item);
    void setUserRoot(quint32 hid)
    {
        m_userRoot = hid;
    }
    // The heap's data blocks, each at most NdbWriter::maxBlockData bytes
    QList<QByteArray> blocks() const;

private:
    struct Page {
        QList<QByteArray> items;
        int bytes = 0;
    };
    QList<Page> m_pages;
    quint8 m_clientSig;
    quint32 m_userRoot = 0;
};

// A BTree on a heap over records sorted by key; returns the HID of its header
quint32 writeBth(HeapWriter &heap, int cbKey, int cbEnt, const QList<BthRecord> &records);

// A value of a PC or TC: a heap item when small enough, else a subnode; returns the HNID
quint32 storeValue(HeapWriter &heap, SubnodeSink &subnodes, const QByteArray &value);

// A property context's heap blocks
// messageSize: for a message, PidTagMessageSize (0x0E08) from the PC's own data bytes, called
// once the PC's layout is known (its value does not change the layout)
QList<QByteArray> buildPropertyContext(const PropertyMap &properties, SubnodeSink &subnodes, const std::function<quint32(qint64 pcBytes)> &messageSize = {});

// A table context's heap blocks. The columns (property tags) get PidTagLtpRowId and
// PidTagLtpRowVer added; each row needs its PidTagLtpRowId (the child's NID). Cells whose tag is
// not a column are left out.
QList<QByteArray> buildTableContext(QList<quint32> columns, const QList<QHash<quint32, PropValue>> &rows, SubnodeSink &subnodes);

}
