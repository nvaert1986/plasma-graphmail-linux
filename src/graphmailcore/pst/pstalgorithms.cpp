// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pstalgorithms.h"
#include "psttables.h"

#include <QtEndian>

namespace GraphMail::Pst
{

namespace
{
const quint8 *const R = Tables::crypt;
const quint8 *const S = Tables::crypt + 256;
const quint8 *const I = Tables::crypt + 512;
}

quint32 crcBytewise(const char *data, qsizetype size, quint32 seed)
{
    quint32 value = seed;
    const auto *bytes = reinterpret_cast<const quint8 *>(data);
    for (qsizetype i = 0; i < size; ++i) {
        value = Tables::crc[0][(value ^ bytes[i]) & 0xFF] ^ (value >> 8);
    }
    return value;
}

quint32 crc(const char *data, qsizetype size, quint32 seed)
{
    // Eight bytes per step with the eight tables (the specification's slicing); the rest bytewise
    quint32 value = seed;
    const auto *bytes = reinterpret_cast<const quint8 *>(data);
    qsizetype i = 0;
    for (; size - i >= 8; i += 8) {
        value ^= qFromLittleEndian<quint32>(bytes + i);
        const quint32 second = qFromLittleEndian<quint32>(bytes + i + 4);
        value = Tables::crc[7][value & 0xFF] ^ Tables::crc[6][(value >> 8) & 0xFF] ^ Tables::crc[5][(value >> 16) & 0xFF] ^ Tables::crc[4][(value >> 24) & 0xFF]
            ^ Tables::crc[3][second & 0xFF] ^ Tables::crc[2][(second >> 8) & 0xFF] ^ Tables::crc[1][(second >> 16) & 0xFF] ^ Tables::crc[0][(second >> 24) & 0xFF];
    }
    return crcBytewise(data + i, size - i, value);
}

void permute(char *data, qsizetype size, bool encode)
{
    const quint8 *table = encode ? R : I;
    auto *bytes = reinterpret_cast<quint8 *>(data);
    for (qsizetype i = 0; i < size; ++i) {
        bytes[i] = table[bytes[i]];
    }
}

void cyclic(char *data, qsizetype size, quint32 key)
{
    auto w = quint16(key ^ (key >> 16));
    auto *bytes = reinterpret_cast<quint8 *>(data);
    for (qsizetype i = 0; i < size; ++i) {
        quint8 b = bytes[i];
        b = quint8(b + quint8(w));
        b = R[b];
        b = quint8(b + quint8(w >> 8));
        b = S[b];
        b = quint8(b - quint8(w >> 8));
        b = I[b];
        b = quint8(b - quint8(w));
        bytes[i] = b;
        w = quint16(w + 1);
    }
}

quint16 signature(quint64 ib, quint64 bid)
{
    const quint64 x = ib ^ bid;
    return quint16(quint16(x >> 16) ^ quint16(x));
}

}
