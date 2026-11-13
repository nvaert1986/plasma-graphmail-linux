// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "messageimport.h"
#include "graphclient.h"
#include "graphrequest.h"
#include "graphutil.h"
#include "mailboxaccess.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>

using namespace GraphCore;

namespace GraphMail
{

MessageImport::MessageImport(GraphClient *client, const QString &root, const QString &folderGraphId, const QString &requestFile, QObject *parent)
    : QObject(parent)
    , m_client(client)
    , m_root(root)
    , m_folder(folderGraphId)
    , m_requestFile(requestFile)
{
}

void MessageImport::request(const QByteArray &verb, const QString &path, const QJsonObject &body, const Done &done)
{
    GraphRequestSpec spec;
    spec.verb = verb;
    spec.pathOrUrl = path;
    spec.headers = {Prefer::header({Prefer::immutableId()})};
    if (verb == "POST") {
        spec.headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
        spec.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    }
    GraphRequest *r = m_client->start(spec);
    connect(r, &GraphRequest::finished, this, [r, done] {
        const GraphResponse response = r->response();
        r->deleteLater();
        done(response);
    });
}

void MessageImport::start()
{
    QFile file(m_requestFile);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("cannot read %1").arg(m_requestFile));
        return;
    }
    m_request = QJsonDocument::fromJson(file.readAll()).object();
    m_files = m_request.value(QLatin1String("files")).toArray();
    m_items = m_request.value(QLatin1String("items")).toArray();
    if (m_folder.isEmpty() || !m_request.value(QLatin1String("message")).isObject()) {
        fail(QStringLiteral("no message or folder"));
        return;
    }
    const QString messageId = m_request.value(QLatin1String("message")).toObject().value(QLatin1String("internetMessageId")).toString();
    if (!m_skipDuplicates || messageId.isEmpty()) {
        create();
        return;
    }
    // Exchange takes a second message with the same Message-ID (tested live), so look first
    QString quoted = messageId;
    quoted.replace(QLatin1Char('\''), QLatin1String("''"));
    const QString filter = QString::fromLatin1(QUrl::toPercentEncoding(QStringLiteral("internetMessageId eq '%1'").arg(quoted)));
    request("GET", m_root + QStringLiteral("/mailFolders/") + m_folder + QStringLiteral("/messages?$select=id&$top=1&$filter=") + filter, {}, [this](const GraphResponse &response) {
        const QJsonArray found = response.ok() ? response.json().value(QLatin1String("value")).toArray() : QJsonArray();
        if (!found.isEmpty()) {
            answer({{QStringLiteral("ok"), true}, {QStringLiteral("duplicate"), true}, {QStringLiteral("id"), found.first().toObject().value(QLatin1String("id")).toString()}});
            return;
        }
        // Not found, or the search failed: import (a duplicate is better than a lost message)
        create();
    });
}

void MessageImport::create()
{
    request("POST", m_root + QStringLiteral("/mailFolders/") + m_folder + QStringLiteral("/messages"), m_request.value(QLatin1String("message")).toObject(),
            [this](const GraphResponse &response) {
                if (!response.ok()) {
                    fail(response);
                    return;
                }
                m_messageId = response.json().value(QLatin1String("id")).toString();
                m_next = 0;
                nextFile();
            });
}

void MessageImport::nextFile()
{
    if (m_next >= m_files.size()) {
        m_next = 0;
        nextItem();
        return;
    }
    const QJsonObject f = m_files.at(m_next++).toObject();
    QFile file(f.value(QLatin1String("path")).toString());
    if (!file.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("cannot read the attachment %1").arg(f.value(QLatin1String("name")).toString()));
        return;
    }
    const QByteArray data = file.readAll();
    const QString base = m_root + QStringLiteral("/messages/") + m_messageId + QStringLiteral("/attachments");
    if (data.size() <= uploadSessionThreshold) {
        QJsonObject body{{QStringLiteral("@odata.type"), QStringLiteral("#microsoft.graph.fileAttachment")},
                         {QStringLiteral("name"), f.value(QLatin1String("name"))},
                         {QStringLiteral("contentType"), f.value(QLatin1String("contentType"))},
                         {QStringLiteral("contentBytes"), QString::fromLatin1(data.toBase64())},
                         {QStringLiteral("isInline"), f.value(QLatin1String("isInline")).toBool()}};
        if (const QString cid = f.value(QLatin1String("contentId")).toString(); !cid.isEmpty()) {
            body.insert(QStringLiteral("contentId"), cid);
        }
        request("POST", base, body, [this](const GraphResponse &response) {
            if (!response.ok()) {
                fail(response);
                return;
            }
            nextFile();
        });
        return;
    }
    QJsonObject item{{QStringLiteral("attachmentType"), QStringLiteral("file")},
                     {QStringLiteral("name"), f.value(QLatin1String("name"))},
                     {QStringLiteral("size"), qint64(data.size())},
                     {QStringLiteral("isInline"), f.value(QLatin1String("isInline")).toBool()}};
    if (const QString cid = f.value(QLatin1String("contentId")).toString(); !cid.isEmpty()) {
        item.insert(QStringLiteral("contentId"), cid);
    }
    request("POST", base + QStringLiteral("/createUploadSession"), {{QStringLiteral("AttachmentItem"), item}}, [this, data](const GraphResponse &response) {
        if (!response.ok()) {
            fail(response);
            return;
        }
        putChunk(data, response.json().value(QLatin1String("uploadUrl")).toString(), 0);
    });
}

void MessageImport::putChunk(const QByteArray &data, const QString &url, qint64 offset)
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
    GraphRequest *r = m_client->start(spec);
    connect(r, &GraphRequest::finished, this, [this, r, data, url, offset, length] {
        const GraphResponse response = r->response();
        r->deleteLater();
        if (!response.ok()) {
            fail(response);
            return;
        }
        if (offset + length < data.size()) {
            putChunk(data, url, offset + length);
        } else {
            nextFile();
        }
    });
}

void MessageImport::nextItem()
{
    if (m_next >= m_items.size()) {
        answer({{QStringLiteral("ok"), true}, {QStringLiteral("id"), m_messageId}});
        return;
    }
    const QJsonObject i = m_items.at(m_next++).toObject();
    const QJsonObject body{{QStringLiteral("@odata.type"), QStringLiteral("#microsoft.graph.itemAttachment")},
                           {QStringLiteral("name"), i.value(QLatin1String("name"))},
                           {QStringLiteral("item"), i.value(QLatin1String("item"))}};
    request("POST", m_root + QStringLiteral("/messages/") + m_messageId + QStringLiteral("/attachments"), body, [this](const GraphResponse &response) {
        if (!response.ok()) {
            fail(response);
            return;
        }
        nextItem();
    });
}

void MessageImport::fail(const GraphResponse &response)
{
    failWith({{QStringLiteral("ok"), false}, {QStringLiteral("problem"), accessProblem(response)}, {QStringLiteral("detail"), response.describe()}});
}

void MessageImport::fail(const QString &detail)
{
    failWith({{QStringLiteral("ok"), false}, {QStringLiteral("problem"), QStringLiteral("error")}, {QStringLiteral("detail"), detail}});
}

void MessageImport::failWith(const QJsonObject &a)
{
    if (m_messageId.isEmpty()) {
        answer(a);
        return;
    }
    // Remove the half message; the answer stays the first failure
    request("DELETE", m_root + QStringLiteral("/messages/") + m_messageId, {}, [this, a](const GraphResponse &) {
        answer(a);
    });
}

void MessageImport::answer(const QJsonObject &answer)
{
    Q_EMIT finished(answer);
    deleteLater();
}

}
