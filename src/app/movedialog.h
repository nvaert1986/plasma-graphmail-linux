// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QLineEdit;
class QSortFilterProxyModel;
class QTreeView;

namespace GraphMail
{

class FolderModel;

// "Move to Folder": the folders of one mailbox (Graph moves within a mailbox only), with a filter
// field: typing part of a name narrows the tree, Enter takes the selected folder.
class MoveDialog : public QDialog
{
    Q_OBJECT

public:
    MoveDialog(FolderModel *folders, const QString &mailboxKey, QWidget *parent = nullptr);

    // 0 when nothing was chosen
    qint64 folder() const;
    // The mailbox itself was chosen (the top of the folder tree; Move Folder only)
    bool mailboxChosen() const;

private:
    QSortFilterProxyModel *m_proxy;
    QLineEdit *m_filter;
    QTreeView *m_tree;
};

}
