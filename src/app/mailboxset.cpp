// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mailboxset.h"
#include "accounts.h"
#include "mailboxworker.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QFileInfo>

namespace GraphMail
{

QString MailboxInfo::label() const
{
    if (own) {
        return address.isEmpty() ? displayName : address;
    }
    return displayName.isEmpty() ? (address.isEmpty() ? mailbox : address) : displayName;
}

MailboxSet::MailboxSet(QObject *parent)
    : QObject(parent)
{
}

MailboxSet::~MailboxSet() = default;

void MailboxSet::reload()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals);
    config->reparseConfiguration();

    QList<MailboxInfo> wanted;
    const QList<GraphCore::Account> accounts = GraphCore::Accounts::all();
    for (const GraphCore::Account &account : accounts) {
        wanted.append({account.id, QStringLiteral("me"), account.displayName, account.userPrincipalName, true});
        const KConfigGroup group = config->group(QStringLiteral("Account ") + account.id);
        const QStringList shared = group.readEntry("SharedMailboxes", QStringList());
        for (const QString &mailbox : shared) {
            const KConfigGroup names = group.group(QStringLiteral("Mailbox ") + mailbox);
            MailboxInfo info{account.id, mailbox, names.readEntry("Name"), names.readEntry("Address"), false};
            // Mailboxes added before phase 7 have no Access entry: everything
            const QStringList access = names.readEntry("Access", QStringList());
            if (!access.isEmpty()) {
                info.mail = access.contains(QLatin1String("mail"));
                info.calendar = access.contains(QLatin1String("calendar"));
                info.contacts = access.contains(QLatin1String("contacts"));
            }
            wanted.append(info);
        }
    }

    bool changedAny = false;
    // Drop mailboxes that are gone
    for (auto it = m_open.begin(); it != m_open.end();) {
        const bool keep = std::any_of(wanted.cbegin(), wanted.cend(), [&](const MailboxInfo &m) {
            return m.key() == (*it)->info.key();
        });
        if (keep) {
            ++it;
        } else {
            it = m_open.erase(it);
            changedAny = true;
        }
    }
    for (const MailboxInfo &info : std::as_const(wanted)) {
        auto existing = std::find_if(m_open.begin(), m_open.end(), [&](const std::unique_ptr<Open> &o) {
            return o->info.key() == info.key();
        });
        if (existing == m_open.end()) {
            m_open.push_back(std::make_unique<Open>());
            existing = std::prev(m_open.end());
            changedAny = true;
        }
        Open &open = **existing;
        if (open.info.label() != info.label() || open.info.mail != info.mail || open.info.calendar != info.calendar || open.info.contacts != info.contacts) {
            changedAny = true;
        }
        open.info = info;
        if (!open.store) {
            const QString dir = cacheDirectory(info.account, info.mailbox);
            if (QFileInfo::exists(dir + QStringLiteral("/cache.db"))) {
                auto store = std::make_unique<Store>();
                QString error;
                if (store->open(dir + QStringLiteral("/cache.db"), Store::Mode::ReadOnly, &error)) {
                    open.store = std::move(store);
                    open.blobs = std::make_unique<BlobStore>(dir + QStringLiteral("/blobs"));
                    changedAny = true;
                }
            }
        }
    }
    // The primary account first: graphmailrc [General] PrimaryAccount, else the account with the most
    // mail (the one the user works in; a second account is often a test or admin one). New mail,
    // events and tasks start in it, and it heads the folder pane.
    QString primary = config->group(QStringLiteral("General")).readEntry("PrimaryAccount");
    if (primary.isEmpty()) {
        qint64 most = -1;
        for (const auto &open : m_open) {
            const qint64 count = open->info.own && open->store ? open->store->messageCount() : -1;
            if (count > most) {
                most = count;
                primary = open->info.account;
            }
        }
    }
    m_primary = primary;
    // Then the other own mailboxes, then shared ones by name (the folder pane's order)
    std::stable_sort(m_open.begin(), m_open.end(), [&primary](const std::unique_ptr<Open> &a, const std::unique_ptr<Open> &b) {
        if (a->info.own != b->info.own) {
            return a->info.own;
        }
        const bool aPrimary = a->info.account == primary;
        const bool bPrimary = b->info.account == primary;
        if (a->info.own && aPrimary != bPrimary) {
            return aPrimary;
        }
        return a->info.label().localeAwareCompare(b->info.label()) < 0;
    });
    if (changedAny) {
        Q_EMIT changed();
    }
}

QString MailboxSet::primaryAccount() const
{
    return m_primary;
}

QList<MailboxInfo> MailboxSet::mailboxes() const
{
    QList<MailboxInfo> result;
    for (const auto &open : m_open) {
        result.append(open->info);
    }
    return result;
}

const MailboxSet::Open *MailboxSet::find(const QString &key) const
{
    for (const auto &open : m_open) {
        if (open->info.key() == key) {
            return open.get();
        }
    }
    return nullptr;
}

std::optional<MailboxInfo> MailboxSet::mailbox(const QString &key) const
{
    const Open *open = find(key);
    return open ? std::optional<MailboxInfo>(open->info) : std::nullopt;
}

Store *MailboxSet::store(const QString &key) const
{
    const Open *open = find(key);
    return open ? open->store.get() : nullptr;
}

BlobStore *MailboxSet::blobs(const QString &key) const
{
    const Open *open = find(key);
    return open ? open->blobs.get() : nullptr;
}

QByteArray MailboxSet::inlinePicture(const QString &contentId) const
{
    for (const MailboxInfo &box : mailboxes()) {
        Store *s = store(box.key());
        BlobStore *b = blobs(box.key());
        const QString blob = s && b ? s->blobOfContentId(contentId) : QString();
        if (!blob.isEmpty()) {
            QString error;
            if (const std::optional<QByteArray> data = b->get(blob, &error)) {
                return *data;
            }
        }
    }
    return {};
}

QImage MailboxSet::photo(const QString &key) const
{
    return m_photos.value(key);
}

void MailboxSet::setPhoto(const QString &key, const QImage &photo)
{
    m_photos.insert(key, photo);
    Q_EMIT photoChanged(key);
}

}
