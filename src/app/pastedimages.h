// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QSet>

#include <functional>

class QNetworkAccessManager;

namespace KPIMTextEdit
{
class RichTextComposer;
}

namespace GraphMail
{

// Pictures pasted from a web page (an Outlook on the web signature, a mail opened in the browser)
// point at the web or carry their data in a data: URL; the editor shows only pictures it holds
// itself, and only those are saved and sent. This fetches them and puts them into the editor.
// blob: and cid: pictures exist only inside the browser: they are reported, not fetched.
class PastedImages : public QObject
{
    Q_OBJECT

public:
    explicit PastedImages(KPIMTextEdit::RichTextComposer *editor);
    // cid: pictures: Outlook on the web's signature names its pictures by Content-ID; the same
    // pictures are inline attachments of mail in the cache (the user's own sent mail)
    void setContentIdResolver(std::function<QByteArray(const QString &contentId)> resolver);

Q_SIGNALS:
    // Pictures that could not be brought in (browser-only links, a download that failed)
    void unavailable(int count);

private:
    void scan();
    void embed(const QString &name, const QByteArray &data);

    KPIMTextEdit::RichTextComposer *m_editor;
    QNetworkAccessManager *m_network = nullptr;
    // Addresses being fetched, or reported as unavailable (not tried again on every change)
    QSet<QString> m_seen;
    bool m_scanQueued = false;
    std::function<QByteArray(const QString &)> m_resolver;
    int m_failed = 0;
    int m_pending = 0;
};

}
