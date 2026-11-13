// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QtGlobal>

// The algorithms of [MS-PST] section 5 (docs/PST-NOTES.md "Algorithms")
namespace GraphMail::Pst
{
// The 32-bit CRC of pages, blocks and the header (seed 0)
quint32 crc(const char *data, qsizetype size, quint32 seed = 0);
inline quint32 crc(const QByteArray &data)
{
    return crc(data.constData(), data.size());
}
// Byte by byte, as a check of crc() in the tests
quint32 crcBytewise(const char *data, qsizetype size, quint32 seed = 0);

// NDB_CRYPT_PERMUTE, in place
void permute(char *data, qsizetype size, bool encode);
// NDB_CRYPT_CYCLIC, in place; key = the low 32 bits of the block's BID; symmetric
void cyclic(char *data, qsizetype size, quint32 key);

// The signature of a page or block at this offset with this BID
quint16 signature(quint64 ib, quint64 bid);
}
