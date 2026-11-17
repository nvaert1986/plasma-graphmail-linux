// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWebEngineView>

class QWebEngineProfile;

namespace GraphMail
{

class BlobStore;
class Store;
class MailSchemeHandler;
class RemoteContentBlocker;

// The body of a message, rendered by QtWebEngine and locked down:
// - JavaScript, plugins, file access and pop-ups off; an off-the-record profile (no cookies or
//   cache kept between messages);
// - the body itself comes from the cache through the graphmail: scheme (no 2 MB setHtml limit),
//   inline images through cid: (Store attachments by content id);
// - every http(s) request is refused unless remote content is allowed for this message (the user's
//   "Load images", or a trusted sender): images from the internet can tell the sender the mail
//   was opened;
// - clicking a link opens it in the default browser; the view itself never navigates away.
class MailView : public QWebEngineView
{
    Q_OBJECT

public:
    // Must run before QApplication exists (Qt requires custom schemes to be registered early)
    static void registerSchemes();
    // Call before a window holding a MailView is shown. Chromium draws through the GPU; on Wayland
    // Qt otherwise creates the window without GPU support first and rebuilds it once the view
    // starts drawing, which the user sees as the window closing and opening again.
    static void prepareWindow(QWidget *window);

    explicit MailView(QWidget *parent = nullptr);
    ~MailView() override;

    // Shows the message's cached body; remote content only when allowRemote
    void showBody(Store *store, BlobStore *blobs, qint64 message, bool allowRemote);
    // A plain page with this text (no body cached yet, errors)
    void showText(const QString &text);
    // The body with Outlook's print header in front, black on white (docs/PHASE9B.md)
    void showForPrinting(Store *store, BlobStore *blobs, qint64 message, bool allowRemote, const QString &headerHtml);

    // Whether the HTML refers to anything on the internet (images, style sheets, backgrounds)
    static bool hasRemoteContent(const QByteArray &html);

    // View > Zoom (docs/PHASE9B.md 9b.25): the size of every message, remembered
    static double zoom();
    static void setZoom(double factor);
    // A dark desktop theme shows HTML mail dark too (colours inverted, pictures kept), unless the
    // user turns it off; printing never
    static bool darkTheme();
    static bool darkMessages();
    static void setDarkMessages(bool on);

    // A clicked link (http, https, mailto) to the desktop; in link test mode only logged
    // ("link test: opened <scheme> <length> blank=<bool>" in the journal), so tests open nothing
    static void openLink(const QUrl &url, bool newWindow);
    static void setLinkTestMode(bool on);
    // Tests: clicks (real mouse events) the first link whose text contains this
    void clickLinkForTests(const QString &text, int attempts = 20);

Q_SIGNALS:
    // From the right-click menu: Print..., and New Message To... on an e-mail link
    void printRequested();
    void mailtoRequested(const QString &link);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    // Save Image As... (ask where) or Open Image (a file in the cache, opened in the desktop's viewer)
    void downloadImage(bool open);
    bool m_openDownload = false;

    QWebEngineProfile *m_profile;
    MailSchemeHandler *m_handler;
    RemoteContentBlocker *m_blocker;
    int m_generation = 0;
};

}
