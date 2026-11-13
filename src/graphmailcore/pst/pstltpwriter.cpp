// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pstltpwriter.h"

#include <QtEndian>

#include <algorithm>

namespace GraphMail::Pst
{

namespace
{
constexpr quint8 typeTC = 0x7C;
constexpr quint8 typeBTH = 0xB5;
constexpr quint8 typePC = 0xBC;
constexpr quint8 nidTypeLtp = 0x1F;
constexpr quint32 tagRowId = 0x67F20003;
constexpr quint32 tagRowVer = 0x67F30003;
// Heap items per block: hidIndex has 11 bits
constexpr int maxItemsPerBlock = 0x7FF;
// BTH index and leaf items are kept under 90% full (2.6.2.2.2)
constexpr int bthFill = HeapWriter::maxItem * 9 / 10;

QByteArray le16(quint16 v)
{
    QByteArray b(2, 0);
    qToLittleEndian(v, b.data());
    return b;
}
QByteArray le32(quint32 v)
{
    QByteArray b(4, 0);
    qToLittleEndian(v, b.data());
    return b;
}

// HNHDR on block 0; HNBITMAPHDR on block 8 and every 128th after; HNPAGEHDR elsewhere (2.3.1)
int headerSize(int block)
{
    if (block == 0) {
        return 12;
    }
    return block >= 8 && (block - 8) % 128 == 0 ? 66 : 2;
}

// The fill level of a block from its free space (2.3.1.2)
quint8 fillLevel(int freeBytes)
{
    static const int bounds[] = {3584, 2560, 2048, 1792, 1536, 1280, 1024, 768, 512, 256, 128, 64, 32, 16, 8};
    for (int i = 0; i < 15; ++i) {
        if (freeBytes >= bounds[i]) {
            return quint8(i);
        }
    }
    return 0xF;
}

// Fill levels of blocks [first, first + count) as 4-bit values, two per byte, the lower block in
// the low nibble (the spec does not say; see PST-NOTES)
QByteArray fillLevels(const QList<int> &sizes, int first, int count)
{
    QByteArray out(count / 2, 0);
    for (int i = 0; i < count && first + i < sizes.size(); ++i) {
        const quint8 level = fillLevel(NdbWriter::maxBlockData - sizes.at(first + i));
        out[i / 2] = char(quint8(out.at(i / 2)) | (i % 2 ? level << 4 : level));
    }
    return out;
}

quint32 bthInto(HeapWriter &heap, quint32 header, int cbKey, int cbEnt, const QList<BthRecord> &records)
{
    quint32 root = 0;
    int levels = 0;
    if (!records.isEmpty()) {
        struct Ref {
            QByteArray key;
            quint32 hid;
        };
        QList<Ref> level;
        const int perLeaf = std::max(1, bthFill / (cbKey + cbEnt));
        for (qsizetype at = 0; at < records.size(); at += perLeaf) {
            QByteArray item;
            const qsizetype end = std::min(records.size(), at + perLeaf);
            for (qsizetype i = at; i < end; ++i) {
                item += records.at(i).key + records.at(i).data;
            }
            level.append({records.at(at).key, heap.allocate(item)});
        }
        const int perIndex = std::max(2, bthFill / (cbKey + 4));
        while (level.size() > 1) {
            QList<Ref> next;
            for (qsizetype at = 0; at < level.size(); at += perIndex) {
                QByteArray item;
                const qsizetype end = std::min(level.size(), at + perIndex);
                for (qsizetype i = at; i < end; ++i) {
                    item += level.at(i).key + le32(level.at(i).hid);
                }
                next.append({level.at(at).key, heap.allocate(item)});
            }
            level = next;
            ++levels;
        }
        root = level.first().hid;
    }
    heap.replace(header, QByteArray(1, char(typeBTH)) + char(cbKey) + char(cbEnt) + char(levels) + le32(root));
    return header;
}

// The width of a TC column in the row: inline fixed values up to 8 bytes, else an HNID
int columnWidth(quint32 tag)
{
    const quint16 type = quint16(tag & 0xFFFF);
    const int size = (type & PtypMultiple) ? 0 : fixedSize(type);
    return size > 0 && size <= 8 ? size : 4;
}
bool inlineColumn(quint32 tag)
{
    const quint16 type = quint16(tag & 0xFFFF);
    const int size = (type & PtypMultiple) ? 0 : fixedSize(type);
    return size > 0 && size <= 8;
}
}

quint32 SubnodeSink::add(quint8 type, quint64 bidData, quint64 bidSub, qint64 nestedBytes)
{
    const quint32 nid = m_next++ << 5 | (type & 0x1F);
    m_entries.append({nid, bidData, bidSub});
    m_bytes += m_ndb.dataBytes(bidData) + nestedBytes;
    return nid;
}

void SubnodeSink::add(quint32 nid, quint64 bidData, quint64 bidSub, qint64 nestedBytes)
{
    m_entries.append({nid, bidData, bidSub});
    m_bytes += m_ndb.dataBytes(bidData) + nestedBytes;
}

quint64 SubnodeSink::write()
{
    return m_ndb.writeSubnodes(m_entries);
}

quint32 HeapWriter::allocate(const QByteArray &item)
{
    Q_ASSERT(item.size() <= maxItem);
    const auto fits = [&](const Page &p, int block) {
        const int count = int(p.items.size()) + 1;
        // Header, items, a byte of padding at most, then cAlloc, cFree and count + 1 offsets
        return count <= maxItemsPerBlock && headerSize(block) + p.bytes + int(item.size()) + 1 + 4 + 2 * (count + 1) <= NdbWriter::maxBlockData;
    };
    if (m_pages.isEmpty() || !fits(m_pages.last(), int(m_pages.size()) - 1)) {
        m_pages.append(Page());
    }
    Page &p = m_pages.last();
    p.items.append(item);
    p.bytes += int(item.size());
    return quint32(m_pages.size() - 1) << 16 | quint32(p.items.size()) << 5;
}

void HeapWriter::replace(quint32 hid, const QByteArray &item)
{
    QByteArray &old = m_pages[int(hid >> 16)].items[int((hid >> 5) & 0x7FF) - 1];
    Q_ASSERT(old.size() == item.size());
    old = item;
}

QList<QByteArray> HeapWriter::blocks() const
{
    // A heap has at least its first block, even without items
    const qsizetype count = std::max<qsizetype>(1, m_pages.size());
    QList<QByteArray> out;
    QList<int> sizes;
    for (qsizetype b = 0; b < count; ++b) {
        const Page p = b < m_pages.size() ? m_pages.at(b) : Page();
        QByteArray block(headerSize(int(b)), 0);
        QList<quint16> offsets{quint16(block.size())};
        for (const QByteArray &item : p.items) {
            block += item;
            offsets.append(quint16(block.size()));
        }
        if (block.size() % 2) {
            block += '\0';
        }
        qToLittleEndian(quint16(block.size()), block.data());
        block += le16(quint16(p.items.size())) + le16(0);
        for (const quint16 at : std::as_const(offsets)) {
            block += le16(at);
        }
        sizes.append(int(block.size()));
        // Every block of a data tree but the last is full (scanpst: "middle page not full");
        // the free space follows the page map, as in Outlook's files
        if (b + 1 < count) {
            block.resize(NdbWriter::maxBlockData, 0);
        }
        out.append(block);
    }
    // HNHDR: bSig, bClientSig, hidUserRoot, the fill levels of blocks 0-7; HNBITMAPHDR: 128 more
    QByteArray &first = out.first();
    first[2] = char(0xEC);
    first[3] = char(m_clientSig);
    first.replace(4, 4, le32(m_userRoot));
    first.replace(8, 4, fillLevels(sizes, 0, 8));
    for (qsizetype b = 8; b < out.size(); b += 128) {
        out[b].replace(2, 64, fillLevels(sizes, int(b), 128));
    }
    return out;
}

quint32 writeBth(HeapWriter &heap, int cbKey, int cbEnt, const QList<BthRecord> &records)
{
    return bthInto(heap, heap.allocate(QByteArray(8, 0)), cbKey, cbEnt, records);
}

quint32 storeValue(HeapWriter &heap, SubnodeSink &subnodes, const QByteArray &value)
{
    // Empty: HNID 0, as Outlook writes it (a zero-length item reads as freed to scanpst)
    if (value.isEmpty()) {
        return 0;
    }
    if (value.size() <= HeapWriter::maxItem) {
        return heap.allocate(value);
    }
    return subnodes.add(nidTypeLtp, subnodes.ndb().writeData(value));
}

QList<QByteArray> buildPropertyContext(const PropertyMap &properties, SubnodeSink &subnodes, const std::function<quint32(qint64 pcBytes)> &messageSize)
{
    HeapWriter heap(typePC);
    // The BTH header first, as at HID 0x20 in the spec's samples
    const quint32 header = heap.allocate(QByteArray(8, 0));
    QList<quint16> ids = properties.keys();
    std::sort(ids.begin(), ids.end());
    QList<BthRecord> records;
    for (const quint16 id : std::as_const(ids)) {
        const PropValue &v = properties[id];
        const int size = (v.type & PtypMultiple) ? 0 : fixedSize(v.type);
        quint32 value = 0;
        if (size > 0 && size <= 4) {
            QByteArray inlined = v.data.left(size);
            inlined.resize(4, 0);
            value = qFromLittleEndian<quint32>(inlined.constData());
        } else if (size > 4) {
            QByteArray fixed = v.data.left(size);
            fixed.resize(size, 0);
            value = heap.allocate(fixed);
        } else {
            value = storeValue(heap, subnodes, v.data);
        }
        records.append({le16(id), le16(v.type) + le32(value)});
    }
    if (messageSize) {
        // Lay the PC out once on a copy to learn its size, then set the size and lay it out
        HeapWriter trial = heap;
        bthInto(trial, header, 2, 6, records);
        qint64 pcBytes = 0;
        for (const QByteArray &b : trial.blocks()) {
            pcBytes += b.size();
        }
        for (BthRecord &r : records) {
            if (r.key == le16(0x0E08)) {
                r.data = le16(PtypInteger32) + le32(messageSize(pcBytes));
            }
        }
    }
    bthInto(heap, header, 2, 6, records);
    heap.setUserRoot(header);
    return heap.blocks();
}

QList<QByteArray> buildTableContext(QList<quint32> columns, const QList<QHash<quint32, PropValue>> &rows, SubnodeSink &subnodes)
{
    columns.removeAll(tagRowId);
    columns.removeAll(tagRowVer);
    std::sort(columns.begin(), columns.end());
    columns.erase(std::unique(columns.begin(), columns.end()), columns.end());

    // Row layout (2.3.4.4.1): RowId at 0 (bit 0), RowVer at 4 (bit 1), then 8- and 4-byte
    // values, 2-byte, 1-byte, and the cell existence bits
    QList<TableColumn> layout{{tagRowId, 0, 4, 0}, {tagRowVer, 4, 4, 1}};
    int at = 8;
    quint16 rgib[4] = {};
    int bit = 2;
    for (const int width : {8, 4, 2, 1}) {
        for (const quint32 tag : std::as_const(columns)) {
            if (columnWidth(tag) == width) {
                layout.append({tag, quint16(at), quint8(width), quint8(bit++)});
                at += width;
            }
        }
        if (width == 4) {
            rgib[0] = quint16(at);
        } else if (width == 2) {
            rgib[1] = quint16(at);
        } else if (width == 1) {
            rgib[2] = quint16(at);
        }
    }
    const int cebSize = (int(layout.size()) + 7) / 8;
    rgib[3] = quint16(at + cebSize);
    const int rowSize = rgib[3];

    HeapWriter heap(typeTC);
    const quint32 info = heap.allocate(QByteArray(22 + 8 * layout.size(), 0));

    // The RowIndex: row ID -> row number, sorted by row ID
    QList<std::pair<quint32, quint32>> index;
    for (int i = 0; i < rows.size(); ++i) {
        index.append({quint32(rows.at(i).value(tagRowId).toInt()), quint32(i)});
    }
    std::sort(index.begin(), index.end());
    QList<BthRecord> indexRecords;
    for (const auto &[id, number] : std::as_const(index)) {
        indexRecords.append({le32(id), le32(number)});
    }
    const quint32 rowIndex = writeBth(heap, 4, 4, indexRecords);

    QByteArray matrix;
    for (const QHash<quint32, PropValue> &row : rows) {
        QByteArray r(rowSize, 0);
        for (const TableColumn &col : std::as_const(layout)) {
            const auto cell = row.constFind(col.tag);
            const bool always = col.tag == tagRowId || col.tag == tagRowVer;
            if (cell == row.constEnd() && !always) {
                continue;
            }
            r[rgib[2] + col.bit / 8] = char(quint8(r.at(rgib[2] + col.bit / 8)) | (1 << (7 - col.bit % 8)));
            if (cell == row.constEnd()) {
                continue;
            }
            if (inlineColumn(col.tag)) {
                const QByteArray v = cell->data.left(col.size);
                r.replace(col.offset, v.size(), v);
            } else {
                r.replace(col.offset, 4, le32(storeValue(heap, subnodes, cell->data)));
            }
        }
        matrix += r;
    }
    quint32 hnidRows = 0;
    if (!rows.isEmpty()) {
        if (matrix.size() <= HeapWriter::maxItem) {
            hnidRows = heap.allocate(matrix);
        } else {
            // Whole rows per block; every block but the last fills its 8 KB (2.3.4.4)
            const int perBlock = NdbWriter::maxBlockData / rowSize;
            QList<QByteArray> blocks;
            for (qsizetype from = 0; from < rows.size(); from += perBlock) {
                QByteArray b = matrix.mid(from * rowSize, qsizetype(perBlock) * rowSize);
                if (from + perBlock < rows.size()) {
                    b.resize(NdbWriter::maxBlockData, 0);
                }
                blocks.append(b);
            }
            hnidRows = subnodes.add(nidTypeLtp, subnodes.ndb().writeBlocks(blocks));
        }
    }

    // TCINFO: bType, cCols, rgib, hidRowIndex, hnidRows, hidIndex, the TCOLDESCs by tag
    QByteArray tcinfo = QByteArray(1, char(typeTC)) + char(layout.size());
    for (const quint16 g : rgib) {
        tcinfo += le16(g);
    }
    tcinfo += le32(rowIndex) + le32(hnidRows) + le32(0);
    std::sort(layout.begin(), layout.end(), [](const TableColumn &a, const TableColumn &b) {
        return a.tag < b.tag;
    });
    for (const TableColumn &col : std::as_const(layout)) {
        tcinfo += le32(col.tag) + le16(col.offset) + char(col.size) + char(col.bit);
    }
    heap.replace(info, tcinfo);
    heap.setUserRoot(info);
    return heap.blocks();
}

}
