// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "conversationmodel.h"
#include "messagelistmodel.h"

#include <QMimeData>

namespace GraphMail
{

// internalId: 0 for a conversation row, conversation index + 1 for a message inside it

ConversationModel::ConversationModel(QObject *parent)
    : QAbstractItemModel(parent)
{
}

QList<ConversationModel::Conversation> ConversationModel::load(int count) const
{
    QList<Conversation> result;
    if (!m_store) {
        return result;
    }
    std::optional<ConversationCursor> cursor;
    while (result.size() < count) {
        const int want = std::min(pageSize, int(count - result.size()));
        const QList<ConversationRow> page = m_store->conversationPage(m_folder, cursor, want, m_focus);
        for (const ConversationRow &row : page) {
            result.append({row, {}, false});
        }
        if (page.size() < want) {
            break;
        }
        cursor = ConversationCursor{page.constLast().latest.received, page.constLast().key};
    }
    return result;
}

void ConversationModel::setFolder(const QString &mailboxKey, Store *store, qint64 folder, bool showRecipients)
{
    beginResetModel();
    m_showRecipients = showRecipients;
    m_mailboxKey = mailboxKey;
    m_store = store;
    m_folder = folder;
    m_conversations = load(pageSize);
    m_atEnd = m_conversations.size() < pageSize;
    endResetModel();
}

void ConversationModel::clear()
{
    beginResetModel();
    m_store = nullptr;
    m_conversations.clear();
    m_atEnd = true;
    endResetModel();
}

void ConversationModel::refresh()
{
    if (!m_store) {
        return;
    }
    const int wanted = std::max(pageSize, int(m_conversations.size()));
    beginResetModel();
    m_conversations = load(wanted);
    m_atEnd = m_conversations.size() < wanted;
    endResetModel();
}

const QList<ListRow> &ConversationModel::messagesOf(int conversation) const
{
    const Conversation &c = m_conversations.at(conversation);
    if (!c.loaded) {
        c.messages = m_store ? m_store->conversationMessages(m_folder, c.row.key, m_focus) : QList<ListRow>();
        c.loaded = true;
    }
    return c.messages;
}

QList<qint64> ConversationModel::messagesOf(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return {};
    }
    if (index.internalId() != 0) {
        return {index.data(MessageListModel::IdRole).toLongLong()};
    }
    QList<qint64> ids;
    const QList<ListRow> &messages = messagesOf(index.row());
    for (const ListRow &m : messages) {
        ids.append(m.id);
    }
    return ids;
}

QModelIndex ConversationModel::index(int row, int column, const QModelIndex &parent) const
{
    if (column != 0 || row < 0) {
        return {};
    }
    if (!parent.isValid()) {
        return row < m_conversations.size() ? createIndex(row, 0, quintptr(0)) : QModelIndex();
    }
    if (parent.internalId() != 0 || row >= messagesOf(parent.row()).size()) {
        return {};
    }
    return createIndex(row, 0, quintptr(parent.row() + 1));
}

QModelIndex ConversationModel::parent(const QModelIndex &child) const
{
    if (!child.isValid() || child.internalId() == 0) {
        return {};
    }
    return createIndex(int(child.internalId() - 1), 0, quintptr(0));
}

int ConversationModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid()) {
        return int(m_conversations.size());
    }
    if (parent.internalId() != 0 || m_conversations.at(parent.row()).row.count < 2) {
        return 0;
    }
    return int(messagesOf(parent.row()).size());
}

int ConversationModel::columnCount(const QModelIndex &) const
{
    return 1;
}

bool ConversationModel::hasChildren(const QModelIndex &parent) const
{
    if (!parent.isValid()) {
        return !m_conversations.isEmpty();
    }
    // Without loading the messages: the count is known
    return parent.internalId() == 0 && m_conversations.at(parent.row()).row.count > 1;
}

QVariant ConversationModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) {
        return {};
    }
    const bool isConversation = index.internalId() == 0;
    const ListRow &r = isConversation ? m_conversations.at(index.row()).row.latest : messagesOf(int(index.internalId() - 1)).at(index.row());
    const ConversationRow *c = isConversation ? &m_conversations.at(index.row()).row : nullptr;
    switch (role) {
    case Qt::DisplayRole:
    case MessageListModel::SubjectRole:
        return r.subject;
    case Qt::ToolTipRole:
        return r.senderAddress;
    case MessageListModel::IdRole:
        return r.id;
    case MessageListModel::SenderNameRole:
        return r.senderName;
    case MessageListModel::SenderAddressRole:
        return r.senderAddress;
    case MessageListModel::PreviewRole:
        return r.preview;
    case MessageListModel::ReceivedRole:
        return r.received;
    case MessageListModel::IsReadRole:
        return c ? !c->anyUnread : r.isRead;
    case MessageListModel::FlagRole:
        return c && c->anyFlagged ? 1 : r.flag;
    case MessageListModel::ImportanceRole:
        return r.importance;
    case MessageListModel::HasAttachmentsRole:
        return c ? c->anyAttachments : r.hasAttachments;
    case MessageListModel::ConversationCountRole:
        return c ? c->count : 0;
    case MessageListModel::ToListRole:
        return r.toList;
    case MessageListModel::ShowRecipientsRole:
        return m_showRecipients;
    case MessageListModel::MailboxKeyRole:
        return m_mailboxKey;
    case MessageListModel::CategoriesRole:
        return r.categories;
    default:
        return {};
    }
}

bool ConversationModel::canFetchMore(const QModelIndex &parent) const
{
    return !parent.isValid() && !m_atEnd && m_store;
}

void ConversationModel::fetchMore(const QModelIndex &parent)
{
    if (!canFetchMore(parent) || m_conversations.isEmpty()) {
        return;
    }
    const ConversationRow &last = m_conversations.constLast().row;
    const QList<ConversationRow> page = m_store->conversationPage(m_folder, ConversationCursor{last.latest.received, last.key}, pageSize, m_focus);
    m_atEnd = page.size() < pageSize;
    if (page.isEmpty()) {
        return;
    }
    beginInsertRows({}, int(m_conversations.size()), int(m_conversations.size() + page.size()) - 1);
    for (const ConversationRow &row : page) {
        m_conversations.append({row, {}, false});
    }
    endInsertRows();
}

Qt::ItemFlags ConversationModel::flags(const QModelIndex &index) const
{
    return QAbstractItemModel::flags(index) | (index.isValid() ? Qt::ItemIsDragEnabled : Qt::NoItemFlags);
}

QStringList ConversationModel::mimeTypes() const
{
    return {MessageListModel::mimeType()};
}

QMimeData *ConversationModel::mimeData(const QModelIndexList &indexes) const
{
    // Dragging a conversation moves all its messages, as in Outlook
    QStringList lines{m_mailboxKey};
    for (const QModelIndex &index : indexes) {
        const QList<qint64> ids = messagesOf(index);
        for (qint64 id : ids) {
            lines.append(QString::number(id));
        }
    }
    auto *data = new QMimeData;
    data->setData(MessageListModel::mimeType(), lines.join(QLatin1Char('\n')).toLatin1());
    return data;
}

}
