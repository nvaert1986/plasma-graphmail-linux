// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

class QProgressDialog;
class QWidget;

namespace GraphMail
{

class BlobStore;
class DaemonClient;
class MailboxSet;
class Store;

// Export (docs/PHASE9B.md): Save As .eml and mbox. The original MIME comes from Exchange; without
// it (offline, an outage) the message is rebuilt from the cache and says so in a header.
namespace MimeExport
{
// The message as RFC 5322 text from what the cache holds: headers, the body (HTML or text) and the
// attachments that are on this computer; X-GraphMail-Rebuilt tells it is not the original
QByteArray rebuild(Store *store, BlobStore *blobs, qint64 message);
// One mbox entry (mboxrd): the "From " line, the message with LF line ends and ">From " escaping,
// a blank line after it
QByteArray mboxEntry(const QByteArray &mime, const QString &sender, const QDateTime &when);
// "=?UTF-8?B?...?=" when the text is not plain ASCII
QByteArray encodeWord(const QString &text);
}

// Runs one export: every message fetched as MIME through the daemon, rebuilt when that fails (and
// rebuilt at once for the rest after the first failure, so an outage does not cost a wait each)
class MimeExporter : public QObject
{
    Q_OBJECT

public:
    MimeExporter(MailboxSet *mailboxes, DaemonClient *daemon, const QString &mailboxKey, const QList<qint64> &messages, const QString &file, bool mbox,
                 QWidget *parent);
    void start();

Q_SIGNALS:
    // original: messages exported as Exchange keeps them; rebuilt: from the cache
    void finished(bool ok, int original, int rebuilt, const QString &file);

private:
    void next();
    void add(const QByteArray &mime, qint64 message, bool original);
    void finish();

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
    QString m_mailboxKey;
    QList<qint64> m_messages;
    QString m_file;
    bool m_mbox;
    QWidget *m_parent;
    int m_index = 0;
    int m_original = 0;
    int m_rebuilt = 0;
    bool m_offline = false;
    bool m_cancelled = false;
    QByteArray m_out;
    QString m_tempDir;
    QProgressDialog *m_progress = nullptr;
};

}
