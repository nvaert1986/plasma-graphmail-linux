// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "store.h"

#include <QAbstractListModel>

#include <functional>

namespace GraphMail
{

// One row of search results (docs/PHASE8.md): results can come from several mailboxes
struct SearchHit {
    QString mailboxKey;
    ListRow row;
    QString folderName;
};

// The message list of one folder (or the results of a search), newest first, loaded in screens of
// 200 rows by keyset paging while the view scrolls (Store::listPage; fast deep in a 250k folder).
class MessageListModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        SenderNameRole,
        SenderAddressRole,
        SubjectRole,
        PreviewRole,
        ReceivedRole,
        IsReadRole,
        FlagRole,
        ImportanceRole,
        HasAttachmentsRole,
        // Conversation view: messages in the conversation (0 in the flat list)
        ConversationCountRole,
        ToListRole,
        // Sent Items, Drafts, Outbox: the row shows who it went to, as in Outlook
        ShowRecipientsRole,
        // Search results: the mailbox and folder each row is in
        MailboxKeyRole,
        FolderNameRole,
        // The message's categories (names)
        CategoriesRole,
    };

    static constexpr int pageSize = 200;

    explicit MessageListModel(QObject *parent = nullptr);

    void setFolder(const QString &mailboxKey, Store *store, qint64 folder, bool showRecipients = false);
    // Focused Inbox (docs/PHASE9.md): -1 every message, 1 Focused, 0 Other; applies from the next setFolder
    void setFocusFilter(int focus)
    {
        m_focus = focus;
    }
    // Search results (newest 500) replace the list; folder 0 searches the whole mailbox
    void setSearch(const QString &mailboxKey, Store *store, const QString &text, qint64 folder);
    // Search results from one or more mailboxes (newest first); run gives them again after a
    // change in a cache, so read and flag states stay current
    void setResults(const std::function<QList<SearchHit>()> &run);
    void clear();
    // Reads the loaded rows again (the daemon changed the cache): keeps how far the list was loaded
    void refresh();

    Store *store() const;
    qint64 folder() const;
    bool isSearch() const;
    // The mailbox of a row: the model's for a folder, the row's own for search results
    QString mailboxKeyOf(int row) const;
    ListRow row(int row) const;
    int rowOfId(qint64 id) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;

    // The drag payload between the message list and the folder pane: "<mailbox key>\n<id>\n..."
    static QString mimeType();

private:
    QList<ListRow> load(int count) const;

    QString m_mailboxKey;
    Store *m_store = nullptr;
    qint64 m_folder = 0;
    int m_focus = -1;
    QString m_search;
    std::function<QList<SearchHit>()> m_run;
    QStringList m_keys;
    QStringList m_folderNames;
    bool m_showRecipients = false;
    QList<ListRow> m_rows;
    bool m_atEnd = true;
};

}
