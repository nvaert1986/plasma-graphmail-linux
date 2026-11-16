// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "collectionsync.h"
#include "outgoing.h"

#include <QHash>
#include <QObject>

#include <functional>

namespace GraphMail
{

// Brings the messages the user writes to Exchange (docs/PHASE4.md), one at a time per mailbox:
// creates the draft (createReply/createReplyAll/createForward for answers, so Exchange threads
// them and copies the inline images or attachments), updates it, syncs the attachments (upload
// sessions above 3 MB) and sends it when asked. A save that arrives while a run is busy makes the
// run go again. Network trouble retries later; Exchange refusing for good marks it failed and
// keeps the draft. After sending from another mailbox it reads the sent copy to learn whether
// Exchange sent as that mailbox or on its behalf.
class OutboxEngine : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 uploadSessionThreshold = 3 * 1024 * 1024;
    // Upload sessions take multiples of 320 KiB
    static constexpr qint64 uploadChunk = 12 * 320 * 1024;

    explicit OutboxEngine(SyncContext *context, QObject *parent = nullptr);

    // Starts the oldest due message when nothing runs
    void process();
    bool busy() const;
    // Tests: how long to wait before reading the sent copy, and how often to try
    void setLearnDelay(int ms, int attempts);

Q_SIGNALS:
    // state: saved, sent, failed, discarded
    void changed(const QString &uuid, const QString &state, const QString &error);
    // right: as, behalf, denied
    void sendRightLearned(const QString &fromMailbox, const QString &right);
    void networkError();
    // The draft was changed in Outlook while it was changed here: both versions are kept, the
    // user's as a new draft (docs/PHASE8.md)
    void draftConflict(const QString &uuid, const QString &subject);

private:
    using Done = std::function<void(const GraphCore::GraphResponse &)>;
    void request(const QByteArray &verb, const QString &path, const QJsonObject &body, const Done &done, const QByteArray &ifMatch = {});
    void run(const Outgoing &outgoing);
    void ensureDraft();
    void patchDraft();
    void syncAttachments();
    void deleteNext(QStringList ids);
    void uploadNext();
    void uploadSmall(const OutgoingAttachment &attachment, const QByteArray &data);
    void uploadLarge(const OutgoingAttachment &attachment, const QByteArray &data);
    void putChunk(const OutgoingAttachment &attachment, const QByteArray &data, const QString &url, qint64 offset);
    void uploaded(const QString &key, const QString &graphId);
    void sendDraft();
    // @mentions onto the draft (beta), then postSend()
    void addMentions();
    void postSend();
    void learn(const QString &mailbox, const QString &internetMessageId, int attemptsLeft);

    void discard();
    void finishSaved();
    void finishSent();
    // Network trouble or throttling: try again later. Anything else: failed, the draft stays.
    void error(const GraphCore::GraphResponse &response);
    // Changes the stored record (the composer may have saved a newer revision meanwhile)
    void persist(const std::function<void(Outgoing &)> &change);
    void done();
    QString messagePath() const;

    SyncContext *m_ctx;
    bool m_busy = false;
    Outgoing m_run;
    bool m_recreated = false;
    // Held for Undo send: uuid -> the revision already in Exchange
    QHash<QString, int> m_held;
    // Attachments were added or removed in this run: the draft's version changed with them
    bool m_attachmentsChanged = false;
    int m_learnDelayMs = 5000;
    int m_learnAttempts = 4;
};

}
