// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>

namespace GraphCore
{

using HeaderList = QList<QPair<QByteArray, QByteArray>>;

struct GraphResponse {
    int status = 0; // HTTP status, 0 when the request never got an answer
    QByteArray body;
    QString errorString;
    // Headers of the final answer (after redirects and retries)
    HeaderList headers;

    bool ok() const
    {
        return status >= 200 && status < 300;
    }
    // No answer at all: offline, DNS, timeout, a dropped connection, or cancelled
    bool isNetworkError() const
    {
        return status == 0;
    }
    QJsonObject json() const;
    // Case-insensitive; empty when absent
    QByteArray header(const QByteArray &name) const;
    // Graph error code ("ErrorItemNotFound", "notAllowed", ...) and message, when present
    QString graphErrorCode() const;
    QString describe() const;
};

}
