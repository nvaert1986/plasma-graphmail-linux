// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QJsonObject>

// Forward Contact > As a vCard (docs/PHASE9B.md 9b.30)
namespace GraphMail::VCard
{

// A Graph contact or directory entry as a vCard 3.0 (RFC 2426): UTF-8, CRLF, folded at 75 octets
QByteArray fromContact(const QJsonObject &contact);

}
