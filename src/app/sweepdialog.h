// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QJsonObject>

class QComboBox;
class QLabel;
class QRadioButton;

namespace GraphMail
{

class FolderModel;

// Outlook's Sweep (docs/PHASE9B.md 9b.23): the messages from one sender in a folder deleted or
// moved at once; optionally also every future message (an Inbox rule), or all but the newest
class SweepDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Scope { All, AllAndFuture, KeepNewest };

    // count: the sender's messages in the folder; future: whether a rule can be made (own mailbox)
    SweepDialog(FolderModel *folders, const QString &mailboxKey, const QString &sender, const QString &folderName, int count, bool future,
                QWidget *parent = nullptr);

    Scope scope() const;
    // 0: to Deleted Items, else the folder to move to
    qint64 target() const;

    // The Inbox rule for "any future messages": from this address, to Deleted Items or this folder
    static QJsonObject rule(const QString &address, const QString &folderGraphId, int sequence);

private:
    FolderModel *m_folders;
    QString m_mailboxKey;
    QRadioButton *m_all;
    QRadioButton *m_future;
    QRadioButton *m_keepNewest;
    QComboBox *m_action;
    qint64 m_target = 0;
};

}
