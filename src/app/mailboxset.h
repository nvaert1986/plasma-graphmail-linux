// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "blobstore.h"
#include "store.h"

#include <QObject>
#include <QImage>
#include <QHash>

#include <memory>
#include <vector>

namespace GraphMail
{

struct MailboxInfo {
    QString account;
    // "me" or the object id of a shared mailbox
    QString mailbox;
    QString displayName;
    QString address;
    bool own = false;
    // What the mailbox gives (graphmailrc Access, docs/PHASE7.md); the own mailbox has everything
    bool mail = true;
    bool calendar = true;
    bool contacts = true;

    QString key() const
    {
        return account + QLatin1Char('/') + mailbox;
    }
    // What the folder pane shows: the address for the own mailbox (like Outlook), the name for
    // shared ones
    QString label() const;
};

// The mailboxes the daemon syncs, read from graphaccountsrc and graphmailrc (so the app also
// shows the cached mail while the daemon is not running), each with its cache opened read-only.
// The app never writes to a cache: changes go to the daemon.
class MailboxSet : public QObject
{
    Q_OBJECT

public:
    explicit MailboxSet(QObject *parent = nullptr);
    ~MailboxSet() override;

    // Reads the configuration again and opens caches that appeared since (a first sync)
    void reload();
    QList<MailboxInfo> mailboxes() const;
    std::optional<MailboxInfo> mailbox(const QString &key) const;
    // nullptr while the mailbox has no cache yet
    Store *store(const QString &key) const;
    BlobStore *blobs(const QString &key) const;
    // An inline picture by its Content-ID from any mailbox's cache (empty when none has it)
    QByteArray inlinePicture(const QString &contentId) const;
    // The account Graph Mail starts in (see reload())
    QString primaryAccount() const;
    // The mailbox's photo (docs/PHASE9.md), null while unknown or when it has none
    QImage photo(const QString &key) const;
    void setPhoto(const QString &key, const QImage &photo);

Q_SIGNALS:
    void changed();
    void photoChanged(const QString &key);

private:
    struct Open {
        MailboxInfo info;
        std::unique_ptr<Store> store;
        std::unique_ptr<BlobStore> blobs;
    };
    const Open *find(const QString &key) const;
    QString m_primary;
    QHash<QString, QImage> m_photos;

    std::vector<std::unique_ptr<Open>> m_open;
};

}
