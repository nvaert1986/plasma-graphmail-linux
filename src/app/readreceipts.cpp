// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "readreceipts.h"

#include <KConfigGroup>
#include <KSharedConfig>

namespace GraphMail::ReadReceipts
{

namespace
{
KConfigGroup group()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Reading"));
}
}

Mode mode()
{
    const QString text = group().readEntry("ReadReceipts", QString());
    if (text == QLatin1String("always")) {
        return Mode::Always;
    }
    return text == QLatin1String("never") ? Mode::Never : Mode::Ask;
}

void setMode(Mode mode)
{
    KConfigGroup g = group();
    g.writeEntry("ReadReceipts", mode == Mode::Always ? QStringLiteral("always") : mode == Mode::Never ? QStringLiteral("never") : QStringLiteral("ask"));
    g.sync();
}

Decision decide(Mode mode, std::optional<bool> requested, bool byHand)
{
    if (mode == Mode::Always) {
        return Decision::Send;
    }
    if (mode == Mode::Never || byHand) {
        return Decision::DontSend;
    }
    // Asked: only when the sender wants one. Not known yet: wait, marking read now would clear a
    // request the user was never asked about
    if (!requested) {
        return Decision::Wait;
    }
    return *requested ? Decision::Ask : Decision::DontSend;
}

}
