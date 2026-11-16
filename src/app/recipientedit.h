// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "outgoing.h"

#include <QLineEdit>

class QCompleter;
class QStringListModel;

namespace GraphMail
{

class MailboxSet;

// The addresses the composer completes: the people the user mails most (/me/people, fetched by
// the daemon), contacts and recent correspondents of every synced mailbox. Read once per composer.
class AddressBook : public QObject
{
    Q_OBJECT

public:
    AddressBook(MailboxSet *mailboxes, QObject *parent = nullptr);
    QStringListModel *model() const;
    // Check Names: the entries ("Name <address>") holding every word typed, accents ignored
    static QStringList matches(const QStringList &entries, const QString &typed);

private:
    QStringListModel *m_model;
};

// A To, Cc or Bcc line: "Name <address>; address; ...". Completes the part being typed (after the
// last separator) from the address book, matching names and addresses anywhere.
class RecipientEdit : public QLineEdit
{
    Q_OBJECT

public:
    RecipientEdit(AddressBook *book, QWidget *parent = nullptr);

    QList<OutgoingRecipient> recipients(QStringList *invalid = nullptr) const;
    void setRecipients(const QList<OutgoingRecipient> &recipients);
    // Check Names: the typed part (as recipients() reports it invalid) becomes this recipient
    void replacePart(const QString &part, const OutgoingRecipient &recipient);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void complete();
    void insertCompletion(const QString &completion);
    // Where the part being typed starts
    int tokenStart() const;

    QCompleter *m_completer;
};

}
