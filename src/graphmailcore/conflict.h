// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QStringList>

namespace GraphMail
{

// A change made here against an item that was changed elsewhere meanwhile (Graph answered 412 to
// If-Match, docs/PHASE8.md). previous: the item as the change was made on; server: the item now.
// The fields the user changed that the other side changed too: empty means the user's change can
// simply go onto the server's version (a merge).
QStringList conflictingFields(const QJsonObject &changes, const QJsonObject &previous, const QJsonObject &server);

// The If-Match value for a change made on this version of an item; empty when unknown
QByteArray ifMatch(const QJsonObject &previous);

}
