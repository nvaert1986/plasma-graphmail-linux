// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace GraphMail
{

struct OutgoingRecipient {
    QString name;
    QString address;
};

struct OutgoingAttachment {
    // Chosen by the composer; ties a save to the attachment uploaded for an earlier save
    QString key;
    QString name;
    QString contentType;
    qint64 size = 0;
    // The content in the mailbox's BlobStore (the daemon copies the file there on save)
    QString blob;
    // A file the composer wants attached; replaced by `blob` when the daemon stores it
    QString path;
    bool isInline = false;
    QString contentId;
    // The attachment on the draft in Exchange, once uploaded
    QString graphId;
    // Uploaded earlier, removed by the user since: deleted from the draft, then dropped
    bool removed = false;
};

// A message being written (docs/PHASE4.md): the composer saves it through the daemon, the mailbox's
// OutboxEngine keeps the draft in Exchange up to date and sends it. Stored in the cache's outgoing
// table, so writing and sending survive being offline and restarts.
struct Outgoing {
    enum class State { Pending = 0, Saved = 1, Failed = 2, Discard = 3 };

    QString uuid;
    // new, reply, replyAll, forward, or draft (an existing draft opened again)
    QString kind = QStringLiteral("new");
    // The message replied to or forwarded (graph id, same mailbox)
    QString source;
    // The draft in Exchange (graph id), once created
    QString draftId;
    QString internetMessageId;
    // The draft's version after Graph Mail's last save (If-Match: a change in Outlook meanwhile
    // gives 412, docs/PHASE8.md); empty after attachment changes, which change it too
    QString draftEtag;
    // Sending from another mailbox: its address and its mailbox key part (object id)
    QString fromAddress;
    QString fromName;
    QString fromMailbox;
    // What the user picked on the Send button: "as", "behalf" or empty
    QString sendMode;
    QList<OutgoingRecipient> to;
    QList<OutgoingRecipient> cc;
    QList<OutgoingRecipient> bcc;
    // Where answers go instead of the sender (Outlook's Reply To); empty: the sender
    QList<OutgoingRecipient> replyTo;
    // People @mentioned in the text (Outlook's mentions): Graph's beta mentions, added to the draft
    // before the send (a PATCH of the draft ignores them, probed 2026-11-16)
    QList<OutgoingRecipient> mentions;
    QString subject;
    QString body;
    bool html = true;
    // 0 low, 1 normal, 2 high
    int importance = 1;
    // Receipts asked of the recipients' mail systems
    bool readReceipt = false;
    bool deliveryReceipt = false;
    // MAPI PR_SENSITIVITY: 0 normal, 1 personal, 2 private, 3 confidential
    int sensitivity = 0;
    // Outlook's Delay Delivery (PR_DEFERRED_SEND_TIME, docs/PHASE9.md): Exchange keeps the message
    // until then. 0 = none; -1 = it was set on the draft and is taken away again
    qint64 deferUntil = 0;
    // Undo send: the draft is uploaded at once, the send waits until then (milliseconds)
    qint64 sendAfter = 0;
    QList<OutgoingAttachment> attachments;
    // Attachments createForward copies from the original that the user removed (by name)
    QStringList dropSourceAttachments;
    // false: keep it as a draft; true: send once the draft is up to date
    bool send = false;
    // The composer's own state (its text without the quoted original, the signature, ...): kept as
    // it is, so a draft opens again the way it was left
    QJsonObject ui;
    // Raised by every save of the composer: an upload that started earlier runs again
    int revision = 0;

    State state = State::Pending;
    int attempts = 0;
    qint64 nextAttempt = 0;
    qint64 updated = 0;
    QString lastError;

    QJsonObject toJson() const;
    static Outgoing fromJson(const QJsonObject &json);

    // The properties a PATCH of the draft sets
    QJsonObject graphMessage() const;

    // A new save from the composer over what the engine already did: keeps the draft id and the
    // uploaded attachments, marks uploaded ones the user removed for deletion
    static Outgoing merged(const Outgoing &stored, Outgoing incoming);
};

QJsonObject graphRecipient(const OutgoingRecipient &recipient);
// "Name <address>; address, ..." as typed, or as the cache keeps recipients. A comma only separates
// once the part before it holds an address ("Doe, Jane <...>" stays one). Parts that are no
// address go to `invalid`.
QList<OutgoingRecipient> parseRecipients(const QString &text, QStringList *invalid = nullptr);
// The parts of such a line as typed, untrimmed (parseRecipients' first step)
QStringList splitRecipients(const QString &text);
QString formatRecipient(const OutgoingRecipient &recipient);

}
