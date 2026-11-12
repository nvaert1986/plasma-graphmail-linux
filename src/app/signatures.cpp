// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "signatures.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace GraphMail
{

namespace
{
KSharedConfig::Ptr config()
{
    KSharedConfig::Ptr c = KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals);
    // The daemon writes to the same file
    c->reparseConfiguration();
    return c;
}

KConfigGroup mailboxGroup(const KSharedConfig::Ptr &c, const QString &account, const QString &mailbox)
{
    return c->group(QStringLiteral("Account ") + account).group(QStringLiteral("Mailbox ") + mailbox);
}

Signature read(const KConfigGroup &group, const QString &id)
{
    Signature s;
    s.id = id;
    s.name = group.readEntry("Name");
    s.html = group.readEntry("Html");
    const QJsonArray images = QJsonDocument::fromJson(group.readEntry("Images").toUtf8()).array();
    for (const QJsonValue &v : images) {
        const QJsonObject o = v.toObject();
        s.images.append({o.value(QLatin1String("name")).toString(), QByteArray::fromBase64(o.value(QLatin1String("png")).toString().toLatin1())});
    }
    return s;
}
}

QList<Signature> Signatures::all()
{
    const KSharedConfig::Ptr c = config();
    QList<Signature> result;
    const QStringList order = c->group(QStringLiteral("Signatures")).readEntry("Order", QStringList());
    for (const QString &id : order) {
        const KConfigGroup group = c->group(QStringLiteral("Signature ") + id);
        if (group.exists()) {
            result.append(read(group, id));
        }
    }
    return result;
}

std::optional<Signature> Signatures::find(const QString &id)
{
    if (id.isEmpty()) {
        return std::nullopt;
    }
    const KSharedConfig::Ptr c = config();
    const KConfigGroup group = c->group(QStringLiteral("Signature ") + id);
    return group.exists() ? std::optional<Signature>(read(group, id)) : std::nullopt;
}

void Signatures::save(const QList<Signature> &signatures)
{
    const KSharedConfig::Ptr c = config();
    // Signatures that were deleted
    const QStringList old = c->group(QStringLiteral("Signatures")).readEntry("Order", QStringList());
    for (const QString &id : old) {
        c->deleteGroup(QStringLiteral("Signature ") + id);
    }
    QStringList order;
    for (const Signature &s : signatures) {
        KConfigGroup group = c->group(QStringLiteral("Signature ") + s.id);
        group.writeEntry("Name", s.name);
        group.writeEntry("Html", s.html);
        QJsonArray images;
        for (const Signature::Image &image : s.images) {
            images.append(QJsonObject{{QStringLiteral("name"), image.name}, {QStringLiteral("png"), QString::fromLatin1(image.png.toBase64())}});
        }
        group.writeEntry("Images", QString::fromUtf8(QJsonDocument(images).toJson(QJsonDocument::Compact)));
        order.append(s.id);
    }
    c->group(QStringLiteral("Signatures")).writeEntry("Order", order);
    c->sync();
}

QString Signatures::defaultFor(const QString &account, const QString &mailbox, bool reply)
{
    const QString id = mailboxGroup(config(), account, mailbox).readEntry(reply ? "SignatureReply" : "SignatureNew");
    return find(id) ? id : QString();
}

void Signatures::setDefault(const QString &account, const QString &mailbox, bool reply, const QString &id)
{
    const KSharedConfig::Ptr c = config();
    KConfigGroup group = mailboxGroup(c, account, mailbox);
    group.writeEntry(reply ? "SignatureReply" : "SignatureNew", id);
    c->sync();
}

}
