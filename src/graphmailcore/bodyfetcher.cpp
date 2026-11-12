// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bodyfetcher.h"
#include "searchquery.h"
#include "blobstore.h"
#include "graphmapper.h"
#include "graphutil.h"
#include "graphmail_debug.h"

#include <QDateTime>
#include <QJsonArray>
#include <QScopeGuard>

#include <algorithm>

using namespace GraphCore;

namespace GraphMail
{

namespace
{
constexpr qint64 dayMs = 24LL * 3600 * 1000;
// Inline images are part of the message's look; bigger "inline" parts are rare and fetched on demand
constexpr qint64 maxInlineSize = 5 * 1024 * 1024;
constexpr qint64 maxOfflineAttachmentSize = 25 * 1024 * 1024;
constexpr int refillSize = 50;
// When nothing is missing, look again after this long
constexpr qint64 idleRecheckMs = 60 * 1000;
}

BodyFetcher::BodyFetcher(SyncContext *context, QObject *parent)
    : QObject(parent)
    , m_ctx(context)
{
}

void BodyFetcher::setWindowDays(int days)
{
    m_windowDays = days;
    m_queue.clear();
    m_nextRefill = 0;
}

void BodyFetcher::setAttachmentsOffline(bool on)
{
    m_attachmentsOffline = on;
}

int BodyFetcher::inFlight() const
{
    return int(m_inFlight.size());
}

qint64 BodyFetcher::now() const
{
    return m_ctx->now ? m_ctx->now() : QDateTime::currentMSecsSinceEpoch();
}

void BodyFetcher::fetchNow(qint64 message)
{
    const std::optional<MessageRow> m = m_ctx->store->message(message);
    if (!m) {
        return;
    }
    if (!m->bodyBlob.isEmpty()) {
        // Bodies cached before 2026-11-11 may lack their inline images (see fetch()), and a message
        // with attachments needs their list for the reading pane: get what is missing now
        QString error;
        const std::optional<QByteArray> body = m_ctx->blobs->get(m->bodyBlob, &error);
        const QList<AttachmentRow> attachments = m_ctx->store->attachments(message);
        const bool inlineMissing = body && body->contains("cid:")
            && std::none_of(attachments.cbegin(), attachments.cend(), [](const AttachmentRow &a) {
                   return a.isInline && !a.blob.isEmpty();
               });
        const bool listMissing = m->hasAttachments && attachments.isEmpty();
        if ((!inlineMissing && !listMissing) || m_inFlight.contains(message)) {
            Q_EMIT bodyReady(message);
            return;
        }
        m_inFlight.insert(message);
        fetchAttachments(message, m->graphId, [this, message] {
            m_inFlight.remove(message);
            Q_EMIT bodyReady(message);
        });
        return;
    }
    m_failed.remove(message);
    if (!m_urgent.contains(message)) {
        m_urgent.prepend(message);
    }
}

void BodyFetcher::fetchAttachmentNow(qint64 attachment)
{
    // Find the attachment row and its message
    const std::optional<qint64> message = m_ctx->store->messageOfAttachment(attachment);
    if (!message) {
        Q_EMIT attachmentReady(0, attachment, false);
        return;
    }
    const std::optional<MessageRow> m = m_ctx->store->message(*message);
    const QList<AttachmentRow> rows = m_ctx->store->attachments(*message);
    const auto row = std::find_if(rows.cbegin(), rows.cend(), [attachment](const AttachmentRow &a) {
        return a.id == attachment;
    });
    if (!m || row == rows.cend()) {
        Q_EMIT attachmentReady(*message, attachment, false);
        return;
    }
    if (!row->blob.isEmpty() && m_ctx->blobs->contains(row->blob)) {
        Q_EMIT attachmentReady(*message, attachment, true);
        return;
    }
    // $value: the raw bytes of a file attachment, the MIME of an attached mail or Outlook item
    GraphRequest *request = m_ctx->client->start({"GET",
                                                  m_ctx->root + QStringLiteral("/messages/") + m->graphId + QStringLiteral("/attachments/") + row->graphId
                                                      + QStringLiteral("/$value"),
                                                  {},
                                                  {Prefer::header({Prefer::immutableId()})},
                                                  true,
                                                  nullptr});
    const qint64 messageId = *message;
    connect(request, &GraphRequest::finished, this, [this, request, messageId, attachment] {
        const GraphResponse response = request->response();
        request->deleteLater();
        if (!response.ok()) {
            if (response.isNetworkError()) {
                Q_EMIT networkError();
            }
            Q_EMIT attachmentReady(messageId, attachment, false);
            return;
        }
        QString error;
        const QString blob = m_ctx->blobs->put(response.body, &error);
        const bool ok = !blob.isEmpty() && m_ctx->store->setAttachmentBlob(attachment, blob);
        Q_EMIT attachmentReady(messageId, attachment, ok);
    });
}

void BodyFetcher::process(int slots)
{
    // Opened messages first, even beyond the slots the scheduler offers (one at a time)
    while (!m_urgent.isEmpty() && (slots > 0 || m_urgentInFlight.isEmpty())) {
        const qint64 message = m_urgent.takeFirst();
        if (m_inFlight.contains(message)) {
            continue;
        }
        if (const std::optional<MessageRow> m = m_ctx->store->message(message)) {
            fetch(message, m->graphId, true);
            --slots;
        }
    }
    if (m_windowDays == 0 || slots <= 0) {
        return;
    }
    if (m_queue.isEmpty() && now() >= m_nextRefill) {
        const qint64 since = m_windowDays < 0 ? 0 : now() - qint64(m_windowDays) * dayMs;
        const QList<QPair<qint64, QString>> missing = m_ctx->store->messagesWithoutBody(since, refillSize + int(m_failed.size() + m_inFlight.size()));
        for (const auto &entry : missing) {
            if (!m_failed.contains(entry.first) && !m_inFlight.contains(entry.first)) {
                m_queue.append(entry);
            }
        }
        if (m_queue.isEmpty()) {
            m_nextRefill = now() + idleRecheckMs;
        }
    }
    while (slots > 0 && !m_queue.isEmpty()) {
        const auto [message, graphId] = m_queue.takeFirst();
        if (!m_inFlight.contains(message)) {
            fetch(message, graphId, false);
            --slots;
        }
    }
}

void BodyFetcher::fetch(qint64 message, const QString &graphId, bool urgent)
{
    m_inFlight.insert(message);
    if (urgent) {
        m_urgentInFlight.insert(message);
    }
    GraphRequest *request = m_ctx->client->start({"GET",
                                                  m_ctx->root + QStringLiteral("/messages/") + graphId + QStringLiteral("?$select=body,hasAttachments"),
                                                  {},
                                                  {Prefer::header({Prefer::immutableId(), Prefer::htmlBody()})},
                                                  true,
                                                  nullptr});
    connect(request, &GraphRequest::finished, this, [this, request, message, graphId, urgent] {
        const GraphResponse response = request->response();
        request->deleteLater();
        if (response.isNetworkError()) {
            m_inFlight.remove(message);
            m_urgentInFlight.remove(message);
            if (urgent) {
                m_urgent.prepend(message);
            }
            Q_EMIT networkError();
            return;
        }
        if (!response.ok()) {
            qCDebug(GRAPHMAIL) << "No body for message" << message << response.describe();
            m_failed.insert(message);
            done(message, urgent);
            return;
        }
        const QJsonObject json = response.json();
        const QJsonObject body = json.value(QLatin1String("body")).toObject();
        const QByteArray content = body.value(QLatin1String("content")).toString().toUtf8();
        const int type = body.value(QLatin1String("contentType")).toString() == QLatin1String("html") ? 1 : 2;
        QString error;
        const QString blob = m_ctx->blobs->put(content, &error);
        if (blob.isEmpty() || !m_ctx->store->setBody(message, blob, type, content.size(), bodyText(content, type == 1))) {
            qCWarning(GRAPHMAIL) << "Could not store a body:" << error << m_ctx->store->lastError();
            m_ctx->blobs->remove(blob);
            m_failed.insert(message);
            done(message, urgent);
            return;
        }
        // Graph's hasAttachments is false when a message only has inline images (a logo in a
        // signature): a cid: reference in the body is the other reason to list the attachments
        if (json.value(QLatin1String("hasAttachments")).toBool() || content.contains("cid:")) {
            // An opened message is shown once its inline images are there too
            fetchAttachments(message, graphId, [this, message, urgent] {
                done(message, urgent);
            });
            return;
        }
        done(message, urgent);
    });
}

void BodyFetcher::done(qint64 message, bool urgent)
{
    m_inFlight.remove(message);
    m_urgentInFlight.remove(message);
    if (urgent) {
        Q_EMIT bodyReady(message);
    }
}

// The list (metadata only: no contentBytes), then the content of inline parts
void BodyFetcher::fetchAttachments(qint64 message, const QString &graphId, const std::function<void()> &done)
{
    GraphRequest *request = m_ctx->client->start({"GET",
                                                  m_ctx->root + QStringLiteral("/messages/") + graphId
                                                      + QStringLiteral("/attachments?$select=id,name,contentType,size,isInline"),
                                                  {},
                                                  {Prefer::header({Prefer::immutableId()})},
                                                  true,
                                                  nullptr});
    connect(request, &GraphRequest::finished, this, [this, request, message, graphId, done] {
        const GraphResponse response = request->response();
        request->deleteLater();
        if (!response.ok()) {
            done(); // the body is there; attachments are listed again when the message is opened
            return;
        }
        QList<AttachmentRow> rows;
        const QJsonArray list = response.json().value(QLatin1String("value")).toArray();
        for (const QJsonValue &value : list) {
            rows.append(Mapper::attachment(value.toObject()));
        }
        if (!m_ctx->store->replaceAttachments(message, rows)) {
            done();
            return;
        }
        const QList<AttachmentRow> stored = m_ctx->store->attachments(message);
        QList<AttachmentRow> wanted;
        for (const AttachmentRow &a : stored) {
            if ((a.isInline && a.size <= maxInlineSize) || (m_attachmentsOffline && a.size <= maxOfflineAttachmentSize)) {
                wanted.append(a);
            }
        }
        if (wanted.isEmpty()) {
            done();
            return;
        }
        // done() once every wanted part has been fetched (or failed)
        auto pending = std::make_shared<int>(int(wanted.size()));
        for (const AttachmentRow &a : std::as_const(wanted)) {
            fetchAttachmentContent(message, graphId, a, [pending, done] {
                if (--*pending == 0) {
                    done();
                }
            });
        }
    });
}

// The full attachment object: contentBytes, and contentId to resolve cid: links in the body
void BodyFetcher::fetchAttachmentContent(qint64 message, const QString &graphId, const AttachmentRow &attachment, const std::function<void()> &done)
{
    GraphRequest *request = m_ctx->client->start(
        {"GET", m_ctx->root + QStringLiteral("/messages/") + graphId + QStringLiteral("/attachments/") + attachment.graphId, {}, {Prefer::header({Prefer::immutableId()})}, true, nullptr});
    connect(request, &GraphRequest::finished, this, [this, request, message, attachment, done] {
        const GraphResponse response = request->response();
        request->deleteLater();
        // Every way out of here counts as finished for the waiting message
        const auto finished = qScopeGuard(done);
        if (!response.ok()) {
            return;
        }
        const QJsonObject json = response.json();
        const QByteArray bytes = QByteArray::fromBase64(json.value(QLatin1String("contentBytes")).toString().toLatin1());
        if (bytes.isEmpty()) {
            return; // an attached Outlook item or a reference attachment: on demand later
        }
        QString error;
        const QString blob = m_ctx->blobs->put(bytes, &error);
        if (blob.isEmpty()) {
            return;
        }
        // The rows may have been replaced meanwhile (message fetched again); match by graph id.
        // contentId (for cid: links in the body) only comes with the full object.
        const QList<AttachmentRow> current = m_ctx->store->attachments(message);
        for (const AttachmentRow &row : current) {
            if (row.graphId == attachment.graphId) {
                m_ctx->store->setAttachmentBlob(row.id, blob, json.value(QLatin1String("contentId")).toString());
                return;
            }
        }
        m_ctx->blobs->remove(blob);
    });
}

void BodyFetcher::sweep()
{
    if (m_windowDays >= 0) {
        const qint64 before = now() - qint64(m_windowDays) * dayMs;
        const QStringList dropped = m_ctx->store->sweepBodies(before);
        for (const QString &blob : dropped) {
            m_ctx->blobs->remove(blob);
        }
        if (!dropped.isEmpty()) {
            qCInfo(GRAPHMAIL) << "Dropped" << dropped.size() << "offline bodies and images outside the window";
        }
    }
    // Files nothing refers to (a folder deleted with its messages, a crash between write and commit)
    QStringList onDisk = m_ctx->blobs->allIds();
    const QStringList referenced = m_ctx->store->referencedBlobs();
    const QSet<QString> keep(referenced.cbegin(), referenced.cend());
    int orphans = 0;
    for (const QString &id : std::as_const(onDisk)) {
        if (!keep.contains(id)) {
            m_ctx->blobs->remove(id);
            ++orphans;
        }
    }
    if (orphans > 0) {
        qCInfo(GRAPHMAIL) << "Removed" << orphans << "orphaned blob files";
    }
}

}
