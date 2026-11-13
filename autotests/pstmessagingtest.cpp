// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pst/mapigraph.h"
#include "pst/mapiitems.h"
#include "pst/pstmessaging.h"
#include "pstsamples.h"

#include <QTest>

#include <functional>
#include <QtEndian>

using namespace GraphMail;

namespace
{
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
QByteArray utf16(const QString &s)
{
    return QByteArray(reinterpret_cast<const char *>(s.utf16()), s.size() * 2);
}
// A NAMEID record: id or string offset, N bit and GUID index, property index
QByteArray nameId(quint32 id, bool isString, quint16 guid, quint16 index)
{
    return le32(id) + le16(quint16(guid << 1 | (isString ? 1 : 0))) + le16(index);
}
}

class PstMessagingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // [MS-PST] 2.4.7: numbered and string names, the two well-known sets and the GUID stream
    void nameMap()
    {
        // An invented property set GUID, stored little-endian as on disk
        const QUuid example(0x12345678, 0x9ABC, 0xDEF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08);
        const QByteArray guidStream = QByteArray::fromHex("78563412bc9af0de0102030405060708");
        const QString name = QStringLiteral("x-example-header");
        // Each name: its length in bytes, the text, padded to 4 bytes
        QByteArray strings = le32(4) + utf16(QStringLiteral("ab"));
        const quint32 second = quint32(strings.size());
        strings += le32(quint32(name.size() * 2)) + utf16(name);
        Pst::PropertyMap map;
        map.insert(0x0002, {Pst::PtypBinary, guidStream});
        map.insert(0x0003,
                   {Pst::PtypBinary,
                    nameId(0x8205, false, 1, 0) + nameId(second, true, 2, 1) + nameId(0x0001, false, 3, 2) + nameId(0, true, 0, 3)
                        + nameId(0x10, false, 9, 4)});
        map.insert(0x0004, {Pst::PtypBinary, strings});

        const QHash<quint16, Pst::NamedProperty> names = Pst::parseNameMap(map);
        QCOMPARE(names.size(), 4);
        QCOMPARE(names.value(0x8000).guid, Pst::psMapi);
        QVERIFY(!names.value(0x8000).isString);
        QCOMPARE(names.value(0x8000).id, 0x8205u);
        QCOMPARE(names.value(0x8001).guid, Pst::psPublicStrings);
        QVERIFY(names.value(0x8001).isString);
        QCOMPARE(names.value(0x8001).name, name);
        QCOMPARE(names.value(0x8002).guid, example);
        QCOMPARE(names.value(0x8002).id, 1u);
        QCOMPARE(names.value(0x8003).name, QStringLiteral("ab"));
        QVERIFY(names.value(0x8003).guid.isNull());
        // A GUID index past the stream is skipped
        QVERIFY(!names.contains(0x8004));
    }

    // [MS-PST] 2.5.3.1.1: a stored subject may start with 1 and the prefix length
    void subjects()
    {
        QCOMPARE(Pst::decodeSubject(QStringLiteral(u"\u0001\u0005RE: Example")), QStringLiteral("RE: Example"));
        QCOMPARE(Pst::decodeSubject(QStringLiteral(u"\u0001\u0001Example")), QStringLiteral("Example"));
        QCOMPARE(Pst::decodeSubject(QStringLiteral("Plain subject")), QStringLiteral("Plain subject"));
        QCOMPARE(Pst::decodeSubject(QString()), QString());

        Pst::Message m;
        m.properties.insert(0x0037, {Pst::PtypString, utf16(QStringLiteral(u"\u0001\u0005FW: Acme"))});
        QCOMPARE(m.string(0x0037), QStringLiteral("FW: Acme"));
    }

    // [MS-PST] 2.4.3.2 with the sample store of 3.10: the IPM subtree is NID 0x8022
    void entryIds()
    {
        QString error;
        const std::optional<Pst::Heap> heap = Pst::Heap::fromBlocks({pstSample_storeHeap()}, &error);
        QVERIFY(heap);
        const std::optional<Pst::PropertyMap> store = Pst::readPropertyContext(*heap, Pst::NodeContext(nullptr, {}), &error);
        QVERIFY(store);
        QCOMPARE(Pst::entryIdNid(store->value(0x35E0).data), 0x8022u);
        QCOMPARE(Pst::entryIdNid(store->value(0x35E3).data), 0x8062u);
        QCOMPARE(Pst::entryIdNid(store->value(0x35E7).data), 0x8042u);
        QCOMPARE(Pst::entryIdNid(QByteArray(10, 0)), 0u);
        QCOMPARE(Pst::entryIdNid(QByteArray(1, 1) + QByteArray(23, 0)), 0u);
        QCOMPARE(Pst::withNidType(0x8022, Pst::NidTypeHierarchyTable), 0x802Du);
        QCOMPARE(Pst::withNidType(0x8022, Pst::NidTypeContentsTable), 0x802Eu);
    }

    // ANSI strings by the message's code page
    void codepages()
    {
        Pst::Message m;
        m.properties.insert(0x0037, {Pst::PtypString8, QByteArray("Caf\xe9 \x80")});
        // Windows-1252 by default: 0x80 is the euro sign
        QCOMPARE(m.string(0x0037), QStringLiteral(u"Café €"));
        m.properties.insert(0x3FFD, {Pst::PtypInteger32, le32(65001)});
        m.properties.insert(0x0E1D, {Pst::PtypString8, QByteArray("Caf\xc3\xa9")});
        QCOMPARE(m.string(0x0E1D), QStringLiteral(u"Café"));
    }

    // A PST made by Outlook (PST_OUTLOOK_SAMPLE=file; such files hold real names, so none is in
    // the repository): every folder and message reads and converts, and the folders Outlook
    // hides are known as hidden
    void outlookSample()
    {
        const QString path = qEnvironmentVariable("PST_OUTLOOK_SAMPLE");
        if (path.isEmpty()) {
            QSKIP("PST_OUTLOOK_SAMPLE not set");
        }
        Pst::PstFile pst;
        QString error;
        QVERIFY2(pst.open(path, &error), qPrintable(error));
        int folders = 0;
        int hidden = 0;
        int messages = 0;
        std::function<void(quint32, int)> walk = [&](quint32 nid, int depth) {
            QVERIFY(depth < 32);
            const std::optional<QList<Pst::FolderInfo>> children = pst.subfolders(nid, &error);
            QVERIFY2(children, qPrintable(error));
            for (const Pst::FolderInfo &f : *children) {
                ++folders;
                hidden += f.hidden ? 1 : 0;
                const std::optional<QList<quint32>> ids = pst.messageIds(f.nid, false, &error);
                QVERIFY2(ids, qPrintable(error));
                QCOMPARE(ids->size(), f.count);
                for (const quint32 id : *ids) {
                    const std::optional<Pst::Message> m = pst.message(id, &error);
                    QVERIFY2(m, qPrintable(error));
                    ++messages;
                    switch (MapiItems::kindOf(m->string(0x001A))) {
                    case MapiItems::Kind::Mail:
                        MapiGraph::toGraph(*m, pst.namedProperties());
                        break;
                    case MapiItems::Kind::Contact:
                        QVERIFY(!MapiItems::contact(*m, pst.namedProperties()).json.isEmpty());
                        break;
                    case MapiItems::Kind::Event:
                        QVERIFY(MapiItems::event(*m, pst.namedProperties()).json.contains(QLatin1String("start")));
                        break;
                    case MapiItems::Kind::Task:
                        QVERIFY(MapiItems::task(*m, pst.namedProperties()).json.contains(QLatin1String("title")));
                        break;
                    case MapiItems::Kind::Other:
                        break;
                    }
                }
                walk(f.nid, depth + 1);
            }
        };
        walk(pst.ipmSubtree(), 0);
        qInfo() << folders << "folders," << hidden << "hidden," << messages << "messages";
        QVERIFY(folders > 0);
        // Outlook always makes some (Quick Step Settings, Conversation Action Settings)
        QVERIFY(hidden > 0);
    }
};

QTEST_GUILESS_MAIN(PstMessagingTest)
#include "pstmessagingtest.moc"
