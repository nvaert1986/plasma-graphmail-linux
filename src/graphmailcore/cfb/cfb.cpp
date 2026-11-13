// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cfb.h"

#include <QSet>
#include <QtEndian>

#include <algorithm>

namespace GraphMail::Cfb
{

namespace
{
constexpr quint32 maxRegSect = 0xFFFFFFFA;
constexpr quint32 difSect = 0xFFFFFFFC;
constexpr quint32 fatSect = 0xFFFFFFFD;
constexpr quint32 endOfChain = 0xFFFFFFFE;
constexpr quint32 freeSect = 0xFFFFFFFF;
constexpr quint32 noStream = 0xFFFFFFFF;
constexpr int miniSectorSize = 64;
constexpr quint32 miniStreamCutoff = 0x1000;
constexpr int dirEntrySize = 128;
constexpr quint8 typeStorage = 1;
constexpr quint8 typeStream = 2;
constexpr quint8 typeRoot = 5;
constexpr int headerDifat = 109;
// Storages nested deeper than this are refused (a damaged file can loop)
constexpr int maxDepth = 64;
constexpr char signature[] = "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1";

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
void put16(QByteArray &b, qsizetype at, quint16 v)
{
    qToLittleEndian(v, b.data() + at);
}
void put32(QByteArray &b, qsizetype at, quint32 v)
{
    qToLittleEndian(v, b.data() + at);
}
void put64(QByteArray &b, qsizetype at, quint64 v)
{
    qToLittleEndian(v, b.data() + at);
}

bool fail(QString *error, const QString &text)
{
    if (error) {
        *error = text;
    }
    return false;
}

class Reader
{
public:
    Reader(const QByteArray &file, QString *error)
        : m_file(file)
        , m_error(error)
    {
    }

    std::optional<Entry> run()
    {
        const char *h = m_file.constData();
        if (m_file.size() < 512 || !m_file.startsWith(QByteArrayView(signature, 8))) {
            fail(m_error, QStringLiteral("not a compound file"));
            return std::nullopt;
        }
        const quint16 major = u16(h + 26);
        const quint16 shift = u16(h + 30);
        if (u16(h + 28) != 0xFFFE || !((major == 3 && shift == 9) || (major == 4 && shift == 12)) || u16(h + 32) != 6) {
            fail(m_error, QStringLiteral("unsupported compound file version"));
            return std::nullopt;
        }
        m_sectorSize = 1 << shift;
        m_major = major;
        const quint32 fatSectors = u32(h + 44);
        const quint32 firstDir = u32(h + 48);
        m_cutoff = u32(h + 56);
        const quint32 firstMiniFat = u32(h + 60);
        quint32 difat = u32(h + 68);
        const quint32 difatSectors = u32(h + 72);

        // The FAT sectors: 109 in the header, the rest in the DIFAT chain
        QList<quint32> fatList;
        for (int i = 0; i < headerDifat && quint32(fatList.size()) < fatSectors; ++i) {
            fatList.append(u32(h + 76 + 4 * i));
        }
        QSet<quint32> seenDifat;
        for (quint32 n = 0; n < difatSectors && quint32(fatList.size()) < fatSectors; ++n) {
            if (difat > maxRegSect || seenDifat.contains(difat)) {
                fail(m_error, QStringLiteral("damaged DIFAT"));
                return std::nullopt;
            }
            seenDifat.insert(difat);
            const QByteArray s = sector(difat);
            if (s.isEmpty()) {
                return std::nullopt;
            }
            const int per = m_sectorSize / 4 - 1;
            for (int i = 0; i < per && quint32(fatList.size()) < fatSectors; ++i) {
                fatList.append(u32(s.constData() + 4 * i));
            }
            difat = u32(s.constData() + 4 * per);
        }
        for (const quint32 f : std::as_const(fatList)) {
            const QByteArray s = sector(f);
            if (s.isEmpty()) {
                return std::nullopt;
            }
            for (int i = 0; i < m_sectorSize / 4; ++i) {
                m_fat.append(u32(s.constData() + 4 * i));
            }
        }

        const std::optional<QByteArray> dir = chain(firstDir, m_fat, false, -1);
        if (!dir) {
            return std::nullopt;
        }
        m_dir = *dir;
        if (m_dir.size() < dirEntrySize || quint8(m_dir.at(66)) != typeRoot) {
            fail(m_error, QStringLiteral("no root entry"));
            return std::nullopt;
        }
        if (const std::optional<QByteArray> miniFat = firstMiniFat <= maxRegSect ? chain(firstMiniFat, m_fat, false, -1) : QByteArray()) {
            for (qsizetype at = 0; at + 4 <= miniFat->size(); at += 4) {
                m_miniFat.append(u32(miniFat->constData() + at));
            }
        } else {
            return std::nullopt;
        }
        const quint32 rootStart = u32(m_dir.constData() + 116);
        const qint64 rootSize = qint64(m_major == 3 ? u32(m_dir.constData() + 120) : u64(m_dir.constData() + 120));
        if (rootStart <= maxRegSect) {
            const std::optional<QByteArray> mini = chain(rootStart, m_fat, false, rootSize);
            if (!mini) {
                return std::nullopt;
            }
            m_miniStream = *mini;
        }
        Entry root;
        root.name = QStringLiteral("Root Entry");
        root.storage = true;
        QSet<quint32> visited{0};
        if (!children(u32(m_dir.constData() + 76), root, visited, 0)) {
            return std::nullopt;
        }
        return root;
    }

private:
    QByteArray sector(quint32 n)
    {
        const qint64 at = (qint64(n) + 1) * m_sectorSize;
        if (n > maxRegSect || at + m_sectorSize > m_file.size()) {
            // The last sector may be cut short by writers that do not pad the file
            if (n <= maxRegSect && at < m_file.size()) {
                QByteArray s = m_file.mid(at, m_sectorSize);
                s.resize(m_sectorSize, 0);
                return s;
            }
            fail(m_error, QStringLiteral("sector %1 outside the file").arg(n));
            return {};
        }
        return m_file.mid(at, m_sectorSize);
    }

    // A chain's bytes, of the FAT (regular sectors) or the mini FAT (64-byte sectors of the mini
    // stream); size -1: the whole chain
    std::optional<QByteArray> chain(quint32 start, const QList<quint32> &table, bool mini, qint64 size)
    {
        QByteArray out;
        const int unit = mini ? miniSectorSize : m_sectorSize;
        QSet<quint32> seen;
        for (quint32 n = start; n != endOfChain;) {
            if (n > maxRegSect || n >= quint32(table.size()) || seen.contains(n)) {
                fail(m_error, QStringLiteral("damaged sector chain"));
                return std::nullopt;
            }
            seen.insert(n);
            if (mini) {
                if (qint64(n + 1) * unit > m_miniStream.size()) {
                    fail(m_error, QStringLiteral("mini sector outside the mini stream"));
                    return std::nullopt;
                }
                out += m_miniStream.mid(qint64(n) * unit, unit);
            } else {
                const QByteArray s = sector(n);
                if (s.isEmpty()) {
                    return std::nullopt;
                }
                out += s;
            }
            if (size >= 0 && out.size() >= size) {
                break;
            }
            n = table.at(n);
        }
        if (size >= 0) {
            if (out.size() < size) {
                fail(m_error, QStringLiteral("stream shorter than its size"));
                return std::nullopt;
            }
            out.truncate(size);
        }
        return out;
    }

    // The siblings of a storage (a binary tree from its Child ID), in tree order
    bool children(quint32 id, Entry &parent, QSet<quint32> &visited, int depth)
    {
        if (id == noStream) {
            return true;
        }
        if (depth > maxDepth || qint64(id + 1) * dirEntrySize > m_dir.size() || visited.contains(id)) {
            return fail(m_error, QStringLiteral("damaged directory"));
        }
        visited.insert(id);
        const char *e = m_dir.constData() + qint64(id) * dirEntrySize;
        if (!children(u32(e + 68), parent, visited, depth)) {
            return false;
        }
        const int nameBytes = std::min<int>(u16(e + 64), 64);
        Entry entry;
        entry.name = QString::fromUtf16(reinterpret_cast<const char16_t *>(e), std::max(0, nameBytes / 2 - 1));
        const quint8 type = quint8(e[66]);
        if (type == typeStorage) {
            entry.storage = true;
            if (!children(u32(e + 76), entry, visited, depth + 1)) {
                return false;
            }
        } else if (type == typeStream) {
            const quint32 start = u32(e + 116);
            const qint64 size = qint64(m_major == 3 ? u32(e + 120) : u64(e + 120));
            if (size > 0) {
                const std::optional<QByteArray> data = quint32(size) < m_cutoff ? chain(start, m_miniFat, true, size) : chain(start, m_fat, false, size);
                if (!data) {
                    return false;
                }
                entry.data = *data;
            }
        } else {
            return fail(m_error, QStringLiteral("directory entry of type %1 in the tree").arg(type));
        }
        parent.children.append(entry);
        return children(u32(e + 72), parent, visited, depth);
    }

    const QByteArray &m_file;
    QString *m_error;
    int m_sectorSize = 512;
    int m_major = 3;
    quint32 m_cutoff = miniStreamCutoff;
    QList<quint32> m_fat;
    QList<quint32> m_miniFat;
    QByteArray m_dir;
    QByteArray m_miniStream;
};

struct DirEntry {
    QString name;
    quint8 type = 0;
    quint32 left = noStream;
    quint32 right = noStream;
    quint32 child = noStream;
    quint32 start = 0;
    quint64 size = 0;
    const QByteArray *data = nullptr;
};

class Writer
{
public:
    QByteArray run(const Entry &root)
    {
        m_dir.append({QStringLiteral("Root Entry"), typeRoot});
        add(root, 0);

        // Streams: small ones in the mini stream, the rest in sectors of their own
        QByteArray mini;
        QList<quint32> miniFat;
        QList<std::pair<int, const QByteArray *>> big;
        for (int i = 0; i < m_dir.size(); ++i) {
            DirEntry &d = m_dir[i];
            if (d.type != typeStream) {
                continue;
            }
            d.size = quint64(d.data->size());
            if (d.data->isEmpty()) {
                d.start = endOfChain;
            } else if (quint32(d.data->size()) < miniStreamCutoff) {
                const quint32 first = quint32(miniFat.size());
                const int count = int((d.data->size() + miniSectorSize - 1) / miniSectorSize);
                for (int s = 0; s < count; ++s) {
                    miniFat.append(s + 1 < count ? first + quint32(s) + 1 : endOfChain);
                }
                mini += *d.data;
                mini.resize(qsizetype(miniFat.size()) * miniSectorSize, 0);
                d.start = first;
            } else {
                big.append({i, d.data});
            }
        }

        QByteArray body;
        QList<quint32> fat;
        const auto place = [&](const QByteArray &bytes) -> quint32 {
            if (bytes.isEmpty()) {
                return endOfChain;
            }
            const quint32 first = quint32(fat.size());
            const int count = int((bytes.size() + 511) / 512);
            for (int s = 0; s < count; ++s) {
                fat.append(s + 1 < count ? first + quint32(s) + 1 : endOfChain);
            }
            body += bytes;
            body.resize(qsizetype(fat.size()) * 512, 0);
            return first;
        };
        for (const auto &[index, data] : std::as_const(big)) {
            m_dir[index].start = place(*data);
        }
        m_dir[0].start = place(mini);
        m_dir[0].size = quint64(mini.size());
        QByteArray miniFatBytes;
        for (const quint32 v : std::as_const(miniFat)) {
            miniFatBytes += le32(v);
        }
        while (miniFatBytes.size() % 512) {
            miniFatBytes += le32(freeSect);
        }
        const quint32 firstMiniFat = place(miniFatBytes);
        const quint32 miniFatSectors = quint32(miniFatBytes.size() / 512);

        // The directory, four entries a sector, free entries to fill the last
        QByteArray dir;
        for (const DirEntry &d : std::as_const(m_dir)) {
            dir += entryBytes(d);
        }
        while (dir.size() % 512) {
            dir += entryBytes(DirEntry());
        }
        const quint32 firstDir = place(dir);

        // The FAT covers everything including itself and the DIFAT
        const quint32 dataSectors = quint32(fat.size());
        quint32 fatSectors = 0;
        quint32 difatSectors = 0;
        for (;;) {
            const quint32 total = dataSectors + fatSectors + difatSectors;
            const quint32 needFat = (total + 127) / 128;
            const quint32 needDifat = needFat > headerDifat ? (needFat - headerDifat + 126) / 127 : 0;
            if (needFat == fatSectors && needDifat == difatSectors) {
                break;
            }
            fatSectors = needFat;
            difatSectors = needDifat;
        }
        QList<quint32> fatLocations;
        for (quint32 i = 0; i < fatSectors; ++i) {
            fatLocations.append(dataSectors + i);
            fat.append(fatSect);
        }
        QList<quint32> difatLocations;
        for (quint32 i = 0; i < difatSectors; ++i) {
            difatLocations.append(dataSectors + fatSectors + i);
            fat.append(difSect);
        }
        while (quint32(fat.size()) < fatSectors * 128) {
            fat.append(freeSect);
        }
        for (const quint32 v : std::as_const(fat)) {
            body += le32(v);
        }
        for (quint32 i = 0; i < difatSectors; ++i) {
            QByteArray s;
            for (quint32 k = 0; k < 127; ++k) {
                const quint32 at = headerDifat + i * 127 + k;
                s += le32(at < fatSectors ? fatLocations.at(int(at)) : freeSect);
            }
            s += le32(i + 1 < difatSectors ? difatLocations.at(int(i + 1)) : endOfChain);
            body += s;
        }

        QByteArray header(512, 0);
        header.replace(0, 8, QByteArray(signature, 8));
        put16(header, 24, 0x003E);
        put16(header, 26, 3);
        put16(header, 28, 0xFFFE);
        put16(header, 30, 9);
        put16(header, 32, 6);
        put32(header, 44, fatSectors);
        put32(header, 48, firstDir);
        put32(header, 56, miniStreamCutoff);
        put32(header, 60, miniFatSectors ? firstMiniFat : endOfChain);
        put32(header, 64, miniFatSectors);
        put32(header, 68, difatSectors ? difatLocations.first() : endOfChain);
        put32(header, 72, difatSectors);
        for (int i = 0; i < headerDifat; ++i) {
            put32(header, 76 + 4 * i, quint32(i) < fatSectors ? fatLocations.at(i) : freeSect);
        }
        return header + body;
    }

private:
    static QByteArray le32(quint32 v)
    {
        QByteArray b(4, 0);
        put32(b, 0, v);
        return b;
    }

    // Children get ids next to each other; their tree hangs from the parent's Child ID
    void add(const Entry &storage, int parent)
    {
        QList<const Entry *> sorted;
        for (const Entry &c : storage.children) {
            sorted.append(&c);
        }
        std::sort(sorted.begin(), sorted.end(), [](const Entry *a, const Entry *b) {
            return lessThan(a->name, b->name);
        });
        const int first = int(m_dir.size());
        for (const Entry *c : std::as_const(sorted)) {
            DirEntry d;
            d.name = c->name.left(31);
            d.type = c->storage ? typeStorage : typeStream;
            d.data = &c->data;
            m_dir.append(d);
        }
        m_dir[parent].child = tree(first, first + int(sorted.size()) - 1);
        for (int i = 0; i < sorted.size(); ++i) {
            if (sorted.at(i)->storage) {
                add(*sorted.at(i), first + i);
            }
        }
    }

    // A balanced binary search tree over ids lo..hi (sorted), as the id of its top
    quint32 tree(int lo, int hi)
    {
        if (lo > hi) {
            return noStream;
        }
        const int mid = (lo + hi) / 2;
        m_dir[mid].left = tree(lo, mid - 1);
        m_dir[mid].right = tree(mid + 1, hi);
        return quint32(mid);
    }

    static QByteArray entryBytes(const DirEntry &d)
    {
        QByteArray e(dirEntrySize, 0);
        if (d.type != 0) {
            const QString name = d.name;
            for (int i = 0; i < name.size(); ++i) {
                put16(e, 2 * i, name.at(i).unicode());
            }
            put16(e, 64, quint16((name.size() + 1) * 2));
            e[66] = char(d.type);
            // All black: allowed by 2.6.4, and every sibling tree here is balanced anyway
            e[67] = 1;
        }
        put32(e, 68, d.left);
        put32(e, 72, d.right);
        put32(e, 76, d.child);
        if (d.type == typeStream || d.type == typeRoot) {
            put32(e, 116, d.start);
            put64(e, 120, d.size);
        }
        return e;
    }

    QList<DirEntry> m_dir;
};
}

const Entry *Entry::child(const QString &name) const
{
    for (const Entry &c : children) {
        if (c.name.compare(name, Qt::CaseInsensitive) == 0) {
            return &c;
        }
    }
    return nullptr;
}

Entry &Entry::addStorage(const QString &name)
{
    Entry e;
    e.name = name;
    e.storage = true;
    children.append(e);
    return children.last();
}

void Entry::addStream(const QString &name, const QByteArray &bytes)
{
    Entry e;
    e.name = name;
    e.data = bytes;
    children.append(e);
}

bool lessThan(const QString &a, const QString &b)
{
    if (a.size() != b.size()) {
        return a.size() < b.size();
    }
    for (qsizetype i = 0; i < a.size(); ++i) {
        // Simple uppercase per UTF-16 unit; surrogates stay as they are
        const char16_t x = a.at(i).isSurrogate() ? a.at(i).unicode() : a.at(i).toUpper().unicode();
        const char16_t y = b.at(i).isSurrogate() ? b.at(i).unicode() : b.at(i).toUpper().unicode();
        if (x != y) {
            return x < y;
        }
    }
    return false;
}

std::optional<Entry> read(const QByteArray &file, QString *error)
{
    return Reader(file, error).run();
}

QByteArray write(const Entry &root)
{
    return Writer().run(root);
}

}
