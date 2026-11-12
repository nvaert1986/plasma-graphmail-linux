// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace GraphMail
{

class CategoryList;
class FolderModel;
class MailboxSet;

// Quick Steps (docs/PHASE9.md), as Outlook's: a name and actions run one after the other on the
// selected messages. Outlook keeps them in hidden mailbox items that Graph does not reach, so
// they are kept on this computer (graphmailrc [QuickSteps]). An action is {"type": read, unread,
// flag, complete, clearflag, categorize ("category"), move ("mailbox", "folder", "folderName"),
// delete, archive, reply, replyAll, forward ("to")}.
namespace QuickSteps
{
// The user's, or Outlook's two defaults (Done, Reply & Delete) before any were saved
QJsonArray all();
void save(const QJsonArray &steps);
// What a step does, in words
QString describe(const QJsonObject &step);
QString actionText(const QJsonObject &action);
}

// Manage Quick Steps: the list, new, change, delete, order
class QuickStepsDialog : public QDialog
{
    Q_OBJECT

public:
    QuickStepsDialog(MailboxSet *mailboxes, FolderModel *folders, CategoryList *categories, const QString &mailboxKey, QWidget *parent = nullptr);
    void accept() override;

private:
    void fill();
    bool edit(QJsonObject *step);

    MailboxSet *m_mailboxes;
    FolderModel *m_folders;
    CategoryList *m_categories;
    QString m_mailboxKey;
    QJsonArray m_steps;
    QListWidget *m_list;
};

}
