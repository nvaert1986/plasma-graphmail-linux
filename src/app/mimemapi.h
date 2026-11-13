// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pst/pstmessaging.h"

#include <QStringList>

// From MIME to MAPI properties (PST and .msg export, docs/PHASE9B.md): subject, sender,
// recipients, dates, Internet headers and IDs, the HTML and text bodies, attachments (inline
// images by content ID) and attached messages. Property IDs from [MS-OXPROPS].
namespace GraphMail::MimeMapi
{

// What the cache knows beyond the MIME
struct Extras {
    bool read = true;
    // 0 not flagged, 1 flagged, 2 complete (as the store keeps it)
    int flag = 0;
    // 0 low, 1 normal, 2 high
    int importance = 1;
    QStringList categories;
    // Delivery time, ms since 1970; 0: the Date header
    qint64 received = 0;
};

// keywords: the property ID of the named property Keywords in the target file, 0 for none
Pst::Message fromMime(const QByteArray &mime, const Extras &extras, quint16 keywords);

// The other way (opening a .msg, docs/PHASE9B.md): MIME rebuilt from the properties, for the
// message viewer. Headers from the properties; bodies; attachments, inline images by Content-ID;
// attached messages as message/rfc822.
QByteArray toMime(const Pst::Message &message);

// A .msg file as an .eml file beside the given path (for the message viewer); false with the reason
bool msgToEml(const QString &msgPath, const QString &emlPath, QString *error);
// A message's MIME as a .msg file's bytes; categories as the named property Keywords
QByteArray mimeToMsg(const QByteArray &mime, const Extras &extras);

}
