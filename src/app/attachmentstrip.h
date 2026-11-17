// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "store.h"

#include <QListWidget>

namespace GraphMail
{

// The attachments of the message on screen, as in Outlook: a row of boxes (file type icon, name,
// size) under the header. Inline images are part of the body and not listed. A click opens an
// attachment, the context menu also saves it or all of them.
class AttachmentStrip : public QListWidget
{
    Q_OBJECT

public:
    explicit AttachmentStrip(QWidget *parent = nullptr);

    // Returns whether anything is shown (also "Loading attachments..." while the list is not
    // in the cache yet)
    bool setAttachments(const QList<AttachmentRow> &attachments, bool listPending);

    // Size as Plasma shows it ("526.4 KB")
    static QString formatSize(qint64 bytes);
    // The icon of a file type by name, as Dolphin shows it
    static QIcon iconFor(const QString &name, const QString &contentType);

Q_SIGNALS:
    void openRequested(qint64 attachment);
    void previewRequested(qint64 attachment);
    void saveRequested(qint64 attachment);
    // Save into a folder Graph Sync syncs (OneDrive, SharePoint); only offered when it is installed
    void saveToCloudRequested(qint64 attachment);
    void saveAllRequested();

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void fitHeight();
};

}
