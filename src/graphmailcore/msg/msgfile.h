// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pst/pstmessaging.h"

// Outlook .msg files ([MS-OXMSG], docs/PHASE9B.md): one message in a compound file, with its
// recipients, attachments, attached messages and named properties. The same property model as the
// PST code, so the conversions (MIME, Graph) serve both.
namespace GraphMail::Msg
{

struct File {
    Pst::Message message;
    // Named properties by property ID (0x8000 and up)
    QHash<quint16, Pst::NamedProperty> names;
};

std::optional<File> read(const QByteArray &bytes, QString *error);
// A Unicode .msg; property IDs of 0x8000 and up must be in names
QByteArray write(const File &file);

}
