// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pastedimages.h"

#include <KPIMTextEdit/RichTextComposer>
#include <KPIMTextEdit/RichTextComposerControler>
#include <KPIMTextEdit/RichTextComposerImages>

#include <QFile>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace GraphMail
{

PastedImages::PastedImages(KPIMTextEdit::RichTextComposer *editor)
    : QObject(editor)
    , m_editor(editor)
{
    // A paste arrives as one change; the scan runs once it is in
    connect(editor->document(), &QTextDocument::contentsChanged, this, [this] {
        if (!m_scanQueued) {
            m_scanQueued = true;
            QTimer::singleShot(0, this, &PastedImages::scan);
        }
    });
}

void PastedImages::setContentIdResolver(std::function<QByteArray(const QString &contentId)> resolver)
{
    m_resolver = std::move(resolver);
    // A signature opened before this was set: its pictures again
    m_seen.clear();
    scan();
}

void PastedImages::scan()
{
    m_scanQueued = false;
    QStringList names;
    for (QTextBlock block = m_editor->document()->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextImageFormat format = it.fragment().charFormat().toImageFormat();
            if (!format.isValid() || m_seen.contains(format.name())) {
                continue;
            }
            const QString scheme = QUrl(format.name()).scheme().toLower();
            if (scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("data") || scheme == QLatin1String("file")
                || scheme == QLatin1String("blob") || scheme == QLatin1String("cid")) {
                names.append(format.name());
            }
        }
    }
    for (const QString &name : std::as_const(names)) {
        if (m_seen.contains(name)) {
            continue;
        }
        m_seen.insert(name);
        const QUrl url(name);
        const QString scheme = url.scheme().toLower();
        if (scheme == QLatin1String("data")) {
            // data:image/png;base64,....
            const QString path = name.section(QLatin1Char(':'), 1);
            const QString payload = path.section(QLatin1Char(','), 1);
            const bool base64 = path.section(QLatin1Char(','), 0, 0).endsWith(QLatin1String(";base64"));
            embed(name, base64 ? QByteArray::fromBase64(payload.toLatin1()) : QByteArray::fromPercentEncoding(payload.toLatin1()));
        } else if (scheme == QLatin1String("file")) {
            QFile file(url.toLocalFile());
            embed(name, file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray());
        } else if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) {
            if (!m_network) {
                m_network = new QNetworkAccessManager(this);
                m_network->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
                m_network->setTransferTimeout(20000);
            }
            ++m_pending;
            QNetworkReply *reply = m_network->get(QNetworkRequest(url));
            connect(reply, &QNetworkReply::finished, this, [this, reply, name] {
                reply->deleteLater();
                --m_pending;
                embed(name, reply->error() == QNetworkReply::NoError ? reply->readAll() : QByteArray());
            });
        } else if (scheme == QLatin1String("cid") && m_resolver) {
            embed(name, m_resolver(name.mid(4)));
        } else {
            // blob: (and cid: without a cached copy) only exist inside the browser that copied them
            embed(name, {});
        }
    }
}

void PastedImages::embed(const QString &name, const QByteArray &data)
{
    const QImage image = QImage::fromData(data);
    if (image.isNull()) {
        ++m_failed;
    } else {
        // A local name: the editor keeps the picture, it is saved with a signature and sent inline
        const QString local = QStringLiteral("pasted-%1.png").arg(QUuid::createUuid().toString(QUuid::Id128).left(12));
        m_editor->composerControler()->composerImages()->loadImage(image, name, local);
        // Every use of the address is replaced: pasting it again fetches it again
        m_seen.remove(name);
    }
    // One report per paste, once its downloads are done
    if (m_pending == 0 && m_failed > 0) {
        Q_EMIT unavailable(m_failed);
        m_failed = 0;
    }
}

}
