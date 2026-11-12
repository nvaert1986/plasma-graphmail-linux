// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "blobstore.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>

#include <zstd.h>

namespace GraphMail
{

namespace
{
// Fast, and still about as small as gzip -9 for mail bodies
constexpr int compressionLevel = 3;
// Graph allows attachments up to 150 MB; anything claiming far more is a damaged file
constexpr unsigned long long maxBlobSize = 1024ULL * 1024 * 1024;
constexpr QLatin1StringView suffix(".zst");
}

BlobStore::BlobStore(const QString &root)
    : m_root(root)
{
}

QString BlobStore::root() const
{
    return m_root;
}

bool BlobStore::isValidId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
    return pattern.match(id).hasMatch();
}

QString BlobStore::pathFor(const QString &id) const
{
    return m_root + QLatin1Char('/') + id.left(2) + QLatin1Char('/') + id + suffix;
}

QString BlobStore::put(const QByteArray &data, QString *error)
{
    quint32 random[4];
    QRandomGenerator::system()->fillRange(random);
    const QString id = QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(random), sizeof(random)).toHex());

    QByteArray compressed(qsizetype(ZSTD_compressBound(size_t(data.size()))), Qt::Uninitialized);
    const size_t written = ZSTD_compress(compressed.data(), size_t(compressed.size()), data.constData(), size_t(data.size()), compressionLevel);
    if (ZSTD_isError(written)) {
        *error = QStringLiteral("compression failed: ") + QString::fromLatin1(ZSTD_getErrorName(written));
        return {};
    }
    compressed.truncate(qsizetype(written));

    const QString path = pathFor(id);
    if (!QDir().mkpath(QFileInfo(path).path())) {
        *error = QStringLiteral("cannot create %1").arg(QFileInfo(path).path());
        return {};
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(compressed) != compressed.size() || !file.commit()) {
        *error = QStringLiteral("cannot write %1: %2").arg(path, file.errorString());
        return {};
    }
    return id;
}

std::optional<QByteArray> BlobStore::get(const QString &id, QString *error) const
{
    if (!isValidId(id)) {
        *error = QStringLiteral("invalid blob id");
        return std::nullopt;
    }
    QFile file(pathFor(id));
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("blob %1 missing: %2").arg(id, file.errorString());
        return std::nullopt;
    }
    const QByteArray compressed = file.readAll();
    const unsigned long long size = ZSTD_getFrameContentSize(compressed.constData(), size_t(compressed.size()));
    // put() always records the size in the frame; unknown or absurd sizes mean a damaged file
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > maxBlobSize) {
        *error = QStringLiteral("blob %1 is damaged").arg(id);
        return std::nullopt;
    }
    QByteArray data(qsizetype(size), Qt::Uninitialized);
    const size_t read = ZSTD_decompress(data.data(), size_t(data.size()), compressed.constData(), size_t(compressed.size()));
    if (ZSTD_isError(read) || read != size) {
        *error = QStringLiteral("blob %1 is damaged: %2").arg(id, QString::fromLatin1(ZSTD_isError(read) ? ZSTD_getErrorName(read) : "size mismatch"));
        return std::nullopt;
    }
    return data;
}

bool BlobStore::remove(const QString &id)
{
    return isValidId(id) && QFile::remove(pathFor(id));
}

bool BlobStore::contains(const QString &id) const
{
    return isValidId(id) && QFileInfo::exists(pathFor(id));
}

qint64 BlobStore::storedSize(const QString &id) const
{
    if (!isValidId(id)) {
        return -1;
    }
    const QFileInfo info(pathFor(id));
    return info.exists() ? info.size() : -1;
}

QStringList BlobStore::allIds() const
{
    QStringList ids;
    QDirIterator it(m_root, {QStringLiteral("*.zst")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString id = it.nextFileInfo().completeBaseName();
        if (isValidId(id)) {
            ids.append(id);
        }
    }
    return ids;
}

}
