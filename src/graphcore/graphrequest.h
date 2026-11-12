// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "graphresponse.h"

#include <QObject>
#include <QTimer>
#include <QUrl>

class QIODevice;
class QNetworkReply;

namespace GraphCore
{

class GraphClient;

struct GraphRequestSpec {
    QByteArray verb = "GET";
    // A Graph path ("/me/messages") or an absolute URL (nextLink, deltaLink, upload session)
    QString pathOrUrl;
    QByteArray body;
    HeaderList headers;
    // false for pre-authenticated URLs (upload sessions): those must never carry our token
    bool authenticated = true;
    // Receives the body of a 2xx answer instead of GraphResponse::body (downloads). A QFile or
    // QBuffer is emptied again when a download has to start over.
    QIODevice *sink = nullptr;
    // Attempts for a failing (5xx, network) request; 0: the client's policy. A write the user
    // waits for in a dialog gets few, so a refusal comes back quickly.
    int maxAttempts = 0;
};

// One request with GraphClient's rules: follows redirects (the token never leaves the Graph host),
// refreshes the token once after a 401, retries network failures and 5xx with backoff, and on
// 429/503 holds back every request to the same mailbox (or account) until Retry-After has passed.
// At most four requests per mailbox run at once, process-wide. Never blocks: waits are timers and
// token refreshes run on the thread pool. Created by GraphClient::start(), owned by the client;
// lives in the client's thread.
class GraphRequest : public QObject
{
    Q_OBJECT

public:
    ~GraphRequest() override;

    bool isFinished() const;
    GraphResponse response() const;
    // Gives up; finished() follows with status 0 and "cancelled"
    void abort();

Q_SIGNALS:
    // Emitted once, also after abort()
    void finished();

private:
    friend class GraphClient;
    GraphRequest(GraphClient *client, const GraphRequestSpec &spec);

    void next();
    void sendWithToken(const QString &token);
    void onReplyFinished();
    void drain();
    void finish(const GraphResponse &response);
    bool cancelled() const;
    bool needsToken() const;
    void releaseSlot();

    GraphClient *m_client;
    GraphRequestSpec m_spec;
    QUrl m_url;
    QString m_throttleKey;
    int m_slotLimit = 0;
    bool m_hasSlot = false;
    int m_attempts = 0;
    int m_redirects = 0;
    bool m_refreshedAfter401 = false;
    bool m_forceRefresh = false;
    bool m_paced = false;
    bool m_aborted = false;
    bool m_finished = false;
    // Backoff before the next attempt (ms since the epoch)
    qint64 m_notBeforeMs = 0;
    QNetworkReply *m_reply = nullptr;
    // Polls the client's cancel flag while a reply is in flight
    QTimer m_watchdog;
    GraphResponse m_response;
};

}
