// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "messagelistmodel.h"

#include <QMimeData>

namespace GraphMail
{

namespace
{
// Search results: one screen, newest first (FTS has no cheap keyset paging)
constexpr int searchLimit = 500;
}

MessageListModel::MessageListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

QString MessageListModel::mimeType()
{
    return QStringLiteral("application/x-graphmail-message-ids");
}

Store *MessageListModel::store() const
{
    return m_store;
}

qint64 MessageListModel::folder() const
{
    return m_folder;
}

bool MessageListModel::isSearch() const
{
    return !m_search.isEmpty() || bool(m_run);
}

QList<ListRow> MessageListModel::load(int count) const
{
    if (!m_store) {
        return {};
    }
    if (!m_search.isEmpty()) {
        return m_store->search(m_search, m_folder, searchLimit);
    }
    QList<ListRow> rows;
    std::optional<PageCursor> cursor;
    while (rows.size() < count) {
        const int want = std::min(pageSize, int(count - rows.size()));
        const QList<ListRow> page = m_store->listPage(m_folder, cursor, want, m_focus);
        rows += page;
        if (page.size() < want) {
            break; // the end of the folder
        }
        cursor = PageCursor{rows.constLast().received, rows.constLast().id};
    }
    return rows;
}

void MessageListModel::setFolder(const QString &mailboxKey, Store *store, qint64 folder, bool showRecipients)
{
    beginResetModel();
    m_run = nullptr;
    m_keys.clear();
    m_folderNames.clear();
    m_showRecipients = showRecipients;
    m_mailboxKey = mailboxKey;
    m_store = store;
    m_folder = folder;
    m_search.clear();
    m_rows = load(pageSize);
    m_atEnd = m_rows.size() < pageSize;
    endResetModel();
}

void MessageListModel::setSearch(const QString &mailboxKey, Store *store, const QString &text, qint64 folder)
{
    beginResetModel();
    m_run = nullptr;
    m_keys.clear();
    m_folderNames.clear();
    m_mailboxKey = mailboxKey;
    m_showRecipients = false;
    m_store = store;
    m_folder = folder;
    m_search = text;
    m_rows = load(0);
    m_atEnd = true;
    endResetModel();
}

void MessageListModel::setResults(const std::function<QList<SearchHit>()> &run)
{
    beginResetModel();
    m_run = run;
    m_search.clear();
    m_showRecipients = false;
    m_folder = 0;
    const QList<SearchHit> hits = m_run();
    m_rows.clear();
    m_keys.clear();
    m_folderNames.clear();
    for (const SearchHit &hit : hits) {
        m_rows.append(hit.row);
        m_keys.append(hit.mailboxKey);
        m_folderNames.append(hit.folderName);
    }
    // The first row's mailbox stands for the list (drag and drop, store()); actions use each row's
    m_mailboxKey = m_keys.value(0);
    m_atEnd = true;
    endResetModel();
}

QString MessageListModel::mailboxKeyOf(int row) const
{
    return m_run ? m_keys.value(row) : m_mailboxKey;
}

void MessageListModel::clear()
{
    beginResetModel();
    m_run = nullptr;
    m_keys.clear();
    m_folderNames.clear();
    m_store = nullptr;
    m_folder = 0;
    m_search.clear();
    m_rows.clear();
    m_atEnd = true;
    endResetModel();
}

void MessageListModel::refresh()
{
    if (m_run) {
        const QList<SearchHit> hits = m_run();
        bool same = hits.size() == m_rows.size();
        for (qsizetype i = 0; same && i < hits.size(); ++i) {
            same = hits.at(i).row.id == m_rows.at(i).id && hits.at(i).mailboxKey == m_keys.at(i);
        }
        if (!same) {
            setResults(m_run);
            return;
        }
        for (qsizetype i = 0; i < hits.size(); ++i) {
            m_rows[i] = hits.at(i).row;
        }
        if (!m_rows.isEmpty()) {
            Q_EMIT dataChanged(index(0), index(int(m_rows.size()) - 1));
        }
        return;
    }
    if (!m_store) {
        return;
    }
    const int wanted = std::max(pageSize, int(m_rows.size()));
    const QList<ListRow> rows = load(wanted);
    // Same ids in the same order: only the content changed (read, flag): no reset, the view
    // keeps its selection and scroll position
    bool sameIds = rows.size() == m_rows.size();
    for (qsizetype i = 0; sameIds && i < rows.size(); ++i) {
        sameIds = rows.at(i).id == m_rows.at(i).id;
    }
    if (sameIds) {
        m_rows = rows;
        if (!m_rows.isEmpty()) {
            Q_EMIT dataChanged(index(0), index(int(m_rows.size()) - 1));
        }
        return;
    }
    beginResetModel();
    m_rows = rows;
    m_atEnd = !m_search.isEmpty() || m_rows.size() < wanted;
    endResetModel();
}

ListRow MessageListModel::row(int row) const
{
    return m_rows.value(row);
}

int MessageListModel::rowOfId(qint64 id) const
{
    for (qsizetype i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).id == id) {
            return int(i);
        }
    }
    return -1;
}

int MessageListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant MessageListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) {
        return {};
    }
    const ListRow &r = m_rows.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return r.subject;
    case Qt::ToolTipRole:
        return r.senderAddress;
    case IdRole:
        return r.id;
    case SenderNameRole:
        return r.senderName;
    case SenderAddressRole:
        return r.senderAddress;
    case SubjectRole:
        return r.subject;
    case PreviewRole:
        return r.preview;
    case ReceivedRole:
        return r.received;
    case IsReadRole:
        return r.isRead;
    case FlagRole:
        return r.flag;
    case ImportanceRole:
        return r.importance;
    case HasAttachmentsRole:
        return r.hasAttachments;
    case ToListRole:
        return r.toList;
    case MailboxKeyRole:
        return mailboxKeyOf(index.row());
    case CategoriesRole:
        return m_rows.at(index.row()).categories;
    case FolderNameRole:
        return m_folderNames.value(index.row());
    case ShowRecipientsRole:
        return m_showRecipients;
    default:
        return {};
    }
}

bool MessageListModel::canFetchMore(const QModelIndex &parent) const
{
    return !parent.isValid() && !m_atEnd && m_store;
}

void MessageListModel::fetchMore(const QModelIndex &parent)
{
    if (!canFetchMore(parent) || m_rows.isEmpty()) {
        return;
    }
    const QList<ListRow> page = m_store->listPage(m_folder, PageCursor{m_rows.constLast().received, m_rows.constLast().id}, pageSize, m_focus);
    m_atEnd = page.size() < pageSize;
    if (page.isEmpty()) {
        return;
    }
    beginInsertRows({}, int(m_rows.size()), int(m_rows.size() + page.size()) - 1);
    m_rows += page;
    endInsertRows();
}

Qt::ItemFlags MessageListModel::flags(const QModelIndex &index) const
{
    return QAbstractListModel::flags(index) | (index.isValid() ? Qt::ItemIsDragEnabled : Qt::NoItemFlags);
}

QStringList MessageListModel::mimeTypes() const
{
    return {mimeType()};
}

QMimeData *MessageListModel::mimeData(const QModelIndexList &indexes) const
{
    // One mailbox per drag: search results from other mailboxes than the first row's stay out
    const QString key = indexes.isEmpty() ? m_mailboxKey : mailboxKeyOf(indexes.first().row());
    QStringList ids{key};
    for (const QModelIndex &index : indexes) {
        if (mailboxKeyOf(index.row()) == key) {
            ids.append(QString::number(index.data(IdRole).toLongLong()));
        }
    }
    auto *data = new QMimeData;
    data->setData(mimeType(), ids.join(QLatin1Char('\n')).toLatin1());
    return data;
}

}
