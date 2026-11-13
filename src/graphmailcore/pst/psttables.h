// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QtGlobal>

// The data tables of [MS-PST] section 5, generated into psttables.cpp from the specification
namespace GraphMail::Pst::Tables
{
// mpbbCrypt: R (0..255), S (256..511), I (512..767)
extern const quint8 crypt[768];
// CrcTableOffset32, 40, 48, 56, 64, 72, 80, 88
extern const quint32 crc[8][256];
}
