// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QPlainTextEdit;

namespace GraphMail
{

class DaemonClient;
class MailboxSet;

// File > Message Details (Outlook's File > Properties, "Internet headers"): the message's own
// header lines as they arrived, for delivery questions (Received, authentication results, spam
// verdicts). Fetched from Exchange as MIME; offline the rebuilt copy says it is not the original.
class MessageDetailsDialog : public QDialog
{
    Q_OBJECT

public:
    // Opens the dialog at once and fills it when the message has arrived
    static MessageDetailsDialog *open(QWidget *parent, MailboxSet *mailboxes, DaemonClient *daemon, const QString &mailboxKey, qint64 message);
    // The header block of a message (up to the first empty line), as text
    static QString headerBlock(const QByteArray &mime);

private:
    explicit MessageDetailsDialog(const QString &subject, QWidget *parent);
    void show(const QString &headers, bool original);

    QPlainTextEdit *m_text;
};

}
