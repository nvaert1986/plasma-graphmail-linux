// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "searchquery.h"

#include <KXmlGuiWindow>

#include <QModelIndex>
#include <QPersistentModelIndex>
#include <QPointer>

#include <QDate>
#include <QHash>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>

#include <memory>
#include <functional>

class KBusyIndicatorWidget;
class KMessageWidget;
class KToggleAction;
class QTabBar;
class QTemporaryDir;
class QActionGroup;
class QComboBox;
class QMenu;
class QLabel;
class QLineEdit;
class QSplitter;
class QToolButton;
class QTreeView;

namespace GraphCore
{
class AccountsClient;
}

namespace GraphMail
{

class CategoryList;
class ConversationModel;
class MessageDelegate;
class DaemonClient;
class FolderModel;
class MailboxSet;
class MessageListModel;
class CalendarWindow;
class ContactsWindow;
class MailboxesDialog;
class OfflineDialog;
class UpdatesDialog;
class AutoRepliesDialog;
class RulesDialog;
class Shell;
class ProfilePhotos;
class MailboxSettingsDialog;
class WaitingWindow;
class TasksWindow;
class MessageWindow;
class ReadingPane;

// The Outlook-like window: folders on the left, the message list in the middle, the reading pane
// on the right. Reads the caches directly; every change goes to the daemon.
class MainWindow : public KXmlGuiWindow
{
    Q_OBJECT

public:
    MainWindow(MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent = nullptr);
    ~MainWindow() override;

    // "<account>/<mailbox>/<folder id>", or "<account>/<mailbox>" for its Inbox
    void selectFolder(const QString &path);
    // Shows a message (a notification's Open): its folder, then the message itself
    void showMessage(const QString &mailboxKey, qint64 message);
    // Automatic mark-as-read on or off (screenshots and tests never change a mailbox)
    void setMarkReadEnabled(bool enabled);
    // Tests: no confirmation questions (an off-screen dialog would block the run)
    void setTestMode()
    {
        m_testMode = true;
    }
    // Selects a row of the message list (screenshots)
    void selectRow(int row);
    // A composer: new (message 0), reply, replyAll, forward or draft, in the current mailbox
    QWidget *openComposer(const QString &kind, qint64 message, const QString &outgoing = {});
    // A new message with these synced OneDrive/SharePoint files as links (Graph Sync's Dolphin
    // action "Send Link by E-mail"), from the account whose location holds them
    QWidget *composeWithCloudLinks(const QStringList &paths);
    // A message that could not be sent (the notification's Open Draft)
    QWidget *openOutgoing(const QString &mailboxKey, const QString &uuid);
    // The calendar window (created once, raised afterwards)
    CalendarWindow *openCalendar();
    // The calendar module, made when needed, without switching to it
    CalendarWindow *calendarModule();
    // Tests: search folders ("create" name|filter, "open", "change" name|filter, "delete"); only
    // names marked [TEST Graph Mail]; logs the outcome
    void searchFolderForTests(const QString &action, const QString &name, const QString &filter);
    // Tests: tick these filters ("attachments", "unread", "read", "flagged", "important", "tome",
    // "ccme", "received:<0-4>", "between:<from date>:<to date>", "<field>:<text>" for from, to,
    // cc, subject, body, attachment, "category:<name>") and search
    void filterForTests(const QStringList &filters, const QString &scope);
    // Tests: type this in the search field with this scope ("folder", "mailbox", "all")
    void searchForTests(const QString &text, const QString &scope);
    // View > Waiting to Be Sent (docs/PHASE8.md)
    WaitingWindow *openWaiting();
    // Tests: give the selected message this category, or take it away; "manage" opens the
    // dialog and runs create/recolor/delete there
    void categoryForTests(const QString &action, const QString &name, const QString &preset);
    // Settings > Offline (docs/PHASE8.md)
    OfflineDialog *openOfflineSettings();
    // Settings > Updates (only in builds with the update check)
    UpdatesDialog *openUpdates();
    // Automatic Replies (docs/PHASE9.md), for this own mailbox or the current one
    AutoRepliesDialog *openAutoReplies(const QString &mailboxKey = {});
    // Tests: the bar as it looks when replies are being sent, without turning them on
    void showAutoRepliesBarForTests();
    // The bar's Turn Off: every own mailbox that sends replies now stops
    void turnOffAutoReplies();
    // Settings > Rules (docs/PHASE9.md), for this own mailbox or the current one
    RulesDialog *openRules(const QString &mailboxKey = {});
    // The main window, for windows that hand it something (the composer's undo send)
    static MainWindow *instance();
    // Undo (Ctrl+Z, docs/PHASE9B.md 9b.21): called before messages are moved or deleted, keeps
    // where they are now; a delete in Deleted Items is for good and is not kept
    void rememberForUndo(const QString &mailboxKey, const QList<qint64> &messages, bool deleting);
    // Sweep (docs/PHASE9B.md 9b.23) without the dialog (also tests): scope is SweepDialog::Scope,
    // target 0 deletes
    void sweepMessages(const QString &mailboxKey, qint64 message, int scope, qint64 target);
    // Undo send (docs/PHASE9.md): a bar with Undo while the message waits; Undo brings it back
    // as a draft in a composer
    void offerUndoSend(const QString &mailboxKey, const QString &uuid, const QString &subject, int seconds);
    // Tests: the bar for a message the composer sent ("key|uuid|subject"), Undo pressed after
    // this many seconds
    void undoSendForTests(const QString &record, int afterSeconds);
    // Follow up (docs/PHASE9.md): the selected messages flagged with dates, as Outlook's Today,
    // Tomorrow, This Week, Next Week; "none" flagged without dates, "custom" asks
    void flagSelected(const QString &when);
    // Message > New from Template
    void openTemplates();
    // Settings > Mailbox Settings (docs/PHASE9.md), for this own mailbox or the current one
    MailboxSettingsDialog *openMailboxSettings(const QString &mailboxKey = {});
    // Settings > Mailboxes (docs/PHASE7.md)
    MailboxesDialog *openMailboxes();
    ContactsWindow *openContacts();
    TasksWindow *openTasks();
    // Ctrl+1..4 from any window: mail, calendar, contacts, tasks; in the shell's one window
    void navigate(int module);
    // The one window around the modules (docs/PHASE9.md); without it the modules open as windows
    void setShell(Shell *shell)
    {
        m_shell = shell;
    }
    // A module (2 calendar, 3 contacts, 4 tasks) in a window of its own, as Outlook's Open in New Window
    void openInNewWindow(int module);
    // File > Print (docs/PHASE9B.md): the message shown or the first selected; tests: into a PDF
    void printMessage(const QString &pdfFile = {}, const std::function<void(bool)> &done = {});
    // Save As (docs/PHASE9B.md): one message as .eml, several as mbox; a whole folder as mbox.
    // file empty: asks; done(ok) when written
    void exportMessages(const QList<qint64> &messages, const QString &file = {}, const std::function<void(bool)> &done = {});
    void exportFolder(const QString &mailboxKey, qint64 folder, const QString &file = {}, const std::function<void(bool)> &done = {});
    // Outlook Data Files (docs/PHASE9B.md): export folders with their subfolders (none: the
    // whole mailbox) to a PST; import a PST's mail into a mailbox. Without a file the user picks one.
    void exportPst(const QString &mailboxKey, const QList<qint64> &folders, const QString &file = {}, const std::function<void(bool)> &done = {}, bool itemsOnly = false);
    void importPst(const QString &mailboxKey, const QString &file = {}, bool merge = false, bool ask = true, const std::function<void(bool)> &done = {});
    // .msg and .eml files: shown in the message viewer, or imported into a folder (none: the
    // folder shown). Without files the user picks them.
    // The viewer shown, nullptr when nothing was
    QWidget *openMessageFile(const QString &file = {});
    // Tests: a file's window without waiting on it (the message viewer, or the PST import dialog)
    QWidget *showFileForTests(const QString &file);
    void importMessageFiles(const QString &mailboxKey, qint64 folder, QStringList files = {}, const std::function<void(bool)> &done = {});
    // Settings > Archive Folder...: kept in the mailbox when Exchange takes it, else on this computer
    void chooseArchiveFolder(const QString &mailboxKey, qint64 folder = 0, const std::function<void(bool)> &done = {});
    // Files given on the command line or from the file manager: .msg and .eml in the message
    // viewer, .pst and .ost into the import (for the mailbox shown)
    void openFiles(const QStringList &files);
    // A mailto: link (the desktop's mail program): a new message from it; files it asks to attach
    // only after the user agrees
    QWidget *openMailto(const QString &link);
    // Dragging messages out (docs/PHASE9B.md 9b.33): the messages as .eml files in a private
    // folder that lives until the app quits; the original MIME from Exchange, else rebuilt
    QList<QUrl> writeMessageFiles(const QString &mailboxKey, const QList<qint64> &messages);
    // File > Message Details for the message shown (Internet headers); nullptr when none
    QWidget *openMessageDetails();
    // Reply with Meeting: a new meeting with the message's people and text
    QWidget *replyWithMeeting(const QString &mailboxKey, qint64 message);
    // A message dropped on Calendar or Tasks: an appointment / a task with its text
    QWidget *newAppointmentFromMail(const QString &mailboxKey, qint64 message);
    void newTaskFromMail(const QString &mailboxKey, qint64 message);
    // Tests: the selected messages
    QList<qint64> selectedMessageIds() const;
private:
    void runExport(const QString &mailboxKey, const QList<qint64> &messages, const QString &file, bool mbox, const std::function<void(bool)> &done);
public:
    // The mailboxes' photos (docs/PHASE9.md)
    MailboxSet *mailboxes() const
    {
        return m_mailboxes;
    }
    DaemonClient *daemon() const
    {
        return m_daemon;
    }
    ProfilePhotos *photos() const
    {
        return m_photos;
    }
    void showModule(int module, QWidget *page);
    // A message in its own window (a flagged mail's task)
    void openMessageById(const QString &mailboxKey, qint64 message);
    // Tests: the selected message in its own window
    QWidget *openSelectedMessage();
    // Tests: the reading pane's rendered body as PDF; quits when written. False without a message.
    bool printBodyToPdf(const QString &file);
    // Tests: as if "Load Images" was clicked for the message on screen
    void loadImages();
    ReadingPane *readingPane() const;

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupActions();
    void folderChanged();
    void messageChanged();
    void collectionChanged(const QString &account, const QString &mailbox, const QString &collection);
    void reloadFolders();
    void updateStatus();
    void updateActions();
    QList<qint64> selectedMessages() const;
    // The rows an action works on: the selection, or the row of a hover button (0.14.3)
    QModelIndexList actionRows() const;
    QPersistentModelIndex m_hoverRow;
    void hoverAction(const QModelIndex &index, const QString &action);
    // The message in the reading pane, else the first selected; 0 when none
    qint64 shownMessage() const;
    void markSelected(bool read);
    void toggleFlag();
    void deleteSelected();
    void moveSelected(qint64 folder);
    void moveToFolder();
    void archiveSelected();
    // Archive (9b.3): the folder Archive moves to: the one chosen on this computer, else the
    // mailbox's archive folder; 0 when there is none
    qint64 archiveFolder(const QString &mailboxKey) const;
    void search();
    // Graph's $search for the mailboxes in scope; merged into the results when it answers
    void searchOnline();
    // The results bar: count < 0 hides it; busy while Microsoft 365 is searched
    void showResults(int count, bool busy, const QString &note = {});
    // The Filters menu (docs/PHASE8.md): options to tick instead of keywords to know. They are
    // added to what is typed; a filter alone searches too.
    SearchQuery searchQuery() const;
    bool filtersActive() const;
    QString filtersText() const;
    void clearFilters();
    // Outlook's search folders (docs/PHASE8.md): the list per mailbox, one opened, the menu
    void loadSearchFolders();
    // Categories (docs/PHASE9.md): every mailbox's list, the Categorize menu, the labels
    void loadCategories();
    void fillCategorizeMenu(QMenu *menu);
    void toggleCategory(const QString &name, bool on);
    void clearCategories();
    void showCategoryLabels();
    // Automatic replies: each own mailbox's setting read, the bar shown while any sends replies
    void loadAutoReplies();
    void showAutoRepliesBar();
    void messageMenu(const QPoint &pos);
    // The message menu (right-click on a mail, the reading pane's "...")
    void fillMessageMenu(QMenu *menu);
    // Focused Inbox (docs/PHASE9.md): the tabs over the Inbox and the filter they give the list
    int focusFilter() const;
    void updateFocusTabs();
    // Move to Focused/Other; always: also an override for the sender. target -1: to the other tab
    void moveFocus(bool always, int target = -1);
    // Report Junk / Not Junk (in Junk Email); junk -1: by the folder
    void reportJunk(int junk = -1);
    // Ignore Conversation, or stop ignoring it; on -1: the opposite of now
    void ignoreConversation(int on = -1);
    void loadIgnored();
public:
    // View > Reading Pane: "right", "bottom" or "off" (docs/PHASE9.md)
    void setReadingPane(const QString &where);
    // Tests: the reading features on the selected messages without dialogs
    void reading9ForTests(const QString &focusMove, const QString &junk, const QString &ignore);
    void setFocusTabForTests(int tab);
    // Quick Steps (docs/PHASE9.md): the actions of a step on the selected messages; tests too
    void runQuickStep(const QJsonObject &step);
    bool runQuickStepForTests(const QString &name);
    // The mailbox shown (account/mailbox)
    QString mailboxKey() const
    {
        return m_mailboxKey;
    }
private:
    void showSearchFolder(const QString &mailboxKey, const QString &id, const QString &name);
    void folderMenu(const QPoint &pos);
    // The online archive in Outlook on the web (the native one waits for Graph, docs/PHASE9B.md)
    void openOnlineArchive(const QString &mailboxKey);
    void editSearchFolder(const QString &mailboxKey, const QString &id, const QString &name, const QString &filter);
    void deleteSearchFolder(const QString &mailboxKey, const QString &id, const QString &name, bool ask);
    // The mailboxes (and folders, empty = all) the scope covers
    QList<QPair<QString, QList<qint64>>> searchTargets() const;
    // Double-click or Enter: the message in its own window (raised when it is open already)
    void openMessageWindow(const QModelIndex &index);
    QString account() const;
    QString mailbox() const;
    // The conversation view while browsing a folder, the flat list otherwise (search results)
    void applyListModel();
    bool conversationView() const;

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
    FolderModel *m_folderModel;
    MessageListModel *m_messageModel;
    ConversationModel *m_conversationModel;
    bool m_conversations = false;
    // The open folder is Sent Items, Drafts or the Outbox
    bool m_showRecipients = false;
    QSplitter *m_splitter;
    // Reading pane right (in m_splitter), bottom (list and pane in this one) or off
    QWidget *m_listPane = nullptr;
    QSplitter *m_readingSplitter = nullptr;
    QString m_readingPane;
    QTreeView *m_folders;
    // A one-column tree: rows always span the visible width, and conversations (step 3.6) nest
    QTreeView *m_messages;
    QList<QPointer<MessageWindow>> m_messageWindows;
    Shell *m_shell = nullptr;
    ProfilePhotos *m_photos = nullptr;
    // Modules opened in windows of their own; closed with the application
    QList<QPointer<QWidget>> m_detached;
    QPointer<CalendarWindow> m_calendar;
    QPointer<ContactsWindow> m_contacts;
    QPointer<TasksWindow> m_tasks;
    ReadingPane *m_reading;
    QLabel *m_status;
    QLineEdit *m_searchField;
    // The connection state in the status bar; opens Waiting to Be Sent
    QToolButton *m_connection = nullptr;
    // Above the message list while searching, as Outlook's "Results" bar: what is searched where,
    // busy while Microsoft 365 is asked, how many, and the way back to the folder
    QWidget *m_resultsBar = nullptr;
    QLabel *m_resultsText = nullptr;
    KBusyIndicatorWidget *m_resultsBusy = nullptr;
    QToolButton *m_filtersButton = nullptr;
    CategoryList *m_categoryList = nullptr;
    // Outlook's "Automatic replies are being sent" bar; the settings per own mailbox key
    KMessageWidget *m_autoRepliesBar = nullptr;
    QHash<QString, QJsonObject> m_autoReplies;
    QStringList m_autoRepliesForTests;
    QTimer m_autoRepliesTimer;
    KMessageWidget *m_undoBar = nullptr;
    // No account yet, or an account that has to sign in again: the way to System Settings >
    // Microsoft 365 Accounts (the suite's one sign-in)
    KMessageWidget *m_accountsBar = nullptr;
    GraphCore::AccountsClient *m_accounts = nullptr;
    void updateAccountsBar();
    std::unique_ptr<QTemporaryDir> m_dragFiles;
    struct UndoStep {
        QString mailboxKey;
        // Message and the folder it was in
        QList<QPair<qint64, qint64>> moves;
    };
    QList<UndoStep> m_undoSteps;
    QAction *m_undoAction = nullptr;
    void undoLast();
    void sweep();
    void showAllInboxes();
    // Outlook on the web's message menu (2026-11-16): Copy, Report Phishing, Find Related,
    // a rule from the message, Move and Copy with the folders used last
    void reportPhishing();
    void copySelected(qint64 folder);
    void rememberFolder(const QString &mailboxKey, qint64 folder);
    void fillFolderMenu(QMenu *menu, bool copy);
    void findRelated(bool conversation);
    void createRuleFromMessage();
    QHash<QString, QList<qint64>> m_recentFolders;
    void refreshList();
    bool m_favoritesClosed = false;
    // The unread count on the task manager icon (docs/PHASE9B.md 9b.31)
    void updateUnreadBadge();
    QTimer m_badgeTimer;
    // Clean Up Old Mail (docs/PHASE9B.md 9b.18): interactive shows what would happen first
    void runCleanup(bool interactive, bool notify = true);
    // Empty Folder: Deleted Items for good, another folder (Junk Email) to Deleted Items
    void emptyFolder(const QString &mailboxKey, qint64 folder);
    // Outlook's folder pane: New (parent 0: the top of the mailbox), Rename, Move, Delete, Mark All
    // as Read; through the daemon, the folder tree's sync shows the result
    void newFolder(const QString &mailboxKey, qint64 parent);
    void renameFolder(const QString &mailboxKey, qint64 folder);
    void moveFolder(const QString &mailboxKey, qint64 folder);
    void deleteFolder(const QString &mailboxKey, qint64 folder);
    void markAllRead(const QString &mailboxKey, qint64 folder);
    void folderChange(const QString &mailboxKey, const QString &kind, const QJsonObject &args, const QString &done);
public:
    // Tests: run the cleanup as the schedule does, without a desktop notification
    void runCleanupForTests()
    {
        runCleanup(false, false);
    }
    void undoForTests()
    {
        undoLast();
    }
private:
    QTimer m_cleanupTimer;
    int m_badge = -1;
    int m_drags = 0;
    KToggleAction *m_focusedInbox = nullptr;
    QTabBar *m_focusTabs = nullptr;
    QString m_folderWellKnown;
    // The conversations ignored in the current mailbox
    QStringList m_ignored;
    bool m_testMode = false;
    QTimer m_undoTimer;
    QString m_undoMailboxKey;
    QString m_undoUuid;
    MessageDelegate *m_delegate = nullptr;
    QAction *m_filterAttachments = nullptr;
    QAction *m_filterUnread = nullptr;
    QAction *m_filterFlagged = nullptr;
    QAction *m_filterImportant = nullptr;
    QActionGroup *m_filterReceived = nullptr;
    QAction *m_filterRead = nullptr;
    QAction *m_filterToMe = nullptr;
    QAction *m_filterCcMe = nullptr;
    QMenu *m_filterCategories = nullptr;
    // Filters that ask for a word: field ("from", "to", "cc", "subject", "body", "attachment")
    // -> its action and the value given
    QHash<QString, QAction *> m_textFilterActions;
    QHash<QString, QString> m_textFilters;
    QStringList m_categoryFilters;
    // Received between two dates (the period "Between...")
    QDate m_rangeFrom;
    QDate m_rangeTo;
    QTimer m_searchTimer;
    // Search (docs/PHASE8.md): where to look, Graph's results per mailbox, the folder pane's mailbox
    QComboBox *m_searchScope = nullptr;
    QTimer m_onlineTimer;
    quint64 m_searchRun = 0;
    std::shared_ptr<QHash<QString, QList<qint64>>> m_onlineHits;
    QString m_folderMailboxKey;
    QTimer m_markReadTimer;
    bool m_markReadEnabled = true;
    // What the list shows
    QString m_mailboxKey;
    qint64 m_folder = 0;
};

}
