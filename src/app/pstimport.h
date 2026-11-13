// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pst/mapiitems.h"
#include "pst/pstmessaging.h"

#include <QDialog>
#include <QObject>
#include <QStringList>

#include <memory>

class QComboBox;
class QLabel;
class QProgressDialog;
class QTreeWidget;
class QTreeWidgetItem;

namespace GraphMail
{

class DaemonClient;

namespace PstImport
{
// Whether a password matches the PST's stored CRC. The spec does not say which text encoding
// or CRC variant is hashed, so the usual ones are tried (docs/PST-NOTES.md)
bool passwordMatches(const QString &password, quint32 storedCrc);
// Writes the daemon's request for one message into dir (request.json and its attachment
// files); the request file's path, empty when it could not be written
QString writeRequest(const Pst::Message &message, const QHash<quint16, Pst::NamedProperty> &names, const QString &dir, QStringList *skipped);
}

// Imports .msg and .eml files into a mail folder (File > Import Message Files...), one at a time
// through the daemon, with progress; what is no mail or fails is listed
class MessageFileImporter : public QObject
{
    Q_OBJECT

public:
    MessageFileImporter(DaemonClient *daemon, const QString &mailboxKey, const QStringList &files, const QString &folderGraphId, QWidget *parent);
    void start();

Q_SIGNALS:
    void finished(bool ok, int imported, int skipped, int failed, const QStringList &log);

private:
    void next();
    void finish(bool ok);
    void send(const QString &kind, const QJsonObject &args, const QString &dir, const QString &shown, const QStringList &skipped);

    DaemonClient *m_daemon;
    QString m_mailboxKey;
    QStringList m_files;
    QString m_folder;
    // The default To Do list, once asked (for .msg tasks)
    QString m_taskList;
    QWidget *m_parent;
    int m_index = 0;
    int m_imported = 0;
    int m_skipped = 0;
    int m_failed = 0;
    bool m_cancelled = false;
    QStringList m_log;
    QString m_tempDir;
    QProgressDialog *m_progress = nullptr;
};

// File > Import Outlook Data File (docs/PHASE9B.md): the PST's folders with their item counts,
// all chosen to start with; where they go: a new folder named after the file, or the mailbox's
// own folders (folders of the same name are used, as Outlook's import does)
class PstImportDialog : public QDialog
{
    Q_OBJECT

public:
    PstImportDialog(Pst::PstFile *pst, const QString &fileName, const QString &mailboxLabel, QWidget *parent);
    // The chosen folders, parents before their children
    QList<quint32> folders() const;
    // The PST folder each chosen folder was found under (0 for the top)
    QHash<quint32, quint32> parents() const;
    // Empty: into the mailbox's own folders
    QString newFolderName() const;

private:
    void addChildren(QTreeWidgetItem *item, quint32 nid, int depth);

    Pst::PstFile *m_pst;
    QString m_fileName;
    QTreeWidget *m_tree;
    QComboBox *m_target;
};

// Runs an import: creates the folders through the daemon, then each message (mail only; contacts,
// calendar items and tasks are left out and listed), one at a time, with progress and cancel
class PstImporter : public QObject
{
    Q_OBJECT

public:
    // targetParent: the graph id (or well-known name) the folders are made under
    PstImporter(DaemonClient *daemon, const QString &mailboxKey, std::shared_ptr<Pst::PstFile> pst, const QList<quint32> &folders, const QHash<quint32, quint32> &parents,
                const QString &targetParent, const QString &newFolderName, QWidget *parent);
    void start();

Q_SIGNALS:
    void finished(bool ok, int imported, int skipped, int failed, const QStringList &log);

private:
    void nextFolder();
    void nextMessage();
    // Contacts, calendar items and tasks: the container for a folder of that kind, then its items
    void startItemFolder(quint32 nid, MapiItems::Kind kind);
    void importItem(const Pst::Message &message);
    void query(const QString &kind, const QJsonObject &args, const std::function<void(const QJsonObject &)> &done);
    void finish(bool ok);

    DaemonClient *m_daemon;
    QString m_mailboxKey;
    std::shared_ptr<Pst::PstFile> m_pst;
    QList<quint32> m_folders;
    QHash<quint32, quint32> m_parents;
    QString m_targetParent;
    QString m_newFolderName;
    QWidget *m_parent;
    // PST folder -> graph id of the folder made for it
    QHash<quint32, QString> m_made;
    QString m_top;
    int m_folderIndex = 0;
    QList<quint32> m_messages;
    int m_messageIndex = 0;
    QString m_currentFolder;
    QString m_currentName;
    MapiItems::Kind m_currentKind = MapiItems::Kind::Mail;
    int m_imported = 0;
    int m_skipped = 0;
    int m_failed = 0;
    int m_total = 0;
    int m_done = 0;
    bool m_cancelled = false;
    QStringList m_log;
    QString m_tempDir;
    QProgressDialog *m_progress = nullptr;
};

}
