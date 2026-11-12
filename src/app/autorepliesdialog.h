// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QDialogButtonBox;
class QLabel;
class QRadioButton;
class QTabWidget;

namespace KPIMTextEdit
{
class RichTextComposer;
}

namespace GraphMail
{

class DaemonClient;
class MailboxSet;
struct AutomaticReplies;

// Automatic Replies... (docs/PHASE9.md): Outlook's dialog for the own mailboxes. Off, on, or on
// in a time range; one message inside the organisation, one (or none, or to contacts only)
// outside. Read from and written to the mailbox's settings, so Outlook shows the same.
class AutoRepliesDialog : public QDialog
{
    Q_OBJECT

public:
    AutoRepliesDialog(MailboxSet *mailboxes, DaemonClient *daemon, const QString &mailboxKey, QWidget *parent = nullptr);

    void accept() override;

    // Tests: once loaded, set "off", "on" or "scheduled|<start ISO>|<end ISO>" with this text inside
    // and outside (to contacts only; "-" clears both) and save as OK does; logs the outcome
    void saveForTests(const QString &spec, const QString &text);

Q_SIGNALS:
    // Saved for this mailbox: the setting as Graph's automaticRepliesSetting
    void saved(const QString &mailboxKey, const QJsonObject &setting);
    void loadedForTests();

private:
    void load();
    void showReplies(const AutomaticReplies &replies);
    AutomaticReplies current() const;
    void updateEnabled();
    void save(bool close);
    KPIMTextEdit::RichTextComposer *editor(QWidget *parent, QLayout *layout);

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
    QComboBox *m_mailbox;
    QRadioButton *m_off;
    QRadioButton *m_on;
    QCheckBox *m_scheduled;
    QDateTimeEdit *m_start;
    QDateTimeEdit *m_end;
    QTabWidget *m_tabs;
    KPIMTextEdit::RichTextComposer *m_inside = nullptr;
    QCheckBox *m_outside;
    QRadioButton *m_contactsOnly;
    QRadioButton *m_anyone;
    KPIMTextEdit::RichTextComposer *m_outsideText = nullptr;
    QLabel *m_status;
    QDialogButtonBox *m_buttons;
    bool m_loaded = false;
};

}
