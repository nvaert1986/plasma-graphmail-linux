// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "signatures.h"

#include <QDialog>

class KActionCollection;
class QComboBox;
class QListWidget;

namespace KPIMTextEdit
{
class RichTextComposer;
}

namespace GraphMail
{

class MailboxSet;

// Settings > Signatures: several signatures, each edited as rich text with pictures, and per
// mailbox which one new messages and replies get (or none)
class SignatureDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SignatureDialog(MailboxSet *mailboxes, QWidget *parent = nullptr);

    void accept() override;

    // Shows a signature in an editor: its HTML and its pictures
    static void load(KPIMTextEdit::RichTextComposer *editor, const Signature &signature);
    // What an editor holds, as a signature's HTML and pictures
    static void store(KPIMTextEdit::RichTextComposer *editor, Signature *signature);

private:
    void showSignature(int row);
    void keepCurrent();
    void fillDefaults();
    void defaultChanged(bool reply);

    MailboxSet *m_mailboxes;
    QList<Signature> m_signatures;
    int m_current = -1;
    QListWidget *m_list;
    KPIMTextEdit::RichTextComposer *m_editor;
    KActionCollection *m_actions;
    QComboBox *m_mailbox;
    QComboBox *m_forNew;
    QComboBox *m_forReply;
    // Defaults changed in the dialog, written on OK: "account/mailbox" -> {new, reply}
    QHash<QString, QPair<QString, QString>> m_defaults;
};

}
