// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QString>

#include <functional>

class QPainter;
class QPrinter;
class QRectF;
class QTreeWidget;
class QWidget;

namespace GraphMail
{

class BlobStore;
class Store;
struct AttachmentRow;
struct MessageRow;

// File > Print (docs/PHASE9B.md): a print preview with the printer's own dialog behind it, or
// straight into a PDF file (tests). Paper is black on white also with a dark colour scheme.
namespace Printing
{
// Outlook's memo style: the mailbox's name over a thick line, then From, Sent, To, Cc, Subject
// and the attachments
QString messageHeader(const MessageRow &message, const QList<AttachmentRow> &attachments, const QString &mailboxName);

// A message from the cache with that header; done(ok) when printed, cancelled or written
void printMessage(QWidget *parent, Store *store, BlobStore *blobs, qint64 message, bool allowRemote, const QString &mailboxName, const QString &pdfFile = {},
                  const std::function<void(bool)> &done = {});

// Anything drawn by the caller: paint(painter, printer) for each preview or print
void print(QWidget *parent, const QString &title, const std::function<void(QPainter *, QPrinter *)> &paint, const QString &pdfFile = {});

// An HTML document (contacts, tasks), paged by QTextDocument
void printHtml(QWidget *parent, const QString &title, const QString &html, const QString &pdfFile = {});

// A list as it is shown (its visible rows and columns; a ticked first column as "done") as an
// HTML table with a title, for printHtml
QString treeHtml(const QTreeWidget *tree, const QString &title);

// A widget drawn into the rectangle, scaled to fit, with a light palette while it is drawn
void renderLight(QWidget *widget, QPainter *painter, const QRectF &target);
}

}
