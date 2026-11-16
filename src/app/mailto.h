// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

class QUrl;

// mailto: links (RFC 6068), so Graph Mail can be the desktop's mail program: links on web pages,
// xdg-email and "send as e-mail" in other programs. attach/attachment (not in the RFC, used by
// xdg-email, Thunderbird and KMail) name local files to attach; the caller asks the user first.
namespace GraphMail::Mailto
{

struct Link {
    QStringList to;
    QStringList cc;
    QStringList bcc;
    QString subject;
    QString body;
    // Local paths
    QStringList attachments;
};

bool isMailto(const QString &argument);
Link parse(const QUrl &url);

}
