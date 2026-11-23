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
class QCompleter;
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
    // new, reply, replyAll, forward, forwardAsAttachment, editAsNew (Outlook's Resend), or draft
    // (a message in Drafts opened again)
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
    // A new message filled in from outside (a mailto: link): recipients, subject, text, files
    void prefill(const QStringList &to, const QStringList &cc, const QStringList &bcc, const QString &subject, const QString &text, const QStringList &files);
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
    // Synced OneDrive/SharePoint files as links (Graph Sync, phase 10 step 7): attach these
    // (--compose-cloud, Dolphin's "Send Link by E-mail" through --compose-link); paths outside the
    // account's synced folders are refused
    void attachCloudFiles(const QStringList &paths);
    // Tests: "name link|uploading|error" per OneDrive file
    QStringList cloudStateForTests() const;
    void applyTemplate(const QJsonObject &templateJson);
    // Tests: the Options menu without dialogs: "read", "delivery", "sensitivity=N", "defer=ISO"
    void setOptionsForTests(const QStringList &options);

    // Check Names (Ctrl+K, docs/PHASE9B.md 9b.13): every typed part that is no address is looked up
    // in the address book, then in the directory; several hits are offered to choose from.
    // done(true) when nothing is left unresolved
    void checkNames(const std::function<void(bool)> &done = {});
    // Tests: the To line as it is now, or typed as is (names without addresses too)
    QString recipientsForTests() const;
    void typeRecipientsForTests(const QString &text);
    // Tests: these keys typed into the text; the links in it afterwards ("text -> href", ...)
    QStringList typeBodyForTests(const QString &text);
    // Drops these files on the text; gives the inline pictures and the attachments
    QStringList dropFilesForTests(const QStringList &paths);
    // Plays keys on the text (see the function for the signs); gives the text after each undo
    QStringList keysForTests(const QString &script);
    // Inserts HTML (a paste) and, a few seconds later, lists the pictures: name and whether the
    // editor holds it
    void insertHtmlForTests(const QString &html);
    QStringList imagesForTests() const;

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    // A web address typed before a space, Enter or Tab becomes a link, as in Outlook (HTML only)
    void linkTypedAddress();
    // Dropped files: pictures into the text, other files as attachments
    void dropFiles(const QStringList &paths);
    // Ctrl+Z / Ctrl+Y: Qt's undo, but a selection stays when only the look changed back
    void undoText(bool undo);
    // Outlook's @mentions: an "@" at the start of a word lists the people of the conversation and
    // the address book; the one picked becomes "@Name" (a mailto link) and goes on the To line.
    // At save the links are the message's mentions (the outbox adds them to the draft)
    void updateMention();
    void startMention();
    void endMention();
    void insertMention(const QString &entry);
    QList<OutgoingRecipient> mentionsInText() const;
    // Font Color "Automatic" / Highlight "No Color": removes the colour from the selection
    void clearTextColor(bool background);
    // Selects the picture under this point of the text; false when there is none
    bool selectImageAt(const QPoint &viewportPoint);
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
    void addOriginalAttachments(const MessageRow &original);
    void attachOriginal(const MessageRow &original);
    void startAsNew(const MessageRow &original);
    void fillAsNew();
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
    // Undo: the steps and cursor after the last typed key; an Enter right after joins that step
    int m_typingSteps = -1;
    int m_typingEnd = -1;
    bool m_replayingKey = false;
    bool m_lineEnded = false;
    // The @mention being typed: the list, and where its "@" is (-1: none)
    QCompleter *m_mention = nullptr;
    int m_mentionStart = -1;
    // The picture selected by the mouse press, kept selected by its release
    QTextCursor m_clickedImage;
    // The From entry before "Other Address..." was picked (back to it on Cancel)
    int m_fromBefore = 0;

    AddressBook *m_book;
    KMessageWidget *m_notice;
    QComboBox *m_from;
    RecipientEdit *m_to;
    RecipientEdit *m_cc;
    RecipientEdit *m_bcc;
    QLabel *m_bccLabel;
    // Outlook's Reply To (Options > Show Reply To)
    RecipientEdit *m_replyTo;
    QLabel *m_replyToLabel;
    QLineEdit *m_subject;
    QListWidget *m_attachmentList;
    KPIMTextEdit::RichTextComposer *m_editor;
    // The spelling language box (0.14.4)
    class SpellLanguage *m_spell = nullptr;
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
    KToggleAction *m_showReplyTo;
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
    // Edit as New: the original whose text fills the editor once its body is here
    qint64 m_asNewSource = 0;
    // The quote with each line prefixed (Settings > Reply and Forward)
    bool m_quotePrefixed = false;

    // Attachments in the list: ours (files, or on the draft already) and, for a forward, the
    // original's that Exchange copies (by name; removing one drops it from the draft)
    struct Item {
        OutgoingAttachment attachment;
        bool fromOriginal = false;
    };
    QList<Item> m_items;

    // OneDrive/SharePoint files sent as links (Graph Sync, phase 10 step 7; composercloud.cpp).
    // Only offered when Graph Sync is installed and syncs locations of this account
    struct CloudFile {
        QString path;
        QString name;
        // The online address; empty until Graph Sync knows the file online
        QString url;
        // Put into OneDrive's Attachments folder (over 25 MB): Graph Sync is uploading it
        bool uploading = false;
        QString error;
        // Where its link goes in the text
        QTextCursor cursor;
    };
    QList<CloudFile> m_cloud;
    // Outlook's limit: a larger attachment goes to OneDrive and is sent as a link
    static constexpr qint64 largeAttachmentBytes = 25LL * 1024 * 1024;
    QTimer *m_cloudPoll = nullptr;
    // The recipients given access to the files already (Send again after a refusal)
    QStringList m_sharedWith;
    bool m_sharing = false;
    // Send pressed while files were uploading: sends itself once they are online
    bool m_sendWhenReady = false;
    QString accountId() const;
    void attachFromCloud();
    void addCloudFile(const QString &path, bool uploading);
    void requestCloudLink(int index);
    // False: not yet (uploads, giving access); Send runs again by itself when it is done
    bool cloudReadyToSend();
    QStringList recipientAddresses() const;
    // Over 25 MB (Outlook's limit for a link instead): into OneDrive's Attachments folder as a
    // link; false when that is not possible (no Graph Sync, no OneDrive of this account)
    bool putInOneDrive(const QString &path);
    QJsonArray cloudJson() const;
    void restoreCloud(const QJsonArray &files);
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
