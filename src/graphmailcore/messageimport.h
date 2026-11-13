// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>

namespace GraphCore
{
class GraphClient;
struct GraphResponse;
}

namespace GraphMail
{

// Imports one message into a mail folder (PST and .msg import, docs/PHASE9B.md): creates it as
// received mail, then its file attachments (upload sessions above 3 MB, as the outbox does) and
// its attached messages. When a later step fails the new message is deleted again, so a retry
// never leaves a half message behind. The request is a JSON file the app wrote:
// {"message": {...}, "items": [{"name", "item"}], "files": [{"name", "contentType", "contentId",
// "isInline", "path"}]}; the files hold the attachment bytes.
class MessageImport : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 uploadSessionThreshold = 3 * 1024 * 1024;
    static constexpr qint64 uploadChunk = 12 * 320 * 1024;

    MessageImport(GraphCore::GraphClient *client, const QString &root, const QString &folderGraphId, const QString &requestFile, QObject *parent = nullptr);
    void start();
    // Leave the message out when one with its internetMessageId is in the folder already (answered
    // {"ok": true, "duplicate": true, "id"})
    void setSkipDuplicates(bool skip)
    {
        m_skipDuplicates = skip;
    }

Q_SIGNALS:
    // {"ok", "problem", "detail", "id"}; the object deletes itself afterwards
    void finished(const QJsonObject &answer);

private:
    using Done = std::function<void(const GraphCore::GraphResponse &)>;
    void request(const QByteArray &verb, const QString &path, const QJsonObject &body, const Done &done);
    void create();
    void nextFile();
    void putChunk(const QByteArray &data, const QString &url, qint64 offset);
    void nextItem();
    void fail(const GraphCore::GraphResponse &response);
    void fail(const QString &detail);
    void failWith(const QJsonObject &answer);
    void answer(const QJsonObject &answer);

    GraphCore::GraphClient *m_client;
    QString m_root;
    QString m_folder;
    QString m_requestFile;
    QJsonObject m_request;
    bool m_skipDuplicates = false;
    QJsonArray m_files;
    QJsonArray m_items;
    QString m_messageId;
    int m_next = 0;
};

}
