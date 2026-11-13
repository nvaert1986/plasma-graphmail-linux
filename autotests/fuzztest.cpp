// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cfb/cfb.h"
#include "msg/msgfile.h"
#include "pst/mapigraph.h"
#include "pst/mapiitems.h"
#include "pst/pstndbwriter.h"
#include "pst/pstwriter.h"
#include "pst/pstalgorithms.h"
#include "pst/rtf.h"

#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

using namespace GraphMail;

namespace
{
QByteArray le32(quint32 v)
{
    QByteArray b(4, 0);
    qToLittleEndian(v, b.data());
    return b;
}
Pst::PropValue str(const QString &s)
{
    return {Pst::PtypString, QByteArray(reinterpret_cast<const char *>(s.utf16()), s.size() * 2)};
}

Pst::Message sampleMessage(int i)
{
    Pst::Message m;
    m.properties.insert(0x0037, str(QStringLiteral("Example %1").arg(i)));
    m.properties.insert(0x1000, str(QStringLiteral("Example body %1").arg(i).repeated(i % 7 + 1)));
    m.properties.insert(0x0E07, {Pst::PtypInteger32, le32(quint32(i % 2))});
    QHash<quint32, Pst::PropValue> to;
    to.insert(0x0C150003, {Pst::PtypInteger32, le32(1)});
    to.insert(0x3003001F, str(QStringLiteral("person%1@example.com").arg(i)));
    m.recipients.append(to);
    if (i % 3 == 0) {
        Pst::Attachment a;
        a.properties.insert(0x3705, {Pst::PtypInteger32, le32(1)});
        a.properties.insert(0x3707, str(QStringLiteral("file%1.bin").arg(i)));
        a.properties.insert(0x3701, {Pst::PtypBinary, QByteArray(500 * i + 10, char('a' + i % 26))});
        m.attachments.append(a);
        Pst::Attachment inner;
        inner.properties.insert(0x3705, {Pst::PtypInteger32, le32(5)});
        inner.embedded = std::make_shared<Pst::Message>();
        inner.embedded->properties.insert(0x0037, str(QStringLiteral("Inner")));
        m.attachments.append(inner);
    }
    return m;
}

// Rounds per reader: FUZZ_ROUNDS=n for a long run by hand, a short one in the suite
int rounds(int normal)
{
    const int asked = qEnvironmentVariableIntValue("FUZZ_ROUNDS");
    return asked > 0 ? asked : normal;
}

// Damage: flipped bytes, a cut-off end, or a run of random bytes somewhere
QByteArray mutate(QByteArray data, QRandomGenerator &rng)
{
    switch (rng.bounded(4)) {
    case 0:
        for (int i = 0, n = 1 + int(rng.bounded(8)); i < n; ++i) {
            data[qsizetype(rng.bounded(quint32(data.size())))] = char(rng.bounded(256));
        }
        break;
    case 1:
        data.truncate(qsizetype(rng.bounded(quint32(data.size()))));
        break;
    case 2: {
        const qsizetype at = qsizetype(rng.bounded(quint32(data.size())));
        for (qsizetype i = at; i < std::min(data.size(), at + 64); ++i) {
            data[i] = char(rng.bounded(256));
        }
        break;
    }
    default:
        // A 32-bit field set to a nasty value at a 4-byte boundary
        if (data.size() >= 4) {
            const qsizetype at = qsizetype(rng.bounded(quint32(data.size() / 4))) * 4;
            static const quint32 nasty[] = {0, 1, 0xFFFFFFFF, 0xFFFFFFFE, 0x7FFFFFFF, 0x80000000, 0x10000};
            qToLittleEndian(nasty[rng.bounded(7)], data.data() + at);
        }
        break;
    }
    return data;
}

// Everything a reader can be asked, so damage anywhere is reached
void walkPst(const QString &path)
{
    Pst::PstFile f;
    QString error;
    if (!f.open(path, &error)) {
        return;
    }
    QList<quint32> folders{f.ipmSubtree(), Pst::nidRootFolder};
    QSet<quint32> seen;
    while (!folders.isEmpty() && seen.size() < 200) {
        const quint32 nid = folders.takeFirst();
        if (seen.contains(nid)) {
            continue;
        }
        seen.insert(nid);
        (void)f.folder(nid, &error);
        if (const auto subs = f.subfolders(nid, &error)) {
            for (const Pst::FolderInfo &s : *subs) {
                folders.append(s.nid);
            }
        }
        for (const bool assoc : {false, true}) {
            if (const auto ids = f.messageIds(nid, assoc, &error)) {
                for (const quint32 id : ids->mid(0, 50)) {
                    if (const auto m = f.message(id, &error)) {
                        (void)MapiGraph::toGraph(*m, f.namedProperties());
                        (void)MapiItems::contact(*m, f.namedProperties());
                        (void)MapiItems::event(*m, f.namedProperties());
                        (void)MapiItems::task(*m, f.namedProperties());
                    }
                }
            }
        }
    }
    (void)f.ndb().forEachBlock([](const Pst::BlockEntry &) {});
}
}

class FuzzTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Damaged PSTs are refused or read in part, never a crash or a hang
    void pst()
    {
        QTemporaryDir dir;
        const QString good = dir.filePath(QStringLiteral("good.pst"));
        QString error;
        Pst::PstWriter w;
        QVERIFY(w.create(good, QStringLiteral("Fuzz"), &error));
        const quint32 folder = w.addFolder(Pst::PstWriter::nidIpmSubtree, QStringLiteral("Inbox"));
        const quint32 sub = w.addFolder(folder, QStringLiteral("Sub"));
        for (int i = 0; i < 40; ++i) {
            QVERIFY(w.addMessage(i % 2 ? folder : sub, sampleMessage(i)));
        }
        QVERIFY(w.finish(&error));
        QFile in(good);
        QVERIFY(in.open(QIODevice::ReadOnly));
        const QByteArray bytes = in.readAll();
        walkPst(good);

        QRandomGenerator rng(20261113);
        const QString bad = dir.filePath(QStringLiteral("bad.pst"));
        for (int round = 0; round < rounds(300); ++round) {
            QFile out(bad);
            QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
            out.write(mutate(bytes, rng));
            out.close();
            walkPst(bad);
        }
    }

    // Trees that point back at themselves (an SIBLOCK naming itself, an XXBLOCK below an XXBLOCK)
    // are refused instead of recursing until the stack runs out
    void selfReferences()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("loop.pst"));
        QString error;
        Pst::NdbWriter w;
        QVERIFY(w.create(path, 0, &error));
        QList<Pst::SubnodeEntry> subs;
        for (quint32 i = 1; i <= 400; ++i) {
            subs.append({i << 5 | 0x1F, w.writeData(QByteArray::number(i)), 0});
        }
        const quint64 si = w.writeSubnodes(subs);
        // Over 1021 blocks: an XXBLOCK
        const quint64 tree = w.writeData(QByteArray(1100 * Pst::NdbWriter::maxBlockData, 'x'));
        w.addNode({0x21, w.writeData(QByteArray()), si, 0});
        w.addNode({0x41, tree, 0, 0});
        QVERIFY(w.finish(&error));

        // Point the first child of each tree's top block at the top block itself, CRC fixed
        const auto loop = [&](quint64 bid, int childAt) {
            Pst::Ndb r;
            QVERIFY(r.open(path, &error));
            const Pst::BlockEntry e = *r.block(bid);
            r.close();
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadWrite));
            f.seek(qint64(e.ib));
            QByteArray data = f.read(e.size);
            qToLittleEndian(bid, data.data() + childAt);
            f.seek(qint64(e.ib));
            f.write(data);
            const qint64 trailer = qint64(e.ib) + (e.size + 16 + 63) / 64 * 64 - 16;
            QByteArray crc(4, 0);
            qToLittleEndian(Pst::crc(data.constData(), data.size()), crc.data());
            f.seek(trailer + 4);
            f.write(crc);
        };
        loop(si, 8 + 8);
        loop(tree, 8);
        Pst::Ndb r;
        QVERIFY(r.open(path, &error));
        // Refused for the loop, not for a checksum: the patching kept the blocks valid
        QVERIFY(!r.subnodes(si));
        QVERIFY2(r.error().contains(QLatin1String("damaged subnode block")), qPrintable(r.error()));
        Pst::Ndb r2;
        QVERIFY(r2.open(path, &error));
        QVERIFY(!r2.treeData(tree));
        QVERIFY2(r2.error().contains(QLatin1String("damaged XBLOCK")), qPrintable(r2.error()));
        QVERIFY(!r2.treeBlocks(tree));
    }

    void msgAndCfb()
    {
        Msg::File file;
        file.message = sampleMessage(3);
        Pst::NamedProperty kw;
        kw.guid = Pst::psPublicStrings;
        kw.isString = true;
        kw.name = QStringLiteral("Keywords");
        file.names.insert(0x8000, kw);
        file.message.properties.insert(0x8000, {Pst::PtypString | Pst::PtypMultiple, le32(1) + le32(8) + str(QStringLiteral("Acme")).data});
        const QByteArray bytes = Msg::write(file);
        QString error;
        QVERIFY(Msg::read(bytes, &error));
        QRandomGenerator rng(1113);
        for (int round = 0; round < rounds(2000); ++round) {
            const QByteArray bad = mutate(bytes, rng);
            if (const auto m = Msg::read(bad, &error)) {
                (void)MapiGraph::toGraph(m->message, m->names);
            }
        }
    }

    // Series patterns and time zone definitions from damaged items
    void recurrence()
    {
        const QByteArray sample = QByteArray::fromHex("043004300B2001000000C0210000010000000000000032000000222000000C0000000000000000000000000000008"
                                                      "020BC0C20ADBC0C0630000009300000580200007602000000000000000000000000");
        QRandomGenerator rng(7);
        QStringList notes;
        for (int round = 0; round < rounds(5000); ++round) {
            const QByteArray bad = mutate(sample, rng);
            notes.clear();
            (void)MapiItems::recurrence(bad, QStringLiteral("W. Europe Standard Time"), &notes);
            (void)MapiItems::timeZoneName(bad);
        }
    }

    void rtf()
    {
        const QByteArray sample = QByteArray::fromHex("2d0000002b0000004c5a4675f1c5c7a703000a0072637067313235423"
                                                      "20af32068656c090020627705b06c647d0a800fa0");
        QRandomGenerator rng(42);
        QString error;
        for (int round = 0; round < rounds(3000); ++round) {
            QByteArray bad = mutate(sample, rng);
            // Half the time with a recomputed CRC, so the decompressor itself is reached
            if (round % 2 && bad.size() >= 16) {
                const QByteArray contents = bad.mid(16, qsizetype(qFromLittleEndian<quint32>(bad.constData())) - 12);
                qToLittleEndian(Pst::crc(contents.constData(), contents.size()), bad.data() + 12);
            }
            (void)Rtf::decompress(bad, &error);
            (void)Rtf::deencapsulate(bad);
        }
    }
};

QTEST_GUILESS_MAIN(FuzzTest)
#include "fuzztest.moc"
