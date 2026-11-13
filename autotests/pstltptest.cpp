// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pst/pstltp.h"
#include "pstsamples.h"

#include <QTest>
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

// A heap block: its header (ibHnpm filled in here), the items, then the page map
QByteArray heapBlock(QByteArray header, const QList<QByteArray> &items)
{
    QByteArray b = header;
    QList<quint16> offsets{quint16(b.size())};
    for (const QByteArray &item : items) {
        b += item;
        offsets.append(quint16(b.size()));
    }
    if (b.size() % 2) {
        b += '\0';
    }
    b.replace(0, 2, le16(quint16(b.size())));
    b += le16(quint16(items.size())) + le16(0);
    for (const quint16 at : std::as_const(offsets)) {
        b += le16(at);
    }
    return b;
}

QByteArray firstHeader(quint8 clientSig, quint32 userRoot)
{
    return QByteArray(2, 0) + char(0xEC) + char(clientSig) + le32(userRoot) + QByteArray(4, 0);
}

quint32 hid(int block, int index)
{
    return quint32(block) << 16 | quint32(index) << 5;
}
}

class PstLtpTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // [MS-PST] 3.8: the heap of the message store: eight items, a PC at HID 0x20
    void sampleHeap()
    {
        QString error;
        const std::optional<Pst::Heap> heap = Pst::Heap::fromBlocks({pstSample_storeHeap()}, &error);
        QVERIFY2(heap, qPrintable(error));
        QCOMPARE(heap->clientSignature(), 0xBC);
        QCOMPARE(heap->userRoot(), 0x20u);
        QCOMPARE(heap->item(0x20), QByteArray::fromHex("b502060040000000"));
        QCOMPARE(heap->item(0x40)->size(), 0x58);
        QCOMPARE(heap->item(0x100)->size(), 24);
        QVERIFY(!heap->item(0x120));
        QVERIFY(!heap->item(0));
        QVERIFY(!heap->item(hid(1, 1)));
        // An NID is no HID
        QVERIFY(!heap->item(0x21));
    }

    // [MS-PST] 3.9 and 3.10: the BTH and the message store's properties
    void sampleStore()
    {
        QString error;
        const std::optional<Pst::Heap> heap = Pst::Heap::fromBlocks({pstSample_storeHeap()}, &error);
        QVERIFY(heap);
        const std::optional<QList<Pst::BthRecord>> records = Pst::readBth(*heap, heap->userRoot(), &error);
        QVERIFY2(records, qPrintable(error));
        QCOMPARE(records->size(), 11);

        const std::optional<Pst::PropertyMap> props = Pst::readPropertyContext(*heap, Pst::NodeContext(nullptr, {}), &error);
        QVERIFY2(props, qPrintable(error));
        QCOMPARE(props->size(), 11);
        QCOMPARE(props->value(0x3001).type, Pst::PtypString);
        QCOMPARE(props->value(0x3001).toString(), QStringLiteral("UNICODE1"));
        QCOMPARE(props->value(0x0E38).toInt(), 0);
        QCOMPARE(props->value(0x35DF).toInt(), 0x89);
        QCOMPARE(props->value(0x67FF).toInt(), 0);
        QCOMPARE(props->value(0x6633).type, Pst::PtypBoolean);
        QVERIFY(props->value(0x6633).toBool());
        QCOMPARE(props->value(0x66FA).toInt(), 0x000E000D);
        QCOMPARE(props->value(0x0FF9).data, QByteArray::fromHex("229db50adcd9944385de90aeb07d1270"));
        QCOMPARE(props->value(0x0E34).data, QByteArray::fromHex("01000000f55ef6669569cc4c83d1d8739899028501000000"));
        QCOMPARE(props->value(0x35E0).data.right(4), QByteArray::fromHex("22800000"));
        QCOMPARE(props->value(0x35E3).data.right(4), QByteArray::fromHex("62800000"));
        QCOMPARE(props->value(0x35E7).data.right(4), QByteArray::fromHex("42800000"));
    }

    // [MS-PST] 3.11 and 3.12: the root folder's hierarchy table, three subfolders
    void sampleTable()
    {
        QString error;
        const std::optional<Pst::Heap> heap = Pst::Heap::fromBlocks({pstSample_hierarchyTable()}, &error);
        QVERIFY2(heap, qPrintable(error));
        QCOMPARE(heap->clientSignature(), 0x7C);
        const std::optional<Pst::Table> table = Pst::readTableContext(*heap, Pst::NodeContext(nullptr, {}), true, &error);
        QVERIFY2(table, qPrintable(error));
        QCOMPARE(table->columns.size(), 13);
        // The dump has 0x0E300102 where the decoded listing of 3.12 says 0x0E300003; the bytes win
        QCOMPARE(table->columns.first().tag, 0x0E300102u);
        QCOMPARE(table->columns.at(2).offset, 32);
        QCOMPARE(table->columns.at(7).size, 1);
        QCOMPARE(table->columns.at(7).bit, 5);
        QCOMPARE(table->rows.size(), 3);

        const QStringList names{QStringLiteral("Top of Personal Folders"), QStringLiteral("Search Root"), QStringLiteral("SPAM Search Folder 2")};
        const QList<quint32> ids{0x8022, 0x8042, 0x2223};
        for (int i = 0; i < 3; ++i) {
            const QHash<quint32, Pst::PropValue> &row = table->rows.at(i);
            QCOMPARE(row.value(0x67F20003).toInt(), ids.at(i));
            QCOMPARE(row.value(0x3001001F).toString(), names.at(i));
            QCOMPARE(row.value(0x36020003).toInt(), 0);
            QCOMPARE(row.value(0x36030003).toInt(), 0);
            QCOMPARE(row.value(0x360A000B).toBool(), i == 0);
            // Cells without their existence bit are left out. The CEB is FC 00 (bits 0 to 5), so
            // ReplChangenum is absent, although the decoded listing of 3.12 shows it as 0
            QVERIFY(!row.contains(0x0E330014));
            QVERIFY(!row.contains(0x0E300102));
            QVERIFY(!row.contains(0x3613001F));
        }
    }

    // Items in later blocks are found by the block index of their HID
    void multiBlockHeap()
    {
        const QByteArray first = heapBlock(firstHeader(0xBC, hid(0, 1)), {QByteArrayLiteral("first"), QByteArrayLiteral("second item")});
        const QByteArray second = heapBlock(QByteArray(2, 0), {QByteArrayLiteral("in block one")});
        QString error;
        const std::optional<Pst::Heap> heap = Pst::Heap::fromBlocks({first, second}, &error);
        QVERIFY2(heap, qPrintable(error));
        QCOMPARE(*heap->item(hid(0, 1)), QByteArrayLiteral("first"));
        QCOMPARE(*heap->item(hid(0, 2)), QByteArrayLiteral("second item"));
        QCOMPARE(*heap->item(hid(1, 1)), QByteArrayLiteral("in block one"));
        QVERIFY(!heap->item(hid(1, 2)));
        QVERIFY(!heap->item(hid(2, 1)));
    }

    // A BTH with an index level: the leaves of every child, in order
    void bthIndexLevel()
    {
        const QByteArray leafA = le16(1) + QByteArrayLiteral("aaaa") + le16(2) + QByteArrayLiteral("bbbb");
        const QByteArray leafB = le16(7) + QByteArrayLiteral("cccc");
        const QByteArray index = le16(1) + le32(hid(0, 3)) + le16(7) + le32(hid(0, 4));
        const QByteArray header = QByteArray(1, char(0xB5)) + char(2) + char(4) + char(1) + le32(hid(0, 2));
        QString error;
        const std::optional<Pst::Heap> heap = Pst::Heap::fromBlocks({heapBlock(firstHeader(0xB5, hid(0, 1)), {header, index, leafA, leafB})}, &error);
        QVERIFY2(heap, qPrintable(error));
        const std::optional<QList<Pst::BthRecord>> records = Pst::readBth(*heap, hid(0, 1), &error);
        QVERIFY2(records, qPrintable(error));
        QCOMPARE(records->size(), 3);
        QCOMPARE(records->at(1).key, le16(2));
        QCOMPARE(records->at(2).data, QByteArrayLiteral("cccc"));

        // An empty BTH has no root
        const QByteArray empty = QByteArray(1, char(0xB5)) + char(2) + char(4) + char(0) + le32(0);
        const std::optional<Pst::Heap> emptyHeap = Pst::Heap::fromBlocks({heapBlock(firstHeader(0xB5, hid(0, 1)), {empty})}, &error);
        QVERIFY(emptyHeap);
        QVERIFY(Pst::readBth(*emptyHeap, hid(0, 1), &error)->isEmpty());
    }

    // A PC's values: inline up to 4 bytes, larger fixed ones and variable ones in the heap
    void propertyValues()
    {
        const QByteArray time = QByteArray::fromHex("00403ef064a1d401");
        const QByteArray subject = QStringLiteral("Example subject").toUtf8();
        const QString acme = QStringLiteral("Acme");
        QByteArray utf16;
        for (const QChar c : acme) {
            utf16 += le16(c.unicode());
        }
        // MV string: ulCount, offsets from the start of the value, the items
        const QByteArray mv = le32(2) + le32(12) + le32(16) + utf16.left(4) + utf16.mid(4);
        const QByteArray records = le16(0x0017) + le16(Pst::PtypInteger32) + le32(2) //
            + le16(0x0037) + le16(Pst::PtypString8) + le32(hid(0, 3)) //
            + le16(0x0039) + le16(Pst::PtypTime) + le32(hid(0, 4)) //
            + le16(0x0057) + le16(Pst::PtypBoolean) + le32(1) //
            + le16(0x3001) + le16(Pst::PtypString) + le32(0) //
            + le16(0x8000) + le16(Pst::PtypString | Pst::PtypMultiple) + le32(hid(0, 5));
        const QByteArray header = QByteArray(1, char(0xB5)) + char(2) + char(6) + char(0) + le32(hid(0, 2));
        QString error;
        const std::optional<Pst::Heap> heap = Pst::Heap::fromBlocks({heapBlock(firstHeader(0xBC, hid(0, 1)), {header, records, subject, time, mv})}, &error);
        QVERIFY2(heap, qPrintable(error));
        const std::optional<Pst::PropertyMap> props = Pst::readPropertyContext(*heap, Pst::NodeContext(nullptr, {}), &error);
        QVERIFY2(props, qPrintable(error));
        QCOMPARE(props->value(0x0017).toInt(), 2);
        QCOMPARE(props->value(0x0037).toString(), QStringLiteral("Example subject"));
        QCOMPARE(props->value(0x0039).toTime(), QDateTime(QDate(2019, 1, 1), QTime(0, 0), QTimeZone::UTC));
        QVERIFY(props->value(0x0057).toBool());
        // HNID 0: an empty value
        QVERIFY(props->contains(0x3001));
        QVERIFY(props->value(0x3001).toString().isEmpty());
        const QList<QByteArray> items = props->value(0x8000).items();
        QCOMPARE(items.size(), 2);
        QCOMPARE((Pst::PropValue{Pst::PtypString, items.at(0)}).toString(), QStringLiteral("Ac"));
        QCOMPARE((Pst::PropValue{Pst::PtypString, items.at(1)}).toString(), QStringLiteral("me"));

        // A value in a subnode needs the NDB; without it the PC is refused, not misread
        const QByteArray subRecord = le16(0x1000) + le16(Pst::PtypBinary) + le32(0x41);
        const std::optional<Pst::Heap> subHeap = Pst::Heap::fromBlocks({heapBlock(firstHeader(0xBC, hid(0, 1)), {header, subRecord})}, &error);
        QVERIFY(subHeap);
        QVERIFY(!Pst::readPropertyContext(*subHeap, Pst::NodeContext(nullptr, {}), &error));
    }

    // MV values of a fixed-size type are a plain array
    void multiValuedFixed()
    {
        const Pst::PropValue v{Pst::PtypInteger32 | Pst::PtypMultiple, le32(5) + le32(6) + le32(7)};
        const QList<QByteArray> items = v.items();
        QCOMPARE(items.size(), 3);
        QCOMPARE(items.at(2), le32(7));
    }

    // Damaged heaps are refused
    void damagedHeap()
    {
        QString error;
        QVERIFY(!Pst::Heap::fromBlocks({}, &error));
        QByteArray noSignature = pstSample_storeHeap();
        noSignature[2] = 0;
        QVERIFY(!Pst::Heap::fromBlocks({noSignature}, &error));
        QByteArray mapOutside = pstSample_storeHeap();
        mapOutside.replace(0, 2, le16(0x0400));
        QVERIFY(!Pst::Heap::fromBlocks({mapOutside}, &error));
        // An allocation offset past the page map
        QByteArray badOffset = pstSample_storeHeap();
        badOffset.replace(0xEC + 4 + 2, 2, le16(0x00F0));
        QVERIFY(!Pst::Heap::fromBlocks({badOffset}, &error));
        // A TC asked of a PC heap
        const std::optional<Pst::Heap> pc = Pst::Heap::fromBlocks({pstSample_storeHeap()}, &error);
        QVERIFY(!Pst::readTableContext(*pc, Pst::NodeContext(nullptr, {}), true, &error));
    }
};

QTEST_GUILESS_MAIN(PstLtpTest)
#include "pstltptest.moc"
