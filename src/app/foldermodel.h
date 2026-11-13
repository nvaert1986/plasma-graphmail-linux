// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractItemModel>
#include <QHash>

#include <memory>
#include <vector>

namespace GraphMail
{

class MailboxSet;

// An Outlook search folder (docs/PHASE8.md): Exchange keeps its contents, Graph Mail shows them
struct SearchFolderInfo {
    QString id;
    QString name;
    QString filter;
};

// The folder pane: one top node per mailbox, its folders as a tree below it. Well-known folders
// come first in Outlook's order, the rest alphabetically. Unread counts come from the cache
// (Graph's totalItemCount also counts non-mail items, phase 2 exit gate).
class FolderModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    enum Roles {
        UnreadRole = Qt::UserRole + 1,
        MailboxKeyRole,
        FolderIdRole, // 0 for a mailbox node
        WellKnownRole,
        IsMailboxRole,
        // "<mailbox key>" or "<mailbox key>/<folder id>": stable across reloads (expansion, selection)
        PathKeyRole,
        // The "Search Folders" group of a mailbox, and a search folder in it (its Graph id, filter)
        IsSearchGroupRole,
        SearchFolderIdRole,
        SearchFilterRole,
    };

    explicit FolderModel(MailboxSet *mailboxes, QObject *parent = nullptr);
    ~FolderModel() override;

    // Builds the tree again from the caches (folders added, renamed, removed)
    void reload();
    // Outlook hides some system folders (Working Set, Quick Step Settings, RSS, Sync Issues, ...);
    // Graph does not mark them hidden, so they are recognised by role or name. Off by default.
    void setShowAllFolders(bool show);
    static bool isSystemFolder(const QString &name, const QString &wellKnown);
    // The mailbox's Outlook search folders, below its folders
    void setSearchFolders(const QString &mailboxKey, const QList<SearchFolderInfo> &folders);
    // Only the unread counts of one mailbox
    void updateCounts(const QString &mailboxKey);

    QModelIndex indexForPath(const QString &pathKey) const;
    QModelIndex indexForWellKnown(const QString &mailboxKey, const QString &wellKnown) const;
    QModelIndex indexForFolder(const QString &mailboxKey, qint64 folder) const;

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QStringList mimeTypes() const override;
    Qt::DropActions supportedDropActions() const override;
    bool canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent) override;

Q_SIGNALS:
    // Messages dropped on a folder of the same mailbox
    void moveRequested(const QString &mailboxKey, qint64 folder, const QList<qint64> &messages);
    // .msg or .eml files dropped on a mail folder: an import
    void filesDropped(const QString &mailboxKey, qint64 folder, const QStringList &files);

private:
    struct Node {
        bool isMailbox = false;
        QString mailboxKey;
        qint64 folder = 0;
        QString graphId;
        QString name;
        QString wellKnown;
        QString filter;
        int unread = 0;
        Node *parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;
    };
    Node *nodeFor(const QModelIndex &index) const;
    QModelIndex indexOf(Node *node) const;
    void sortChildren(Node *node);
    Node *findNode(Node *node, const std::function<bool(const Node *)> &match) const;

    MailboxSet *m_mailboxes;
    Node m_root;
    bool m_showAll = false;
    QHash<QString, QList<SearchFolderInfo>> m_searchFolders;
};

}
