// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "unsubscribe.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

namespace GraphMail::Unsubscribe
{

Way choose(const QString &header, bool oneClick)
{
    // "<mailto:list@example.com?subject=unsubscribe>, <https://example.com/u/123>": the links in
    // angle brackets, in the list's order of preference
    static const QRegularExpression link(QStringLiteral("<([^>]*)>"));
    QUrl web;
    QUrl secureWeb;
    QUrl mail;
    for (auto it = link.globalMatch(header); it.hasNext();) {
        // Long links may be folded over lines, the spaces are not part of them
        QString text = it.next().captured(1);
        text.remove(QRegularExpression(QStringLiteral("\\s")));
        const QUrl url(text, QUrl::StrictMode);
        if (!url.isValid()) {
            continue;
        }
        const QString scheme = url.scheme().toLower();
        if (scheme == QLatin1String("mailto") && mail.isEmpty() && !url.path().isEmpty()) {
            mail = url;
        } else if (scheme == QLatin1String("https") && secureWeb.isEmpty() && !url.host().isEmpty()) {
            secureWeb = url;
        } else if (scheme == QLatin1String("http") && web.isEmpty() && !url.host().isEmpty()) {
            web = url;
        }
    }
    if (oneClick && !secureWeb.isEmpty()) {
        return {Kind::OneClick, secureWeb};
    }
    if (!mail.isEmpty()) {
        return {Kind::Mail, mail};
    }
    if (!secureWeb.isEmpty()) {
        return {Kind::Browser, secureWeb};
    }
    if (!web.isEmpty()) {
        return {Kind::Browser, web};
    }
    return {};
}

void post(const QUrl &url, QObject *context, const std::function<void(bool ok, const QString &detail)> &done)
{
    auto *manager = new QNetworkAccessManager(context);
    // No cookie jar is shared with anything: the request carries no context of the user
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    QNetworkReply *reply = manager->post(request, QByteArrayLiteral("List-Unsubscribe=One-Click"));
    QObject::connect(reply, &QNetworkReply::finished, context, [reply, manager, done] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
        const QString detail = ok ? QString() : (status > 0 ? QStringLiteral("HTTP %1").arg(status) : reply->errorString());
        reply->deleteLater();
        manager->deleteLater();
        done(ok, detail);
    });
}

}
