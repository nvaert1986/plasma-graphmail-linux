// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mailview.h"
#include "blobstore.h"
#include "graphmail_debug.h"
#include "store.h"

#include <KLocalizedString>

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QMenu>
#include <QMimeDatabase>
#include <QOpenGLWidget>
#include <QRegularExpression>
#include <QWebEngineContextMenuRequest>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>

#include <atomic>

namespace GraphMail
{

namespace
{
const QByteArray bodyScheme = QByteArrayLiteral("graphmail");
const QByteArray cidScheme = QByteArrayLiteral("cid");

// The page the view loads; refuses every navigation except its own loads
class MailPage : public QWebEnginePage
{
public:
    using QWebEnginePage::QWebEnginePage;

protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override
    {
        if (url.scheme() == QLatin1String(bodyScheme) || url.scheme() == QLatin1String("about") || url.scheme() == QLatin1String("data")) {
            return type != NavigationTypeLinkClicked;
        }
        // A link the user clicked (or a form): never inside the reading pane
        if (type == NavigationTypeLinkClicked && isMainFrame
            && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("mailto"))) {
            QDesktopServices::openUrl(url);
        }
        return false;
    }
    QWebEnginePage *createWindow(WebWindowType) override
    {
        // target="_blank" links come here: refuse the window (the click itself does nothing)
        return nullptr;
    }
};
}

// Serves graphmail:body/<message id> (the cached body) and cid:<content id> (inline images)
class MailSchemeHandler : public QWebEngineUrlSchemeHandler
{
public:
    using QWebEngineUrlSchemeHandler::QWebEngineUrlSchemeHandler;

    void setMessage(Store *store, BlobStore *blobs, qint64 message)
    {
        m_store = store;
        m_blobs = blobs;
        m_message = message;
        m_printHeader.clear();
    }

    // Printing (docs/PHASE9B.md): this block goes in front of the body, on white paper colours
    void setPrintHeader(const QString &html)
    {
        m_printHeader = html;
    }

    void requestStarted(QWebEngineUrlRequestJob *job) override
    {
        const QUrl url = job->requestUrl();
        qCDebug(GRAPHMAIL) << "Mail view request" << url;
        if (!m_store || !m_blobs) {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }
        if (url.scheme() == QLatin1String(bodyScheme)) {
            const std::optional<MessageRow> m = m_store->message(m_message);
            QString error;
            const std::optional<QByteArray> body = (m && !m->bodyBlob.isEmpty()) ? m_blobs->get(m->bodyBlob, &error) : std::nullopt;
            if (!body) {
                job->fail(QWebEngineUrlRequestJob::UrlNotFound);
                return;
            }
            const bool printing = !m_printHeader.isEmpty();
            QByteArray html = m->bodyType == 1 ? *body : plainToHtml(QString::fromUtf8(*body), printing).toUtf8();
            if (printing) {
                // Right after <body ...>, else at the start
                static const QRegularExpression bodyTag(QStringLiteral("<body[^>]*>"), QRegularExpression::CaseInsensitiveOption);
                QString page = QString::fromUtf8(html);
                const QRegularExpressionMatch match = bodyTag.match(page);
                page.insert(match.hasMatch() ? match.capturedEnd() : 0, m_printHeader);
                html = page.toUtf8();
            }
            reply(job, QByteArrayLiteral("text/html;charset=utf-8"), html);
            return;
        }
        // cid:image001.png@01DD658D.988833C0 (sometimes with <...> or percent-encoding)
        QString contentId = QUrl::fromPercentEncoding(url.toString(QUrl::RemoveScheme).toUtf8());
        contentId.remove(QLatin1Char('<')).remove(QLatin1Char('>'));
        const QList<AttachmentRow> attachments = m_store->attachments(m_message);
        for (const AttachmentRow &a : attachments) {
            QString id = a.contentId;
            id.remove(QLatin1Char('<')).remove(QLatin1Char('>'));
            if (!a.blob.isEmpty() && id.compare(contentId, Qt::CaseInsensitive) == 0) {
                QString error;
                if (const std::optional<QByteArray> data = m_blobs->get(a.blob, &error)) {
                    const QByteArray type = a.contentType.isEmpty() ? QMimeDatabase().mimeTypeForData(*data).name().toLatin1() : a.contentType.toLatin1();
                    reply(job, type, *data);
                    return;
                }
            }
        }
        qCDebug(GRAPHMAIL) << "No inline image for" << url << "among" << attachments.size() << "attachments";
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
    }

private:
    // A text/plain body in the desktop's font and colours (black on white for paper), web and mail
    // addresses clickable
    static QString plainToHtml(const QString &text, bool printing = false)
    {
        static const QRegularExpression links(QStringLiteral("(https?://[^\\s<>\"']+|mailto:[^\\s<>\"']+)"));
        QString html = text.toHtmlEscaped();
        html.replace(links, QStringLiteral("<a href=\"\\1\">\\1</a>"));
        const QPalette palette = QApplication::palette();
        return QStringLiteral("<html><body style=\"margin: 12px; background: %1; color: %2; font-family: '%3'; font-size: %4pt\">"
                              "<div style=\"white-space: pre-wrap; word-wrap: break-word\">%5</div><style>a { color: %6 }</style></body></html>")
            .arg(printing ? QStringLiteral("#ffffff") : palette.color(QPalette::Base).name(),
                 printing ? QStringLiteral("#000000") : palette.color(QPalette::Text).name(),
                 QApplication::font().family(),
                 QString::number(QApplication::font().pointSizeF()),
                 html,
                 printing ? QStringLiteral("#0645ad") : palette.color(QPalette::Link).name());
    }

    static void reply(QWebEngineUrlRequestJob *job, const QByteArray &type, const QByteArray &data)
    {
        auto *buffer = new QBuffer(job);
        buffer->setData(data);
        buffer->open(QIODevice::ReadOnly);
        job->reply(type, buffer);
    }

    Store *m_store = nullptr;
    BlobStore *m_blobs = nullptr;
    qint64 m_message = 0;
    QString m_printHeader;
};

// Runs on Chromium's IO thread: only an atomic flag
class RemoteContentBlocker : public QWebEngineUrlRequestInterceptor
{
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;

    void setAllowRemote(bool allow)
    {
        m_allow = allow;
    }

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        const QString scheme = info.requestUrl().scheme();
        if (scheme == QLatin1String(bodyScheme) || scheme == QLatin1String(cidScheme) || scheme == QLatin1String("data")) {
            return;
        }
        if ((scheme == QLatin1String("http") || scheme == QLatin1String("https")) && m_allow.load()) {
            return;
        }
        // file:, ftp:, and http(s) unless allowed
        info.block(true);
    }

private:
    std::atomic_bool m_allow{false};
};

void MailView::registerSchemes()
{
    QWebEngineUrlScheme body(bodyScheme);
    body.setSyntax(QWebEngineUrlScheme::Syntax::Path);
    body.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::LocalScheme | QWebEngineUrlScheme::LocalAccessAllowed);
    QWebEngineUrlScheme::registerScheme(body);
    QWebEngineUrlScheme cid(cidScheme);
    cid.setSyntax(QWebEngineUrlScheme::Syntax::Path);
    cid.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::LocalScheme);
    QWebEngineUrlScheme::registerScheme(cid);
}

MailView::MailView(QWidget *parent)
    : QWebEngineView(parent)
    // No storage name: off the record, nothing kept on disk
    , m_profile(new QWebEngineProfile(this))
    , m_handler(new MailSchemeHandler(this))
    , m_blocker(new RemoteContentBlocker(this))
{
    m_profile->installUrlSchemeHandler(bodyScheme, m_handler);
    m_profile->installUrlSchemeHandler(cidScheme, m_handler);
    m_profile->setUrlRequestInterceptor(m_blocker);
    m_profile->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    m_profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);

    QWebEngineSettings *settings = m_profile->settings();
    settings->setAttribute(QWebEngineSettings::JavascriptEnabled, false);
    settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    settings->setAttribute(QWebEngineSettings::AutoLoadImages, true);
    settings->setAttribute(QWebEngineSettings::ErrorPageEnabled, false);
    settings->setAttribute(QWebEngineSettings::DnsPrefetchEnabled, false);
    settings->setAttribute(QWebEngineSettings::NavigateOnDropEnabled, false);
    settings->setAttribute(QWebEngineSettings::FocusOnNavigationEnabled, false);
    settings->setAttribute(QWebEngineSettings::WebRTCPublicInterfacesOnly, true);
    // Text without a font of its own (often the body of a signature table) gets Outlook's default
    // instead of Chromium's Times New Roman: Aptos, else Calibri or its metric twin Carlito, else the
    // desktop font
    QString standard = QApplication::font().family();
    for (const QString &family : {QStringLiteral("Aptos"), QStringLiteral("Calibri"), QStringLiteral("Carlito")}) {
        if (QFontDatabase::hasFamily(family)) {
            standard = family;
            break;
        }
    }
    settings->setFontFamily(QWebEngineSettings::StandardFont, standard);

    auto *page = new MailPage(m_profile, this);
    // Mail is written for a white page; the reading pane around it follows the theme
    page->setBackgroundColor(Qt::white);
    setPage(page);
}

MailView::~MailView()
{
    // The page uses the profile: delete it first
    delete page();
}

bool MailView::hasRemoteContent(const QByteArray &html)
{
    // href alone is a link, not content: only <link> style sheets count among the hrefs
    static const QRegularExpression styleSheet(QStringLiteral("<link[^>]+href\\s*=\\s*[\"']?\\s*https?:"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression content(QStringLiteral("(?:src|background)\\s*=\\s*[\"']?\\s*https?:|url\\(\\s*[\"']?\\s*https?:"),
                                            QRegularExpression::CaseInsensitiveOption);
    const QString text = QString::fromUtf8(html);
    return content.match(text).hasMatch() || styleSheet.match(text).hasMatch();
}

void MailView::prepareWindow(QWidget *window)
{
    // A hidden OpenGL child makes Qt create the window with a GPU surface from the start
    auto *gl = new QOpenGLWidget(window);
    gl->setObjectName(QStringLiteral("gpuSurfaceHint"));
    gl->setFixedSize(0, 0);
    gl->hide();
}

void MailView::showBody(Store *store, BlobStore *blobs, qint64 message, bool allowRemote)
{
    m_handler->setMessage(store, blobs, message);
    m_blocker->setAllowRemote(allowRemote);
    // The body is served by a local scheme, and Chromium keeps local pages away from the internet
    // unless this is on (found 2026-11-11: "Load Images" loaded nothing). The blocker above still
    // refuses every request while remote content is not allowed.
    m_profile->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, allowRemote);
    // A new URL every time, so Chromium never serves an older page from memory
    load(QUrl(QStringLiteral("graphmail:body/%1/%2").arg(message).arg(++m_generation)));
}

void MailView::showForPrinting(Store *store, BlobStore *blobs, qint64 message, bool allowRemote, const QString &headerHtml)
{
    showBody(store, blobs, message, allowRemote);
    // After showBody, which clears it: the next request of the page gets the header
    m_handler->setPrintHeader(headerHtml);
}

void MailView::showText(const QString &text)
{
    m_handler->setMessage(nullptr, nullptr, 0);
    m_blocker->setAllowRemote(false);
    const QString html = QStringLiteral("<html><body style=\"font-family: sans-serif; color: #555; margin: 16px\">%1</body></html>")
                             .arg(text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    setHtml(html, QUrl(QStringLiteral("graphmail:text")));
}

void MailView::contextMenuEvent(QContextMenuEvent *event)
{
    // Only what makes sense for mail: copy, select all, and links (opened outside)
    QMenu menu(this);
    QWebEngineContextMenuRequest *request = lastContextMenuRequest();
    if (request && !request->selectedText().isEmpty()) {
        menu.addAction(pageAction(QWebEnginePage::Copy));
    }
    if (request && request->linkUrl().isValid()) {
        const QUrl link = request->linkUrl();
        menu.addAction(QIcon::fromTheme(QStringLiteral("internet-web-browser")), i18nc("@action:inmenu", "Open Link in Browser"), this, [link] {
            QDesktopServices::openUrl(link);
        });
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18nc("@action:inmenu", "Copy Link Address"), this, [link] {
            QApplication::clipboard()->setText(link.toString());
        });
    }
    menu.addAction(pageAction(QWebEnginePage::SelectAll));
    menu.exec(event->globalPos());
}

}
