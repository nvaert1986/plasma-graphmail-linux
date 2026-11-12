// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "outgoing.h"
#include "store.h"

namespace GraphMail
{

class BlobStore;

// Replies and forwards the way Outlook writes them: the answer on top, then a line, a header block
// (From, Sent, To, Cc, Subject) and the original message untouched
namespace Quote
{
// "RE: subject" or "FW: subject", without stacking prefixes ("RE: RE:", "AW:", "WG:" stay one)
QString subject(const QString &kind, const QString &original);
// The attachment reminder (docs/PHASE9.md): whether the user's own text speaks of an attachment
// (English, Dutch, German, French); the quoted original is not part of it
bool mentionsAttachment(const QString &text);
// The header block above the original
QString headerHtml(const MessageRow &original);
QString headerText(const MessageRow &original);
// The original's body from the cache as HTML to put under the header (its styles kept, its <html>
// and <body> taken off), or as text; empty when the body is not cached
QString originalHtml(const MessageRow &original, BlobStore *blobs);
QString originalText(const MessageRow &original, BlobStore *blobs);
// Reply: the sender. Reply all: the sender and everyone in To and Cc, without the user's own
// addresses and without doubles.
void recipients(const MessageRow &original, bool all, const QStringList &ownAddresses, QList<OutgoingRecipient> *to, QList<OutgoingRecipient> *cc);
}

}
