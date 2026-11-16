// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pst/pstwriter.h"

#include <QObject>
#include <QSet>

class QProgressDialog;
class QWidget;

namespace GraphMail
{

class DaemonClient;
class MailboxSet;

// Export to an Outlook Data File (docs/PHASE9B.md): folders with their subfolders (or the whole
// mailbox) into a new PST. Each message comes from Exchange as MIME, or is rebuilt from the cache
// when Exchange does not answer, as the mbox export does; read state, flag, importance and
// categories come from the cache. The file is written next to the target and renamed when done.
class PstExporter : public QObject
{
    Q_OBJECT

public:
    // folders: the folders to export with their subfolders; empty for the whole mailbox
    PstExporter(MailboxSet *mailboxes, DaemonClient *daemon, const QString &mailboxKey, const QList<qint64> &folders, const QString &file, QWidget *parent);
    void start();
    // Tests: contacts, calendars and To Do lists only, no mail
    void setItemsOnly(bool itemsOnly)
    {
        m_itemsOnly = itemsOnly;
    }
    // Only these messages of the given folders, without their subfolders (Clean Up Old Mail)
    void setMessages(const QSet<qint64> &messages)
    {
        m_only = messages;
    }

    // The messages written from Exchange's own MIME (not rebuilt from the cache, not skipped): only
    // these may be removed afterwards (Cleanup's "Export to PST, then remove")
    const QList<qint64> &savedOriginals() const
    {
        return m_saved;
    }

Q_SIGNALS:
    // items: contacts, calendar items and tasks (a whole mailbox's export)
    void finished(bool ok, int original, int rebuilt, int items, const QString &file, const QString &error);

private:
    struct Job {
        qint64 message;
        quint32 folder;
    };
    void plan(qint64 folder, quint32 parent);
    // Contacts, the mailbox's own calendars and its To Do lists, straight from the cache
    void addItems();
    void next();
    void add(const QByteArray &mime, const Job &job, bool original);
    void finish();

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
    QString m_mailboxKey;
    QList<qint64> m_folders;
    QString m_file;
    QString m_part;
    QWidget *m_parent;
    Pst::PstWriter m_writer;
    quint16 m_keywords = 0;
    QList<Job> m_jobs;
    int m_index = 0;
    int m_original = 0;
    int m_rebuilt = 0;
    int m_items = 0;
    bool m_itemsOnly = false;
    QSet<qint64> m_only;
    bool m_offline = false;
    QList<qint64> m_saved;
    bool m_cancelled = false;
    QString m_error;
    QString m_tempDir;
    QProgressDialog *m_progress = nullptr;
};

}
