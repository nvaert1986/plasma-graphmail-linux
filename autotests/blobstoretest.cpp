// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "blobstore.h"

#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

using namespace GraphMail;

class BlobStoreTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir dir;

private Q_SLOTS:
    void roundTripAndCompression()
    {
        BlobStore store(dir.path());
        const QByteArray body = QByteArray("<html><body><p>Hello, this is a mail body.</p></body></html>\n").repeated(2000);
        QString error;
        const QString id = store.put(body, &error);
        QVERIFY2(BlobStore::isValidId(id), qPrintable(error));
        QVERIFY(store.contains(id));
        // Repetitive HTML compresses very well
        QVERIFY(store.storedSize(id) < body.size() / 20);
        QCOMPARE(store.get(id, &error).value(), body);
        QVERIFY(QFile::exists(dir.path() + QLatin1Char('/') + id.left(2) + QLatin1Char('/') + id + QStringLiteral(".zst")));
    }

    void emptyAndBinary()
    {
        BlobStore store(dir.path());
        QString error;
        const QString empty = store.put({}, &error);
        QVERIFY2(!empty.isEmpty(), qPrintable(error));
        QCOMPARE(store.get(empty, &error).value(), QByteArray());

        // Random bytes (an image or a zip attachment) do not compress but must survive unchanged
        QByteArray binary(5 * 1024 * 1024, Qt::Uninitialized);
        QRandomGenerator::global()->fillRange(reinterpret_cast<quint32 *>(binary.data()), binary.size() / 4);
        const QString id = store.put(binary, &error);
        QCOMPARE(store.get(id, &error).value(), binary);
    }

    void idsAreUniqueAndListed()
    {
        QTemporaryDir own;
        BlobStore store(own.path());
        QString error;
        QStringList ids;
        for (int i = 0; i < 50; ++i) {
            ids.append(store.put(QByteArray::number(i), &error));
        }
        QCOMPARE(QSet<QString>(ids.cbegin(), ids.cend()).size(), 50);
        QStringList listed = store.allIds();
        listed.sort();
        ids.sort();
        QCOMPARE(listed, ids);

        QVERIFY(store.remove(ids.first()));
        QVERIFY(!store.contains(ids.first()));
        QVERIFY(!store.remove(ids.first()));
        QCOMPARE(store.allIds().size(), 49);
    }

    void refusesBadIds()
    {
        BlobStore store(dir.path());
        QString error;
        for (const QString &bad : {QStringLiteral("../../etc/passwd"), QStringLiteral("ABCDEF0123456789abcdef0123456789"), QString(),
                                   QStringLiteral("0123456789abcdef0123456789abcde")}) {
            QVERIFY(!BlobStore::isValidId(bad));
            QVERIFY(!store.get(bad, &error).has_value());
            QVERIFY(!store.remove(bad));
            QCOMPARE(store.storedSize(bad), -1);
        }
        QVERIFY(!store.get(QStringLiteral("0123456789abcdef0123456789abcdef"), &error).has_value());
        QVERIFY(error.contains(QLatin1String("missing")));
    }

    void damagedBlobIsReported()
    {
        BlobStore store(dir.path());
        QString error;
        const QString id = store.put(QByteArray("some body").repeated(100), &error);
        QFile file(dir.path() + QLatin1Char('/') + id.left(2) + QLatin1Char('/') + id + QStringLiteral(".zst"));
        QVERIFY(file.open(QIODevice::ReadWrite));
        const QByteArray content = file.readAll();
        file.seek(0);
        file.write(content.left(content.size() / 2)); // a truncated write
        file.resize(content.size() / 2);
        file.close();
        QVERIFY(!store.get(id, &error).has_value());
        QVERIFY(error.contains(QLatin1String("damaged")));
    }
};

QTEST_GUILESS_MAIN(BlobStoreTest)

#include "blobstoretest.moc"
