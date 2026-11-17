// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "outboxengine.h"
#include "blobstore.h"
#include "graphclient.h"
#include "graphmail_debug.h"
#include "graphrequest.h"
#include "graphutil.h"
#include "store.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

using namespace GraphCore;

namespace GraphMail
{

namespace
{
constexpr qint64 firstRetryMs = 30 * 1000;
constexpr qint64 maxRetryMs = 30 * 60 * 1000;

// The new attachment's id from an upload session's last answer:
// Location: https://outlook.office.com/api/v2.0/Users('...')/Messages('...')/Attachments('<id>')
QString idFromLocation(const QByteArray &location)
{
    static const QRegularExpression pattern(QStringLiteral(R"(Attachments\('([^']+)'\))"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(QUrl::fromPercentEncoding(location));
    return match.hasMatch() ? match.captured(1) : QString();
}
}

OutboxEngine::OutboxEngine(SyncContext *context, QObject *parent)
    : QObject(parent)
    , m_ctx(context)
{
}

bool OutboxEngine::busy() const
{
    return m_busy;
}

void OutboxEngine::setLearnDelay(int ms, int attempts)
{
    m_learnDelayMs = ms;
    m_learnAttempts = attempts;
}

void OutboxEngine::process()
{
    if (m_busy) {
        return;
    }
    const qint64 now = m_ctx->now ? m_ctx->now() : QDateTime::currentMSecsSinceEpoch();
    const QList<Outgoing> due = m_ctx->store->dueOutgoing(now);
    if (!due.isEmpty()) {
        run(due.constFirst());
    }
}

QString OutboxEngine::messagePath() const
{
    return m_ctx->root + QStringLiteral("/messages/") + m_run.draftId;
}

void OutboxEngine::request(const QByteArray &verb, const QString &path, const QJsonObject &body, const Done &done, const QByteArray &ifMatch)
{
    GraphRequestSpec spec;
    spec.verb = verb;
    spec.pathOrUrl = path;
    spec.headers = {Prefer::header({Prefer::immutableId()})};
    if (!ifMatch.isEmpty()) {
        spec.headers.append({QByteArrayLiteral("If-Match"), ifMatch});
    }
    if (verb == "POST" || verb == "PATCH") {
        spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
        spec.body = body.isEmpty() ? QByteArray("{}") : QJsonDocument(body).toJson(QJsonDocument::Compact);
    }
    GraphRequest *r = m_ctx->client->start(spec);
    connect(r, &GraphRequest::finished, this, [r, done] {
        const GraphResponse response = r->response();
        r->deleteLater();
        done(response);
    });
}

void OutboxEngine::run(const Outgoing &outgoing)
{
    m_busy = true;
    m_run = outgoing;
    m_recreated = false;
    m_attachmentsChanged = false;
    if (m_run.state == Outgoing::State::Discard) {
        m_held.remove(m_run.uuid);
        discard();
        return;
    }
    // The end of an Undo send hold: this revision is in Exchange already, so straight to sending
    // (sendDraft checks the hold and Undo again); after a restart the full path runs
    if (m_run.send && !m_run.draftId.isEmpty() && m_held.value(m_run.uuid, -1) == m_run.revision) {
        m_held.remove(m_run.uuid);
        sendDraft();
        return;
    }
    m_held.remove(m_run.uuid);
    ensureDraft();
}

void OutboxEngine::ensureDraft()
{
    if (!m_run.draftId.isEmpty()) {
        patchDraft();
        return;
    }
    QString path = m_ctx->root + QStringLiteral("/messages");
    const bool answer = !m_run.source.isEmpty() && (m_run.kind == QLatin1String("reply") || m_run.kind == QLatin1String("replyAll") || m_run.kind == QLatin1String("forward"));
    if (answer) {
        const char *action = m_run.kind == QLatin1String("reply") ? "/createReply" : m_run.kind == QLatin1String("replyAll") ? "/createReplyAll" : "/createForward";
        path += QLatin1Char('/') + m_run.source + QLatin1String(action);
    }
    // A new message is created with its content at once; an answer gets it in the PATCH
    request("POST", path, answer ? QJsonObject() : m_run.graphMessage(), [this, answer](const GraphResponse &response) {
        if (!response.ok()) {
            error(response);
            return;
        }
        const QJsonObject draft = response.json();
        m_run.draftId = draft.value(QLatin1String("id")).toString();
        m_run.internetMessageId = draft.value(QLatin1String("internetMessageId")).toString();
        m_run.draftEtag = draft.value(QLatin1String("@odata.etag")).toString();
        persist([this](Outgoing &o) {
            o.draftId = m_run.draftId;
            o.internetMessageId = m_run.internetMessageId;
            o.draftEtag = m_run.draftEtag;
        });
        if (answer) {
            patchDraft();
        } else {
            syncAttachments();
        }
    });
}

void OutboxEngine::patchDraft()
{
    request("PATCH", messagePath(), m_run.graphMessage(), [this](const GraphResponse &response) {
        // 412: changed in Outlook since Graph Mail saved it: that version stays, this one
        // becomes a second draft
        const bool conflict = response.status == 412;
        if ((response.status == 404 || conflict) && !m_recreated) {
            // Deleted in Outlook meanwhile: start a new draft (once)
            m_recreated = true;
            if (conflict) {
                qCInfo(GRAPHMAIL) << "A draft was changed in Outlook too: both versions are kept";
                Q_EMIT draftConflict(m_run.uuid, m_run.subject);
            }
            m_run.draftId.clear();
            m_run.draftEtag.clear();
            for (OutgoingAttachment &a : m_run.attachments) {
                a.graphId.clear();
            }
            m_run.attachments.removeIf([](const OutgoingAttachment &a) {
                return a.removed;
            });
            persist([](Outgoing &o) {
                o.draftId.clear();
                o.draftEtag.clear();
                for (OutgoingAttachment &a : o.attachments) {
                    a.graphId.clear();
                }
                o.attachments.removeIf([](const OutgoingAttachment &a) {
                    return a.removed;
                });
            });
            ensureDraft();
            return;
        }
        if (!response.ok()) {
            error(response);
            return;
        }
        const QString imid = response.json().value(QLatin1String("internetMessageId")).toString();
        const QString etag = response.json().value(QLatin1String("@odata.etag")).toString();
        m_run.draftEtag = etag;
        if (!imid.isEmpty()) {
            m_run.internetMessageId = imid;
        }
        persist([imid, etag](Outgoing &o) {
            if (!imid.isEmpty()) {
                o.internetMessageId = imid;
            }
            o.draftEtag = etag;
        });
        syncAttachments();
    }, m_run.draftEtag.toUtf8());
}

void OutboxEngine::syncAttachments()
{
    // Nothing of ours and nothing to drop: the draft's attachments are what Exchange made
    const bool ours = std::any_of(m_run.attachments.cbegin(), m_run.attachments.cend(), [](const OutgoingAttachment &a) {
        return a.graphId.isEmpty() || a.removed || a.graphId == QLatin1String("?");
    });
    if (!ours && m_run.dropSourceAttachments.isEmpty()) {
        sendDraft();
        return;
    }
    request("GET", messagePath() + QStringLiteral("/attachments?$select=id,name,size,isInline"), {}, [this](const GraphResponse &response) {
        if (!response.ok()) {
            error(response);
            return;
        }
        struct Remote {
            QString id;
            QString name;
            qint64 size;
        };
        QList<Remote> remote;
        const QJsonArray value = response.json().value(QLatin1String("value")).toArray();
        for (const QJsonValue &v : value) {
            const QJsonObject o = v.toObject();
            remote.append({o.value(QLatin1String("id")).toString(), o.value(QLatin1String("name")).toString(), o.value(QLatin1String("size")).toInteger()});
        }
        QStringList mine;
        for (OutgoingAttachment &a : m_run.attachments) {
            if (a.graphId == QLatin1String("?")) {
                // An upload session without a Location header: find it by name
                for (const Remote &r : std::as_const(remote)) {
                    if (r.name == a.name && !mine.contains(r.id)) {
                        a.graphId = r.id;
                        break;
                    }
                }
                if (a.graphId == QLatin1String("?")) {
                    a.graphId.clear();
                }
            } else if (!a.graphId.isEmpty() && std::none_of(remote.cbegin(), remote.cend(), [&a](const Remote &r) {
                           return r.id == a.graphId;
                       })) {
                // Gone from the draft: upload again (or forget it when it was removed)
                a.graphId.clear();
            }
            if (!a.graphId.isEmpty()) {
                mine.append(a.graphId);
            }
        }
        QStringList remove;
        for (const OutgoingAttachment &a : std::as_const(m_run.attachments)) {
            if (a.removed && !a.graphId.isEmpty()) {
                remove.append(a.graphId);
            }
        }
        for (const Remote &r : std::as_const(remote)) {
            if (!mine.contains(r.id) && m_run.dropSourceAttachments.contains(r.name)) {
                remove.append(r.id);
            }
        }
        deleteNext(remove);
    });
}

void OutboxEngine::deleteNext(QStringList ids)
{
    if (ids.isEmpty()) {
        // Removed attachments are gone from the draft now
        m_run.attachments.removeIf([](const OutgoingAttachment &a) {
            return a.removed;
        });
        m_run.dropSourceAttachments.clear();
        persist([](Outgoing &o) {
            o.attachments.removeIf([](const OutgoingAttachment &a) {
                return a.removed;
            });
            o.dropSourceAttachments.clear();
        });
        uploadNext();
        return;
    }
    const QString id = ids.takeFirst();
    request("DELETE", messagePath() + QStringLiteral("/attachments/") + id, {}, [this, ids](const GraphResponse &response) {
        if (!response.ok() && response.status != 404) {
            error(response);
            return;
        }
        m_run.draftEtag.clear();
        persist([](Outgoing &o) {
            o.draftEtag.clear();
        });
        deleteNext(ids);
    });
}

void OutboxEngine::uploadNext()
{
    for (const OutgoingAttachment &a : std::as_const(m_run.attachments)) {
        if (!a.graphId.isEmpty() || a.removed) {
            continue;
        }
        QString blobError;
        const std::optional<QByteArray> data = m_ctx->blobs->get(a.blob, &blobError);
        if (!data) {
            // The content is lost (should not happen): leave the attachment out and say so
            qCWarning(GRAPHMAIL).noquote() << "Attachment content missing for" << a.name << blobError;
            GraphResponse missing;
            missing.status = 400;
            missing.errorString = QStringLiteral("the content of the attachment \"%1\" is missing").arg(a.name);
            error(missing);
            return;
        }
        if (data->size() < uploadSessionThreshold) {
            uploadSmall(a, *data);
        } else {
            uploadLarge(a, *data);
        }
        return;
    }
    sendDraft();
}

void OutboxEngine::uploadSmall(const OutgoingAttachment &attachment, const QByteArray &data)
{
    QJsonObject body{{QStringLiteral("@odata.type"), QStringLiteral("#microsoft.graph.fileAttachment")},
                     {QStringLiteral("name"), attachment.name},
                     {QStringLiteral("contentType"), attachment.contentType},
                     {QStringLiteral("contentBytes"), QString::fromLatin1(data.toBase64())},
                     {QStringLiteral("isInline"), attachment.isInline}};
    if (!attachment.contentId.isEmpty()) {
        body.insert(QStringLiteral("contentId"), attachment.contentId);
    }
    const QString key = attachment.key;
    request("POST", messagePath() + QStringLiteral("/attachments"), body, [this, key](const GraphResponse &response) {
        if (!response.ok()) {
            error(response);
            return;
        }
        uploaded(key, response.json().value(QLatin1String("id")).toString());
    });
}

void OutboxEngine::uploadLarge(const OutgoingAttachment &attachment, const QByteArray &data)
{
    QJsonObject item{{QStringLiteral("attachmentType"), QStringLiteral("file")},
                     {QStringLiteral("name"), attachment.name},
                     {QStringLiteral("size"), qint64(data.size())},
                     {QStringLiteral("isInline"), attachment.isInline}};
    if (!attachment.contentId.isEmpty()) {
        item.insert(QStringLiteral("contentId"), attachment.contentId);
    }
    request("POST",
            messagePath() + QStringLiteral("/attachments/createUploadSession"),
            {{QStringLiteral("AttachmentItem"), item}},
            [this, attachment, data](const GraphResponse &response) {
                if (!response.ok()) {
                    error(response);
                    return;
                }
                putChunk(attachment, data, response.json().value(QLatin1String("uploadUrl")).toString(), 0);
            });
}

void OutboxEngine::putChunk(const OutgoingAttachment &attachment, const QByteArray &data, const QString &url, qint64 offset)
{
    const qint64 length = std::min(uploadChunk, qint64(data.size()) - offset);
    GraphRequestSpec spec;
    spec.verb = "PUT";
    spec.pathOrUrl = url;
    spec.body = data.mid(offset, length);
    // The upload URL carries its own authorization: never our token
    spec.authenticated = false;
    spec.headers = {{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/octet-stream")},
                    {QByteArrayLiteral("Content-Range"), QStringLiteral("bytes %1-%2/%3").arg(offset).arg(offset + length - 1).arg(data.size()).toLatin1()}};
    GraphRequest *r = m_ctx->client->start(spec);
    connect(r, &GraphRequest::finished, this, [this, r, attachment, data, url, offset, length] {
        const GraphResponse response = r->response();
        r->deleteLater();
        if (!response.ok()) {
            error(response);
            return;
        }
        if (offset + length < data.size()) {
            putChunk(attachment, data, url, offset + length);
            return;
        }
        const QString id = idFromLocation(response.header("Location"));
        uploaded(attachment.key, id.isEmpty() ? QStringLiteral("?") : id);
    });
}

void OutboxEngine::uploaded(const QString &key, const QString &graphId)
{
    for (OutgoingAttachment &a : m_run.attachments) {
        if (a.key == key) {
            a.graphId = graphId;
        }
    }
    // The draft's version changed with the attachment: the next save goes without If-Match
    m_run.draftEtag.clear();
    persist([key, graphId](Outgoing &o) {
        for (OutgoingAttachment &a : o.attachments) {
            if (a.key == key) {
                a.graphId = graphId;
            }
        }
        o.draftEtag.clear();
    });
    uploadNext();
}

void OutboxEngine::sendDraft()
{
    // A newer save arrived while this run was busy: bring that one up first
    const std::optional<Outgoing> stored = m_ctx->store->outgoing(m_run.uuid);
    if (stored && stored->revision != m_run.revision && stored->state == Outgoing::State::Pending) {
        done();
        process();
        return;
    }
    // Undo send took it back meanwhile: it stays a draft
    if (!m_run.send || (stored && !stored->send)) {
        finishSaved();
        return;
    }
    // Undo send: the draft is in Exchange, the send waits for the user's few seconds
    const qint64 now = m_ctx->now ? m_ctx->now() : QDateTime::currentMSecsSinceEpoch();
    if (stored && stored->sendAfter > now) {
        const qint64 after = stored->sendAfter;
        persist([after](Outgoing &o) {
            o.nextAttempt = after;
        });
        m_held.insert(m_run.uuid, m_run.revision);
        const QString uuid = m_run.uuid;
        done();
        Q_EMIT changed(uuid, QStringLiteral("held"), {});
        return;
    }
    // @mentions go onto the draft first (Graph's beta: a PATCH of the draft ignores them)
    if (!m_run.mentions.isEmpty()) {
        addMentions();
        return;
    }
    postSend();
}

void OutboxEngine::addMentions()
{
    QString beta = m_ctx->client->baseUrl();
    beta.replace(QLatin1String("/v1.0"), QLatin1String("/beta"));
    // What the draft has already: a send tried again adds nobody twice
    request("GET", beta + messagePath() + QStringLiteral("?$select=id&$expand=mentions($select=mentioned)"), {}, [this, beta](const GraphResponse &response) {
        QSet<QString> present;
        const QJsonArray existing = response.json().value(QLatin1String("mentions")).toArray();
        for (const QJsonValue &v : existing) {
            present.insert(v.toObject().value(QLatin1String("mentioned")).toObject().value(QLatin1String("address")).toString().toLower());
        }
        auto missing = std::make_shared<QList<OutgoingRecipient>>();
        for (const OutgoingRecipient &r : std::as_const(m_run.mentions)) {
            if (!r.address.isEmpty() && !present.contains(r.address.toLower())) {
                present.insert(r.address.toLower());
                missing->append(r);
            }
        }
        // One after the other; a mention that cannot be added does not stop the message
        // The step holds itself only weakly; the request under way keeps it alive
        auto next = std::make_shared<std::function<void()>>();
        const std::weak_ptr<std::function<void()>> weak = next;
        *next = [this, beta, missing, weak] {
            if (missing->isEmpty()) {
                postSend();
                return;
            }
            const OutgoingRecipient r = missing->takeFirst();
            const QJsonObject mention{{QStringLiteral("mentioned"), QJsonObject{{QStringLiteral("name"), r.name}, {QStringLiteral("address"), r.address}}}};
            request("POST", beta + messagePath() + QStringLiteral("/mentions"), mention, [step = weak.lock()](const GraphResponse &added) {
                if (!added.ok()) {
                    qCWarning(GRAPHMAIL).noquote() << "mention not added:" << added.describe();
                }
                (*step)();
            });
        };
        if (!response.ok()) {
            qCWarning(GRAPHMAIL).noquote() << "mentions of the draft not read:" << response.describe();
        }
        (*next)();
    });
}

void OutboxEngine::postSend()
{
    request("POST", messagePath() + QStringLiteral("/send"), {}, [this](const GraphResponse &response) {
        if (!response.ok()) {
            if (response.graphErrorCode() == QLatin1String("ErrorSendAsDenied") && !m_run.fromMailbox.isEmpty()) {
                Q_EMIT sendRightLearned(m_run.fromMailbox, QStringLiteral("denied"));
            }
            error(response);
            return;
        }
        finishSent();
    });
}

void OutboxEngine::learn(const QString &mailbox, const QString &internetMessageId, int attemptsLeft)
{
    if (attemptsLeft <= 0 || internetMessageId.isEmpty()) {
        return;
    }
    QPointer<OutboxEngine> self(this);
    QTimer::singleShot(m_learnDelayMs, this, [self, mailbox, internetMessageId, attemptsLeft] {
        if (!self) {
            return;
        }
        QString id = internetMessageId;
        id.replace(QLatin1Char('\''), QStringLiteral("''"));
        const QString path = self->m_ctx->root + QStringLiteral("/mailFolders/sentitems/messages?$select=from,sender&$filter=")
            + QString::fromLatin1(QUrl::toPercentEncoding(QStringLiteral("internetMessageId eq '") + id + QLatin1Char('\'')));
        self->request("GET", path, {}, [self, mailbox, internetMessageId, attemptsLeft](const GraphResponse &response) {
            if (!self) {
                return;
            }
            const QJsonArray value = response.json().value(QLatin1String("value")).toArray();
            if (!response.ok() || value.isEmpty()) {
                self->learn(mailbox, internetMessageId, attemptsLeft - 1);
                return;
            }
            const QJsonObject m = value.first().toObject();
            const QString from = m.value(QLatin1String("from")).toObject().value(QLatin1String("emailAddress")).toObject().value(QLatin1String("address")).toString();
            const QString sender = m.value(QLatin1String("sender")).toObject().value(QLatin1String("emailAddress")).toObject().value(QLatin1String("address")).toString();
            Q_EMIT self->sendRightLearned(mailbox, from.compare(sender, Qt::CaseInsensitive) == 0 ? QStringLiteral("as") : QStringLiteral("behalf"));
        });
    });
}

void OutboxEngine::discard()
{
    auto forget = [this] {
        const std::optional<Outgoing> stored = m_ctx->store->outgoing(m_run.uuid);
        if (stored) {
            for (const OutgoingAttachment &a : stored->attachments) {
                if (!a.blob.isEmpty()) {
                    m_ctx->blobs->remove(a.blob);
                }
            }
        }
        m_ctx->store->removeOutgoing(m_run.uuid);
        const QString uuid = m_run.uuid;
        done();
        Q_EMIT changed(uuid, QStringLiteral("discarded"), {});
    };
    if (m_run.draftId.isEmpty()) {
        forget();
        return;
    }
    request("DELETE", messagePath(), {}, [this, forget](const GraphResponse &response) {
        if (!response.ok() && response.status != 404) {
            error(response);
            return;
        }
        forget();
    });
}

void OutboxEngine::finishSaved()
{
    const int revision = m_run.revision;
    persist([revision](Outgoing &o) {
        // A discard or a newer save meanwhile keeps its state
        if (o.state == Outgoing::State::Pending && o.revision == revision) {
            o.state = Outgoing::State::Saved;
            o.attempts = 0;
            o.nextAttempt = 0;
            o.lastError.clear();
        }
    });
    const QString uuid = m_run.uuid;
    done();
    Q_EMIT changed(uuid, QStringLiteral("saved"), {});
}

void OutboxEngine::finishSent()
{
    const Outgoing sent = m_run;
    if (const std::optional<Outgoing> stored = m_ctx->store->outgoing(sent.uuid)) {
        for (const OutgoingAttachment &a : stored->attachments) {
            if (!a.blob.isEmpty()) {
                m_ctx->blobs->remove(a.blob);
            }
        }
    }
    m_ctx->store->removeOutgoing(sent.uuid);
    done();
    Q_EMIT changed(sent.uuid, QStringLiteral("sent"), {});
    if (!sent.fromMailbox.isEmpty()) {
        learn(sent.fromMailbox, sent.internetMessageId, m_learnAttempts);
    }
}

void OutboxEngine::error(const GraphResponse &response)
{
    const bool transient = response.isNetworkError() || response.status == 408 || response.status == 429 || response.status >= 500;
    const qint64 now = m_ctx->now ? m_ctx->now() : QDateTime::currentMSecsSinceEpoch();
    const QString text = response.userMessage();
    if (transient) {
        const int attempts = m_run.attempts + 1;
        const qint64 next = now + std::min(maxRetryMs, firstRetryMs << std::min(attempts - 1, 10));
        persist([attempts, next, text](Outgoing &o) {
            o.attempts = attempts;
            o.nextAttempt = next;
            o.lastError = text;
        });
        done();
        if (response.isNetworkError()) {
            Q_EMIT networkError();
        }
        return;
    }
    qCWarning(GRAPHMAIL).noquote() << "Exchange refused a message being sent or saved:" << text;
    persist([text](Outgoing &o) {
        o.state = Outgoing::State::Failed;
        // Not sent: it stays a draft the user can open, fix and send again
        o.send = false;
        o.lastError = text;
    });
    const QString uuid = m_run.uuid;
    done();
    Q_EMIT changed(uuid, QStringLiteral("failed"), text);
}

void OutboxEngine::persist(const std::function<void(Outgoing &)> &change)
{
    std::optional<Outgoing> stored = m_ctx->store->outgoing(m_run.uuid);
    if (!stored) {
        return;
    }
    change(*stored);
    m_ctx->store->saveOutgoing(*stored);
}

void OutboxEngine::done()
{
    m_busy = false;
    m_run = {};
}

}
