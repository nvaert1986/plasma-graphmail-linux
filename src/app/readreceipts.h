// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

// Outlook's Tracking option "For any message received that includes a read receipt request"
// (docs/PHASE9B.md 9b.12). Exchange itself sends the receipt the moment a message is marked read;
// "never" marks read with the request cleared in the same change (probed 2026-11-16).
namespace GraphMail::ReadReceipts
{

enum class Mode { Ask, Always, Never };
// Wait: Ask mode and the request not known yet (headers still coming): mark read only once known
enum class Decision { Send, DontSend, Ask, Wait };

Mode mode();
void setMode(Mode mode);
// What marking a message read should do. requested: the sender asked (nullopt: not known yet);
// byHand: Mark as Read by the user, which never sends unless the mode is Always
Decision decide(Mode mode, std::optional<bool> requested, bool byHand);

}
