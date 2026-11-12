// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "store.h"

#include <QAbstractItemModel>

namespace GraphMail
{

// The conversation view of a folder (View > Show as Conversations): one row per conversation with
// its newest message, unread/flagged/attachment when any message is, and the count; expanding it
// shows its messages in the folder. Same roles as MessageListModel, so the same delegate draws it.
class ConversationModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    static constexpr int pageSize = 200;

    explicit ConversationModel(QObject *parent = nullptr);

    void setFolder(const QString &mailboxKey, Store *store, qint64 folder, bool showRecipients = false);
    // Focused Inbox (docs/PHASE9.md): -1 every message, 1 Focused, 0 Other; applies from the next setFolder
    void setFocusFilter(int focus)
    {
        m_focus = focus;
    }
    void clear();
    void refresh();

    // The messages a row stands for: all of a conversation's messages, or the one message
    QList<qint64> messagesOf(const QModelIndex &index) const;

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    bool hasChildren(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;

private:
    struct Conversation {
        ConversationRow row;
        // Loaded when the conversation is expanded
        mutable QList<ListRow> messages;
        mutable bool loaded = false;
    };
    const QList<ListRow> &messagesOf(int conversation) const;
    QList<Conversation> load(int count) const;

    QString m_mailboxKey;
    Store *m_store = nullptr;
    qint64 m_folder = 0;
    int m_focus = -1;
    bool m_showRecipients = false;
    QList<Conversation> m_conversations;
    bool m_atEnd = true;
};

}
