// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

// Message bodies kept only as RTF (PidTagRtfCompressed), as older Outlook items have them:
// decompression ([MS-OXRTFCP]) and the original HTML or text taken back out ([MS-OXRTFEX]).
namespace GraphMail::Rtf
{

// The RTF of a PidTagRtfCompressed value; nullopt with the reason when it is damaged
std::optional<QByteArray> decompress(const QByteArray &compressed, QString *error);

struct Body {
    enum class Kind {
        // \fromhtml1: the original HTML
        Html,
        // \fromtext: the original text
        Text,
        // RTF written as RTF: its text only, the formatting is lost
        PlainFromRtf,
    };
    Kind kind = Kind::PlainFromRtf;
    QString content;
};

// The body an RTF document holds ([MS-OXRTFEX] 2.2.3)
Body deencapsulate(const QByteArray &rtf);

// PidTagRtfCompressed for RTF ([MS-OXRTFCP] 2.3: COMPRESSED, with the CRC)
QByteArray compress(const QByteArray &rtf);
// RTF holding HTML (\fromhtml1: tags in \*\htmltag groups, the text readable as RTF) or text
// (\fromtext), so readers of either get the original back ([MS-OXRTFEX] 2.1)
QByteArray encapsulateHtml(const QString &html);
QByteArray encapsulateText(const QString &text);

}
