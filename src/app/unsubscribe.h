// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QUrl>

#include <functional>

class QObject;

// The reading pane's Unsubscribe (docs/PHASE9B.md 9b.22): a mailing list's List-Unsubscribe
// header (RFC 2369), with List-Unsubscribe-Post for one click (RFC 8058)
namespace GraphMail::Unsubscribe
{

enum class Kind {
    None,
    // An https POST of "List-Unsubscribe=One-Click", no page to visit
    OneClick,
    // A message to the list's address, which the user sends
    Mail,
    // The list's own page in the browser
    Browser,
};

struct Way {
    Kind kind = Kind::None;
    QUrl url;
};

// One click when the list offers it over https, else a mail, else the web page
Way choose(const QString &header, bool oneClick);
// The one-click request: no cookies or other context, as RFC 8058 asks. done(ok, detail)
void post(const QUrl &url, QObject *context, const std::function<void(bool ok, const QString &detail)> &done);

}
