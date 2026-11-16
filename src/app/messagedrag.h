// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QMimeData>
#include <QTreeView>
#include <QUrl>

#include <functional>

namespace GraphMail
{

// The drag of messages out of the list (docs/PHASE9B.md 9b.33): inside the app the folder pane
// and the Calendar/Tasks buttons read the ids, outside (Dolphin, the desktop) and the composer get
// .eml files. The files are written only when a target asks for them, as KMail does.
class MessageDragData : public QMimeData
{
    Q_OBJECT

public:
    // Writes the messages as files and gives their links; set once by the main window
    using Writer = std::function<QList<QUrl>(const QString &mailboxKey, const QList<qint64> &messages)>;
    static void setWriter(Writer writer);

    MessageDragData(const QString &mailboxKey, const QList<qint64> &messages);
    qsizetype count() const
    {
        return m_messages.size();
    }
    QStringList formats() const override;
    bool hasFormat(const QString &mimeType) const override;

protected:
    QVariant retrieveData(const QString &mimeType, QMetaType type) const override;

private:
    QString m_mailboxKey;
    QList<qint64> m_messages;
    mutable bool m_written = false;
    mutable QList<QUrl> m_urls;
};

// The message list: dragging shows a small envelope with the number of messages, as Outlook does,
// instead of a picture of the rows that hides the folder pane
class MessageListView : public QTreeView
{
    Q_OBJECT

public:
    using QTreeView::QTreeView;
    // The envelope (and the count when there is more than one)
    static QPixmap dragPixmap(int count, qreal ratio);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
};

}
