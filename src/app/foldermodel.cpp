// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "foldermodel.h"
#include "avatar.h"
#include "mailboxset.h"
#include "messagelistmodel.h"

#include <KLocalizedString>

#include <QCollator>
#include <QHash>
#include <QIcon>
#include <QMimeData>

namespace GraphMail
{

namespace
{
// Outlook's order of the well-known folders; everything else after them
int rank(const QString &wellKnown)
{
    static const QStringList order{QStringLiteral("inbox"),
                                   QStringLiteral("drafts"),
                                   QStringLiteral("sentitems"),
                                   QStringLiteral("deleteditems"),
                                   QStringLiteral("archive"),
                                   QStringLiteral("junkemail"),
                                   QStringLiteral("outbox")};
    const qsizetype i = order.indexOf(wellKnown);
    return i < 0 ? 100 : int(i);
}

QIcon folderIcon(const QString &wellKnown)
{
    if (wellKnown == QLatin1String("inbox")) {
        return QIcon::fromTheme(QStringLiteral("mail-folder-inbox"));
    }
    if (wellKnown == QLatin1String("drafts")) {
        return QIcon::fromTheme(QStringLiteral("document-edit"));
    }
    if (wellKnown == QLatin1String("sentitems")) {
        return QIcon::fromTheme(QStringLiteral("mail-folder-sent"));
    }
    if (wellKnown == QLatin1String("deleteditems")) {
        return QIcon::fromTheme(QStringLiteral("user-trash"));
    }
    if (wellKnown == QLatin1String("archive")) {
        return QIcon::fromTheme(QStringLiteral("archive-insert"));
    }
    if (wellKnown == QLatin1String("junkemail")) {
        return QIcon::fromTheme(QStringLiteral("mail-mark-junk"));
    }
    if (wellKnown == QLatin1String("outbox")) {
        return QIcon::fromTheme(QStringLiteral("mail-folder-outbox"));
    }
    if (wellKnown == QLatin1String("searchfolders") || wellKnown == QLatin1String("searchfolder")) {
        return QIcon::fromTheme(QStringLiteral("folder-saved-search"));
    }
    return QIcon::fromTheme(QStringLiteral("folder"));
}
}

bool FolderModel::isSystemFolder(const QString &name, const QString &wellKnown)
{
    if (wellKnown == QLatin1String("conversationhistory") || wellKnown == QLatin1String("syncissues")) {
        return true;
    }
    // Folders without a Graph role, in English and Dutch (the languages seen in the mailboxes)
    static const QStringList names{
        QStringLiteral("Working Set"),
        QStringLiteral("Quick Step Settings"),
        QStringLiteral("Instellingen voor snelle stappen"),
        QStringLiteral("RSS Subscriptions"),
        QStringLiteral("RSS Feeds"),
        QStringLiteral("RSS-feeds"),
        QStringLiteral("RSS-kanalen"),
        QStringLiteral("Infected Items"),
        QStringLiteral("Geïnfecteerde items"),
        QStringLiteral("Detected Items"),
        QStringLiteral("Gedetecteerde items"),
        QStringLiteral("Conversation History"),
        QStringLiteral("Gesprekgeschiedenis"),
        QStringLiteral("Sync Issues"),
        QStringLiteral("Problemen met synchronisatie"),
        QStringLiteral("Social Activity Notifications"),
        QStringLiteral("Yammer Root"),
    };
    return names.contains(name, Qt::CaseInsensitive);
}

void FolderModel::setShowAllFolders(bool show)
{
    if (show != m_showAll) {
        m_showAll = show;
        reload();
    }
}

FolderModel::FolderModel(MailboxSet *mailboxes, QObject *parent)
    : QAbstractItemModel(parent)
    , m_mailboxes(mailboxes)
{
    reload();
    // A mailbox's photo (docs/PHASE9.md) replaces its icon
    connect(m_mailboxes, &MailboxSet::photoChanged, this, [this](const QString &key) {
        const QModelIndex index = indexForPath(key);
        if (index.isValid()) {
            Q_EMIT dataChanged(index, index, {Qt::DecorationRole});
        }
    });
}

FolderModel::~FolderModel() = default;

void FolderModel::sortChildren(Node *node)
{
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(node->children.begin(), node->children.end(), [&collator](const std::unique_ptr<Node> &a, const std::unique_ptr<Node> &b) {
        const int ra = rank(a->wellKnown);
        const int rb = rank(b->wellKnown);
        return ra != rb ? ra < rb : collator.compare(a->name, b->name) < 0;
    });
    for (auto &child : node->children) {
        sortChildren(child.get());
    }
}

void FolderModel::reload()
{
    beginResetModel();
    m_root.children.clear();
    const QList<MailboxInfo> mailboxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &info : mailboxes) {
        // A calendar-only mailbox has no folders to show
        if (!info.mail) {
            continue;
        }
        auto mailboxNode = std::make_unique<Node>();
        mailboxNode->isMailbox = true;
        mailboxNode->mailboxKey = info.key();
        mailboxNode->name = info.label();
        mailboxNode->parent = &m_root;

        if (Store *store = m_mailboxes->store(info.key())) {
            const QList<FolderRow> folders = store->folders();
            const QHash<qint64, int> unread = store->unreadCounts();
            QHash<QString, Node *> byGraphId;
            std::vector<std::unique_ptr<Node>> nodes;
            for (const FolderRow &f : folders) {
                auto node = std::make_unique<Node>();
                node->mailboxKey = info.key();
                node->folder = f.id;
                node->graphId = f.graphId;
                node->name = f.name;
                node->wellKnown = f.wellKnown;
                node->unread = unread.value(f.id);
                byGraphId.insert(f.graphId, node.get());
                nodes.push_back(std::move(node));
            }
            // Parents by graph id; folders whose parent is not listed (the mailbox root) go on top
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                const QString parentId = folders.at(qsizetype(i)).parentGraphId;
                Node *parent = byGraphId.value(parentId, mailboxNode.get());
                if (parent == nodes[i].get()) {
                    parent = mailboxNode.get();
                }
                nodes[i]->parent = parent;
            }
            // Move each node into its parent; parents may come after children in the list, which
            // is fine: moving the unique_ptr leaves the node itself (and pointers to it) in place
            for (auto &node : nodes) {
                // A hidden system folder is not attached: it and its subfolders disappear with
                // the vector
                if (!m_showAll && isSystemFolder(node->name, node->wellKnown)) {
                    continue;
                }
                Node *parent = node->parent;
                parent->children.push_back(std::move(node));
            }
        }
        sortChildren(mailboxNode.get());
        // Outlook's search folders, below the folders as in Outlook ("Search Folders")
        if (m_searchFolders.contains(info.key())) {
            auto group = std::make_unique<Node>();
            group->mailboxKey = info.key();
            group->wellKnown = QStringLiteral("searchfolders");
            group->name = i18nc("@item folder pane", "Search Folders");
            group->parent = mailboxNode.get();
            const QList<SearchFolderInfo> searchFolders = m_searchFolders.value(info.key());
            for (const SearchFolderInfo &sf : searchFolders) {
                auto node = std::make_unique<Node>();
                node->mailboxKey = info.key();
                node->wellKnown = QStringLiteral("searchfolder");
                node->graphId = sf.id;
                node->name = sf.name;
                node->filter = sf.filter;
                node->parent = group.get();
                group->children.push_back(std::move(node));
            }
            mailboxNode->children.push_back(std::move(group));
        }
        m_root.children.push_back(std::move(mailboxNode));
    }
    endResetModel();
}

void FolderModel::setSearchFolders(const QString &mailboxKey, const QList<SearchFolderInfo> &folders)
{
    m_searchFolders.insert(mailboxKey, folders);
    reload();
}

void FolderModel::updateCounts(const QString &mailboxKey)
{
    Store *store = m_mailboxes->store(mailboxKey);
    if (!store) {
        return;
    }
    const QHash<qint64, int> unread = store->unreadCounts();
    std::function<void(Node *)> visit = [&](Node *node) {
        for (auto &child : node->children) {
            if (!child->isMailbox && child->mailboxKey == mailboxKey) {
                const int count = unread.value(child->folder);
                if (count != child->unread) {
                    child->unread = count;
                    const QModelIndex index = indexOf(child.get());
                    Q_EMIT dataChanged(index, index, {Qt::DisplayRole, UnreadRole});
                }
            }
            visit(child.get());
        }
    };
    visit(&m_root);
}

FolderModel::Node *FolderModel::nodeFor(const QModelIndex &index) const
{
    return index.isValid() ? static_cast<Node *>(index.internalPointer()) : const_cast<Node *>(&m_root);
}

QModelIndex FolderModel::indexOf(Node *node) const
{
    if (!node || node == &m_root || !node->parent) {
        return {};
    }
    const auto &siblings = node->parent->children;
    for (std::size_t row = 0; row < siblings.size(); ++row) {
        if (siblings[row].get() == node) {
            return createIndex(int(row), 0, node);
        }
    }
    return {};
}

FolderModel::Node *FolderModel::findNode(Node *node, const std::function<bool(const Node *)> &match) const
{
    for (auto &child : node->children) {
        if (match(child.get())) {
            return child.get();
        }
        if (Node *found = findNode(child.get(), match)) {
            return found;
        }
    }
    return nullptr;
}

QModelIndex FolderModel::indexForPath(const QString &pathKey) const
{
    Node *node = findNode(const_cast<Node *>(&m_root), [&pathKey](const Node *n) {
        return (n->isMailbox ? n->mailboxKey : n->mailboxKey + QLatin1Char('/') + QString::number(n->folder)) == pathKey;
    });
    return indexOf(node);
}

QModelIndex FolderModel::indexForWellKnown(const QString &mailboxKey, const QString &wellKnown) const
{
    return indexOf(findNode(const_cast<Node *>(&m_root), [&](const Node *n) {
        return !n->isMailbox && n->mailboxKey == mailboxKey && n->wellKnown == wellKnown;
    }));
}

QModelIndex FolderModel::indexForFolder(const QString &mailboxKey, qint64 folder) const
{
    return indexOf(findNode(const_cast<Node *>(&m_root), [&](const Node *n) {
        return !n->isMailbox && n->mailboxKey == mailboxKey && n->folder == folder;
    }));
}

QModelIndex FolderModel::index(int row, int column, const QModelIndex &parent) const
{
    Node *node = nodeFor(parent);
    if (column != 0 || row < 0 || row >= int(node->children.size())) {
        return {};
    }
    return createIndex(row, column, node->children[std::size_t(row)].get());
}

QModelIndex FolderModel::parent(const QModelIndex &child) const
{
    if (!child.isValid()) {
        return {};
    }
    return indexOf(static_cast<Node *>(child.internalPointer())->parent);
}

int FolderModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0) {
        return 0;
    }
    return int(nodeFor(parent)->children.size());
}

int FolderModel::columnCount(const QModelIndex &) const
{
    return 1;
}

Qt::ItemFlags FolderModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags flags = QAbstractItemModel::flags(index);
    if (index.isValid() && !nodeFor(index)->isMailbox) {
        flags |= Qt::ItemIsDropEnabled;
    }
    return flags;
}

QStringList FolderModel::mimeTypes() const
{
    return {MessageListModel::mimeType()};
}

Qt::DropActions FolderModel::supportedDropActions() const
{
    return Qt::MoveAction;
}

bool FolderModel::canDropMimeData(const QMimeData *data, Qt::DropAction, int, int, const QModelIndex &parent) const
{
    if (!parent.isValid() || !data->hasFormat(MessageListModel::mimeType())) {
        return false;
    }
    const Node *node = nodeFor(parent);
    // Graph moves messages within one mailbox only
    const QString source = QString::fromLatin1(data->data(MessageListModel::mimeType())).section(QLatin1Char('\n'), 0, 0);
    return !node->isMailbox && source == node->mailboxKey;
}

bool FolderModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent)
{
    if (!canDropMimeData(data, action, row, column, parent)) {
        return false;
    }
    const QStringList lines = QString::fromLatin1(data->data(MessageListModel::mimeType())).split(QLatin1Char('\n'));
    QList<qint64> ids;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        ids.append(lines.at(i).toLongLong());
    }
    const Node *node = nodeFor(parent);
    Q_EMIT moveRequested(node->mailboxKey, node->folder, ids);
    // The list changes through the daemon, not through this drop
    return false;
}

QVariant FolderModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return {};
    }
    const Node *node = static_cast<Node *>(index.internalPointer());
    switch (role) {
    case Qt::DisplayRole:
        return node->name;
    case Qt::DecorationRole:
        if (node->isMailbox) {
            const QImage photo = m_mailboxes->photo(node->mailboxKey);
            const std::optional<MailboxInfo> box = photo.isNull() ? std::nullopt : m_mailboxes->mailbox(node->mailboxKey);
            return box ? QIcon(Avatar::pixmap(photo, 32, box->displayName, box->address)) : QIcon::fromTheme(QStringLiteral("mail-client"));
        }
        return folderIcon(node->wellKnown);
    case Qt::ToolTipRole:
        if (!node->isMailbox && node->unread > 0) {
            return QStringLiteral("%1 (%2)").arg(node->name).arg(node->unread);
        }
        return node->name;
    case UnreadRole:
        return node->unread;
    case MailboxKeyRole:
        return node->mailboxKey;
    case FolderIdRole:
        return node->folder;
    case WellKnownRole:
        return node->wellKnown;
    case IsMailboxRole:
        return node->isMailbox;
    case PathKeyRole:
        if (node->wellKnown == QLatin1String("searchfolders")) {
            return QString(node->mailboxKey + QStringLiteral("/searchfolders"));
        }
        if (node->wellKnown == QLatin1String("searchfolder")) {
            return QString(node->mailboxKey + QStringLiteral("/searchfolder:") + node->graphId);
        }
        return node->isMailbox ? node->mailboxKey : node->mailboxKey + QLatin1Char('/') + QString::number(node->folder);
    case IsSearchGroupRole:
        return node->wellKnown == QLatin1String("searchfolders");
    case SearchFolderIdRole:
        return node->wellKnown == QLatin1String("searchfolder") ? node->graphId : QString();
    case SearchFilterRole:
        return node->filter;
    default:
        return {};
    }
}

}
