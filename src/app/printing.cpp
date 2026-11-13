// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printing.h"
#include "mailview.h"
#include "store.h"

#include <KLocalizedString>

#include <QApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QLocale>
#include <QPageLayout>
#include <QPainter>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QStyle>
#include <QTextDocument>
#include <QTreeWidget>
#include <QWebEnginePage>

namespace GraphMail
{

namespace
{
// The paper's colours, whatever the desktop's scheme
QPalette paperPalette()
{
    QPalette palette = QApplication::style()->standardPalette();
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::Window, Qt::white);
    palette.setColor(QPalette::Text, Qt::black);
    palette.setColor(QPalette::WindowText, Qt::black);
    return palette;
}

QString row(const QString &label, const QString &value)
{
    return QStringLiteral("<tr><td style=\"font-weight:bold;padding:1px 14px 1px 0;vertical-align:top;white-space:nowrap\">%1</td><td style=\"padding:1px 0\">%2</td></tr>")
        .arg(label.toHtmlEscaped(), value.toHtmlEscaped());
}

void preview(QWidget *parent, const QString &title, const std::function<void(QPrinter *)> &paint)
{
    QPrintPreviewDialog dialog(parent);
    dialog.setWindowTitle(i18nc("@title:window %1 what is printed", "Print %1", title));
    QObject::connect(&dialog, &QPrintPreviewDialog::paintRequested, &dialog, [&paint](QPrinter *printer) {
        paint(printer);
    });
    dialog.resize(900, 1000);
    dialog.exec();
}
}

QString Printing::messageHeader(const MessageRow &m, const QList<AttachmentRow> &attachments, const QString &mailboxName)
{
    const QString from = m.senderName.isEmpty() ? m.senderAddress
        : m.senderAddress.isEmpty()              ? m.senderName
                                                 : QStringLiteral("%1 <%2>").arg(m.senderName, m.senderAddress);
    const qint64 when = m.sent > 0 ? m.sent : m.received;
    QString rows = row(i18nc("@label printed message header", "From:"), from)
        + row(i18nc("@label printed message header", "Sent:"), QLocale().toString(QDateTime::fromMSecsSinceEpoch(when), QLocale::LongFormat));
    if (!m.toList.isEmpty()) {
        rows += row(i18nc("@label printed message header", "To:"), m.toList);
    }
    if (!m.ccList.isEmpty()) {
        rows += row(i18nc("@label printed message header", "Cc:"), m.ccList);
    }
    rows += row(i18nc("@label printed message header", "Subject:"), m.subject);
    QStringList names;
    for (const AttachmentRow &a : attachments) {
        if (!a.isInline) {
            names.append(i18nc("@item attachment name, size", "%1 (%2)", a.name, QLocale().formattedDataSize(a.size)));
        }
    }
    if (!names.isEmpty()) {
        rows += row(i18nc("@label printed message header", "Attachments:"), names.join(QStringLiteral("; ")));
    }
    return QStringLiteral("<div style=\"all:initial;display:block;font-family:sans-serif;color:#000;background:#fff;margin:0 0 14px 0\">"
                          "<div style=\"font-size:15pt;font-weight:bold;padding-bottom:4px;border-bottom:3px solid #000;margin-bottom:8px\">%1</div>"
                          "<table style=\"font-family:sans-serif;font-size:10pt;color:#000;border-collapse:collapse\">%2</table></div>")
        .arg(mailboxName.toHtmlEscaped(), rows);
}

void Printing::printMessage(QWidget *parent, Store *store, BlobStore *blobs, qint64 message, bool allowRemote, const QString &mailboxName, const QString &pdfFile,
                            const std::function<void(bool)> &done)
{
    const std::optional<MessageRow> m = store ? store->message(message) : std::nullopt;
    if (!m || m->bodyBlob.isEmpty()) {
        if (done) {
            done(false);
        }
        return;
    }
    // A view of its own, never shown: the reading pane stays as it is
    auto *view = new MailView;
    view->setAttribute(Qt::WA_DontShowOnScreen);
    view->resize(800, 1100);
    QObject::connect(view, &QWebEngineView::loadFinished, view, [view, parent, pdfFile, done, subject = m->subject](bool ok) {
        if (!ok) {
            view->deleteLater();
            if (done) {
                done(false);
            }
            return;
        }
        if (!pdfFile.isEmpty()) {
            QObject::connect(view->page(), &QWebEnginePage::pdfPrintingFinished, view, [view, done](const QString &, bool written) {
                view->deleteLater();
                if (done) {
                    done(written);
                }
            });
            view->page()->printToPdf(pdfFile, QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Portrait, QMarginsF(15, 15, 15, 15), QPageLayout::Millimeter));
            return;
        }
        preview(parent, subject, [view](QPrinter *printer) {
            // The view prints asynchronously; the preview needs the pages now
            QEventLoop loop;
            QObject::connect(view, &QWebEngineView::printFinished, &loop, &QEventLoop::quit);
            view->print(printer);
            loop.exec();
        });
        view->deleteLater();
        if (done) {
            done(true);
        }
    });
    view->showForPrinting(store, blobs, message, allowRemote, messageHeader(*m, store->attachments(message), mailboxName));
}

void Printing::print(QWidget *parent, const QString &title, const std::function<void(QPainter *, QPrinter *)> &paint, const QString &pdfFile)
{
    auto run = [&paint](QPrinter *printer) {
        QPainter painter(printer);
        paint(&painter, printer);
    };
    if (!pdfFile.isEmpty()) {
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(pdfFile);
        printer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Landscape, QMarginsF(12, 12, 12, 12), QPageLayout::Millimeter));
        run(&printer);
        return;
    }
    preview(parent, title, run);
}

void Printing::printHtml(QWidget *parent, const QString &title, const QString &html, const QString &pdfFile)
{
    QTextDocument document;
    document.setHtml(html);
    auto run = [&document](QPrinter *printer) {
        document.print(printer);
    };
    if (!pdfFile.isEmpty()) {
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(pdfFile);
        printer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Portrait, QMarginsF(15, 15, 15, 15), QPageLayout::Millimeter));
        run(&printer);
        return;
    }
    preview(parent, title, run);
}

QString Printing::treeHtml(const QTreeWidget *tree, const QString &title)
{
    QString head;
    QList<int> columns;
    for (int c = 0; c < tree->columnCount(); ++c) {
        if (!tree->isColumnHidden(c)) {
            columns.append(c);
            head += QStringLiteral("<th style=\"text-align:left;border-bottom:2px solid #000;padding:3px 10px 3px 0\">%1</th>").arg(tree->headerItem()->text(c).toHtmlEscaped());
        }
    }
    QString rows;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *item = tree->topLevelItem(i);
        if (item->isHidden()) {
            continue;
        }
        rows += QStringLiteral("<tr>");
        for (int c : std::as_const(columns)) {
            QString text = item->text(c).toHtmlEscaped();
            if (c == 0 && (item->flags() & Qt::ItemIsUserCheckable)) {
                text = (item->checkState(0) == Qt::Checked ? QStringLiteral("&#9745; ") : QStringLiteral("&#9744; ")) + text;
            }
            rows += QStringLiteral("<td style=\"border-bottom:1px solid #bbb;padding:3px 10px 3px 0;vertical-align:top\">%1</td>").arg(text);
        }
        rows += QStringLiteral("</tr>");
    }
    return QStringLiteral("<html><body style=\"font-family:sans-serif;font-size:10pt;color:#000\"><h2 style=\"border-bottom:3px solid #000;padding-bottom:4px\">%1</h2>"
                          "<table cellspacing=\"0\" style=\"border-collapse:collapse;width:100%\"><tr>%2</tr>%3</table></body></html>")
        .arg(title.toHtmlEscaped(), head, rows);
}

void Printing::renderLight(QWidget *widget, QPainter *painter, const QRectF &target)
{
    if (!widget || widget->width() <= 0 || widget->height() <= 0) {
        return;
    }
    const QPalette before = widget->palette();
    const bool own = widget->testAttribute(Qt::WA_SetPalette);
    widget->setPalette(paperPalette());
    const qreal scale = std::min(target.width() / widget->width(), target.height() / widget->height());
    painter->save();
    painter->translate(target.topLeft());
    painter->scale(scale, scale);
    widget->render(painter, QPoint(), QRegion(), QWidget::DrawChildren);
    painter->restore();
    if (own) {
        widget->setPalette(before);
    } else {
        widget->setPalette(QPalette());
        widget->setAttribute(Qt::WA_SetPalette, false);
    }
}

}
