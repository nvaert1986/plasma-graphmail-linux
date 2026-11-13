// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pstndb.h"
#include "pstalgorithms.h"

#include <QtEndian>

namespace GraphMail::Pst
{

namespace
{
constexpr int pageSize = 512;
constexpr int maxBlockSize = 8192;
constexpr quint8 ptypeBBT = 0x80;
constexpr quint8 ptypeNBT = 0x81;

quint16 u16(const char *at)
{
    return qFromLittleEndian<quint16>(at);
}
quint32 u32(const char *at)
{
    return qFromLittleEndian<quint32>(at);
}
quint64 u64(const char *at)
{
    return qFromLittleEndian<quint64>(at);
}
}

std::optional<Header> Header::parse(const QByteArray &bytes, QString *error)
{
    if (bytes.size() < 564 || !bytes.startsWith("!BDN") || bytes.mid(8, 2) != "SM") {
        *error = QStringLiteral("not a PST file");
        return std::nullopt;
    }
    const char *d = bytes.constData();
    Header h;
    h.crcPartial = u32(d + 4);
    h.version = u16(d + 10);
    h.clientVersion = u16(d + 12);
    if (h.version == 14 || h.version == 15) {
        h.unicode = false;
    } else if (h.version >= 23) {
        h.unicode = true;
    } else {
        *error = QStringLiteral("unknown PST version %1").arg(h.version);
        return std::nullopt;
    }
    h.crcPartialValid = crc(d + 8, 471) == h.crcPartial;
    if (h.unicode) {
        // Offsets from the field order of 2.2.2.6 (checked against the sample of 3.2)
        h.nextPageBid = u64(d + 32);
        h.unique = u32(d + 40);
        for (int i = 0; i < 32; ++i) {
            h.nids[i] = u32(d + 44 + 4 * i);
        }
        const char *root = d + 180;
        h.fileEof = u64(root + 4);
        h.amapLast = u64(root + 12);
        h.amapFree = u64(root + 20);
        h.pmapFree = u64(root + 28);
        h.nbt = {u64(root + 36), u64(root + 44)};
        h.bbt = {u64(root + 52), u64(root + 60)};
        h.amapValid = quint8(root[68]);
        h.cryptMethod = quint8(d[513]);
        h.nextBid = u64(d + 516);
        h.crcFull = u32(d + 524);
        h.crcFullValid = crc(d + 8, 516) == h.crcFull;
    } else {
        h.nextBid = u32(d + 24);
        h.nextPageBid = u32(d + 28);
        h.unique = u32(d + 32);
        for (int i = 0; i < 32; ++i) {
            h.nids[i] = u32(d + 36 + 4 * i);
        }
        const char *root = d + 164;
        h.fileEof = u32(root + 4);
        h.amapLast = u32(root + 8);
        h.amapFree = u32(root + 12);
        h.pmapFree = u32(root + 16);
        h.nbt = {u32(root + 20), u32(root + 24)};
        h.bbt = {u32(root + 28), u32(root + 32)};
        h.amapValid = quint8(root[36]);
        // root ends at 204; rgbFM 204..331, rgbFP 332..459, bSentinel 460, bCryptMethod 461
        h.cryptMethod = quint8(d[461]);
        h.crcFullValid = true;
    }
    if (h.cryptMethod != 0 && h.cryptMethod != 1 && h.cryptMethod != 2) {
        *error = QStringLiteral("unsupported encoding %1 (Windows Information Protection?)").arg(h.cryptMethod);
        return std::nullopt;
    }
    return h;
}

bool Ndb::open(const QString &path, QString *error)
{
    close();
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::ReadOnly)) {
        *error = m_file.errorString();
        return false;
    }
    const std::optional<Header> h = Header::parse(m_file.read(564), error);
    if (!h) {
        m_file.close();
        return false;
    }
    if (!h->crcPartialValid || !h->crcFullValid) {
        *error = QStringLiteral("the header's checksum is wrong");
        m_file.close();
        return false;
    }
    m_header = *h;
    return true;
}

void Ndb::close()
{
    m_file.close();
    m_header = {};
    m_error.clear();
}

bool Ndb::fail(const QString &text)
{
    m_error = text;
    return false;
}

quint64 Ndb::readId(const char *at) const
{
    return m_header.unicode ? u64(at) : u32(at);
}

QByteArray Ndb::readAt(quint64 ib, qint64 size)
{
    if (!m_file.seek(qint64(ib))) {
        return {};
    }
    return m_file.read(size);
}

std::optional<QByteArray> Ndb::page(quint64 ib, quint8 ptype)
{
    const QByteArray p = readAt(ib, pageSize);
    if (p.size() != pageSize) {
        fail(QStringLiteral("page at %1 is beyond the end of the file").arg(ib));
        return std::nullopt;
    }
    const char *trailer = p.constData() + pageSize - trailerSize();
    if (quint8(trailer[0]) != ptype || quint8(trailer[1]) != ptype) {
        fail(QStringLiteral("page at %1 has type %2, %3 expected").arg(ib).arg(quint8(trailer[0])).arg(ptype));
        return std::nullopt;
    }
    const quint32 stored = m_header.unicode ? u32(trailer + 4) : u32(trailer + 8);
    if (crc(p.constData(), pageSize - trailerSize()) != stored) {
        fail(QStringLiteral("page at %1 has a wrong checksum").arg(ib));
        return std::nullopt;
    }
    return p;
}

bool Ndb::walk(const Bref &root, quint8 ptype, const std::function<bool(const char *entry)> &leaf, std::optional<quint64> key, int depth)
{
    if (depth > 8) {
        return fail(QStringLiteral("BTree deeper than 8 levels"));
    }
    const std::optional<QByteArray> p = page(root.ib, ptype);
    if (!p) {
        return false;
    }
    const char *d = p->constData();
    const int meta = m_header.unicode ? 488 : 496;
    const int count = quint8(d[meta]);
    const int entrySize = quint8(d[meta + 2]);
    const int level = quint8(d[meta + 3]);
    if (entrySize == 0 || count * entrySize > meta) {
        return fail(QStringLiteral("damaged BTree page at %1").arg(root.ib));
    }
    if (level == 0) {
        for (int i = 0; i < count; ++i) {
            if (!leaf(d + i * entrySize)) {
                return true;
            }
        }
        return true;
    }
    // BTENTRY: btkey, BREF; children hold keys >= btkey
    const int idSize = bidSize();
    for (int i = 0; i < count; ++i) {
        const char *e = d + i * entrySize;
        if (key) {
            const quint64 next = i + 1 < count ? readId(d + (i + 1) * entrySize) : 0;
            if (i + 1 < count && next <= *key) {
                continue;
            }
            if (readId(e) > *key) {
                return true; // smaller than every key: not there
            }
        }
        const Bref child{readId(e + idSize), readId(e + 2 * idSize)};
        if (!walk(child, ptype, leaf, key, depth + 1)) {
            return false;
        }
        if (key) {
            return true;
        }
    }
    return true;
}

std::optional<NodeEntry> Ndb::node(quint32 nid)
{
    std::optional<NodeEntry> found;
    const int id = bidSize();
    walk(m_header.nbt, ptypeNBT, [&](const char *e) {
        if (quint32(readId(e)) == nid) {
            found = NodeEntry{nid, readId(e + id), readId(e + 2 * id), u32(e + 3 * id)};
            return false;
        }
        return true;
    }, nid);
    return found;
}

std::optional<BlockEntry> Ndb::block(quint64 bid)
{
    // The reserved low bit is not part of the key
    const quint64 key = bid & ~quint64(1);
    std::optional<BlockEntry> found;
    const int id = bidSize();
    walk(m_header.bbt, ptypeBBT, [&](const char *e) {
        if ((readId(e) & ~quint64(1)) == key) {
            found = BlockEntry{readId(e), readId(e + id), u16(e + 2 * id), u16(e + 2 * id + 2)};
            return false;
        }
        return true;
    }, key);
    return found;
}

bool Ndb::forEachNode(const std::function<void(const NodeEntry &)> &visit)
{
    const int id = bidSize();
    return walk(m_header.nbt, ptypeNBT, [&](const char *e) {
        visit(NodeEntry{quint32(readId(e)), readId(e + id), readId(e + 2 * id), u32(e + 3 * id)});
        return true;
    }, std::nullopt);
}

bool Ndb::forEachBlock(const std::function<void(const BlockEntry &)> &visit)
{
    const int id = bidSize();
    return walk(m_header.bbt, ptypeBBT, [&](const char *e) {
        visit(BlockEntry{readId(e), readId(e + id), u16(e + 2 * id), u16(e + 2 * id + 2)});
        return true;
    }, std::nullopt);
}

std::optional<QByteArray> Ndb::blockData(quint64 bid, bool decode)
{
    const std::optional<BlockEntry> entry = block(bid);
    if (!entry) {
        if (m_error.isEmpty()) {
            fail(QStringLiteral("block %1 is not in the BBT").arg(bid));
        }
        return std::nullopt;
    }
    // The block takes its data plus the trailer, rounded up to 64 bytes
    const int allocated = (entry->size + trailerSize() + 63) / 64 * 64;
    if (allocated > maxBlockSize) {
        fail(QStringLiteral("block %1 is larger than 8 KB").arg(bid));
        return std::nullopt;
    }
    QByteArray raw = readAt(entry->ib, allocated);
    if (raw.size() != allocated) {
        fail(QStringLiteral("block %1 is beyond the end of the file").arg(bid));
        return std::nullopt;
    }
    const char *trailer = raw.constData() + allocated - trailerSize();
    const quint16 cb = u16(trailer);
    const quint32 storedCrc = m_header.unicode ? u32(trailer + 4) : u32(trailer + 8);
    const quint64 storedBid = m_header.unicode ? u64(trailer + 8) : u32(trailer + 4);
    if (cb != entry->size || (storedBid & ~quint64(1)) != (bid & ~quint64(1))) {
        fail(QStringLiteral("block %1 does not match its trailer").arg(bid));
        return std::nullopt;
    }
    raw.truncate(cb);
    if (crc(raw) != storedCrc) {
        fail(QStringLiteral("block %1 has a wrong checksum").arg(bid));
        return std::nullopt;
    }
    if (decode) {
        if (m_header.cryptMethod == 1) {
            permute(raw.data(), raw.size(), false);
        } else if (m_header.cryptMethod == 2) {
            cyclic(raw.data(), raw.size(), quint32(bid));
        }
    }
    return raw;
}

std::optional<QByteArray> Ndb::treeData(quint64 bid)
{
    return treeDataBelow(bid, 2);
}

std::optional<QByteArray> Ndb::treeDataBelow(quint64 bid, int maxLevel)
{
    // Internal BIDs (bit 1) are XBLOCKs or XXBLOCKs; they are not encoded
    if ((bid & 2) == 0) {
        return blockData(bid, true);
    }
    const std::optional<QByteArray> x = blockData(bid, false);
    if (!x || x->size() < 8 || quint8(x->at(0)) != 1) {
        if (x) {
            fail(QStringLiteral("block %1 is no XBLOCK").arg(bid));
        }
        return std::nullopt;
    }
    const int level = quint8(x->at(1));
    const int count = u16(x->constData() + 2);
    const quint32 total = u32(x->constData() + 4);
    // An XXBLOCK points to XBLOCKs only: a tree that points back at itself ends here
    if (8 + count * bidSize() > x->size() || level < 1 || level > maxLevel) {
        fail(QStringLiteral("damaged XBLOCK %1").arg(bid));
        return std::nullopt;
    }
    QByteArray out;
    out.reserve(total);
    for (int i = 0; i < count; ++i) {
        const quint64 child = readId(x->constData() + 8 + i * bidSize());
        // An XXBLOCK points to XBLOCKs, an XBLOCK to data blocks
        const std::optional<QByteArray> part = level == 2 ? treeDataBelow(child, 1) : blockData(child, true);
        if (!part) {
            return std::nullopt;
        }
        out += *part;
    }
    if (quint32(out.size()) != total) {
        fail(QStringLiteral("XBLOCK %1 holds %2 bytes instead of %3").arg(bid).arg(out.size()).arg(total));
        return std::nullopt;
    }
    return out;
}

std::optional<QList<QByteArray>> Ndb::treeBlocks(quint64 bid)
{
    return treeBlocksBelow(bid, 2);
}

std::optional<QList<QByteArray>> Ndb::treeBlocksBelow(quint64 bid, int maxLevel)
{
    if ((bid & 2) == 0) {
        const std::optional<QByteArray> one = blockData(bid, true);
        if (!one) {
            return std::nullopt;
        }
        return QList<QByteArray>{*one};
    }
    const std::optional<QByteArray> x = blockData(bid, false);
    if (!x || x->size() < 8 || quint8(x->at(0)) != 1) {
        if (x) {
            fail(QStringLiteral("block %1 is no XBLOCK").arg(bid));
        }
        return std::nullopt;
    }
    const int level = quint8(x->at(1));
    const int count = u16(x->constData() + 2);
    if (8 + count * bidSize() > x->size() || level < 1 || level > maxLevel) {
        fail(QStringLiteral("damaged XBLOCK %1").arg(bid));
        return std::nullopt;
    }
    QList<QByteArray> out;
    for (int i = 0; i < count; ++i) {
        const quint64 child = readId(x->constData() + 8 + i * bidSize());
        if (level == 2) {
            const std::optional<QList<QByteArray>> part = treeBlocksBelow(child, 1);
            if (!part) {
                return std::nullopt;
            }
            out += *part;
        } else {
            const std::optional<QByteArray> part = blockData(child, true);
            if (!part) {
                return std::nullopt;
            }
            out.append(*part);
        }
    }
    return out;
}

std::optional<QByteArray> Ndb::nodeData(quint32 nid)
{
    const std::optional<NodeEntry> n = node(nid);
    if (!n) {
        if (m_error.isEmpty()) {
            fail(QStringLiteral("node %1 is not in the NBT").arg(nid));
        }
        return std::nullopt;
    }
    return treeData(n->data);
}

std::optional<QHash<quint32, SubnodeEntry>> Ndb::subnodes(quint64 bidSub)
{
    return subnodesBelow(bidSub, 1);
}

std::optional<QHash<quint32, SubnodeEntry>> Ndb::subnodesBelow(quint64 bidSub, int maxLevel)
{
    QHash<quint32, SubnodeEntry> out;
    if (bidSub == 0) {
        return out;
    }
    const std::optional<QByteArray> b = blockData(bidSub, false);
    if (!b || b->size() < 4 || quint8(b->at(0)) != 2) {
        if (b) {
            fail(QStringLiteral("block %1 is no subnode block").arg(bidSub));
        }
        return std::nullopt;
    }
    const int level = quint8(b->at(1));
    const int count = u16(b->constData() + 2);
    const int start = m_header.unicode ? 8 : 4;
    const int id = bidSize();
    const int entrySize = level == 0 ? 3 * id : 2 * id;
    // An SIBLOCK points to SLBLOCKs only
    if (start + count * entrySize > b->size() || level > maxLevel) {
        fail(QStringLiteral("damaged subnode block %1").arg(bidSub));
        return std::nullopt;
    }
    for (int i = 0; i < count; ++i) {
        const char *e = b->constData() + start + i * entrySize;
        if (level == 0) {
            const SubnodeEntry s{quint32(readId(e)), readId(e + id), readId(e + 2 * id)};
            out.insert(s.nid, s);
        } else {
            // SIENTRY: the SLBLOCK holding the next run of subnodes
            const std::optional<QHash<quint32, SubnodeEntry>> more = subnodesBelow(readId(e + id), 0);
            if (!more) {
                return std::nullopt;
            }
            out.insert(*more);
        }
    }
    return out;
}

}
