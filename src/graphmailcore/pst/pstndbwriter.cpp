// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pstndbwriter.h"
#include "pstalgorithms.h"

#include <QtEndian>

#include <algorithm>

namespace GraphMail::Pst
{

namespace
{
constexpr int pageSize = 512;
constexpr int pageBody = 496;
constexpr int trailerSize = 16;
constexpr quint8 ptypeBBT = 0x80;
constexpr quint8 ptypeNBT = 0x81;
constexpr quint8 ptypeFMap = 0x82;
constexpr quint8 ptypePMap = 0x83;
constexpr quint8 ptypeAMap = 0x84;
constexpr quint8 ptypeFPMap = 0x85;
constexpr quint8 ptypeDList = 0x86;
// Entries per XBLOCK/XXBLOCK, SLBLOCK and SIBLOCK
constexpr int perXBlock = (NdbWriter::maxBlockData - 8) / 8;
constexpr int perSLBlock = (NdbWriter::maxBlockData - 8) / 24;
constexpr int perSIBlock = (NdbWriter::maxBlockData - 8) / 16;
// Entries per BTree page (488 bytes of entries); intermediate entries are 24 bytes
constexpr int btEntryBytes = 488;
constexpr int btIndexEntry = 24;
// 64-byte slots one AMap maps
constexpr int slotsPerAMap = pageBody * 8;

quint64 alignUp(quint64 v, quint64 to)
{
    return (v + to - 1) / to * to;
}

void put16(QByteArray &b, int at, quint16 v)
{
    qToLittleEndian(v, b.data() + at);
}
void put32(QByteArray &b, int at, quint32 v)
{
    qToLittleEndian(v, b.data() + at);
}
void put64(QByteArray &b, int at, quint64 v)
{
    qToLittleEndian(v, b.data() + at);
}
QByteArray le16(quint16 v)
{
    QByteArray b(2, 0);
    put16(b, 0, v);
    return b;
}
QByteArray le32(quint32 v)
{
    QByteArray b(4, 0);
    put32(b, 0, v);
    return b;
}
QByteArray le64(quint64 v)
{
    QByteArray b(8, 0);
    put64(b, 0, v);
    return b;
}
}

bool NdbWriter::create(const QString &path, quint8 cryptMethod, QString *error)
{
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = m_file.errorString();
        return false;
    }
    m_crypt = cryptMethod;
    m_error.clear();
    m_nodes.clear();
    m_blocks.clear();
    m_nextBid = 4;
    m_nextPageBid = 1;
    m_treeBytes.clear();
    // Start values for a new file ([MS-PST] 2.2.2.6, rgnid)
    m_nids.fill(0x400);
    m_nids[3] = 0x4000;
    m_nids[4] = 0x10000;
    m_nids[8] = 0x8000;
    m_cursor = Layout::regionBase(0) + Layout::mapPages(0) * pageSize;
    return true;
}

bool NdbWriter::fail(const QString &text)
{
    if (m_error.isEmpty()) {
        m_error = text;
    }
    return false;
}

quint32 NdbWriter::newNid(quint8 type)
{
    const quint32 index = ++m_nids[type & 0x1F];
    return index << 5 | (type & 0x1F);
}

quint64 NdbWriter::allocate(int size, int align)
{
    using namespace Layout;
    // Gaps (alignment, the tail of a region) stay marked as allocated: see finish()
    quint64 pos = alignUp(m_cursor, quint64(align));
    for (;;) {
        const quint64 region = (pos - firstAMap) / regionSize;
        const quint64 dataStart = regionBase(region) + quint64(mapPages(region)) * pageSize;
        pos = std::max(pos, dataStart);
        if (pos + quint64(size) <= regionBase(region + 1)) {
            break;
        }
        pos = regionBase(region + 1);
    }
    m_cursor = pos + quint64(size);
    return pos;
}

void NdbWriter::writeAt(quint64 ib, const QByteArray &bytes)
{
    if (failed()) {
        return;
    }
    if (!m_file.seek(qint64(ib)) || m_file.write(bytes) != bytes.size()) {
        fail(m_file.errorString());
    }
}

quint64 NdbWriter::writeBlock(QByteArray data, bool internal)
{
    if (data.size() > maxBlockData) {
        fail(QStringLiteral("block of %1 bytes").arg(data.size()));
        return 0;
    }
    const quint64 bid = m_nextBid | (internal ? 2 : 0);
    m_nextBid += 4;
    const int cb = int(data.size());
    const int allocated = int(alignUp(quint64(cb) + trailerSize, 64));
    const quint64 ib = allocate(allocated, 64);
    // Only data blocks are encoded; internal blocks (trees, subnodes) are not
    if (!internal) {
        if (m_crypt == 1) {
            permute(data.data(), data.size(), true);
        } else if (m_crypt == 2) {
            cyclic(data.data(), data.size(), quint32(bid));
        }
    }
    QByteArray block(allocated, 0);
    std::copy(data.cbegin(), data.cend(), block.begin());
    const int t = allocated - trailerSize;
    put16(block, t, quint16(cb));
    put16(block, t + 2, signature(ib, bid));
    put32(block, t + 4, crc(data.constData(), data.size()));
    put64(block, t + 8, bid);
    writeAt(ib, block);
    m_blocks.append({bid, ib, quint16(cb), 2});
    return failed() ? 0 : bid;
}

qint64 NdbWriter::dataBytes(quint64 bid) const
{
    return m_treeBytes.value(bid);
}

quint64 NdbWriter::writeData(const QByteArray &data)
{
    if (data.size() <= maxBlockData) {
        const quint64 bid = writeBlock(data, false);
        m_treeBytes.insert(bid, data.size());
        return bid;
    }
    QList<QByteArray> blocks;
    for (qsizetype at = 0; at < data.size(); at += maxBlockData) {
        blocks.append(data.mid(at, maxBlockData));
    }
    return writeBlocks(blocks);
}

quint64 NdbWriter::writeBlocks(const QList<QByteArray> &blocks)
{
    if (blocks.size() <= 1) {
        const quint64 bid = writeBlock(blocks.value(0), false);
        m_treeBytes.insert(bid, blocks.value(0).size());
        return bid;
    }
    quint64 total = 0;
    QList<quint64> bids;
    QList<quint64> sizes;
    for (const QByteArray &b : blocks) {
        bids.append(writeBlock(b, false));
        sizes.append(quint64(b.size()));
        total += quint64(b.size());
    }
    // lcbTotal is 32 bits: a node holds less than 4 GB
    if (total > 0xFFFFFFFFu) {
        fail(QStringLiteral("a value of %1 bytes is too large for a PST").arg(total));
        return 0;
    }
    const auto xblock = [this](int level, const QList<quint64> &ids, quint64 bytes) {
        QByteArray x(8, 0);
        x[0] = 1;
        x[1] = char(level);
        put16(x, 2, quint16(ids.size()));
        put32(x, 4, quint32(bytes));
        for (const quint64 id : ids) {
            x += le64(id);
        }
        return writeBlock(x, true);
    };
    if (bids.size() <= perXBlock) {
        const quint64 bid = xblock(1, bids, total);
        m_treeBytes.insert(bid, qint64(total));
        return bid;
    }
    QList<quint64> xblocks;
    for (qsizetype at = 0; at < bids.size(); at += perXBlock) {
        const QList<quint64> part = bids.mid(at, perXBlock);
        quint64 bytes = 0;
        for (qsizetype i = at; i < at + part.size(); ++i) {
            bytes += sizes.at(i);
        }
        xblocks.append(xblock(1, part, bytes));
    }
    if (xblocks.size() > perXBlock) {
        fail(QStringLiteral("a value is too large for a PST"));
        return 0;
    }
    const quint64 bid = xblock(2, xblocks, total);
    m_treeBytes.insert(bid, qint64(total));
    return bid;
}

quint64 NdbWriter::writeSubnodes(QList<SubnodeEntry> entries)
{
    if (entries.isEmpty()) {
        return 0;
    }
    std::sort(entries.begin(), entries.end(), [](const SubnodeEntry &a, const SubnodeEntry &b) {
        return a.nid < b.nid;
    });
    for (const SubnodeEntry &e : std::as_const(entries)) {
        noteNid(e.nid);
    }
    const auto slblock = [this](const QList<SubnodeEntry> &part) {
        QByteArray b(8, 0);
        b[0] = 2;
        b[1] = 0;
        put16(b, 2, quint16(part.size()));
        for (const SubnodeEntry &e : part) {
            b += le64(e.nid) + le64(e.data) + le64(e.sub);
        }
        return writeBlock(b, true);
    };
    if (entries.size() <= perSLBlock) {
        return slblock(entries);
    }
    QByteArray si(8, 0);
    si[0] = 2;
    si[1] = 1;
    int count = 0;
    for (qsizetype at = 0; at < entries.size(); at += perSLBlock) {
        const QList<SubnodeEntry> part = entries.mid(at, perSLBlock);
        si += le64(part.first().nid) + le64(slblock(part));
        ++count;
    }
    if (count > perSIBlock) {
        fail(QStringLiteral("too many subnodes"));
        return 0;
    }
    put16(si, 2, quint16(count));
    return writeBlock(si, true);
}

void NdbWriter::addNode(const NodeEntry &node)
{
    noteNid(node.nid);
    m_nodes.append(node);
}

void NdbWriter::noteNid(quint32 nid)
{
    // rgnid must cover every NID index in use, subnodes and tables included (scanpst checks)
    quint32 &mark = m_nids[nid & 0x1F];
    mark = std::max(mark, nid >> 5);
}

void NdbWriter::writePage(quint64 ib, QByteArray body, quint8 ptype, quint64 bid)
{
    body.resize(pageBody, 0);
    QByteArray trailer(trailerSize, 0);
    trailer[0] = char(ptype);
    trailer[1] = char(ptype);
    // Map pages carry no signature; BTree and DList pages do
    const bool map = ptype == ptypeAMap || ptype == ptypePMap || ptype == ptypeFMap || ptype == ptypeFPMap;
    put16(trailer, 2, map ? 0 : signature(ib, bid));
    put32(trailer, 4, crc(body.constData(), body.size()));
    put64(trailer, 8, bid);
    writeAt(ib, body + trailer);
}

Bref NdbWriter::writeBTree(const QList<QByteArray> &leaves, const QList<quint64> &keys, quint8 ptype, int leafSize)
{
    struct Child {
        quint64 key;
        Bref ref;
    };
    QList<Child> level;
    const auto page = [&](const QList<QByteArray> &entries, int entrySize, int cLevel) {
        QByteArray body;
        for (const QByteArray &e : entries) {
            body += e;
        }
        body.resize(pageBody, 0);
        body[488] = char(entries.size());
        body[489] = char(btEntryBytes / entrySize);
        body[490] = char(entrySize);
        body[491] = char(cLevel);
        const quint64 ib = allocate(pageSize, pageSize);
        const quint64 bid = m_nextPageBid++;
        writePage(ib, body, ptype, bid);
        return Bref{bid, ib};
    };
    const int perLeaf = btEntryBytes / leafSize;
    qsizetype at = 0;
    do {
        level.append({keys.value(at), page(leaves.mid(at, perLeaf), leafSize, 0)});
        at += perLeaf;
    } while (at < leaves.size());
    const int perIndex = btEntryBytes / btIndexEntry;
    for (int depth = 1; level.size() > 1; ++depth) {
        QList<Child> next;
        for (qsizetype at = 0; at < level.size(); at += perIndex) {
            QList<QByteArray> entries;
            const QList<Child> part = level.mid(at, perIndex);
            for (const Child &c : std::as_const(part)) {
                entries.append(le64(c.key) + le64(c.ref.bid) + le64(c.ref.ib));
            }
            next.append({level.at(at).key, page(entries, btIndexEntry, depth)});
        }
        level = next;
    }
    return level.first().ref;
}

QByteArray NdbWriter::header(const Bref &nbt, const Bref &bbt, quint64 eof, quint64 amapLast, quint64 amapFree) const
{
    QByteArray h(564, 0);
    h.replace(0, 4, "!BDN");
    h.replace(8, 2, "SM");
    put16(h, 10, 23);
    put16(h, 12, 19);
    h[14] = 1;
    h[15] = 1;
    put64(h, 32, m_nextPageBid);
    put32(h, 40, 1);
    for (int i = 0; i < 32; ++i) {
        put32(h, 44 + 4 * i, m_nids[i]);
    }
    // ROOT at 180: dwReserved, ibFileEof, ibAMapLast, cbAMapFree, cbPMapFree, BREFNBT, BREFBBT,
    // fAMapValid
    put64(h, 184, eof);
    put64(h, 192, amapLast);
    put64(h, 200, amapFree);
    put64(h, 208, 0);
    put64(h, 216, nbt.bid);
    put64(h, 224, nbt.ib);
    put64(h, 232, bbt.bid);
    put64(h, 240, bbt.ib);
    h[248] = 2;
    // rgbFM and rgbFP (deprecated, 0xFF), bSentinel, bCryptMethod, bidNextB
    std::fill(h.begin() + 256, h.begin() + 512, char(0xFF));
    h[512] = char(0x80);
    h[513] = char(m_crypt);
    put64(h, 516, m_nextBid);
    put32(h, 524, crc(h.constData() + 8, 516));
    put32(h, 4, crc(h.constData() + 8, 471));
    return h;
}

bool NdbWriter::finish(QString *error)
{
    using namespace Layout;
    // The BTrees, sorted by key
    std::sort(m_blocks.begin(), m_blocks.end(), [](const BlockEntry &a, const BlockEntry &b) {
        return a.bid < b.bid;
    });
    std::sort(m_nodes.begin(), m_nodes.end(), [](const NodeEntry &a, const NodeEntry &b) {
        return a.nid < b.nid;
    });
    for (qsizetype i = 1; i < m_nodes.size(); ++i) {
        if (m_nodes.at(i).nid == m_nodes.at(i - 1).nid) {
            fail(QStringLiteral("node %1 added twice").arg(m_nodes.at(i).nid, 0, 16));
        }
    }
    QList<QByteArray> leaves;
    QList<quint64> keys;
    for (const BlockEntry &b : std::as_const(m_blocks)) {
        QByteArray e = le64(b.bid) + le64(b.ib) + le16(b.size) + le16(b.refs) + QByteArray(4, 0);
        leaves.append(e);
        keys.append(b.bid);
    }
    const Bref bbt = writeBTree(leaves, keys, ptypeBBT, 24);
    leaves.clear();
    keys.clear();
    for (const NodeEntry &n : std::as_const(m_nodes)) {
        leaves.append(le64(n.nid) + le64(n.data) + le64(n.sub) + le32(n.parent) + QByteArray(4, 0));
        keys.append(n.nid);
    }
    const Bref nbt = writeBTree(leaves, keys, ptypeNBT, 32);

    // Everything up to the cursor, rounded to whole pages, is allocated; so every AMap byte is
    // 0x00 or 0xFF and the order of bits within a byte cannot matter
    const quint64 end = alignUp(m_cursor, pageSize);
    const quint64 lastRegion = (end - 1 - firstAMap) / regionSize;
    const quint64 eof = regionBase(lastRegion + 1);
    quint64 freeBytes = 0;
    QList<int> freeSlots;
    for (quint64 r = 0; r <= lastRegion; ++r) {
        const quint64 base = regionBase(r);
        const quint64 used = r < lastRegion ? regionSize : end - base;
        QByteArray bits(pageBody, 0);
        std::fill(bits.begin(), bits.begin() + qsizetype(used / 512), char(0xFF));
        writePage(base, bits, ptypeAMap, base);
        freeBytes += regionSize - used;
        freeSlots.append(int((regionSize - used) / 64));
        quint64 at = base + pageSize;
        // Deprecated maps: no free pages (cbPMapFree is 0 for new files), FMap the longest free run
        if (hasPMap(r)) {
            writePage(at, QByteArray(pageBody, char(0xFF)), ptypePMap, at);
            at += pageSize;
        }
        if (hasFMap(r)) {
            QByteArray fmap(pageBody, 0);
            for (int i = 0; i < pageBody && r + quint64(i) <= lastRegion; ++i) {
                const quint64 mapped = r + quint64(i);
                const quint64 freeRun = mapped < lastRegion ? 0 : (regionSize - (end - regionBase(mapped))) / 64;
                fmap[i] = char(std::min<quint64>(freeRun, 255));
            }
            writePage(at, fmap, ptypeFMap, at);
            at += pageSize;
        }
        if (hasFPMap(r)) {
            writePage(at, QByteArray(pageBody, char(0xFF)), ptypeFPMap, at);
        }
    }

    // The DList: AMaps with free space, most free first
    QList<int> order;
    for (int r = 0; r < freeSlots.size(); ++r) {
        if (freeSlots.at(r) > 0) {
            order.append(r);
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return freeSlots.at(a) > freeSlots.at(b);
    });
    order = order.mid(0, 476 / 4);
    QByteArray list(8, 0);
    list[0] = 1;
    list[1] = char(order.size());
    put32(list, 4, quint32(order.isEmpty() ? lastRegion : quint64(order.first())));
    for (const int r : std::as_const(order)) {
        list += le32(quint32(r) | quint32(std::min(freeSlots.at(r), slotsPerAMap)) << 20);
    }
    writePage(dlist, list, ptypeDList, m_nextPageBid++);

    if (!failed() && !m_file.resize(qint64(eof))) {
        fail(m_file.errorString());
    }
    writeAt(0, header(nbt, bbt, eof, regionBase(lastRegion), freeBytes));
    m_file.close();
    if (failed()) {
        *error = m_error;
        return false;
    }
    return true;
}

}
