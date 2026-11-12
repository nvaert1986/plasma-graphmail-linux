// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <functional>

// A tiny HTTP/1.1 server standing in for graph.microsoft.com in the autotests. Each answer closes
// the connection. Runs in the test's thread: the blocking GraphClient calls spin a local event
// loop, which serves it.
struct FakeRequest {
    QByteArray verb;
    QByteArray target; // path and query
    QHash<QByteArray, QByteArray> headers; // lower-case names
    QByteArray body;
};

struct FakeReply {
    int status = 200;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
    int delayMs = 0;
    // Close the connection without answering (a dropped network)
    bool drop = false;
};

class FakeGraphServer
{
public:
    using Handler = std::function<FakeReply(const FakeRequest &)>;

    FakeGraphServer()
    {
        // Any: reachable as 127.0.0.1 and as localhost (another host name for redirect tests)
        m_server.listen(QHostAddress::Any, 0);
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    read(socket);
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    quint16 port() const
    {
        return m_server.serverPort();
    }
    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1/v1.0").arg(port());
    }
    QString otherHostUrl(const QString &path) const
    {
        return QStringLiteral("http://localhost:%1%2").arg(port()).arg(path);
    }

    Handler handler = [](const FakeRequest &) {
        return FakeReply{200, {{"Content-Type", "application/json"}}, "{}", 0, false};
    };
    QList<FakeRequest> requests;
    int active = 0;
    int maxActive = 0;

private:
    void read(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        FakeRequest request;
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
        request.verb = requestLine.value(0);
        request.target = requestLine.value(1);
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const qsizetype colon = lines.at(i).indexOf(':');
            if (colon > 0) {
                request.headers.insert(lines.at(i).left(colon).trimmed().toLower(), lines.at(i).mid(colon + 1).trimmed());
            }
        }
        const qsizetype length = request.headers.value("content-length", "0").toLongLong();
        if (buffer.size() < headerEnd + 4 + length) {
            return;
        }
        request.body = buffer.mid(headerEnd + 4, length);
        m_buffers.remove(socket);
        requests.append(request);
        ++active;
        maxActive = std::max(maxActive, active);

        const FakeReply reply = handler(request);
        QPointer<QTcpSocket> guard(socket);
        QTimer::singleShot(reply.delayMs, socket, [this, guard, reply] {
            --active;
            if (!guard) {
                return;
            }
            if (reply.drop) {
                guard->abort();
                return;
            }
            QByteArray out = "HTTP/1.1 " + QByteArray::number(reply.status) + " Status\r\n";
            for (const auto &[name, value] : reply.headers) {
                out += name + ": " + value + "\r\n";
            }
            out += "Content-Length: " + QByteArray::number(reply.body.size()) + "\r\nConnection: close\r\n\r\n" + reply.body;
            guard->write(out);
            guard->disconnectFromHost();
        });
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

inline FakeReply jsonReply(int status, const QByteArray &body, QList<QPair<QByteArray, QByteArray>> headers = {})
{
    headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
    return FakeReply{status, headers, body, 0, false};
}
