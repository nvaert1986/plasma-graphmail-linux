// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "outgoing.h"

#include <KXmlGuiWindow>

#include <QHash>
#include <QSet>
#include <QTextCursor>
#include <QTimer>

class KActionMenu;
class KSelectAction;
class KMessageWidget;
class KToggleAction;
class QActionGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QSplitter;

namespace KPIMTextEdit
{
class RichTextComposer;
}

namespace GraphMail
{

class AddressBook;
class BlobStore;
class DaemonClient;
class MailboxSet;
class MailView;
class RecipientEdit;
class Store;
struct MessageRow;

struct ComposeRequest {
    // The mailbox the message is written in (an answer: where the original is)
    QString mailboxKey;
    // new, reply, replyAll, forward, or draft (a message in Drafts opened again)
    QString kind = QStringLiteral("new");
    // The message answered or forwarded, or the draft (row id in the mailbox's cache)
    qint64 message = 0;
    // A message being written that the cache still holds (a draft saved from Graph Mail, or one
    // that could not be sent)
    QString outgoing;
    // A new message to these people (New Message on a contact)
    QList<OutgoingRecipient> to;
};

// Writing a message (docs/PHASE4.md): From, To, Cc, Bcc with address completion, subject, a rich
// text editor with spell checking (or plain text), attachments, the original under an answer as
// Outlook quotes it, signatures, importance. Saves go to the daemon (a draft in Exchange, also
// automatically every minute); Send hands it over and closes. From a shared mailbox the arrow on
// Send chooses Send As or Send on Behalf; what Exchange did before (learned per mailbox) greys out
// what cannot happen.
class ComposerWindow : public KXmlGuiWindow
{
    Q_OBJECT

public:
    ComposerWindow(MailboxSet *mailboxes, DaemonClient *daemon, const ComposeRequest &request, QWidget *parent = nullptr);
    ~ComposerWindow() override;

    // Tests (--compose, --screenshot): never asks anything, never saves by itself
    void setTestMode();
    // Tests (--compose): fill in and send without dialogs
    void setTestInput(const QString &to, const QString &subject, const QString &text, const QStringList &files);
    bool sendNow();
    // Tests: save it as a draft; the record's id
    QString saveNow();
    // Tests: From an address that is not a mailbox in Graph Mail (Send As through the own mailbox)
    void setOtherFromForTests(const QString &address, const QString &name);
    // Templates (docs/PHASE9.md): what the message holds now as a template, and one filled in
    QJsonObject templateJson() const;
    // More To recipients (a Quick Step's Forward To)
    void addTo(const QString &addresses);
    // Tests: "mailbox key|uuid|subject" of the message as last handed to the daemon
    QString lastRecord() const;
    void applyTemplate(const QJsonObject &templateJson);
    // Tests: the Options menu without dialogs: "read", "delivery", "sensitivity=N", "defer=ISO"
    void setOptionsForTests(const QStringList &options);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void setupActions();
    // Options > Delay Delivery: Exchange keeps the message until then
    void askDelay();
    void showDelay();
    // MailTips for the recipients as they are now
    void askMailTips();
    void showMailTips(const QJsonArray &tips);
    void saveAsTemplate();
    void setupFrom();
    void startFrom(const MessageRow *original);
    void restore(const Outgoing &record);
    void restoreFromServer(const QString &json, const MessageRow &draft);
    void buildQuote();
    void showQuote();

    // The selected From entry: its mailbox key (an other address: the own mailbox it goes through)
    QString fromKey() const;
    void fromChanged();
    // From: Other Address... (docs/PHASE7.md); remembered ones come back in the list
    void askOtherFrom();
    void addOtherFrom(const QString &account, const QString &address, const QString &name, bool remember);
    // "other|account|address|name" when an other address is chosen, else empty
    QString otherFrom() const;
    void updateSendMenu();
    QString learnedRight(QString *date) const;

    void insertSignature(const QString &id);
    void addFiles(const QStringList &paths);
    void removeSelectedAttachments();
    void refreshAttachmentList();
    void setDirty();
    void updateTitle();

    // The record as it is now, ready for the daemon
    Outgoing record(bool send);
    bool validate();
    void save(bool send);
    void discard();

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
    ComposeRequest m_request;
    // The mailbox the draft lives in, its cache (read only) and blobs
    QString m_mailboxKey;
    Store *m_store = nullptr;
    BlobStore *m_blobs = nullptr;
    Outgoing m_record;
    bool m_saved = false;
    bool m_dirty = false;
    bool m_closing = false;
    bool m_testMode = false;
    // The From entry before "Other Address..." was picked (back to it on Cancel)
    int m_fromBefore = 0;

    AddressBook *m_book;
    KMessageWidget *m_notice;
    QComboBox *m_from;
    RecipientEdit *m_to;
    RecipientEdit *m_cc;
    RecipientEdit *m_bcc;
    QLabel *m_bccLabel;
    QLineEdit *m_subject;
    QListWidget *m_attachmentList;
    KPIMTextEdit::RichTextComposer *m_editor;
    QSplitter *m_splitter;
    QWidget *m_quoteBox;
    QLabel *m_quoteHeader;
    MailView *m_quoteView;

    KActionMenu *m_send;
    QMenu *m_sendMenu;
    QActionGroup *m_sendModes;
    KToggleAction *m_plainText;
    KToggleAction *m_high;
    KToggleAction *m_low;
    KToggleAction *m_showBcc;
    KActionMenu *m_signatureMenu;
    // Options (docs/PHASE9.md)
    KToggleAction *m_readReceipt;
    KToggleAction *m_deliveryReceipt;
    KSelectAction *m_sensitivity;
    QAction *m_delay;
    qint64 m_deferUntil = 0;
    // MailTips under the recipients, asked a moment after they change
    KMessageWidget *m_mailTips;
    QTimer m_mailTipsTimer;
    QString m_mailTipsAsked;
    qint64 m_maxMessageSize = 0;

    // The original under an answer, as HTML and as text, and the message it came from
    QString m_quoteHtml;
    QString m_quoteText;
    qint64 m_quoteSource = 0;

    // Attachments in the list: ours (files, or on the draft already) and, for a forward, the
    // original's that Exchange copies (by name; removing one drops it from the draft)
    struct Item {
        OutgoingAttachment attachment;
        bool fromOriginal = false;
    };
    QList<Item> m_items;
    QStringList m_dropped;
    // Files and pictures already handed to the daemon (by key): later saves send the key only
    QSet<QString> m_given;
    // Pictures in the text: their name in the editor -> content id in the mail
    QHash<QString, QString> m_imageCids;

    QString m_signatureId;
    QTextCursor m_signatureStart;
    QTextCursor m_signatureEnd;

    QTimer m_autosave;
};

}
