// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "collectionsync.h"

#include <QObject>
#include <QSet>

namespace GraphMail
{

// Keeps bodies offline (docs/PHASE2.md section 4): newest first, within the body window, with the
// message's attachment list and its inline images (the reading pane needs them). Other
// attachments only when "attachments offline" is on. fetchNow() jumps the queue (a message the
// user opens). The daily sweep drops bodies that left the window (not for flagged messages) and
// blob files nothing refers to any more.
class BodyFetcher : public QObject
{
    Q_OBJECT

public:
    explicit BodyFetcher(SyncContext *context, QObject *parent = nullptr);

    // 0 = no prefetch (bodies only when opened), -1 = everything (the OST-like mode)
    void setWindowDays(int days);
    void setAttachmentsOffline(bool on);

    void fetchNow(qint64 message);
    // The content of one attachment row (opening or saving it); attachmentReady() when stored
    void fetchAttachmentNow(qint64 attachment);
    // Starts up to `slots` fetches; the scheduler calls it when collections leave room
    void process(int slots);
    int inFlight() const;
    void sweep();

Q_SIGNALS:
    void bodyReady(qint64 message);
    void attachmentReady(qint64 message, qint64 attachment, bool ok);
    void networkError();

private:
    void fetch(qint64 message, const QString &graphId, bool urgent);
    // The attachment list, then the content of inline parts; done() after all of it
    void fetchAttachments(qint64 message, const QString &graphId, const std::function<void()> &done);
    void fetchAttachmentContent(qint64 message, const QString &graphId, const AttachmentRow &attachment, const std::function<void()> &done);
    void done(qint64 message, bool urgent);
    qint64 now() const;

    SyncContext *m_ctx;
    int m_windowDays = 90;
    bool m_attachmentsOffline = false;
    QList<qint64> m_urgent;
    QList<QPair<qint64, QString>> m_queue;
    QSet<qint64> m_inFlight;
    QSet<qint64> m_urgentInFlight;
    // Messages Graph would not give a body for (deleted meanwhile, ...): not tried again this run
    QSet<qint64> m_failed;
    qint64 m_nextRefill = 0;
};

}
