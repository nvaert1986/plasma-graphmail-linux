// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QList>
#include <QString>

#include <array>
#include <functional>
#include <optional>

// The NDB layer of a PST file ([MS-PST] 2.2, docs/PST-NOTES.md): the header, the node and block
// BTrees, blocks with their trailers and encoding, data trees and subnode trees. Unicode and ANSI
// files are read; nothing here knows what the nodes mean.
namespace GraphMail::Pst
{

struct Bref {
    quint64 bid = 0;
    quint64 ib = 0;
};

struct Header {
    bool unicode = true;
    quint16 version = 0;
    quint16 clientVersion = 0;
    quint8 cryptMethod = 0;
    quint32 crcPartial = 0;
    quint32 crcFull = 0;
    // Whether the stored CRCs match the bytes (dwCRCFull: Unicode only)
    bool crcPartialValid = false;
    bool crcFullValid = false;
    quint64 nextBid = 0;
    quint64 nextPageBid = 0;
    quint32 unique = 0;
    std::array<quint32, 32> nids{};
    // ROOT
    quint64 fileEof = 0;
    quint64 amapLast = 0;
    quint64 amapFree = 0;
    quint64 pmapFree = 0;
    Bref nbt;
    Bref bbt;
    quint8 amapValid = 0;

    // From the first bytes of a file (at least 564); nullopt with the reason when it is no PST
    static std::optional<Header> parse(const QByteArray &bytes, QString *error);
};

struct NodeEntry {
    quint32 nid = 0;
    quint64 data = 0;
    quint64 sub = 0;
    quint32 parent = 0;
};

struct BlockEntry {
    quint64 bid = 0;
    quint64 ib = 0;
    quint16 size = 0;
    quint16 refs = 0;
};

struct SubnodeEntry {
    quint32 nid = 0;
    quint64 data = 0;
    quint64 sub = 0;
};

// A node's type, from the low 5 bits of its NID
inline int nidType(quint32 nid)
{
    return int(nid & 0x1F);
}

// The NDB of an open file. Not thread-safe; one per thread.
class Ndb
{
public:
    bool open(const QString &path, QString *error);
    void close();
    const Header &header() const
    {
        return m_header;
    }
    const QString &error() const
    {
        return m_error;
    }

    std::optional<NodeEntry> node(quint32 nid);
    std::optional<BlockEntry> block(quint64 bid);
    // Every node of the NBT, in key order; false (and error()) when the tree is damaged
    bool forEachNode(const std::function<void(const NodeEntry &)> &visit);
    bool forEachBlock(const std::function<void(const BlockEntry &)> &visit);

    // A node's whole data stream (a data block, or an XBLOCK/XXBLOCK tree), decoded
    std::optional<QByteArray> nodeData(quint32 nid);
    std::optional<QByteArray> treeData(quint64 bid);
    // The same, block by block (a heap addresses its items per data block)
    std::optional<QList<QByteArray>> treeBlocks(quint64 bid);
    // The subnodes under a node's bidSub (SLBLOCKs, or an SIBLOCK of them)
    std::optional<QHash<quint32, SubnodeEntry>> subnodes(quint64 bidSub);

private:
    // Sizes that differ between Unicode and ANSI
    int bidSize() const
    {
        return m_header.unicode ? 8 : 4;
    }
    int trailerSize() const
    {
        return m_header.unicode ? 16 : 12;
    }
    quint64 readId(const char *at) const;
    QByteArray readAt(quint64 ib, qint64 size);
    std::optional<QByteArray> page(quint64 ib, quint8 ptype);
    // A block's data (cb bytes), checked against its trailer; decoded when it is a data block
    std::optional<QByteArray> blockData(quint64 bid, bool decode);
    // The trees with the highest level a block may have there (2 at the top, 1 below an XXBLOCK;
    // 1 for a subnode tree's top, 0 below an SIBLOCK), so a damaged tree cannot recurse forever
    std::optional<QByteArray> treeDataBelow(quint64 bid, int maxLevel);
    std::optional<QList<QByteArray>> treeBlocksBelow(quint64 bid, int maxLevel);
    std::optional<QHash<quint32, SubnodeEntry>> subnodesBelow(quint64 bidSub, int maxLevel);
    bool fail(const QString &text);
    // Leaf entries of the NBT (ptype 0x81) or BBT (0x80) for a key, or all of them
    bool walk(const Bref &root, quint8 ptype, const std::function<bool(const char *entry)> &leaf, std::optional<quint64> key, int depth = 0);

    QFile m_file;
    Header m_header;
    QString m_error;
};

}
