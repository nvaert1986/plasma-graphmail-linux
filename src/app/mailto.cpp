// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mailto.h"

#include <QUrl>
#include <QUrlQuery>

namespace GraphMail::Mailto
{

namespace
{
// Addresses separated by commas (RFC 6068 2), percent-decoded, empty ones left out
QStringList addresses(const QString &text)
{
    QStringList out;
    for (const QString &part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString address = QUrl::fromPercentEncoding(part.trimmed().toUtf8()).trimmed();
        if (!address.isEmpty()) {
            out.append(address);
        }
    }
    return out;
}
}

bool isMailto(const QString &argument)
{
    return argument.startsWith(QLatin1String("mailto:"), Qt::CaseInsensitive);
}

Link parse(const QUrl &url)
{
    Link link;
    if (url.scheme().compare(QLatin1String("mailto"), Qt::CaseInsensitive) != 0) {
        return link;
    }
    link.to = addresses(url.path(QUrl::FullyEncoded));
    // Pairs by hand: a "+" stays a plus (RFC 6068 is not form encoding)
    const QStringList pairs = url.query(QUrl::FullyEncoded).split(QLatin1Char('&'), Qt::SkipEmptyParts);
    for (const QString &pair : pairs) {
        const QString key = QUrl::fromPercentEncoding(pair.section(QLatin1Char('='), 0, 0).toUtf8()).toLower();
        const QString raw = pair.section(QLatin1Char('='), 1);
        const QString value = QUrl::fromPercentEncoding(raw.toUtf8());
        if (key == QLatin1String("to")) {
            link.to += addresses(raw);
        } else if (key == QLatin1String("cc")) {
            link.cc += addresses(raw);
        } else if (key == QLatin1String("bcc")) {
            link.bcc += addresses(raw);
        } else if (key == QLatin1String("subject")) {
            link.subject = value;
        } else if (key == QLatin1String("body")) {
            // Line breaks are %0D%0A in the link
            link.body = QString(value).replace(QLatin1String("\r\n"), QLatin1String("\n"));
        } else if (key == QLatin1String("attach") || key == QLatin1String("attachment")) {
            const QUrl file = value.startsWith(QLatin1Char('/')) ? QUrl::fromLocalFile(value) : QUrl(value);
            if (file.isLocalFile()) {
                link.attachments.append(file.toLocalFile());
            }
        }
    }
    return link;
}

}
