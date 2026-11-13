// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pstndb.h"

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QList>
#include <QString>

#include <array>

// Writes the NDB layer of a new Unicode PST file ([MS-PST] 2.2 and 2.6.1, docs/PST-NOTES.md).
// The file is written once, front to back: blocks go to the file as they are added, the node and
// block BTrees, the allocation maps and the header follow in finish(). Nothing is modified in
// place, so there are no free blocks, no shared blocks and no transactions to keep.
namespace GraphMail::Pst
{

class NdbWriter
{
public:
    // The most data one block holds (8192 bytes less the trailer)
    static constexpr int maxBlockData = 8176;

    NdbWriter() = default;
    NdbWriter(const NdbWriter &) = delete;
    NdbWriter &operator=(const NdbWriter &) = delete;

    // cryptMethod: 0 none, 1 permute (Outlook's default), 2 cyclic
    bool create(const QString &path, quint8 cryptMethod, QString *error);
    // Whether a write failed; the first error is kept
    bool failed() const
    {
        return !m_error.isEmpty();
    }
    const QString &error() const
    {
        return m_error;
    }

    // A new NID of a type, numbered on from the header's rgnid counter as Outlook does
    quint32 newNid(quint8 type);
    // Raises the header's rgnid mark of the NID's type to cover it
    void noteNid(quint32 nid);

    // A node's data: one block, or a data tree over blocks of up to 8176 bytes. Empty data is one
    // empty block. Returns the BID (0 after a failure).
    quint64 writeData(const QByteArray &data);
    // The same over given blocks (a heap's pages, a table's row blocks), each up to 8176 bytes
    quint64 writeBlocks(const QList<QByteArray> &blocks);
    // The bytes of data under a BID writeData or writeBlocks returned (the leaf blocks' cb, as
    // Outlook counts PidTagMessageSize)
    qint64 dataBytes(quint64 bid) const;
    // A subnode tree (SLBLOCKs, under an SIBLOCK when one is not enough); 0 when there are none
    quint64 writeSubnodes(QList<SubnodeEntry> entries);
    // A top-level node; each NID once
    void addNode(const NodeEntry &node);

    // The BTrees, allocation maps and header; the file is complete and closed afterwards
    bool finish(QString *error);

private:
    quint64 allocate(int size, int align);
    quint64 writeBlock(QByteArray data, bool internal);
    void writePage(quint64 ib, QByteArray body, quint8 ptype, quint64 bid);
    void writeAt(quint64 ib, const QByteArray &bytes);
    bool fail(const QString &text);
    // Writes the BTree pages over sorted leaf entries; returns the root
    Bref writeBTree(const QList<QByteArray> &leaves, const QList<quint64> &keys, quint8 ptype, int leafSize);
    QByteArray header(const Bref &nbt, const Bref &bbt, quint64 eof, quint64 amapLast, quint64 amapFree) const;

    QFile m_file;
    QString m_error;
    quint8 m_crypt = 1;
    quint64 m_nextBid = 4;
    quint64 m_nextPageBid = 1;
    std::array<quint32, 32> m_nids{};
    QHash<quint64, qint64> m_treeBytes;
    // The next free byte; everything before it is allocated except the gaps listed
    quint64 m_cursor = 0;
    // Allocated ranges [start, end), merged where they touch
    QList<std::pair<quint64, quint64>> m_allocated;
    QList<NodeEntry> m_nodes;
    QList<BlockEntry> m_blocks;
};

// The layout of the allocation map pages ([MS-PST] 1.3.2 figure 3, 2.2.2.7): an AMap starts every
// region; a PMap follows in every eighth; an FMap in region 128 and every 496th after (the header
// maps the first 128 AMaps); an FPMap in region 8192 and every 31744th after (the header maps the
// first 1024 PMaps). The region indexes of FMap and FPMap are derived, not stated: see PST-NOTES.
namespace Layout
{
constexpr quint64 firstAMap = 0x4400;
constexpr quint64 regionSize = 253952;
constexpr quint64 dlist = 0x4200;
inline quint64 regionBase(quint64 region)
{
    return firstAMap + region * regionSize;
}
inline bool hasPMap(quint64 region)
{
    return region % 8 == 0;
}
inline bool hasFMap(quint64 region)
{
    return region >= 128 && (region - 128) % 496 == 0;
}
inline bool hasFPMap(quint64 region)
{
    return region >= 8192 && (region - 8192) % 31744 == 0;
}
inline int mapPages(quint64 region)
{
    return 1 + (hasPMap(region) ? 1 : 0) + (hasFMap(region) ? 1 : 0) + (hasFPMap(region) ? 1 : 0);
}
}

}
