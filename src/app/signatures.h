// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace GraphMail
{

// A signature the user made in Graph Mail (the company may also add one centrally while the mail
// travels, so having none is fine). HTML as the editor writes it; its pictures are kept as PNG
// under the names the HTML uses.
struct Signature {
    QString id;
    QString name;
    QString html;
    struct Image {
        QString name;
        QByteArray png;
    };
    QList<Image> images;
};

// graphmailrc: [Signature <id>] Name, Html, Images; [Signatures] Order; per mailbox
// [Account <id>][Mailbox <key>] SignatureNew and SignatureReply (an id, or empty for none)
namespace Signatures
{
QList<Signature> all();
std::optional<Signature> find(const QString &id);
void save(const QList<Signature> &signatures);
// reply: the one for replies and forwards, otherwise the one for new messages
QString defaultFor(const QString &account, const QString &mailbox, bool reply);
void setDefault(const QString &account, const QString &mailbox, bool reply, const QString &id);
}

}
