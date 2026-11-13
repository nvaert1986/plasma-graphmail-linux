// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pst/pstalgorithms.h"
#include "pst/psttables.h"
#include "pst/pstndb.h"
#include "pstsamples.h"

#include <QRandomGenerator>
#include <QTest>

using namespace GraphMail;

class PstAlgorithmsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // The tables as the specification gives them: R, S and I permutations, I the inverse of R
    void tables()
    {
        for (int t = 0; t < 3; ++t) {
            QList<bool> seen(256, false);
            for (int i = 0; i < 256; ++i) {
                seen[Pst::Tables::crypt[t * 256 + i]] = true;
            }
            QVERIFY(!seen.contains(false));
        }
        for (int b = 0; b < 256; ++b) {
            QCOMPARE(Pst::Tables::crypt[512 + Pst::Tables::crypt[b]], b);
        }
        // The first table is the common CRC-32 table (the PST CRC has seed 0 and no final inversion)
        QCOMPARE(Pst::Tables::crc[0][1], 0x77073096u);
        QCOMPARE(Pst::Tables::crc[0][255], 0x2D02EF8Du);
        QCOMPARE(Pst::Tables::crc[7][255], 0x264B06E6u);
    }

    // Eight bytes at a time gives the same CRC as byte by byte, for every length and start
    void crcSlicing()
    {
        QByteArray data(4100, Qt::Uninitialized);
        for (char &c : data) {
            c = char(QRandomGenerator::global()->bounded(256));
        }
        for (int start = 0; start < 9; ++start) {
            for (int length : {0, 1, 3, 7, 8, 9, 15, 16, 471, 516, 4000}) {
                QCOMPARE(Pst::crc(data.constData() + start, length), Pst::crcBytewise(data.constData() + start, length));
            }
        }
        QCOMPARE(Pst::crc(nullptr, 0), 0u);
    }

    void permuteRoundTrip()
    {
        const QByteArray original("A Personal Folders file, encoded with the permutation.");
        QByteArray data = original;
        Pst::permute(data.data(), data.size(), true);
        QVERIFY(data != original);
        Pst::permute(data.data(), data.size(), false);
        QCOMPARE(data, original);
    }

    // Cyclic is its own inverse with the same key
    void cyclicRoundTrip()
    {
        const QByteArray original("Encoded with the cyclic cipher; the key is the block's BID.");
        QByteArray data = original;
        Pst::cyclic(data.data(), data.size(), 0x00012344u);
        QVERIFY(data != original);
        Pst::cyclic(data.data(), data.size(), 0x00012344u);
        QCOMPARE(data, original);
    }

    // The sample header of [MS-PST] 3.2: the fields at their offsets and both checksums
    void sampleHeader()
    {
        QString error;
        const std::optional<Pst::Header> h = Pst::Header::parse(pstSample_header(), &error);
        QVERIFY2(h, qPrintable(error));
        QVERIFY(h->unicode);
        QCOMPARE(h->version, quint16(23));
        QCOMPARE(h->clientVersion, quint16(19));
        QCOMPARE(h->crcPartial, 0x379AA90Eu);
        QVERIFY(h->crcPartialValid);
        QCOMPARE(h->crcFull, 0x1FD283D6u);
        QVERIFY(h->crcFullValid);
        QCOMPARE(h->nextPageBid, quint64(0x254));
        QCOMPARE(h->unique, 0x45u);
        QCOMPARE(h->nids[4], 0x10002u);
        QCOMPARE(h->nids[31], 0x40Fu);
        QCOMPARE(h->fileEof, quint64(0x9F2400));
        QCOMPARE(h->amapLast, quint64(0x9B4400));
        QCOMPARE(h->amapFree, quint64(0x12F240));
        QCOMPARE(h->nbt.bid, quint64(0x24B));
        QCOMPARE(h->nbt.ib, quint64(0x905200));
        QCOMPARE(h->bbt.bid, quint64(0x253));
        QCOMPARE(h->bbt.ib, quint64(0x900A00));
        QCOMPARE(h->amapValid, quint8(2));
        QCOMPARE(h->cryptMethod, quint8(1));
        QCOMPARE(h->nextBid, quint64(0x1434));
        // One changed byte: the checksums no longer match
        QByteArray damaged = pstSample_header();
        damaged[100] = char(damaged[100] ^ 1);
        const std::optional<Pst::Header> d = Pst::Header::parse(damaged, &error);
        QVERIFY(d && !d->crcPartialValid && !d->crcFullValid);
        QVERIFY(!Pst::Header::parse(QByteArray(600, 'x'), &error));
    }

    void signature()
    {
        // ib ^ bid = 0x12345678 -> 0x1234 ^ 0x5678
        QCOMPARE(Pst::signature(0x12345678, 0), quint16(0x1234 ^ 0x5678));
        QCOMPARE(Pst::signature(0x5000, 0x5000), quint16(0));
    }
};

QTEST_GUILESS_MAIN(PstAlgorithmsTest)

#include "pstalgorithmstest.moc"
