// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pstltp.h"

#include <QStringDecoder>
#include <QTimeZone>
#include <QtEndian>

#include <algorithm>

namespace GraphMail::Pst
{

namespace
{
constexpr quint8 heapSignature = 0xEC;
constexpr quint8 typeTC = 0x7C;
constexpr quint8 typeBTH = 0xB5;
constexpr quint8 typePC = 0xBC;
constexpr int maxBlockSize = 8192;
// Index levels a BTH can sensibly have; guards against loops in damaged files
constexpr int maxBthLevels = 8;

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

bool isHid(quint32 hnid)
{
    return (hnid & 0x1F) == 0;
}
}

int fixedSize(quint16 type)
{
    switch (type) {
    case PtypBoolean:
        return 1;
    case PtypInteger16:
        return 2;
    case PtypInteger32:
    case PtypFloating32:
    case PtypErrorCode:
        return 4;
    case PtypFloating64:
    case PtypCurrency:
    case PtypFloatingTime:
    case PtypInteger64:
    case PtypTime:
        return 8;
    case PtypGuid:
        return 16;
    default:
        return 0;
    }
}

qint64 PropValue::toInt() const
{
    switch (data.size()) {
    case 1:
        return quint8(data.at(0));
    case 2:
        return qFromLittleEndian<qint16>(data.constData());
    case 4:
        return qFromLittleEndian<qint32>(data.constData());
    case 8:
        return qFromLittleEndian<qint64>(data.constData());
    default:
        return 0;
    }
}

bool PropValue::toBool() const
{
    return !data.isEmpty() && data.at(0) != 0;
}

QString PropValue::toString(int codepage) const
{
    QString s;
    if (type == PtypString8) {
        s = decodeCodepage(data, codepage);
    } else {
        s = QString::fromUtf16(reinterpret_cast<const char16_t *>(data.constData()), data.size() / 2);
    }
    while (s.endsWith(QChar(0))) {
        s.chop(1);
    }
    return s;
}

QString decodeCodepage(const QByteArray &bytes, int codepage)
{
    // Windows code page numbers to the names the decoder knows
    static const QHash<int, QByteArray> names{
        {65001, "UTF-8"},     {20127, "US-ASCII"},  {932, "Shift_JIS"},    {936, "GBK"},         {949, "EUC-KR"},
        {950, "Big5"},        {54936, "GB18030"},   {50220, "ISO-2022-JP"}, {50221, "ISO-2022-JP"}, {50222, "ISO-2022-JP"},
        {51932, "EUC-JP"},    {20866, "KOI8-R"},    {21866, "KOI8-U"},     {1200, "UTF-16LE"},   {1201, "UTF-16BE"},
        {28603, "ISO-8859-13"}, {28605, "ISO-8859-15"},
    };
    QByteArray name = names.value(codepage);
    if (name.isEmpty()) {
        if (codepage >= 28591 && codepage <= 28599) {
            name = "ISO-8859-" + QByteArray::number(codepage - 28590);
        } else {
            name = "windows-" + QByteArray::number(codepage > 0 ? codepage : 1252);
        }
    }
    QStringDecoder decoder(name.constData());
    return decoder.isValid() ? decoder.decode(bytes) : QString::fromLatin1(bytes);
}

QDateTime PropValue::toTime() const
{
    if (data.size() != 8) {
        return {};
    }
    // FILETIME: 100 ns intervals since 1601-01-01; 11644473600 s lie between 1601 and 1970
    const qint64 ft = qFromLittleEndian<qint64>(data.constData());
    if (ft <= 0) {
        return {};
    }
    return QDateTime::fromMSecsSinceEpoch(ft / 10000 - qint64(11644473600) * 1000, QTimeZone::UTC);
}

QList<QByteArray> PropValue::items() const
{
    QList<QByteArray> out;
    if (!(type & PtypMultiple)) {
        out.append(data);
        return out;
    }
    const int base = fixedSize(type & ~PtypMultiple);
    if (base > 0) {
        for (qsizetype at = 0; at + base <= data.size(); at += base) {
            out.append(data.mid(at, base));
        }
        return out;
    }
    // ulCount, rgulDataOffsets[ulCount], the items; the last runs to the end of the value
    if (data.size() < 4) {
        return out;
    }
    const quint32 count = u32(data.constData());
    if (4 + qint64(count) * 4 > data.size()) {
        return out;
    }
    for (quint32 i = 0; i < count; ++i) {
        const quint32 start = u32(data.constData() + 4 + i * 4);
        const quint32 end = i + 1 < count ? u32(data.constData() + 8 + i * 4) : quint32(data.size());
        if (start > end || end > quint32(data.size())) {
            return {};
        }
        out.append(data.mid(start, end - start));
    }
    return out;
}

std::optional<Heap> Heap::fromBlocks(const QList<QByteArray> &blocks, QString *error)
{
    if (blocks.isEmpty() || blocks.first().size() < 12) {
        setError(error, QStringLiteral("empty heap"));
        return std::nullopt;
    }
    const QByteArray &first = blocks.first();
    if (quint8(first.at(2)) != heapSignature) {
        setError(error, QStringLiteral("no heap signature"));
        return std::nullopt;
    }
    Heap heap;
    heap.m_blocks = blocks;
    heap.m_clientSig = quint8(first.at(3));
    heap.m_userRoot = u32(first.constData() + 4);
    for (const QByteArray &b : blocks) {
        // Every block starts with ibHnpm: HNHDR, HNPAGEHDR and HNBITMAPHDR alike
        if (b.size() < 2) {
            setError(error, QStringLiteral("heap block without header"));
            return std::nullopt;
        }
        const int map = u16(b.constData());
        if (map + 4 > b.size()) {
            setError(error, QStringLiteral("heap page map outside its block"));
            return std::nullopt;
        }
        const int count = u16(b.constData() + map);
        if (map + 4 + (count + 1) * 2 > b.size()) {
            setError(error, QStringLiteral("damaged heap page map"));
            return std::nullopt;
        }
        QList<quint16> offsets;
        offsets.reserve(count + 1);
        for (int i = 0; i <= count; ++i) {
            const quint16 at = u16(b.constData() + map + 4 + i * 2);
            if (at > map || (!offsets.isEmpty() && at < offsets.last())) {
                setError(error, QStringLiteral("heap allocations out of order"));
                return std::nullopt;
            }
            offsets.append(at);
        }
        heap.m_offsets.append(offsets);
    }
    return heap;
}

std::optional<QByteArray> Heap::item(quint32 hid) const
{
    if (!isHid(hid)) {
        return std::nullopt;
    }
    const int index = int((hid >> 5) & 0x7FF);
    const int block = int(hid >> 16);
    if (index == 0 || block >= m_offsets.size() || index >= m_offsets.at(block).size()) {
        return std::nullopt;
    }
    const QList<quint16> &offsets = m_offsets.at(block);
    return m_blocks.at(block).mid(offsets.at(index - 1), offsets.at(index) - offsets.at(index - 1));
}

namespace
{
bool collectBth(const Heap &heap, quint32 hid, int level, int cbKey, int cbEnt, QList<BthRecord> &out, QString *error)
{
    const std::optional<QByteArray> records = heap.item(hid);
    if (!records) {
        return setError(error, QStringLiteral("missing BTH records %1").arg(hid, 0, 16));
    }
    const int size = level > 0 ? cbKey + 4 : cbKey + cbEnt;
    if (records->size() % size != 0) {
        return setError(error, QStringLiteral("BTH records of a wrong size"));
    }
    for (int at = 0; at < records->size(); at += size) {
        if (level > 0) {
            if (!collectBth(heap, u32(records->constData() + at + cbKey), level - 1, cbKey, cbEnt, out, error)) {
                return false;
            }
        } else {
            out.append({records->mid(at, cbKey), records->mid(at + cbKey, cbEnt)});
        }
    }
    return true;
}
}

std::optional<QList<BthRecord>> readBth(const Heap &heap, quint32 hidHeader, QString *error)
{
    const std::optional<QByteArray> h = heap.item(hidHeader);
    if (!h || h->size() < 8 || quint8(h->at(0)) != typeBTH) {
        setError(error, QStringLiteral("no BTH header"));
        return std::nullopt;
    }
    const int cbKey = quint8(h->at(1));
    const int cbEnt = quint8(h->at(2));
    const int levels = quint8(h->at(3));
    const quint32 root = u32(h->constData() + 4);
    QList<BthRecord> out;
    // An empty BTH has no root
    if (root == 0) {
        return out;
    }
    if (cbKey == 0 || cbKey > 16 || cbEnt == 0 || cbEnt > 32 || levels > maxBthLevels) {
        setError(error, QStringLiteral("damaged BTH header"));
        return std::nullopt;
    }
    if (!collectBth(heap, root, levels, cbKey, cbEnt, out, error)) {
        return std::nullopt;
    }
    return out;
}

std::optional<QByteArray> NodeContext::resolve(const Heap &heap, quint32 hnid) const
{
    if (hnid == 0) {
        return QByteArray();
    }
    if (isHid(hnid)) {
        return heap.item(hnid);
    }
    const auto sub = m_subnodes.constFind(hnid);
    if (sub == m_subnodes.constEnd() || !m_ndb) {
        return std::nullopt;
    }
    return m_ndb->treeData(sub->data);
}

std::optional<PropertyMap> readPropertyContext(const Heap &heap, const NodeContext &context, QString *error)
{
    if (heap.clientSignature() != typePC) {
        setError(error, QStringLiteral("no property context"));
        return std::nullopt;
    }
    const std::optional<QList<BthRecord>> records = readBth(heap, heap.userRoot(), error);
    if (!records) {
        return std::nullopt;
    }
    PropertyMap out;
    for (const BthRecord &r : *records) {
        if (r.key.size() != 2 || r.data.size() != 6) {
            setError(error, QStringLiteral("property context with wrong record sizes"));
            return std::nullopt;
        }
        PropValue v;
        v.type = u16(r.data.constData());
        const quint32 value = u32(r.data.constData() + 2);
        const int size = fixedSize(v.type);
        if (size > 0 && size <= 4) {
            v.data = r.data.mid(2, size);
        } else {
            // Larger fixed values in the heap; variable and multi-valued ones in the heap or a subnode
            const std::optional<QByteArray> d = size > 4 ? heap.item(value) : context.resolve(heap, value);
            if (!d) {
                setError(error, QStringLiteral("property %1 points nowhere").arg(u16(r.key.constData()), 4, 16, QLatin1Char('0')));
                return std::nullopt;
            }
            v.data = *d;
        }
        out.insert(u16(r.key.constData()), v);
    }
    return out;
}

std::optional<Table> readTableContext(const Heap &heap, const NodeContext &context, bool unicode, QString *error)
{
    if (heap.clientSignature() != typeTC) {
        setError(error, QStringLiteral("no table context"));
        return std::nullopt;
    }
    const std::optional<QByteArray> info = heap.item(heap.userRoot());
    if (!info || info->size() < 22 || quint8(info->at(0)) != typeTC) {
        setError(error, QStringLiteral("no TCINFO"));
        return std::nullopt;
    }
    const int columnCount = quint8(info->at(1));
    if (22 + columnCount * 8 > info->size()) {
        setError(error, QStringLiteral("damaged TCINFO"));
        return std::nullopt;
    }
    const int cebStart = u16(info->constData() + 6);
    const int rowSize = u16(info->constData() + 8);
    const quint32 hidRowIndex = u32(info->constData() + 10);
    const quint32 hnidRows = u32(info->constData() + 14);

    Table table;
    for (int i = 0; i < columnCount; ++i) {
        const char *c = info->constData() + 22 + i * 8;
        const TableColumn col{u32(c), u16(c + 4), quint8(c[6]), quint8(c[7])};
        if (col.offset + col.size > rowSize || cebStart + col.bit / 8 >= rowSize) {
            setError(error, QStringLiteral("column %1 outside the row").arg(col.tag, 8, 16, QLatin1Char('0')));
            return std::nullopt;
        }
        table.columns.append(col);
    }

    const std::optional<QList<BthRecord>> index = readBth(heap, hidRowIndex, error);
    if (!index) {
        return std::nullopt;
    }
    if (index->isEmpty() || hnidRows == 0) {
        return table;
    }
    if (rowSize <= 0) {
        setError(error, QStringLiteral("table rows of no size"));
        return std::nullopt;
    }

    // The row matrix: one heap item, or a subnode whose blocks each hold whole rows
    QList<QByteArray> blocks;
    if (isHid(hnidRows)) {
        const std::optional<QByteArray> one = heap.item(hnidRows);
        if (!one) {
            setError(error, QStringLiteral("missing row matrix"));
            return std::nullopt;
        }
        blocks.append(*one);
    } else {
        const auto sub = context.subnodes().constFind(hnidRows);
        if (sub == context.subnodes().constEnd() || !context.ndb()) {
            setError(error, QStringLiteral("missing row matrix subnode"));
            return std::nullopt;
        }
        const std::optional<QList<QByteArray>> b = context.ndb()->treeBlocks(sub->data);
        if (!b) {
            setError(error, context.ndb()->error());
            return std::nullopt;
        }
        blocks = *b;
    }
    const int rowsPerBlock = (maxBlockSize - (unicode ? 16 : 12)) / rowSize;
    if (rowsPerBlock <= 0) {
        setError(error, QStringLiteral("table rows larger than a block"));
        return std::nullopt;
    }

    QList<quint32> rowIndexes;
    for (const BthRecord &r : *index) {
        if (r.data.size() == 4) {
            rowIndexes.append(u32(r.data.constData()));
        } else if (r.data.size() == 2) {
            rowIndexes.append(u16(r.data.constData()));
        } else {
            setError(error, QStringLiteral("row index with wrong record sizes"));
            return std::nullopt;
        }
    }
    std::sort(rowIndexes.begin(), rowIndexes.end());

    for (const quint32 n : std::as_const(rowIndexes)) {
        const int block = int(n / quint32(rowsPerBlock));
        const qsizetype at = qsizetype(n % quint32(rowsPerBlock)) * rowSize;
        if (block >= blocks.size() || at + rowSize > blocks.at(block).size()) {
            setError(error, QStringLiteral("row %1 outside the row matrix").arg(n));
            return std::nullopt;
        }
        const char *row = blocks.at(block).constData() + at;
        QHash<quint32, PropValue> cells;
        for (const TableColumn &col : std::as_const(table.columns)) {
            if (!(quint8(row[cebStart + col.bit / 8]) & (1 << (7 - col.bit % 8)))) {
                continue;
            }
            PropValue v;
            v.type = quint16(col.tag & 0xFFFF);
            const int size = (v.type & PtypMultiple) ? 0 : fixedSize(v.type);
            if (size > 0 && size <= 8) {
                if (size > col.size) {
                    setError(error, QStringLiteral("column %1 too narrow").arg(col.tag, 8, 16, QLatin1Char('0')));
                    return std::nullopt;
                }
                v.data = QByteArray(row + col.offset, size);
            } else {
                if (col.size < 4) {
                    setError(error, QStringLiteral("column %1 too narrow").arg(col.tag, 8, 16, QLatin1Char('0')));
                    return std::nullopt;
                }
                const std::optional<QByteArray> d = context.resolve(heap, u32(row + col.offset));
                if (!d) {
                    setError(error, QStringLiteral("cell %1 points nowhere").arg(col.tag, 8, 16, QLatin1Char('0')));
                    return std::nullopt;
                }
                v.data = *d;
            }
            cells.insert(col.tag, v);
        }
        table.rows.append(cells);
    }
    return table;
}

std::optional<std::pair<Heap, NodeContext>> openNode(Ndb &ndb, quint64 bidData, quint64 bidSub, QString *error)
{
    const std::optional<QList<QByteArray>> blocks = ndb.treeBlocks(bidData);
    const std::optional<QHash<quint32, SubnodeEntry>> subs = blocks ? ndb.subnodes(bidSub) : std::nullopt;
    if (!subs) {
        setError(error, ndb.error());
        return std::nullopt;
    }
    std::optional<Heap> heap = Heap::fromBlocks(*blocks, error);
    if (!heap) {
        return std::nullopt;
    }
    return std::make_pair(*heap, NodeContext(&ndb, *subs));
}

std::optional<PropertyMap> readNodeProperties(Ndb &ndb, quint64 bidData, quint64 bidSub, QString *error)
{
    const auto node = openNode(ndb, bidData, bidSub, error);
    if (!node) {
        return std::nullopt;
    }
    return readPropertyContext(node->first, node->second, error);
}

std::optional<Table> readNodeTable(Ndb &ndb, quint64 bidData, quint64 bidSub, QString *error)
{
    const auto node = openNode(ndb, bidData, bidSub, error);
    if (!node) {
        return std::nullopt;
    }
    return readTableContext(node->first, node->second, ndb.header().unicode, error);
}

}
