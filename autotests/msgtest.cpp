// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cfb/cfb.h"
#include "msg/msgfile.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

using namespace GraphMail;

namespace
{
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
Pst::PropValue int32(quint32 v)
{
    return {Pst::PtypInteger32, le32(v)};
}

// Runs Python on a file: the .msg checker, or olefile listing a compound file's streams
QString python(const QStringList &args, bool *ran)
{
    const QString py = QStandardPaths::findExecutable(QStringLiteral("python3"));
    *ran = !py.isEmpty();
    if (!*ran) {
        return {};
    }
    QProcess p;
    p.start(py, QStringList{QStringLiteral("-I")} + args);
    p.waitForFinished(120000);
    const QByteArray out = p.readAllStandardOutput() + p.readAllStandardError();
    if (p.exitCode() != 0 && out.contains("No module named 'olefile'")) {
        *ran = false;
        return {};
    }
    // A failing run says so even when it printed nothing
    return p.exitCode() == 0 ? QString::fromUtf8(out).trimmed() : QStringLiteral("exit %1: %2").arg(p.exitCode()).arg(QString::fromUtf8(out));
}

const char *oleList = "import olefile,sys\n"
                      "ole=olefile.OleFileIO(sys.argv[1], raise_defects=olefile.DEFECT_INCORRECT)\n"
                      "for e in sorted(ole.listdir(streams=True, storages=False)):\n"
                      "    print('/'.join(e), ole.get_size(e), ole.openstream(e).read().hex()[:16])\n";

QStringList listing(const Cfb::Entry &e, const QString &prefix)
{
    QStringList out;
    for (const Cfb::Entry &c : e.children) {
        const QString path = prefix.isEmpty() ? c.name : prefix + QLatin1Char('/') + c.name;
        if (c.storage) {
            out += listing(c, path);
        } else {
            out.append(path + QLatin1Char(' ') + QString::number(c.data.size()) + QLatin1Char(' ') + QString::fromLatin1(c.data.left(8).toHex()));
        }
    }
    return out;
}
}

class MsgTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void cfbRoundTrip_data()
    {
        QTest::addColumn<qsizetype>("bigSize");
        QTest::newRow("small file") << qsizetype(100000);
        // Over 109 FAT sectors: the DIFAT holds the rest
        QTest::newRow("with DIFAT") << qsizetype(8) * 1024 * 1024;
    }

    // Storages and streams of every size class come back as written, olefile reads the same
    void cfbRoundTrip()
    {
        QFETCH(qsizetype, bigSize);
        Cfb::Entry root;
        root.storage = true;
        root.addStream(QStringLiteral("empty"), QByteArray());
        root.addStream(QStringLiteral("tiny"), QByteArrayLiteral("x"));
        root.addStream(QStringLiteral("just under the cutoff"), content(1, 4095));
        root.addStream(QStringLiteral("at the cutoff"), content(2, 4096));
        root.addStream(QStringLiteral("big"), content(3, bigSize));
        Cfb::Entry &sub = root.addStorage(QStringLiteral("Storage"));
        for (int i = 0; i < 100; ++i) {
            sub.addStream(QStringLiteral("item %1").arg(i), content(10 + i, 10 * i));
        }
        sub.addStorage(QStringLiteral("nested")).addStream(QStringLiteral("deep"), QByteArrayLiteral("deep data"));
        const QByteArray bytes = Cfb::write(root);
        QString error;
        const std::optional<Cfb::Entry> back = Cfb::read(bytes, &error);
        QVERIFY2(back, qPrintable(error));
        QStringList want = listing(root, {});
        QStringList got = listing(*back, {});
        want.sort();
        got.sort();
        QCOMPARE(got, want);
        QCOMPARE(back->child(QStringLiteral("STORAGE"))->child(QStringLiteral("nested"))->child(QStringLiteral("deep"))->data, QByteArrayLiteral("deep data"));

        QTemporaryDir dir;
        QFile f(dir.filePath(QStringLiteral("test.cfb")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
        f.close();
        bool ran = false;
        QString ole = python({QStringLiteral("-c"), QString::fromLatin1(oleList), f.fileName()}, &ran);
        if (!ran) {
            QSKIP("python3 with olefile not available");
        }
        QStringList oleLines = ole.split(QLatin1Char('\n'));
        oleLines.sort();
        // olefile does not list empty streams the same way: compare the rest
        want.removeIf([](const QString &l) {
            return l.startsWith(QLatin1String("empty "));
        });
        oleLines.removeIf([](const QString &l) {
            return l.startsWith(QLatin1String("empty "));
        });
        QCOMPARE(oleLines, want);
    }

    void names()
    {
        QVERIFY(Cfb::lessThan(QStringLiteral("zz"), QStringLiteral("aaa")));
        QVERIFY(Cfb::lessThan(QStringLiteral("abc"), QStringLiteral("ABD")));
        QVERIFY(!Cfb::lessThan(QStringLiteral("abc"), QStringLiteral("ABC")));
        QString error;
        QVERIFY(!Cfb::read(QByteArrayLiteral("not a compound file at all, just text"), &error));
        QVERIFY(!error.isEmpty());
    }

    // A message with every property kind, recipients, attachments, an attached message and named
    // properties: read back as written and passing the independent checker
    void msgRoundTrip()
    {
        Msg::File file;
        Pst::Message &m = file.message;
        m.properties.insert(0x0037, str(QStringLiteral("Example subject")));
        m.properties.insert(0x001A, str(QStringLiteral("IPM.Note")));
        m.properties.insert(0x1000, str(QStringLiteral("Example body")));
        m.properties.insert(0x1013, {Pst::PtypBinary, QByteArrayLiteral("<p>Example</p>")});
        m.properties.insert(0x0E07, int32(1));
        m.properties.insert(0x0057, {Pst::PtypBoolean, QByteArray(1, 1)});
        m.properties.insert(0x0E06, {Pst::PtypTime, QByteArray::fromHex("00403ef064a1d401")});
        m.properties.insert(0x0E1D, str(QString()));
        m.properties.insert(0x8010, {Pst::PtypString | Pst::PtypMultiple, le32(2) + le32(12) + le32(20) + str(QStringLiteral("Acme")).data + str(QStringLiteral("Red")).data});
        m.properties.insert(0x8011, int32(42));
        m.properties.insert(0x8012, {Pst::PtypGuid, content(4, 16)});
        m.properties.insert(0x8013, {Pst::PtypInteger32 | Pst::PtypMultiple, le32(5) + le32(6)});
        m.properties.insert(0x8014, {Pst::PtypBinary | Pst::PtypMultiple, le32(2) + le32(12) + le32(15) + QByteArrayLiteral("abc") + QByteArrayLiteral("defg")});
        // A named ID without a name is left out
        m.properties.insert(0x8099, int32(7));
        Pst::NamedProperty keywords;
        keywords.guid = Pst::psPublicStrings;
        keywords.isString = true;
        keywords.name = QStringLiteral("Keywords");
        file.names.insert(0x8010, keywords);
        Pst::NamedProperty numbered;
        numbered.guid = QUuid(0x00062008, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
        numbered.id = 0x8503;
        file.names.insert(0x8011, numbered);
        Pst::NamedProperty mapi;
        mapi.guid = Pst::psMapi;
        mapi.id = 0x1234;
        file.names.insert(0x8012, mapi);
        file.names.insert(0x8013, numbered);
        file.names[0x8013].id = 0x8504;
        file.names.insert(0x8014, numbered);
        file.names[0x8014].id = 0x8505;

        QHash<quint32, Pst::PropValue> to;
        to.insert(0x0C150003, int32(1));
        to.insert(0x3001001F, str(QStringLiteral("Bob Example")));
        to.insert(0x3003001F, str(QStringLiteral("bob@example.com")));
        to.insert(0x67F20003, int32(0));
        m.recipients = {to, to};
        Pst::Attachment small;
        small.properties.insert(0x3705, int32(1));
        small.properties.insert(0x3707, str(QStringLiteral("notes.txt")));
        small.properties.insert(0x3701, {Pst::PtypBinary, QByteArrayLiteral("Example attachment text")});
        Pst::Attachment large;
        large.properties.insert(0x3705, int32(1));
        large.properties.insert(0x3707, str(QStringLiteral("drawing.bin")));
        large.properties.insert(0x3701, {Pst::PtypBinary, content(5, 300000)});
        Pst::Message inner;
        inner.properties.insert(0x0037, str(QStringLiteral("Attached example")));
        inner.properties.insert(0x8011, int32(43));
        inner.attachments.append(small);
        inner.recipients.append(to);
        Pst::Attachment attached;
        attached.properties.insert(0x3705, int32(5));
        attached.properties.insert(0x3001, str(QStringLiteral("Attached example")));
        attached.embedded = std::make_shared<Pst::Message>(inner);
        m.attachments = {small, large, attached};

        const QByteArray bytes = Msg::write(file);
        QString error;
        const std::optional<Msg::File> back = Msg::read(bytes, &error);
        QVERIFY2(back, qPrintable(error));
        const Pst::Message &b = back->message;
        QCOMPARE(b.string(0x0037), QStringLiteral("Example subject"));
        QCOMPARE(b.properties.value(0x1013).data, QByteArrayLiteral("<p>Example</p>"));
        QVERIFY(b.properties.value(0x0057).toBool());
        QCOMPARE(b.properties.value(0x0E06).toTime(), QDateTime(QDate(2019, 1, 1), QTime(0, 0), QTimeZone::UTC));
        QVERIFY(!b.properties.contains(0x0E1D));
        QCOMPARE(b.properties.value(0x340D).toInt(), 0x40000);
        QCOMPARE(b.recipients.size(), 2);
        QCOMPARE(b.recipients.at(1).value(0x3003001F).toString(), QStringLiteral("bob@example.com"));
        QVERIFY(!b.recipients.at(0).contains(0x67F20003));
        QCOMPARE(b.recipients.at(1).value(0x30000003).toInt(), 1);
        QCOMPARE(b.attachments.size(), 3);
        QCOMPARE(b.attachments.at(1).properties.value(0x3701).data, content(5, 300000));
        QVERIFY(b.attachments.at(2).embedded);
        QCOMPARE(b.attachments.at(2).embedded->string(0x0037), QStringLiteral("Attached example"));
        QCOMPARE(b.attachments.at(2).embedded->attachments.size(), 1);
        QCOMPARE(b.attachments.at(2).embedded->recipients.size(), 1);

        // Named properties renumbered from 0x8000 in order, the nameless one gone
        QCOMPARE(back->names.size(), 5);
        quint16 kw = 0;
        quint16 num = 0;
        for (auto it = back->names.cbegin(); it != back->names.cend(); ++it) {
            if (it->isString && it->name == QLatin1String("Keywords")) {
                kw = it.key();
            } else if (!it->isString && it->id == 0x8503) {
                num = it.key();
            }
        }
        QVERIFY(kw >= 0x8000 && num >= 0x8000);
        QCOMPARE(b.properties.value(kw).items().size(), 2);
        QCOMPARE(b.properties.value(num).toInt(), 42);
        QCOMPARE(b.attachments.at(2).embedded->properties.value(num).toInt(), 43);
        bool mvBinary = false;
        for (auto it = b.properties.cbegin(); it != b.properties.cend(); ++it) {
            if (it->type == (Pst::PtypBinary | Pst::PtypMultiple)) {
                mvBinary = true;
                QCOMPARE(it->items(), (QList<QByteArray>{QByteArrayLiteral("abc"), QByteArrayLiteral("defg")}));
            }
        }
        QVERIFY(mvBinary);
        QCOMPARE(b.properties.size(), m.properties.size() - 2 + 1);

        QTemporaryDir dir;
        QFile f(dir.filePath(QStringLiteral("example.msg")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
        f.close();
        if (const QString keep = qEnvironmentVariable("MSG_KEEP_DIR"); !keep.isEmpty()) {
            QFile::remove(keep + QStringLiteral("/example.msg"));
            QFile::copy(f.fileName(), keep + QStringLiteral("/example.msg"));
        }
        const QString checker = QStringLiteral(MSG_CHECKER);
        if (!QFile::exists(checker)) {
            QSKIP("msgcheck.py not available");
        }
        bool ran = false;
        const QString problems = python({checker, f.fileName()}, &ran);
        if (!ran) {
            QSKIP("python3 with olefile not available");
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems));
    }
};

QTEST_GUILESS_MAIN(MsgTest)
#include "msgtest.moc"
