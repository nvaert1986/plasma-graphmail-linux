// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

namespace GraphMail
{

// Message bodies, MIME, inline images and attachments, kept as zstd-compressed files next to the
// cache database: <root>/<first two hex digits>/<id>.zst. The database only stores the ids, which
// keeps it small for 30-40 GB mailboxes (docs/PHASE2.md section 9). Writes are atomic (temporary
// file + rename), so a crash never leaves half a blob under a valid id. Used from one thread per
// mailbox; reading from another process (the UI) is safe.
class BlobStore
{
public:
    explicit BlobStore(const QString &root);

    QString root() const;

    // Compresses and stores data; returns the new id, empty on failure
    QString put(const QByteArray &data, QString *error);
    std::optional<QByteArray> get(const QString &id, QString *error) const;
    bool remove(const QString &id);
    bool contains(const QString &id) const;
    // Bytes on disk (compressed); -1 when absent
    qint64 storedSize(const QString &id) const;
    // Every id on disk: with the ids the database still references, this finds orphans (left
    // behind when a folder was deleted with all its messages)
    QStringList allIds() const;

    // 32 lower-case hex digits; anything else is refused, so an id can never point outside root
    static bool isValidId(const QString &id);

private:
    QString pathFor(const QString &id) const;

    QString m_root;
};

}
