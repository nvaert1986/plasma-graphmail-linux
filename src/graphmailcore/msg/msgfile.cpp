// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "msgfile.h"
#include "cfb/cfb.h"
#include "pst/pstalgorithms.h"

#include <QMap>
#include <QtEndian>

#include <algorithm>
#include <functional>

namespace GraphMail::Msg
{

namespace
{
// [MS-OXMSG] 2.2 and 2.4
constexpr QLatin1StringView propertiesStream("__properties_version1.0");
constexpr QLatin1StringView nameidStorage("__nameid_version1.0");
constexpr QLatin1StringView embeddedStorage("__substg1.0_3701000D");
constexpr int topHeader = 32;
constexpr int embeddedHeader = 24;
constexpr int childHeader = 8;
// PROPATTR_READABLE | PROPATTR_WRITABLE
constexpr quint32 propFlags = 0x00000006;
constexpr quint16 propAttachDataObject = 0x3701;
constexpr quint16 propAttachNumber = 0x0E21;
constexpr quint16 propRowid = 0x3000;
constexpr quint16 propStoreSupportMask = 0x340D;
constexpr quint32 storeUnicodeOk = 0x00040000;
constexpr quint32 tagLtpRowId = 0x67F20003;
constexpr quint32 tagLtpRowVer = 0x67F30003;
constexpr int maxDepth = 8;
// Recipients and attachments a .msg can hold (2.2.1, 2.2.2)
constexpr int maxChildren = 2048;

quint32 u32(const char *at)
{
    return qFromLittleEndian<quint32>(at);
}
QByteArray le32(quint32 v)
{
    QByteArray b(4, 0);
    qToLittleEndian(v, b.data());
    return b;
}

QString hex8(quint32 v)
{
    return QString::number(v, 16).rightJustified(8, QLatin1Char('0')).toUpper();
}

QString streamName(quint32 tag)
{
    return QStringLiteral("__substg1.0_") + hex8(tag);
}

// The value types stored inside the property stream (2.1.2): fixed size, PtypGuid excluded
bool inlineType(quint16 type)
{
    return !(type & Pst::PtypMultiple) && type != Pst::PtypGuid && Pst::fixedSize(type) > 0;
}

QByteArray stripNull(QByteArray data, int unit)
{
    while (data.size() >= unit && data.endsWith(QByteArray(unit, 0)) && data.size() % unit == 0) {
        data.chop(unit);
    }
    return data;
}

// Reading ---------------------------------------------------------------------------------------

// The properties of one storage, by tag
std::optional<QList<std::pair<quint32, Pst::PropValue>>> readProperties(const Cfb::Entry &storage, int headerSize, QString *error)
{
    const Cfb::Entry *stream = storage.child(propertiesStream);
    if (!stream || stream->storage || stream->data.size() < headerSize || (stream->data.size() - headerSize) % 16) {
        if (error) {
            *error = QStringLiteral("no property stream in %1").arg(storage.name);
        }
        return std::nullopt;
    }
    QList<std::pair<quint32, Pst::PropValue>> out;
    for (qsizetype at = headerSize; at + 16 <= stream->data.size(); at += 16) {
        const char *e = stream->data.constData() + at;
        const quint32 tag = u32(e);
        const quint16 type = quint16(tag & 0xFFFF);
        Pst::PropValue v;
        v.type = type;
        if (inlineType(type)) {
            v.data = QByteArray(e + 8, Pst::fixedSize(type));
        } else if (type == Pst::PtypObject) {
            continue;
        } else if ((type & Pst::PtypMultiple) && Pst::fixedSize(type & ~Pst::PtypMultiple) == 0) {
            // N values: a length stream and one stream per value (2.1.4.2), into the PST layout
            const Cfb::Entry *lengths = storage.child(streamName(tag));
            const quint16 base = type & ~Pst::PtypMultiple;
            const int entry = base == Pst::PtypBinary ? 8 : 4;
            const qsizetype count = lengths ? lengths->data.size() / entry : 0;
            QList<QByteArray> items;
            for (qsizetype i = 0; i < count && i < 0x10000; ++i) {
                const Cfb::Entry *item = storage.child(streamName(tag) + QStringLiteral("-") + hex8(quint32(i)));
                QByteArray d = item ? item->data : QByteArray();
                if (base == Pst::PtypString) {
                    d = stripNull(d, 2);
                } else if (base == Pst::PtypString8) {
                    d = stripNull(d, 1);
                }
                items.append(d);
            }
            QByteArray mv = le32(quint32(items.size()));
            quint32 offset = 4 + 4 * quint32(items.size());
            QByteArray payload;
            for (const QByteArray &item : std::as_const(items)) {
                mv += le32(offset + quint32(payload.size()));
                payload += item;
            }
            v.data = mv + payload;
        } else {
            const Cfb::Entry *value = storage.child(streamName(tag));
            v.data = value ? value->data : QByteArray();
            if (type == Pst::PtypString) {
                v.data = stripNull(v.data, 2);
            } else if (type == Pst::PtypString8) {
                v.data = stripNull(v.data, 1);
            }
        }
        out.append({tag, v});
    }
    return out;
}

std::optional<Pst::Message> readMessage(const Cfb::Entry &storage, int headerSize, int depth, QString *error)
{
    if (depth > maxDepth) {
        if (error) {
            *error = QStringLiteral("attached messages nested too deeply");
        }
        return std::nullopt;
    }
    const auto props = readProperties(storage, headerSize, error);
    if (!props) {
        return std::nullopt;
    }
    Pst::Message m;
    for (const auto &[tag, v] : *props) {
        m.properties.insert(quint16(tag >> 16), v);
    }
    // Recipients and attachments in the order of their storage numbers
    QList<const Cfb::Entry *> recipients;
    QList<const Cfb::Entry *> attachments;
    for (const Cfb::Entry &c : storage.children) {
        if (c.storage && c.name.startsWith(QLatin1String("__recip_version1.0_#"))) {
            recipients.append(&c);
        } else if (c.storage && c.name.startsWith(QLatin1String("__attach_version1.0_#"))) {
            attachments.append(&c);
        }
    }
    const auto byNumber = [](const Cfb::Entry *a, const Cfb::Entry *b) {
        return a->name.section(QLatin1Char('#'), 1).toUInt(nullptr, 16) < b->name.section(QLatin1Char('#'), 1).toUInt(nullptr, 16);
    };
    std::sort(recipients.begin(), recipients.end(), byNumber);
    std::sort(attachments.begin(), attachments.end(), byNumber);
    recipients = recipients.mid(0, maxChildren);
    attachments = attachments.mid(0, maxChildren);
    for (const Cfb::Entry *r : std::as_const(recipients)) {
        const auto rp = readProperties(*r, childHeader, error);
        if (!rp) {
            return std::nullopt;
        }
        QHash<quint32, Pst::PropValue> row;
        for (const auto &[tag, v] : *rp) {
            row.insert(tag, v);
        }
        m.recipients.append(row);
    }
    for (const Cfb::Entry *a : std::as_const(attachments)) {
        const auto ap = readProperties(*a, childHeader, error);
        if (!ap) {
            return std::nullopt;
        }
        Pst::Attachment attachment;
        for (const auto &[tag, v] : *ap) {
            attachment.properties.insert(quint16(tag >> 16), v);
        }
        if (const Cfb::Entry *inner = a->child(embeddedStorage); inner && inner->storage && inner->child(propertiesStream)) {
            std::optional<Pst::Message> embedded = readMessage(*inner, embeddedHeader, depth + 1, error);
            if (!embedded) {
                return std::nullopt;
            }
            attachment.embedded = std::make_shared<Pst::Message>(std::move(*embedded));
        }
        m.attachments.append(attachment);
    }
    return m;
}

// Writing ---------------------------------------------------------------------------------------

// A property ID as written: named ones renumbered, 0 for a named one without a name (left out)
using Remap = std::function<quint16(quint16)>;

void writeProperties(Cfb::Entry &storage, const QByteArray &header, const QList<std::pair<quint32, Pst::PropValue>> &props, bool embedded)
{
    QByteArray stream = header;
    for (const auto &[tag, v] : props) {
        const quint16 type = quint16(tag & 0xFFFF);
        QByteArray entry = le32(tag) + le32(propFlags);
        if (inlineType(type)) {
            QByteArray value = v.data.left(Pst::fixedSize(type));
            value.resize(8, 0);
            stream += entry + value;
            continue;
        }
        quint32 size = 0;
        if ((type & Pst::PtypMultiple) && Pst::fixedSize(type & ~Pst::PtypMultiple) == 0) {
            const quint16 base = type & ~Pst::PtypMultiple;
            const QList<QByteArray> items = v.items();
            QByteArray lengths;
            for (int i = 0; i < items.size(); ++i) {
                QByteArray item = items.at(i);
                if (base == Pst::PtypBinary) {
                    lengths += le32(quint32(item.size())) + le32(0);
                } else {
                    // Strings end with their null character, counted in the length (2.1.4.2.1.2)
                    item += QByteArray(base == Pst::PtypString ? 2 : 1, 0);
                    lengths += le32(quint32(item.size()));
                }
                storage.addStream(streamName(tag) + QStringLiteral("-") + hex8(quint32(i)), item);
            }
            storage.addStream(streamName(tag), lengths);
            size = quint32(lengths.size());
        } else {
            storage.addStream(streamName(tag), v.data);
            // Strings: the size counts the null character the stream leaves out (2.4.2.2)
            size = quint32(v.data.size()) + (type == Pst::PtypString ? 2 : type == Pst::PtypString8 ? 1 : 0);
        }
        stream += entry + le32(size) + le32(0);
    }
    if (embedded) {
        // The attached message: no stream of its own, a storage beside (2.2.2.1, 2.4.2.2)
        stream += le32(quint32(propAttachDataObject) << 16 | Pst::PtypObject) + le32(propFlags) + le32(0xFFFFFFFF) + le32(1);
    }
    storage.addStream(propertiesStream, stream);
}

// The properties to store, by tag: empty strings are left out (2.4.2.2 MUST have one character)
QList<std::pair<quint32, Pst::PropValue>> storable(const QList<std::pair<quint32, Pst::PropValue>> &in)
{
    QList<std::pair<quint32, Pst::PropValue>> out;
    for (const auto &p : in) {
        const quint16 type = quint16(p.first & 0xFFFF);
        if ((type == Pst::PtypString || type == Pst::PtypString8) && p.second.data.isEmpty()) {
            continue;
        }
        if (type == Pst::PtypObject) {
            continue;
        }
        out.append(p);
    }
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
        return a.first < b.first;
    });
    return out;
}

QList<std::pair<quint32, Pst::PropValue>> tagged(const Pst::PropertyMap &props, const Remap &remap)
{
    QList<std::pair<quint32, Pst::PropValue>> out;
    for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
        if (const quint16 id = remap(it.key()); id != 0) {
            out.append({quint32(id) << 16 | it->type, *it});
        }
    }
    return out;
}

void writeMessage(Cfb::Entry &storage, const Pst::Message &m, bool top, const Remap &remap)
{
    const quint32 recipients = quint32(std::min<qsizetype>(m.recipients.size(), maxChildren));
    const quint32 attachments = quint32(std::min<qsizetype>(m.attachments.size(), maxChildren));
    QByteArray header(8, 0);
    header += le32(recipients) + le32(attachments) + le32(recipients) + le32(attachments);
    if (top) {
        header += QByteArray(8, 0);
    }
    Pst::PropertyMap props = m.properties;
    // Unicode strings (2.1.1.1)
    props.insert(propStoreSupportMask, {Pst::PtypInteger32, le32(storeUnicodeOk)});
    writeProperties(storage, header, storable(tagged(props, remap)), false);

    for (quint32 i = 0; i < recipients; ++i) {
        Cfb::Entry r;
        r.name = QStringLiteral("__recip_version1.0_#") + hex8(i);
        r.storage = true;
        QList<std::pair<quint32, Pst::PropValue>> rp;
        for (auto it = m.recipients.at(int(i)).constBegin(); it != m.recipients.at(int(i)).constEnd(); ++it) {
            // The PST's table bookkeeping is no recipient property
            if (it.key() == tagLtpRowId || it.key() == tagLtpRowVer) {
                continue;
            }
            if (const quint16 id = remap(quint16(it.key() >> 16)); id != 0) {
                rp.append({quint32(id) << 16 | (it.key() & 0xFFFF), *it});
            }
        }
        if (std::none_of(rp.cbegin(), rp.cend(), [](const auto &p) {
                return p.first >> 16 == propRowid;
            })) {
            rp.append({quint32(propRowid) << 16 | Pst::PtypInteger32, {Pst::PtypInteger32, le32(i)}});
        }
        writeProperties(r, QByteArray(8, 0), storable(rp), false);
        storage.children.append(r);
    }
    for (quint32 i = 0; i < attachments; ++i) {
        const Pst::Attachment &a = m.attachments.at(int(i));
        Cfb::Entry s;
        s.name = QStringLiteral("__attach_version1.0_#") + hex8(i);
        Pst::PropertyMap ap = a.properties;
        if (!ap.contains(propAttachNumber)) {
            ap.insert(propAttachNumber, {Pst::PtypInteger32, le32(i)});
        }
        writeProperties(s, QByteArray(8, 0), storable(tagged(ap, remap)), bool(a.embedded));
        if (a.embedded) {
            Cfb::Entry inner;
            inner.name = QString(embeddedStorage);
            inner.storage = true;
            writeMessage(inner, *a.embedded, false, remap);
            s.children.append(inner);
        }
        s.storage = true;
        storage.children.append(s);
    }
}

void collectNamed(const Pst::Message &m, QList<quint16> &ids)
{
    const auto note = [&ids](quint16 id) {
        if (id >= 0x8000 && !ids.contains(id)) {
            ids.append(id);
        }
    };
    for (auto it = m.properties.constBegin(); it != m.properties.constEnd(); ++it) {
        note(it.key());
    }
    for (const auto &r : m.recipients) {
        for (auto it = r.constBegin(); it != r.constEnd(); ++it) {
            note(quint16(it.key() >> 16));
        }
    }
    for (const Pst::Attachment &a : m.attachments) {
        for (auto it = a.properties.constBegin(); it != a.properties.constEnd(); ++it) {
            note(it.key());
        }
        if (a.embedded) {
            collectNamed(*a.embedded, ids);
        }
    }
}

// The named property mapping storage (2.2.3): ID -> name streams and the name -> ID streams
Cfb::Entry nameidEntry(const QList<Pst::NamedProperty> &names)
{
    Cfb::Entry storage;
    storage.name = QString(nameidStorage);
    storage.storage = true;
    QByteArray guids;
    QList<QUuid> guidList;
    QByteArray entries;
    QByteArray strings;
    QMap<quint16, QByteArray> lookup;
    for (int i = 0; i < names.size(); ++i) {
        const Pst::NamedProperty &n = names.at(i);
        quint16 guidIndex = 0;
        if (n.guid == Pst::psMapi) {
            guidIndex = 1;
        } else if (n.guid == Pst::psPublicStrings) {
            guidIndex = 2;
        } else {
            qsizetype at = guidList.indexOf(n.guid);
            if (at < 0) {
                at = guidList.size();
                guidList.append(n.guid);
                QByteArray g(16, 0);
                qToLittleEndian(n.guid.data1, g.data());
                qToLittleEndian(n.guid.data2, g.data() + 4);
                qToLittleEndian(n.guid.data3, g.data() + 6);
                std::copy(n.guid.data4, n.guid.data4 + 8, g.begin() + 8);
                guids += g;
            }
            guidIndex = quint16(3 + at);
        }
        const quint16 kind = quint16(guidIndex << 1 | (n.isString ? 1 : 0));
        QByteArray info(4, 0);
        qToLittleEndian(kind, info.data());
        qToLittleEndian(quint16(i), info.data() + 2);
        quint32 id = n.id;
        quint32 key = n.id;
        if (n.isString) {
            const QByteArray text(reinterpret_cast<const char *>(n.name.utf16()), n.name.size() * 2);
            id = quint32(strings.size());
            strings += le32(quint32(text.size())) + text;
            strings.resize((strings.size() + 3) / 4 * 4, 0);
            // The CRC of the name (the variant is not stated: the PST's, see PST-NOTES)
            key = Pst::crc(text.constData(), text.size());
        }
        entries += le32(id) + info;
        lookup[quint16(0x1000 + (key ^ kind) % 0x1F)] += le32(key) + info;
    }
    storage.addStream(QStringLiteral("__substg1.0_00020102"), guids);
    storage.addStream(QStringLiteral("__substg1.0_00030102"), entries);
    storage.addStream(QStringLiteral("__substg1.0_00040102"), strings);
    for (auto it = lookup.constBegin(); it != lookup.constEnd(); ++it) {
        storage.addStream(streamName(quint32(it.key()) << 16 | 0x0102), it.value());
    }
    return storage;
}
}

std::optional<File> read(const QByteArray &bytes, QString *error)
{
    const std::optional<Cfb::Entry> root = Cfb::read(bytes, error);
    if (!root) {
        return std::nullopt;
    }
    std::optional<Pst::Message> m = readMessage(*root, topHeader, 0, error);
    if (!m) {
        return std::nullopt;
    }
    File f;
    f.message = std::move(*m);
    if (const Cfb::Entry *n = root->child(nameidStorage)) {
        Pst::PropertyMap map;
        for (const quint16 id : {quint16(0x0002), quint16(0x0003), quint16(0x0004)}) {
            if (const Cfb::Entry *s = n->child(streamName(quint32(id) << 16 | 0x0102))) {
                map.insert(id, {Pst::PtypBinary, s->data});
            }
        }
        f.names = Pst::parseNameMap(map);
    }
    return f;
}

QByteArray write(const File &file)
{
    // Named properties numbered from 0x8000 in the order they are met (2.2.3.1.2)
    QList<quint16> used;
    collectNamed(file.message, used);
    QList<Pst::NamedProperty> names;
    QHash<quint16, quint16> renumbered;
    for (const quint16 id : std::as_const(used)) {
        if (file.names.contains(id)) {
            renumbered.insert(id, quint16(0x8000 + names.size()));
            names.append(file.names.value(id));
        }
    }
    const Remap remap = [&renumbered](quint16 id) {
        return id < 0x8000 ? id : renumbered.value(id, 0);
    };
    Cfb::Entry root;
    root.storage = true;
    writeMessage(root, file.message, true, remap);
    root.children.append(nameidEntry(names));
    return Cfb::write(root);
}

}
