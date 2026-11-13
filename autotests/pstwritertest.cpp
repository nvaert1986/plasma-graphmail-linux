// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pst/pstalgorithms.h"
#include "pst/pstltpwriter.h"
#include "pst/pstwriter.h"
#include "pst/pstndb.h"
#include "pst/pstndbwriter.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

using namespace GraphMail;

namespace
{
// Invented content of a given size, different per seed
QByteArray content(int seed, qsizetype size)
{
    QByteArray b(size, 0);
    quint32 x = 0x9E3779B9u * quint32(seed + 1);
    for (qsizetype i = 0; i < size; ++i) {
        x = x * 1664525u + 1013904223u;
        b[i] = char(x >> 24);
    }
    return b;
}

// The independent checker (.dev/tools/pstcheck.py), when the source tree and Python are there
QString runChecker(const QString &path)
{
    const QString script = QStringLiteral(PST_CHECKER);
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (!QFile::exists(script) || python.isEmpty()) {
        return QStringLiteral("SKIP");
    }
    QProcess p;
    p.start(python, {QStringLiteral("-I"), script, path});
    p.waitForFinished(600000);
    const QString out = QString::fromUtf8(p.readAllStandardOutput() + p.readAllStandardError());
    return p.exitCode() == 0 ? QString() : out;
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

#define CHECK_FILE(path)                                                                                                                             \
    do {                                                                                                                                             \
        const QString problems = runChecker(path);                                                                                                  \
        if (problems == QLatin1String("SKIP")) {                                                                                                    \
            qWarning("pstcheck.py not run");                                                                                                         \
        } else {                                                                                                                                     \
            QVERIFY2(problems.isEmpty(), qPrintable(problems));                                                                                     \
        }                                                                                                                                            \
    } while (false)
}

class PstWriterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void ndbRoundTrip_data()
    {
        QTest::addColumn<int>("crypt");
        QTest::newRow("none") << 0;
        QTest::newRow("permute") << 1;
        QTest::newRow("cyclic") << 2;
    }

    // Nodes with small, empty and tree data, and subnodes, read back as written. Raw data goes in
    // internal nodes (type 1): the checker reads message and folder nodes as property contexts
    void ndbRoundTrip()
    {
        QFETCH(int, crypt);
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("ndb.pst"));
        QString error;
        Pst::NdbWriter w;
        QVERIFY(w.create(path, quint8(crypt), &error));

        QHash<quint32, QByteArray> data;
        quint32 treeNid = 0;
        // Small, empty, exactly one block, and a data tree of three blocks
        const QList<qsizetype> sizes{100, 0, Pst::NdbWriter::maxBlockData, 3 * Pst::NdbWriter::maxBlockData - 7};
        for (int i = 0; i < sizes.size(); ++i) {
            const quint32 nid = w.newNid(0x01);
            data.insert(nid, content(i, sizes.at(i)));
            w.addNode({nid, w.writeData(data.value(nid)), 0, 0x8022});
            treeNid = nid;
        }
        // A node with subnodes, one of them with its own subnodes
        const QByteArray inner = content(50, 300);
        const quint64 innerSub = w.writeSubnodes({{0x201F, w.writeData(inner), 0}});
        QList<Pst::SubnodeEntry> subs;
        QHash<quint32, QByteArray> subData;
        for (quint32 i = 0; i < 5; ++i) {
            const quint32 nid = (i + 1) << 5 | 0x1F;
            subData.insert(nid, content(int(100 + i), 2000 + qsizetype(i) * 3000));
            subs.append({nid, w.writeData(subData.value(nid)), i == 2 ? innerSub : 0});
        }
        const quint32 parent = w.newNid(0x01);
        w.addNode({parent, w.writeData(QByteArrayLiteral("parent")), w.writeSubnodes(subs), 0x8022});
        QVERIFY2(w.finish(&error), qPrintable(error));

        Pst::Ndb r;
        QVERIFY2(r.open(path, &error), qPrintable(error));
        QCOMPARE(r.header().cryptMethod, crypt);
        QCOMPARE(r.header().fileEof, quint64(QFile(path).size()));
        QCOMPARE(r.header().amapValid, 2);
        QCOMPARE(r.header().nids[1], 0x405u);
        for (auto it = data.cbegin(); it != data.cend(); ++it) {
            QCOMPARE(r.nodeData(it.key()), std::optional<QByteArray>(it.value()));
            QCOMPARE(r.node(it.key())->parent, 0x8022u);
        }
        const std::optional<Pst::NodeEntry> p = r.node(parent);
        QVERIFY(p);
        const std::optional<QHash<quint32, Pst::SubnodeEntry>> readSubs = r.subnodes(p->sub);
        QVERIFY2(readSubs, qPrintable(r.error()));
        QCOMPARE(readSubs->size(), 5);
        for (auto it = subData.cbegin(); it != subData.cend(); ++it) {
            QCOMPARE(r.treeData(readSubs->value(it.key()).data), std::optional<QByteArray>(it.value()));
        }
        const std::optional<QHash<quint32, Pst::SubnodeEntry>> nested = r.subnodes(readSubs->value(3 << 5 | 0x1F).sub);
        QVERIFY(nested);
        QCOMPARE(r.treeData(nested->value(0x201F).data), std::optional<QByteArray>(inner));
        // A heap reads its data block by block
        const std::optional<QList<QByteArray>> blocks = r.treeBlocks(r.node(treeNid)->data);
        QVERIFY(blocks);
        QCOMPARE(blocks->size(), 3);
        QCOMPARE(blocks->first().size(), Pst::NdbWriter::maxBlockData);
        CHECK_FILE(path);
    }

    // Thousands of nodes and many subnodes: BTrees of several levels and an SIBLOCK
    void ndbManyNodes()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("many.pst"));
        QString error;
        Pst::NdbWriter w;
        QVERIFY(w.create(path, 1, &error));
        QList<quint32> nids;
        for (int i = 0; i < 5000; ++i) {
            const quint32 nid = w.newNid(0x01);
            nids.append(nid);
            w.addNode({nid, w.writeData(QByteArray::number(i)), 0, 0x8022});
        }
        QList<Pst::SubnodeEntry> subs;
        for (quint32 i = 1; i <= 1000; ++i) {
            subs.append({i << 5 | 0x1F, w.writeData(QByteArray::number(i * 7)), 0});
        }
        const quint32 parent = w.newNid(0x01);
        w.addNode({parent, w.writeData(QByteArray()), w.writeSubnodes(subs), 0x8022});
        QVERIFY2(w.finish(&error), qPrintable(error));

        Pst::Ndb r;
        QVERIFY2(r.open(path, &error), qPrintable(error));
        int count = 0;
        quint32 last = 0;
        QVERIFY(r.forEachNode([&](const Pst::NodeEntry &n) {
            QVERIFY(n.nid > last);
            last = n.nid;
            ++count;
        }));
        QCOMPARE(count, 5001);
        QCOMPARE(r.nodeData(nids.at(4321)), std::optional<QByteArray>(QByteArrayLiteral("4321")));
        const std::optional<QHash<quint32, Pst::SubnodeEntry>> readSubs = r.subnodes(r.node(parent)->sub);
        QVERIFY2(readSubs, qPrintable(r.error()));
        QCOMPARE(readSubs->size(), 1000);
        QCOMPARE(r.treeData(readSubs->value(777 << 5 | 0x1F).data), std::optional<QByteArray>(QByteArray::number(777 * 7)));
        CHECK_FILE(path);
    }

    // Over 32 MB: many AMap regions, PMaps, the first FMap, and an XXBLOCK (over 1021 blocks)
    void ndbLargeFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("large.pst"));
        QString error;
        Pst::NdbWriter w;
        QVERIFY(w.create(path, 1, &error));
        const QByteArray big = content(7, qsizetype(34) * 1024 * 1024);
        const quint32 nid = w.newNid(0x01);
        w.addNode({nid, w.writeData(big), 0, 0x8022});
        QVERIFY2(w.finish(&error), qPrintable(error));

        Pst::Ndb r;
        QVERIFY2(r.open(path, &error), qPrintable(error));
        QVERIFY(r.nodeData(nid) == big);
        const quint64 eof = r.header().fileEof;
        QCOMPARE(eof, quint64(QFile(path).size()));
        QCOMPARE((eof - Pst::Layout::firstAMap) % Pst::Layout::regionSize, 0u);
        QVERIFY(eof > Pst::Layout::regionBase(129));

        // The map pages where the layout puts them, with their types and checksums
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const auto pageType = [&](quint64 ib) {
            f.seek(qint64(ib));
            const QByteArray p = f.read(512);
            const quint32 stored = qFromLittleEndian<quint32>(p.constData() + 500);
            return Pst::crc(p.constData(), 496) == stored ? int(quint8(p.at(496))) : -1;
        };
        QCOMPARE(pageType(Pst::Layout::regionBase(0)), 0x84);
        QCOMPARE(pageType(Pst::Layout::regionBase(0) + 512), 0x83);
        QCOMPARE(pageType(Pst::Layout::regionBase(1)), 0x84);
        QCOMPARE(pageType(Pst::Layout::regionBase(8) + 512), 0x83);
        QCOMPARE(pageType(Pst::Layout::regionBase(128) + 1024), 0x82);
        QCOMPARE(pageType(Pst::Layout::dlist), 0x86);
        CHECK_FILE(path);
    }

    // The checker is no rubber stamp: damage in a block, an AMap or a reference count is found
    void checkerCatchesDamage()
    {
        if (runChecker(QString()) == QLatin1String("SKIP")) {
            QSKIP("pstcheck.py not available");
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("damage.pst"));
        QString error;
        Pst::NdbWriter w;
        QVERIFY(w.create(path, 1, &error));
        const quint32 nid = w.newNid(0x01);
        w.addNode({nid, w.writeData(content(3, 5000)), 0, 0x8022});
        QVERIFY(w.finish(&error));
        QVERIFY(runChecker(path).isEmpty());

        Pst::Ndb r;
        QVERIFY(r.open(path, &error));
        const quint64 blockIb = r.block(r.node(nid)->data)->ib;
        r.close();
        const auto damaged = [&](quint64 at, char value) {
            const QString copy = dir.filePath(QStringLiteral("copy.pst"));
            QFile::remove(copy);
            QFile::copy(path, copy);
            QFile f(copy);
            if (!f.open(QIODevice::ReadWrite)) {
                return f.errorString();
            }
            f.seek(qint64(at));
            f.write(QByteArray(1, value));
            f.close();
            return runChecker(copy);
        };
        QVERIFY(damaged(blockIb + 100, 0x5A).contains(QLatin1String("CRC wrong")));
        // The first data slots of the first AMap (after the AMap and PMap pages) marked free
        QVERIFY(damaged(Pst::Layout::firstAMap + 2, 0).contains(QLatin1String("not marked allocated")));

        // A property context whose heap signature is gone (unencoded, so the byte is where it seems)
        const QString pcPath = dir.filePath(QStringLiteral("pc.pst"));
        Pst::NdbWriter pw;
        QVERIFY(pw.create(pcPath, 0, &error));
        Pst::SubnodeSink subs(pw);
        Pst::PropertyMap props;
        props.insert(0x0E07, {Pst::PtypInteger32, le32(1)});
        const quint32 pcNid = pw.newNid(0x04);
        pw.addNode({pcNid, pw.writeBlocks(Pst::buildPropertyContext(props, subs)), subs.write(), 0x8022});
        QVERIFY(pw.finish(&error));
        QVERIFY(runChecker(pcPath).isEmpty());
        QVERIFY(r.open(pcPath, &error));
        const quint64 pcIb = r.block(r.node(pcNid)->data)->ib;
        r.close();
        QFile::remove(path);
        QVERIFY(QFile::copy(pcPath, path));
        QVERIFY(damaged(pcIb + 2, 0).contains(QLatin1String("no heap")));
    }

    // Property contexts: every value kind, large values in subnodes, multi-valued properties
    void pcRoundTrip()
    {
        Pst::PropertyMap props;
        props.insert(0x0017, {Pst::PtypInteger32, le32(2)});
        props.insert(0x0057, {Pst::PtypBoolean, QByteArray(1, 1)});
        props.insert(0x1000, {Pst::PtypString, utf16(QStringLiteral("Example body text"))});
        props.insert(0x0037, {Pst::PtypString, utf16(QString())});
        props.insert(0x0039, {Pst::PtypTime, QByteArray::fromHex("00403ef064a1d401")});
        props.insert(0x0E08, {Pst::PtypInteger16, QByteArray::fromHex("3412")});
        props.insert(0x8001, {Pst::PtypGuid, content(9, 16)});
        props.insert(0x8002, {Pst::PtypInteger64, content(10, 8)});
        props.insert(0x8003, {Pst::PtypFloating64, content(11, 8)});
        // Just inside the heap, just outside it, and a value over several blocks
        props.insert(0x3701, {Pst::PtypBinary, content(12, Pst::HeapWriter::maxItem)});
        props.insert(0x1013, {Pst::PtypBinary, content(13, Pst::HeapWriter::maxItem + 1)});
        props.insert(0x1009, {Pst::PtypBinary, content(14, 100000)});
        props.insert(0x8004, {Pst::PtypInteger32 | Pst::PtypMultiple, le32(1) + le32(2) + le32(3)});
        props.insert(0x8005, {Pst::PtypString | Pst::PtypMultiple, le32(2) + le32(12) + le32(16) + utf16(QStringLiteral("abcd"))});

        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("pc.pst"));
        QString error;
        Pst::NdbWriter w;
        QVERIFY(w.create(path, 1, &error));
        Pst::SubnodeSink subs(w);
        const QList<QByteArray> blocks = Pst::buildPropertyContext(props, subs);
        QCOMPARE(blocks.size(), 1);
        const quint32 nid = w.newNid(0x04);
        w.addNode({nid, w.writeBlocks(blocks), subs.write(), 0x8022});
        QVERIFY2(w.finish(&error), qPrintable(error));

        Pst::Ndb r;
        QVERIFY(r.open(path, &error));
        const std::optional<Pst::NodeEntry> node = r.node(nid);
        const std::optional<Pst::PropertyMap> back = Pst::readNodeProperties(r, node->data, node->sub, &error);
        QVERIFY2(back, qPrintable(error));
        QCOMPARE(back->size(), props.size());
        for (auto it = props.cbegin(); it != props.cend(); ++it) {
            QCOMPARE(back->value(it.key()).type, it.value().type);
            QCOMPARE(back->value(it.key()).data, it.value().data);
        }
        QCOMPARE(back->value(0x8005).items().size(), 2);
        CHECK_FILE(path);
    }

    // A PC over many heap blocks (HNBITMAPHDR at block 8) with a BTH index level
    void pcManyProperties()
    {
        Pst::PropertyMap props;
        for (quint16 id = 0x4000; id < 0x4000 + 3000; ++id) {
            props.insert(id, {Pst::PtypBinary, content(id, 200)});
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("pcmany.pst"));
        QString error;
        Pst::NdbWriter w;
        QVERIFY(w.create(path, 2, &error));
        Pst::SubnodeSink subs(w);
        const QList<QByteArray> blocks = Pst::buildPropertyContext(props, subs);
        QVERIFY(blocks.size() > 9);
        // HNBITMAPHDR on block 8: ibHnpm past 66 header bytes
        QVERIFY(qFromLittleEndian<quint16>(blocks.at(8).constData()) > 66);
        const quint32 nid = w.newNid(0x04);
        w.addNode({nid, w.writeBlocks(blocks), subs.write(), 0x8022});
        QVERIFY2(w.finish(&error), qPrintable(error));

        Pst::Ndb r;
        QVERIFY(r.open(path, &error));
        const std::optional<Pst::NodeEntry> node = r.node(nid);
        const std::optional<Pst::PropertyMap> back = Pst::readNodeProperties(r, node->data, node->sub, &error);
        QVERIFY2(back, qPrintable(error));
        QCOMPARE(back->size(), 3000);
        QCOMPARE(back->value(0x4000 + 2999).data, content(0x4000 + 2999, 200));
        CHECK_FILE(path);
    }

    void tcRoundTrip_data()
    {
        QTest::addColumn<int>("rows");
        QTest::newRow("empty") << 0;
        QTest::newRow("three") << 3;
        QTest::newRow("in a subnode") << 2000;
    }

    // Table contexts: computed layouts, absent cells, large cells, row matrices over blocks
    void tcRoundTrip()
    {
        QFETCH(int, rows);
        const QList<quint32> columns{0x0037001F, 0x0E070003, 0x0E060040, 0x0057000B, 0x00170003, 0x0E08001F, 0x10820102, 0x80010048, 0x00360002};
        QList<QHash<quint32, Pst::PropValue>> table;
        for (int i = 0; i < rows; ++i) {
            QHash<quint32, Pst::PropValue> row;
            row.insert(0x67F20003, {Pst::PtypInteger32, le32(0x200024 + quint32(i) * 32)});
            row.insert(0x67F30003, {Pst::PtypInteger32, le32(quint32(i + 1))});
            row.insert(0x0037001F, {Pst::PtypString, utf16(QStringLiteral("Example subject %1").arg(i))});
            row.insert(0x0E070003, {Pst::PtypInteger32, le32(quint32(i))});
            row.insert(0x0E060040, {Pst::PtypTime, content(i, 8)});
            row.insert(0x00360002, {Pst::PtypInteger16, QByteArray::fromHex("0100")});
            row.insert(0x80010048, {Pst::PtypGuid, content(i + 7, 16)});
            if (i % 2) {
                row.insert(0x0057000B, {Pst::PtypBoolean, QByteArray(1, 1)});
            }
            if (i == 1) {
                // A cell too large for the heap
                row.insert(0x10820102, {Pst::PtypBinary, content(77, 5000)});
            }
            table.append(row);
        }
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("tc.pst"));
        QString error;
        Pst::NdbWriter w;
        QVERIFY(w.create(path, 1, &error));
        Pst::SubnodeSink subs(w);
        const quint32 nid = 0x802E;
        w.addNode({nid, w.writeBlocks(Pst::buildTableContext(columns, table, subs)), subs.write(), 0});
        QVERIFY2(w.finish(&error), qPrintable(error));

        Pst::Ndb r;
        QVERIFY(r.open(path, &error));
        const std::optional<Pst::NodeEntry> node = r.node(nid);
        const std::optional<Pst::Table> back = Pst::readNodeTable(r, node->data, node->sub, &error);
        QVERIFY2(back, qPrintable(error));
        QCOMPARE(back->columns.size(), columns.size() + 2);
        QCOMPARE(back->rows.size(), rows);
        for (int i = 0; i < rows; ++i) {
            const QHash<quint32, Pst::PropValue> &got = back->rows.at(i);
            const QHash<quint32, Pst::PropValue> &want = table.at(i);
            QCOMPARE(got.size(), want.size());
            for (auto it = want.cbegin(); it != want.cend(); ++it) {
                QCOMPARE(got.value(it.key()).data, it.value().data);
            }
        }
        CHECK_FILE(path);
    }

    // A whole PST: folders, messages with recipients and attachments (one an attached message
    // with an attachment of its own), an FAI message, named properties; read back with PstFile
    void messagingRoundTrip()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("mail.pst"));
        QString error;
        Pst::PstWriter w;
        QVERIFY(w.create(path, QStringLiteral("Example export"), &error));
        const quint32 inbox = w.addFolder(Pst::PstWriter::nidIpmSubtree, QStringLiteral("Inbox"));
        const quint32 projects = w.addFolder(inbox, QStringLiteral("Projects"));
        const quint32 calendar = w.addFolder(Pst::PstWriter::nidIpmSubtree, QStringLiteral("Calendar"), QStringLiteral("IPF.Appointment"));
        QVERIFY(inbox && projects && calendar);
        QCOMPARE(w.addFolder(Pst::nidRootFolder, QStringLiteral("Not here")), 0u);

        const QUuid exampleSet(0x12345678, 0x9ABC, 0xDEF0, 1, 2, 3, 4, 5, 6, 7, 8);
        const quint16 keywords = w.namedProperty(Pst::psPublicStrings, QStringLiteral("Keywords"));
        const quint16 numbered = w.namedProperty(exampleSet, 0x8501);
        QCOMPARE(w.namedProperty(Pst::psPublicStrings, QStringLiteral("Keywords")), keywords);

        const auto message = [&](const QString &subject, bool read) {
            Pst::Message m;
            m.properties.insert(0x0037, {Pst::PtypString, utf16(subject)});
            m.properties.insert(0x0042, {Pst::PtypString, utf16(QStringLiteral("Alice Example"))});
            m.properties.insert(0x0E04, {Pst::PtypString, utf16(QStringLiteral("Bob Example"))});
            m.properties.insert(0x0E07, {Pst::PtypInteger32, le32(read ? 1 : 0)});
            m.properties.insert(0x0E06, {Pst::PtypTime, QByteArray::fromHex("00403ef064a1d401")});
            m.properties.insert(0x1000, {Pst::PtypString, utf16(QStringLiteral("Example body for %1").arg(subject))});
            QHash<quint32, Pst::PropValue> to;
            to.insert(0x0C150003, {Pst::PtypInteger32, le32(1)});
            to.insert(0x3001001F, {Pst::PtypString, utf16(QStringLiteral("Bob Example"))});
            to.insert(0x3003001F, {Pst::PtypString, utf16(QStringLiteral("bob@example.com"))});
            to.insert(0x3002001F, {Pst::PtypString, utf16(QStringLiteral("SMTP"))});
            m.recipients.append(to);
            return m;
        };

        Pst::Message first = message(QStringLiteral("Example subject"), false);
        first.properties.insert(keywords, {Pst::PtypString | Pst::PtypMultiple, le32(1) + le32(8) + utf16(QStringLiteral("Acme"))});
        first.properties.insert(numbered, {Pst::PtypInteger32, le32(42)});
        Pst::Attachment small;
        small.properties.insert(0x3704, {Pst::PtypString, utf16(QStringLiteral("notes.txt"))});
        small.properties.insert(0x3701, {Pst::PtypBinary, QByteArrayLiteral("Example attachment text")});
        Pst::Attachment large;
        large.properties.insert(0x3704, {Pst::PtypString, utf16(QStringLiteral("drawing.bin"))});
        large.properties.insert(0x3701, {Pst::PtypBinary, content(5, 300000)});
        Pst::Message inner = message(QStringLiteral("Attached example"), true);
        inner.attachments.append(small);
        Pst::Attachment attached;
        attached.properties.insert(0x3001, {Pst::PtypString, utf16(QStringLiteral("Attached example"))});
        attached.embedded = std::make_shared<Pst::Message>(inner);
        // An OLE object (afStorage): its compound file kept as it is
        Pst::Attachment ole;
        ole.properties.insert(0x3001, {Pst::PtypString, utf16(QStringLiteral("Example object"))});
        ole.storage = QByteArray::fromHex("d0cf11e0a1b11ae1") + content(7, 20000);
        first.attachments = {small, large, attached, ole};
        const quint32 firstNid = w.addMessage(inbox, first);
        QVERIFY(firstNid);
        for (int i = 0; i < 499; ++i) {
            QVERIFY(w.addMessage(inbox, message(QStringLiteral("Example %1").arg(i), i % 3 == 0)));
        }
        QVERIFY(w.addMessage(projects, message(QStringLiteral("Project example"), true)));
        Pst::Message view;
        view.properties.insert(0x001A, {Pst::PtypString, utf16(QStringLiteral("IPM.Microsoft.FolderDesign.NamedView"))});
        QVERIFY(w.addMessage(inbox, view, true));
        QVERIFY2(w.finish(&error), qPrintable(error));
        CHECK_FILE(path);
        // PST_KEEP_DIR=dir keeps the file, to look at it in other software
        if (const QString keep = qEnvironmentVariable("PST_KEEP_DIR"); !keep.isEmpty()) {
            QFile::remove(keep + QStringLiteral("/example-export.pst"));
            QFile::copy(path, keep + QStringLiteral("/example-export.pst"));
        }

        Pst::PstFile r;
        QVERIFY2(r.open(path, &error), qPrintable(error));
        QCOMPARE(r.displayName(), QStringLiteral("Example export"));
        QCOMPARE(r.ipmSubtree(), Pst::PstWriter::nidIpmSubtree);
        QCOMPARE(r.passwordCrc(), 0u);

        const std::optional<QList<Pst::FolderInfo>> root = r.subfolders(Pst::nidRootFolder, &error);
        QVERIFY2(root, qPrintable(error));
        QCOMPARE(root->size(), 3);
        const std::optional<QList<Pst::FolderInfo>> top = r.subfolders(r.ipmSubtree(), &error);
        QVERIFY2(top, qPrintable(error));
        QStringList names;
        for (const Pst::FolderInfo &f : *top) {
            names.append(f.name);
            if (f.name == QLatin1String("Inbox")) {
                QCOMPARE(f.count, 500);
                QCOMPARE(f.unread, 333);
                QVERIFY(f.hasSubfolders);
                QCOMPARE(f.containerClass, QStringLiteral("IPF.Note"));
            }
        }
        names.sort();
        QCOMPARE(names, (QStringList{QStringLiteral("Calendar"), QStringLiteral("Deleted Items"), QStringLiteral("Inbox")}));
        QCOMPARE(r.subfolders(inbox, &error)->first().name, QStringLiteral("Projects"));

        const std::optional<QList<quint32>> ids = r.messageIds(inbox, false, &error);
        QVERIFY2(ids, qPrintable(error));
        QCOMPARE(ids->size(), 500);
        QCOMPARE(r.messageIds(inbox, true, &error)->size(), 1);
        QCOMPARE(r.messageIds(projects, false, &error)->size(), 1);

        const std::optional<Pst::Message> m = r.message(firstNid, &error);
        QVERIFY2(m, qPrintable(error));
        QCOMPARE(m->parent, inbox);
        QCOMPARE(m->string(0x0037), QStringLiteral("Example subject"));
        QCOMPARE(m->string(0x001A), QStringLiteral("IPM.Note"));
        QVERIFY(m->properties.value(0x0E07).toInt() & 0x10);
        QCOMPARE(m->recipients.size(), 1);
        QCOMPARE(m->recipients.first().value(0x3003001F).toString(), QStringLiteral("bob@example.com"));
        QCOMPARE(m->attachments.size(), 4);
        QCOMPARE(m->attachments.at(0).properties.value(0x3701).data, QByteArrayLiteral("Example attachment text"));
        QCOMPARE(m->attachments.at(1).properties.value(0x3701).data, content(5, 300000));
        // PidTagAttachSize as Outlook counts it: data 300000, name 22, method, rendering position
        // and the size itself 4 each
        QCOMPARE(m->attachments.at(1).properties.value(0x0E20).toInt(), 300034);
        QVERIFY(m->attachments.at(2).embedded);
        QCOMPARE(m->attachments.at(2).embedded->string(0x0037), QStringLiteral("Attached example"));
        QCOMPARE(m->attachments.at(2).embedded->attachments.size(), 1);
        QCOMPARE(m->attachments.at(2).properties.value(0x3705).toInt(), 5);
        QVERIFY(!m->attachments.at(3).embedded);
        QCOMPARE(m->attachments.at(3).properties.value(0x3705).toInt(), 6);
        QCOMPARE(m->attachments.at(3).storage, QByteArray::fromHex("d0cf11e0a1b11ae1") + content(7, 20000));

        // Named properties resolve by the file's map
        const Pst::NamedProperty kw = r.namedProperties().value(keywords);
        QVERIFY(kw.isString);
        QCOMPARE(kw.name, QStringLiteral("Keywords"));
        QCOMPARE(kw.guid, Pst::psPublicStrings);
        QCOMPARE(r.namedProperties().value(numbered).guid, exampleSet);
        QCOMPARE(r.namedProperties().value(numbered).id, 0x8501u);
        QCOMPARE(m->properties.value(numbered).toInt(), 42);
    }

    // NIDs number on from the header's start values, as in the spec's examples
    void nidNumbering()
    {
        QTemporaryDir dir;
        QString error;
        Pst::NdbWriter w;
        QVERIFY(w.create(dir.filePath(QStringLiteral("nids.pst")), 1, &error));
        QCOMPARE(w.newNid(0x02), 0x8022u);
        QCOMPARE(w.newNid(0x02), 0x8042u);
        QCOMPARE(w.newNid(0x04), 0x200024u);
    }
};

QTEST_GUILESS_MAIN(PstWriterTest)
#include "pstwritertest.moc"
