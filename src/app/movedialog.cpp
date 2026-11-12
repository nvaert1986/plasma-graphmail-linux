// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "movedialog.h"
#include "foldermodel.h"

#include <KLocalizedString>

#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTreeView>
#include <QVBoxLayout>

namespace GraphMail
{

MoveDialog::MoveDialog(FolderModel *folders, const QString &mailboxKey, QWidget *parent)
    : QDialog(parent)
    , m_proxy(new QSortFilterProxyModel(this))
    , m_filter(new QLineEdit(this))
    , m_tree(new QTreeView(this))
{
    setWindowTitle(i18nc("@title:window", "Move to Folder"));
    m_proxy->setSourceModel(folders);
    m_proxy->setRecursiveFilteringEnabled(true);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setAutoAcceptChildRows(true);

    m_filter->setPlaceholderText(i18nc("@info:placeholder", "Search folders..."));
    m_filter->setClearButtonEnabled(true);
    m_tree->setModel(m_proxy);
    m_tree->setHeaderHidden(true);
    // Only this mailbox: Graph cannot move messages to another mailbox
    const QModelIndex mailbox = m_proxy->mapFromSource(folders->indexForPath(mailboxKey));
    m_tree->setRootIndex(mailbox);
    m_tree->expandAll();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Move"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this, buttons] {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(folder() != 0);
    });
    connect(m_tree, &QTreeView::activated, this, [this] {
        if (folder() != 0) {
            accept();
        }
    });
    connect(m_filter, &QLineEdit::textChanged, this, [this, folders, mailboxKey](const QString &text) {
        m_proxy->setFilterFixedString(text);
        // Filtering resets the root; keep showing only this mailbox
        m_tree->setRootIndex(m_proxy->mapFromSource(folders->indexForPath(mailboxKey)));
        m_tree->expandAll();
        // The first match selected, so Enter moves there
        const QModelIndex first = m_proxy->index(0, 0, m_tree->rootIndex());
        if (first.isValid()) {
            QModelIndex leaf = first;
            while (!text.isEmpty() && m_proxy->rowCount(leaf) > 0 && !leaf.data(Qt::DisplayRole).toString().contains(text, Qt::CaseInsensitive)) {
                leaf = m_proxy->index(0, 0, leaf);
            }
            m_tree->setCurrentIndex(leaf);
        }
    });
    connect(m_filter, &QLineEdit::returnPressed, this, [this] {
        if (folder() != 0) {
            accept();
        }
    });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_filter);
    layout->addWidget(m_tree);
    layout->addWidget(buttons);
    resize(380, 480);
    m_filter->setFocus();
}

qint64 MoveDialog::folder() const
{
    return m_tree->currentIndex().data(FolderModel::FolderIdRole).toLongLong();
}

}
