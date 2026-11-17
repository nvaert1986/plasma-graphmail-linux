// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mainwindow.h"
#include "graphsynclink.h"

#include <graphcore/serviceclient.h>
#include "updates.h"
#include "updatesdialog.h"
#include "graphmail_debug.h"
#include "accesstext.h"
#include "attachmentpreview.h"
#include "autorepliesdialog.h"
#include "calendarwindow.h"
#include "categorylist.h"
#include "categoriesdialog.h"
#include "composer.h"
#include "contactswindow.h"
#include "taskswindow.h"
#include "messagewindow.h"
#include "signaturedialog.h"
#include "sweepdialog.h"
#include "cleanup.h"
#include "conditionalformat.h"
#include "conversationmodel.h"
#include "daemonclient.h"
#include "foldermodel.h"
#include "mailboxesdialog.h"
#include "mailboxset.h"
#include "mailview.h"
#include "messagedelegate.h"
#include "messagedrag.h"
#include "messagelistmodel.h"
#include "movedialog.h"
#include "offlinedialog.h"
#include "readingpane.h"
#include "readreceipts.h"
#include "rulesdialog.h"
#include "mailboxsettingsdialog.h"
#include "templates.h"
#include "quicksteps.h"
#include "shell.h"
#include "pstexport.h"
#include "pstimport.h"
#include "mimemapi.h"
#include <MimeTreeParserWidgets/MessageViewerDialog>
#include <QTemporaryDir>
#include "profilephotos.h"
#include "printing.h"
#include "quote.h"
#include "replyoptions.h"
#include "mailto.h"
#include "messagedetails.h"
#include "mimeexport.h"
#include "waitingwindow.h"
#include "searchfolderdialog.h"
#include "mailboxsettings.h"

#include <KActionCollection>
#include <KActionMenu>
#include <KNotifyConfigWidget>
#include <KSelectAction>
#include <KBusyIndicatorWidget>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KMessageBox>
#include <KMessageWidget>
#include <KNotification>
#include <KSharedConfig>
#include <KStandardAction>
#include <KToolBar>

#include <QAction>
#include <QScrollBar>
#include <QDesktopServices>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QFileDialog>
#include <QFileInfo>
#include <QPointer>
#include <QPushButton>
#include <QListWidget>
#include <QActionGroup>
#include <QSignalBlocker>
#include <QMenu>
#include <QSet>
#include <QJsonDocument>
#include <QComboBox>
#include <QCloseEvent>
#include <QDateEdit>
#include <QDateTime>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QDialog>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMimeData>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <KToggleAction>
#include <QLocale>
#include <QSplitter>
#include <QTabBar>
#include <QStatusBar>
#include <QToolButton>
#include <QTreeView>

namespace GraphMail
{

namespace
{
KConfigGroup windowGroup()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("MainWindow"));
}

// graphmailrc [Reading] MarkReadSeconds: 0 at once, -1 never, otherwise after that many seconds
int markReadSeconds()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Reading")).readEntry("MarkReadSeconds", 2);
}
}

namespace
{
// The one main window (MainWindow::instance()); cleared when it goes
MainWindow *s_mainWindow = nullptr;
}

MainWindow::MainWindow(MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent)
    : KXmlGuiWindow(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
    , m_folderModel(new FolderModel(mailboxes, this))
    , m_messageModel(new MessageListModel(this))
    , m_conversationModel(new ConversationModel(this))
    , m_splitter(new QSplitter(Qt::Horizontal, this))
    , m_folders(new QTreeView(this))
    , m_messages(new MessageListView(this))
    , m_reading(new ReadingPane(this))
    , m_status(new QLabel(this))
    , m_searchField(new QLineEdit(this))
{
    MailView::prepareWindow(this);
    m_reading->setContext(m_daemon, m_mailboxes);
    m_folders->setModel(m_folderModel);
    m_folders->setHeaderHidden(true);
    m_folders->setItemDelegate(new FolderDelegate(m_folders));
    m_folders->setUniformRowHeights(true);
    m_folders->setFrameShape(QFrame::NoFrame);
    // Messages are dragged onto folders to move them
    m_folders->setDragDropMode(QAbstractItemView::DropOnly);
    m_folders->setDropIndicatorShown(true);
    m_folderModel->setShowAllFolders(windowGroup().readEntry("ShowAllFolders", false));
    connect(m_folderModel, &FolderModel::filesDropped, this, [this](const QString &mailboxKey, qint64 folder, const QStringList &files) {
        importMessageFiles(mailboxKey, folder, files);
    });
    connect(m_folderModel, &FolderModel::moveRequested, this, [this](const QString &mailboxKey, qint64 folder, const QList<qint64> &messages) {
        rememberForUndo(mailboxKey, messages, false);
        for (qint64 message : messages) {
            m_daemon->move(mailboxKey.section(QLatin1Char('/'), 0, 0), mailboxKey.section(QLatin1Char('/'), 1), message, folder);
        }
    });

    // The model (flat list or conversations) and its selection signal come from applyListModel()
    m_delegate = new MessageDelegate(m_messages);
    m_delegate->reloadFormatting();
    m_messages->setItemDelegate(m_delegate);
    m_categoryList = new CategoryList(m_daemon, this);
    m_photos = new ProfilePhotos(m_mailboxes, m_daemon, this);
    connect(m_photos, &ProfilePhotos::done, this, [this](const QString &, bool, const QString &text) {
        m_status->setText(text);
    });
    m_delegate->setCategories(m_categoryList);
    connect(m_categoryList, &CategoryList::changed, this, [this] {
        m_messages->viewport()->update();
        showCategoryLabels();
    });
    m_messages->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_messages, &QWidget::customContextMenuRequested, this, &MainWindow::messageMenu);
    m_messages->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_messages->setUniformRowHeights(true);
    m_messages->setHeaderHidden(true);
    m_messages->setRootIsDecorated(false);
    m_messages->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_messages->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_messages->setFrameShape(QFrame::NoFrame);
    m_messages->setDragEnabled(true);
    m_messages->setDragDropMode(QAbstractItemView::DragOnly);
    // Double-click opens the message (a conversation expands with its arrow)
    m_messages->setExpandsOnDoubleClick(false);
    connect(m_messages, &QTreeView::doubleClicked, this, &MainWindow::openMessageWindow);
    m_messages->installEventFilter(this);
    // .eml and .msg files dropped on the list go into the folder shown; messages dragged out of it
    // become .eml files (docs/PHASE9B.md 9b.33)
    m_messages->viewport()->setAcceptDrops(true);
    m_messages->viewport()->installEventFilter(this);
    MessageDragData::setWriter([this](const QString &mailboxKey, const QList<qint64> &messages) {
        return writeMessageFiles(mailboxKey, messages);
    });
    // Scheduled cleanup: looked at a few minutes after the start and then every hour
    const auto scheduledCleanup = [this] {
        const Cleanup::Settings s = Cleanup::settings();
        if (s.everyDays > 0 && QDateTime::currentMSecsSinceEpoch() - s.lastRun >= qint64(s.everyDays) * 24 * 3600 * 1000) {
            runCleanup(false);
        }
    };
    m_cleanupTimer.setInterval(3600 * 1000);
    connect(&m_cleanupTimer, &QTimer::timeout, this, scheduledCleanup);
    m_cleanupTimer.start();
    QTimer::singleShot(5 * 60 * 1000, this, scheduledCleanup);
    // The unread badge follows the counts a moment after they change (a sync changes many at once)
    m_badgeTimer.setSingleShot(true);
    m_badgeTimer.setInterval(1000);
    connect(&m_badgeTimer, &QTimer::timeout, this, &MainWindow::updateUnreadBadge);
    m_badgeTimer.start();

    m_splitter->addWidget(m_folders);
    // The results bar sits above the message list while a search is shown
    auto *listPane = new QWidget(this);
    m_listPane = listPane;
    auto *listLayout = new QVBoxLayout(listPane);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(0);
    m_resultsBar = new QWidget(listPane);
    m_resultsBar->setAutoFillBackground(true);
    m_resultsBar->setBackgroundRole(QPalette::AlternateBase);
    auto *resultsLayout = new QHBoxLayout(m_resultsBar);
    resultsLayout->setContentsMargins(8, 4, 4, 4);
    m_resultsBusy = new KBusyIndicatorWidget(m_resultsBar);
    m_resultsBusy->setFixedSize(16, 16);
    m_resultsText = new QLabel(m_resultsBar);
    m_resultsText->setWordWrap(true);
    m_resultsText->setTextFormat(Qt::PlainText);
    auto *exitSearch = new QToolButton(m_resultsBar);
    exitSearch->setIcon(QIcon::fromTheme(QStringLiteral("window-close")));
    exitSearch->setAutoRaise(true);
    exitSearch->setToolTip(i18nc("@info:tooltip", "Leave the search and show the folder again (Esc)"));
    connect(exitSearch, &QToolButton::clicked, this, [this] {
        m_searchField->clear();
        clearFilters();
        search();
    });
    resultsLayout->addWidget(m_resultsBusy);
    resultsLayout->addWidget(m_resultsText, 1);
    resultsLayout->addWidget(exitSearch);
    m_resultsBar->hide();
    // Focused Inbox: Focused and Other over the Inbox, as Outlook's
    m_focusTabs = new QTabBar(listPane);
    m_focusTabs->addTab(i18nc("@title:tab Focused Inbox", "Focused"));
    m_focusTabs->addTab(i18nc("@title:tab Focused Inbox", "Other"));
    m_focusTabs->setDrawBase(true);
    m_focusTabs->setExpanding(false);
    m_focusTabs->hide();
    connect(m_focusTabs, &QTabBar::currentChanged, this, [this] {
        if (m_folder != 0) {
            folderChanged();
        }
    });
    listLayout->addWidget(m_focusTabs);
    listLayout->addWidget(m_resultsBar);
    listLayout->addWidget(m_messages, 1);
    m_splitter->addWidget(listPane);
    m_splitter->addWidget(m_reading);
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setStretchFactor(2, 1);
    m_splitter->setChildrenCollapsible(false);
    const QByteArray state = windowGroup().readEntry("Splitter", QByteArray());
    if (state.isEmpty() || !m_splitter->restoreState(state)) {
        m_splitter->setSizes({240, 420, 640});
    }
    // Outlook's bar above everything while automatic replies are being sent
    m_autoRepliesBar = new KMessageWidget(this);
    m_autoRepliesBar->setMessageType(KMessageWidget::Warning);
    m_autoRepliesBar->setIcon(QIcon::fromTheme(QStringLiteral("mail-reply-sender")));
    m_autoRepliesBar->setWordWrap(false);
    m_autoRepliesBar->setCloseButtonVisible(false);
    m_autoRepliesBar->hide();
    auto *turnOff = new QAction(i18nc("@action:button automatic replies", "Turn Off"), m_autoRepliesBar);
    connect(turnOff, &QAction::triggered, this, &MainWindow::turnOffAutoReplies);
    m_autoRepliesBar->addAction(turnOff);
    auto *changeReplies = new QAction(i18nc("@action:button automatic replies", "Change..."), m_autoRepliesBar);
    connect(changeReplies, &QAction::triggered, this, [this] {
        openAutoReplies();
    });
    m_autoRepliesBar->addAction(changeReplies);
    s_mainWindow = this;
    // Undo send: "Sending ..." with Undo for the few seconds the message waits
    m_undoBar = new KMessageWidget(this);
    m_undoBar->setMessageType(KMessageWidget::Positive);
    m_undoBar->setIcon(QIcon::fromTheme(QStringLiteral("mail-send")));
    m_undoBar->setCloseButtonVisible(false);
    m_undoBar->hide();
    auto *undo = new QAction(QIcon::fromTheme(QStringLiteral("edit-undo")), i18nc("@action:button undo send", "Undo"), m_undoBar);
    connect(undo, &QAction::triggered, this, [this] {
        const QString key = m_undoMailboxKey;
        const QString uuid = m_undoUuid;
        m_undoTimer.stop();
        m_undoBar->animatedHide();
        m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("undoSend"),
                        QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("uuid"), uuid}}).toJson(QJsonDocument::Compact)), this,
                        [this, key, uuid](const QString &answer) {
                            qWarning() << "undo send test:" << answer;
                            if (answer == QLatin1String("true")) {
                                openOutgoing(key, uuid);
                            } else {
                                m_status->setText(i18nc("@info:status", "Too late: the message was sent."));
                            }
                        });
    });
    m_undoBar->addAction(undo);
    m_undoTimer.setSingleShot(true);
    connect(&m_undoTimer, &QTimer::timeout, m_undoBar, &KMessageWidget::animatedHide);
    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(m_undoBar);
    centralLayout->addWidget(m_autoRepliesBar);
    m_accountsBar = new KMessageWidget(this);
    m_accountsBar->setCloseButtonVisible(false);
    m_accountsBar->setWordWrap(true);
    m_accountsBar->hide();
    auto *accountSettings = new QAction(QIcon::fromTheme(QStringLiteral("system-users")), i18nc("@action:button", "Microsoft 365 Accounts..."), m_accountsBar);
    connect(accountSettings, &QAction::triggered, this, [] {
        GraphCore::AccountService::openSettings();
    });
    m_accountsBar->addAction(accountSettings);
    centralLayout->addWidget(m_accountsBar);
    m_accounts = new GraphCore::AccountsClient(this);
    connect(m_accounts, &GraphCore::AccountsClient::accountsChanged, this, &MainWindow::updateAccountsBar);
    connect(m_accounts, &GraphCore::AccountsClient::signInNeeded, this, &MainWindow::updateAccountsBar);
    QTimer::singleShot(0, this, &MainWindow::updateAccountsBar);
    centralLayout->addWidget(m_splitter, 1);
    m_readingSplitter = new QSplitter(Qt::Vertical, this);
    m_readingSplitter->setChildrenCollapsible(false);
    m_readingSplitter->hide();
    setCentralWidget(central);
    statusBar()->addWidget(m_status, 1);
    m_connection = new QToolButton(this);
    m_connection->setAutoRaise(true);
    m_connection->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connect(m_connection, &QToolButton::clicked, this, &MainWindow::openWaiting);
    statusBar()->addPermanentWidget(m_connection);

    m_markReadTimer.setSingleShot(true);
    connect(&m_markReadTimer, &QTimer::timeout, this, [this] {
        const qint64 message = m_reading->message();
        Store *store = m_mailboxes->store(m_mailboxKey);
        const std::optional<MessageRow> row = (store && message != 0) ? store->message(message) : std::nullopt;
        if (row && !row->isRead) {
            const QString account = m_mailboxKey.section(QLatin1Char('/'), 0, 0);
            const QString mailbox = m_mailboxKey.section(QLatin1Char('/'), 1);
            // A read receipt the user is asked about waits for the reading pane's answer
            const ReadReceipts::Decision decision = ReadReceipts::decide(ReadReceipts::mode(), store->readReceiptRequested(message), false);
            if (decision == ReadReceipts::Decision::Wait) {
                m_reading->markReadWhenKnown();
            } else if (decision != ReadReceipts::Decision::Ask) {
                m_daemon->markRead(account, mailbox, message, true, decision == ReadReceipts::Decision::Send);
            }
        }
    });

    connect(m_folders->selectionModel(), &QItemSelectionModel::currentChanged, this, &MainWindow::folderChanged);
    connect(m_folders, &QTreeView::collapsed, this, [this](const QModelIndex &index) {
        if (index.data(FolderModel::IsFavoritesGroupRole).toBool()) {
            m_favoritesClosed = true;
        }
    });
    connect(m_folders, &QTreeView::expanded, this, [this](const QModelIndex &index) {
        if (index.data(FolderModel::IsFavoritesGroupRole).toBool()) {
            m_favoritesClosed = false;
        }
    });
    m_folders->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_folders, &QWidget::customContextMenuRequested, this, &MainWindow::folderMenu);
    m_conversations = windowGroup().readEntry("ConversationView", false);
    applyListModel();
    connect(m_reading, &ReadingPane::bodyNeeded, this, [this](qint64 message) {
        m_daemon->fetchBody(m_mailboxKey.section(QLatin1Char('/'), 0, 0), m_mailboxKey.section(QLatin1Char('/'), 1), message);
    });
    connect(m_daemon, &DaemonClient::collectionChanged, this, [this](const QString &account, const QString &mailbox, const QString &collection, int) {
        collectionChanged(account, mailbox, collection);
    });
    connect(m_daemon, &DaemonClient::bodyReady, this, [this](const QString &account, const QString &mailbox, qint64 message) {
        if (account + QLatin1Char('/') + mailbox == m_mailboxKey && message == m_reading->message()) {
            m_reading->reload();
        }
    });
    connect(m_reading, &ReadingPane::mailtoRequested, this, &MainWindow::openMailto);
    connect(m_reading, &ReadingPane::printRequested, this, [this] {
        printMessage();
    });
    connect(m_reading, &ReadingPane::attachmentNeeded, this, [this](qint64 attachment) {
        m_daemon->fetchAttachment(m_mailboxKey.section(QLatin1Char('/'), 0, 0), m_mailboxKey.section(QLatin1Char('/'), 1), attachment);
    });
    connect(m_daemon, &DaemonClient::attachmentReady, this, [this](const QString &account, const QString &mailbox, qint64 message, qint64 attachment, bool ok) {
        if (account + QLatin1Char('/') + mailbox == m_mailboxKey) {
            m_reading->attachmentReady(message, attachment, ok);
        }
    });
    connect(m_daemon, &DaemonClient::statusChanged, this, &MainWindow::updateStatus);
    connect(m_daemon, &DaemonClient::runningChanged, this, &MainWindow::updateStatus);
    connect(m_daemon, &DaemonClient::runningChanged, this, [this](bool running) {
        if (running) {
            loadSearchFolders();
            loadCategories();
            loadAutoReplies();
            m_photos->load();
        }
    });
    QTimer::singleShot(0, m_photos, &ProfilePhotos::load);
    QTimer::singleShot(0, this, &MainWindow::loadSearchFolders);
    QTimer::singleShot(0, this, &MainWindow::loadCategories);
    QTimer::singleShot(0, this, &MainWindow::loadAutoReplies);
    // Outlook or the web may change them, and a scheduled range starts or ends on its own
    m_autoRepliesTimer.setInterval(5 * 60 * 1000);
    connect(&m_autoRepliesTimer, &QTimer::timeout, this, &MainWindow::loadAutoReplies);
    m_autoRepliesTimer.start();
    connect(m_mailboxes, &MailboxSet::changed, this, &MainWindow::reloadFolders);

    setupActions();
    setupGUI(QSize(1300, 800), Default, QStringLiteral("graphmailui.rc"));
    // The search bar on a row of its own above the toolbar, on the left, as in Outlook. At every
    // start: a saved window layout (also one written before the bar had its row) would otherwise
    // put it beside the toolbar again
    {
        KToolBar *searchBar = toolBar(QStringLiteral("searchToolBar"));
        KToolBar *mainBar = toolBar(QStringLiteral("mainToolBar"));
        removeToolBar(searchBar);
        removeToolBar(mainBar);
        addToolBar(Qt::TopToolBarArea, searchBar);
        addToolBarBreak(Qt::TopToolBarArea);
        addToolBar(Qt::TopToolBarArea, mainBar);
        searchBar->show();
        mainBar->show();
    }

    // The own mailbox open, its Inbox selected
    for (int row = 0; row < m_folderModel->rowCount(); ++row) {
        const QModelIndex mailbox = m_folderModel->index(row, 0);
        m_folders->expand(mailbox);
    }
    if (const QList<MailboxInfo> all = m_mailboxes->mailboxes(); !all.isEmpty()) {
        // The mailbox used last (graphmailrc [MainWindow] LastMailbox), so New Message and mailto:
        // links start there too; otherwise the first account's own mailbox
        QString start = all.constFirst().key();
        const QString last = windowGroup().readEntry("LastMailbox");
        for (const MailboxInfo &box : all) {
            if (box.key() == last && box.mail) {
                start = last;
            }
        }
        selectFolder(start);
        // The own mailbox's name on top stays visible
        m_folders->scrollToTop();
    }
    updateStatus();
    updateActions();
}

MainWindow::~MainWindow()
{
    if (s_mainWindow == this) {
        s_mainWindow = nullptr;
        MessageDragData::setWriter({});
    }
}

MainWindow *MainWindow::instance()
{
    return s_mainWindow;
}

void MainWindow::setupActions()
{
    KActionCollection *actions = actionCollection();

    QAction *sync = actions->addAction(QStringLiteral("sync"));
    sync->setText(i18nc("@action", "Sync"));
    sync->setToolTip(i18nc("@info:tooltip", "Check for new mail now"));
    sync->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    actions->setDefaultShortcut(sync, Qt::Key_F9);
    connect(sync, &QAction::triggered, this, [this] {
        m_daemon->syncNow();
        m_mailboxes->reload();
    });

    QAction *markRead = actions->addAction(QStringLiteral("mark_read"));
    markRead->setText(i18nc("@action", "Mark as Read"));
    markRead->setIcon(QIcon::fromTheme(QStringLiteral("mail-mark-read")));
    // Outlook uses Ctrl+Q, but that is Quit in every KDE application (and KXmlGui refuses the
    // clash with a dialog): KDE's standard wins, Ctrl+Shift+U pairs with Ctrl+U for unread
    actions->setDefaultShortcut(markRead, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_U));
    connect(markRead, &QAction::triggered, this, [this] {
        markSelected(true);
    });

    QAction *markUnread = actions->addAction(QStringLiteral("mark_unread"));
    markUnread->setText(i18nc("@action", "Mark as Unread"));
    markUnread->setIcon(QIcon::fromTheme(QStringLiteral("mail-mark-unread")));
    actions->setDefaultShortcut(markUnread, QKeySequence(Qt::CTRL | Qt::Key_U));
    connect(markUnread, &QAction::triggered, this, [this] {
        markSelected(false);
    });

    // Flag: a click flags or unflags; the arrow gives Outlook's follow-up dates
    auto *flag = new KActionMenu(QIcon::fromTheme(QStringLiteral("flag-red")), i18nc("@action follow up on a message", "Flag"), this);
    flag->setPopupMode(QToolButton::MenuButtonPopup);
    actions->addAction(QStringLiteral("flag"), flag);
    actions->setDefaultShortcut(flag, Qt::Key_Insert);
    connect(flag, &QAction::triggered, this, &MainWindow::toggleFlag);
    const std::pair<const char *, KLocalizedString> followUps[] = {{"today", ki18nc("@action:inmenu follow up", "Today")},
                                                                   {"tomorrow", ki18nc("@action:inmenu follow up", "Tomorrow")},
                                                                   {"thisweek", ki18nc("@action:inmenu follow up", "This Week")},
                                                                   {"nextweek", ki18nc("@action:inmenu follow up", "Next Week")},
                                                                   {"none", ki18nc("@action:inmenu follow up", "No Date")},
                                                                   {"custom", ki18nc("@action:inmenu follow up", "Custom...")}};
    for (const auto &[when, text] : followUps) {
        const QString w = QLatin1String(when);
        flag->menu()->addAction(QIcon::fromTheme(QStringLiteral("flag-red")), text.toString(), this, [this, w] {
            flagSelected(w);
        });
    }
    flag->menu()->addSeparator();
    flag->menu()->addAction(QIcon::fromTheme(QStringLiteral("task-complete")), i18nc("@action:inmenu follow up", "Mark Complete"), this, [this] {
        flagSelected(QStringLiteral("complete"));
    });
    flag->menu()->addAction(QIcon::fromTheme(QStringLiteral("flag")), i18nc("@action:inmenu follow up", "Clear Flag"), this, [this] {
        flagSelected(QStringLiteral("clear"));
    });

    // Categorize, as Outlook's: the mailbox's categories to tick, Clear All, All Categories...
    auto *categorize = new KActionMenu(QIcon::fromTheme(QStringLiteral("tag")), i18nc("@action", "Categorize"), this);
    categorize->setPopupMode(QToolButton::InstantPopup);
    connect(categorize->menu(), &QMenu::aboutToShow, this, [this, categorize] {
        fillCategorizeMenu(categorize->menu());
    });
    actions->addAction(QStringLiteral("categorize"), categorize);

    // Quick Steps, as Outlook's group on the Home tab
    auto *quickSteps = new KActionMenu(QIcon::fromTheme(QStringLiteral("quickopen")), i18nc("@action", "Quick Steps"), this);
    quickSteps->setPopupMode(QToolButton::InstantPopup);
    connect(quickSteps->menu(), &QMenu::aboutToShow, this, [this, quickSteps] {
        QMenu *menu = quickSteps->menu();
        menu->clear();
        const QJsonArray steps = QuickSteps::all();
        for (const QJsonValue &v : steps) {
            const QJsonObject step = v.toObject();
            QAction *a = menu->addAction(QIcon::fromTheme(QStringLiteral("quickopen")), step.value(QLatin1String("name")).toString(), this, [this, step] {
                runQuickStep(step);
            });
            a->setToolTip(QuickSteps::describe(step));
            a->setEnabled(!selectedMessages().isEmpty());
        }
        menu->addSeparator();
        menu->addAction(QIcon::fromTheme(QStringLiteral("configure")), i18nc("@action:inmenu", "Manage Quick Steps..."), this, [this] {
            QuickStepsDialog dialog(m_mailboxes, m_folderModel, m_categoryList, m_mailboxKey, this);
            dialog.exec();
        });
    });
    actions->addAction(QStringLiteral("quick_steps"), quickSteps);

    QAction *remove = actions->addAction(QStringLiteral("delete"));
    remove->setText(i18nc("@action", "Delete"));
    remove->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete")));
    actions->setDefaultShortcut(remove, Qt::Key_Delete);
    connect(remove, &QAction::triggered, this, &MainWindow::deleteSelected);

    // Deleted Items and Junk Email: Outlook's Empty Folder (also in the folder's menu)
    QAction *empty = actions->addAction(QStringLiteral("empty_folder"));
    empty->setText(i18nc("@action", "Empty Folder"));
    empty->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete-shred")));
    empty->setVisible(false);
    connect(empty, &QAction::triggered, this, [this] {
        emptyFolder(m_mailboxKey, m_folder);
    });

    // Outlook's New Folder (Ctrl+Shift+E): inside the folder shown, or at the top of its mailbox
    QAction *folderNew = actions->addAction(QStringLiteral("new_folder"));
    folderNew->setText(i18nc("@action", "New Folder..."));
    folderNew->setIcon(QIcon::fromTheme(QStringLiteral("folder-new")));
    actions->setDefaultShortcut(folderNew, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    connect(folderNew, &QAction::triggered, this, [this] {
        if (!m_mailboxKey.isEmpty()) {
            newFolder(m_mailboxKey, m_folder);
        }
    });

    // Outlook on the web's toolbar says just "Meeting"
    if (QAction *meeting = actions->action(QStringLiteral("reply_with_meeting"))) {
        meeting->setIconText(i18nc("@action short toolbar text for Reply with Meeting", "Meeting"));
    }
    QAction *phishing = actions->addAction(QStringLiteral("report_phishing"));
    phishing->setText(i18nc("@action", "Report Phishing"));
    phishing->setIcon(QIcon::fromTheme(QStringLiteral("security-low")));
    connect(phishing, &QAction::triggered, this, &MainWindow::reportPhishing);
    // One button for both, as Outlook's Read / Unread (the menus keep both)
    QAction *toggleRead = actions->addAction(QStringLiteral("toggle_read"));
    toggleRead->setText(i18nc("@action", "Mark as Read"));
    toggleRead->setIcon(QIcon::fromTheme(QStringLiteral("mail-mark-read")));
    connect(toggleRead, &QAction::triggered, this, [this] {
        const Store *store = m_mailboxes->store(m_mailboxKey);
        const QList<qint64> ids = selectedMessages();
        const std::optional<MessageRow> first = store && !ids.isEmpty() ? store->message(ids.constFirst()) : std::nullopt;
        markSelected(!(first && first->isRead));
    });
    QAction *copy = actions->addAction(QStringLiteral("copy_to"));
    copy->setText(i18nc("@action", "Copy to Folder..."));
    copy->setIcon(QIcon::fromTheme(QStringLiteral("edit-copy")));
    connect(copy, &QAction::triggered, this, [this] {
        MoveDialog dialog(m_folderModel, m_mailboxKey, this);
        dialog.setWindowTitle(i18nc("@title:window", "Copy to Folder"));
        if (dialog.exec() == QDialog::Accepted && dialog.folder() != 0) {
            copySelected(dialog.folder());
        }
    });
    QAction *ruleFrom = actions->addAction(QStringLiteral("create_rule"));
    ruleFrom->setText(i18nc("@action", "Create Rule from This Message..."));
    ruleFrom->setIcon(QIcon::fromTheme(QStringLiteral("view-filter")));
    connect(ruleFrom, &QAction::triggered, this, &MainWindow::createRuleFromMessage);

    QAction *move = actions->addAction(QStringLiteral("move"));
    move->setText(i18nc("@action", "Move to Folder..."));
    move->setIconText(i18nc("@action short toolbar text", "Move"));
    move->setIcon(QIcon::fromTheme(QStringLiteral("go-jump")));
    actions->setDefaultShortcut(move, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    connect(move, &QAction::triggered, this, &MainWindow::moveToFolder);

    QAction *archive = actions->addAction(QStringLiteral("archive"));
    archive->setText(i18nc("@action move to the Archive folder", "Archive"));
    archive->setIcon(QIcon::fromTheme(QStringLiteral("archive-insert")));
    actions->setDefaultShortcut(archive, Qt::Key_Backspace);
    connect(archive, &QAction::triggered, this, &MainWindow::archiveSelected);

    auto *showAll = new KToggleAction(i18nc("@action:inmenu", "Show All Folders"), this);
    showAll->setToolTip(i18nc("@info:tooltip", "Also show the system folders Outlook hides (Sync Issues, RSS, Working Set, ...)"));
    showAll->setChecked(windowGroup().readEntry("ShowAllFolders", false));
    actions->addAction(QStringLiteral("show_all_folders"), showAll);
    connect(showAll, &KToggleAction::toggled, this, [this](bool on) {
        KConfigGroup group = windowGroup();
        group.writeEntry("ShowAllFolders", on);
        group.sync();
        m_folderModel->setShowAllFolders(on);
        reloadFolders();
    });

    auto *conversations = new KToggleAction(QIcon::fromTheme(QStringLiteral("view-conversation-balloon")), i18nc("@action:inmenu", "Show as Conversations"), this);
    conversations->setChecked(windowGroup().readEntry("ConversationView", false));
    actions->addAction(QStringLiteral("show_conversations"), conversations);

    auto *readingPane = new KSelectAction(QIcon::fromTheme(QStringLiteral("view-split-left-right")), i18nc("@action", "Reading Pane"), this);
    readingPane->setItems({i18nc("@item reading pane", "Right"), i18nc("@item reading pane", "Bottom"), i18nc("@item reading pane", "Off")});
    actions->addAction(QStringLiteral("reading_pane"), readingPane);
    connect(readingPane, &KSelectAction::indexTriggered, this, [this](int index) {
        static const char *const places[] = {"right", "bottom", "off"};
        setReadingPane(QLatin1String(places[std::clamp(index, 0, 2)]));
    });
    {
        const QString where = windowGroup().readEntry("ReadingPane", QStringLiteral("right"));
        readingPane->setCurrentItem(where == QLatin1String("bottom") ? 1 : where == QLatin1String("off") ? 2 : 0);
        QTimer::singleShot(0, this, [this, where] {
            if (m_readingPane.isEmpty()) {
                setReadingPane(where);
            }
        });
    }

    m_focusedInbox = new KToggleAction(i18nc("@action", "Focused Inbox"), this);
    m_focusedInbox->setChecked(windowGroup().readEntry("FocusedInbox", true));
    actions->addAction(QStringLiteral("focused_inbox"), m_focusedInbox);
    connect(m_focusedInbox, &KToggleAction::toggled, this, [this](bool on) {
        KConfigGroup group = windowGroup();
        group.writeEntry("FocusedInbox", on);
        group.sync();
        if (m_folder != 0) {
            folderChanged();
        }
    });
    QAction *moveFocusAction = actions->addAction(QStringLiteral("focus_move"));
    moveFocusAction->setIcon(QIcon::fromTheme(QStringLiteral("mail-move")));
    connect(moveFocusAction, &QAction::triggered, this, [this] {
        moveFocus(false);
    });
    QAction *alwaysFocusAction = actions->addAction(QStringLiteral("focus_always"));
    connect(alwaysFocusAction, &QAction::triggered, this, [this] {
        moveFocus(true);
    });
    QAction *junk = actions->addAction(QStringLiteral("report_junk"));
    junk->setIcon(QIcon::fromTheme(QStringLiteral("mail-mark-junk")));
    actions->setDefaultShortcut(junk, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_J));
    connect(junk, &QAction::triggered, this, [this] {
        reportJunk();
    });
    QAction *ignore = actions->addAction(QStringLiteral("ignore_conversation"));
    ignore->setIcon(QIcon::fromTheme(QStringLiteral("mail-thread-ignored")));
    connect(ignore, &QAction::triggered, this, [this] {
        ignoreConversation();
    });
    connect(conversations, &KToggleAction::toggled, this, [this](bool on) {
        KConfigGroup group = windowGroup();
        group.writeEntry("ConversationView", on);
        group.sync();
        m_conversations = on;
        m_reading->clear();
        if (m_folder != 0) {
            folderChanged();
        }
        applyListModel();
    });

    // Search as in Outlook: a bar of its own at the top, the scope and the field on its left
    m_searchField->setPlaceholderText(i18nc("@info:placeholder", "Search"));
    m_searchField->setClearButtonEnabled(true);
    m_searchField->addAction(QIcon::fromTheme(QStringLiteral("search")), QLineEdit::LeadingPosition);
    m_searchField->setMinimumWidth(320);
    QAction *focusSearch = actions->addAction(QStringLiteral("search"));
    focusSearch->setText(i18nc("@action", "Search Mail"));
    focusSearch->setIcon(QIcon::fromTheme(QStringLiteral("search")));
    // Ctrl+E as in Outlook; Ctrl+F is Forward there
    actions->setDefaultShortcut(focusSearch, QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(focusSearch, &QAction::triggered, this, [this] {
        m_searchField->setFocus();
        m_searchField->selectAll();
    });
    // Search while typing, after a short pause (FTS answers in well under a second)
    m_searchTimer.setSingleShot(true);
    m_searchTimer.setInterval(300);
    connect(&m_searchTimer, &QTimer::timeout, this, &MainWindow::search);
    connect(m_searchField, &QLineEdit::textChanged, this, [this] {
        m_searchTimer.start();
    });
    connect(m_searchField, &QLineEdit::returnPressed, this, &MainWindow::search);
    // Esc in the field leaves the search, as in Outlook
    auto *leave = new QAction(m_searchField);
    leave->setShortcut(Qt::Key_Escape);
    leave->setShortcutContext(Qt::WidgetShortcut);
    m_searchField->addAction(leave);
    connect(leave, &QAction::triggered, this, [this] {
        if (!m_searchField->text().isEmpty() || filtersActive()) {
            m_searchField->clear();
            clearFilters();
            search();
        }
    });
    // Where to search, as Outlook's scope list; remembered
    m_searchScope = new QComboBox(this);
    m_searchScope->addItem(i18nc("@item:inlistbox search scope", "Current Folder"), QStringLiteral("folder"));
    m_searchScope->addItem(i18nc("@item:inlistbox search scope", "Current Mailbox"), QStringLiteral("mailbox"));
    m_searchScope->addItem(i18nc("@item:inlistbox search scope", "All Mailboxes"), QStringLiteral("all"));
    m_searchScope->setCurrentIndex(std::max(0, m_searchScope->findData(KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)
                                                                            ->group(QStringLiteral("Search"))
                                                                            .readEntry("Scope", QStringLiteral("mailbox")))));
    m_searchScope->setToolTip(i18nc("@info:tooltip", "Where to search"));
    // One widget for the whole bar: it takes the bar's width, the search sits on the left
    auto *box = new QWidget(this);
    box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *boxLayout = new QHBoxLayout(box);
    boxLayout->setContentsMargins(0, 2, 0, 2);
    boxLayout->setSpacing(0);
    boxLayout->addSpacing(6);
    boxLayout->addWidget(m_searchScope);
    boxLayout->addWidget(m_searchField, 3);
    // Filters to tick, easier than Outlook's keywords; typing keywords still works
    auto *filters = new QToolButton(box);
    m_filtersButton = filters;
    filters->setIcon(QIcon::fromTheme(QStringLiteral("view-filter")));
    filters->setText(i18nc("@action:button", "Filters"));
    filters->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    filters->setPopupMode(QToolButton::InstantPopup);
    filters->setAutoRaise(true);
    auto *filterMenu = new QMenu(filters);
    auto checkable = [this, filterMenu](const QString &text) {
        QAction *a = filterMenu->addAction(text);
        a->setCheckable(true);
        connect(a, &QAction::toggled, this, &MainWindow::search);
        return a;
    };
    m_filterAttachments = checkable(i18nc("@option:check search filter", "Has Attachments"));
    m_filterUnread = checkable(i18nc("@option:check search filter", "Unread"));
    m_filterRead = checkable(i18nc("@option:check search filter", "Read"));
    // Unread and read exclude each other
    connect(m_filterUnread, &QAction::toggled, this, [this](bool on) {
        if (on) {
            const QSignalBlocker blocker(m_filterRead);
            m_filterRead->setChecked(false);
        }
    });
    connect(m_filterRead, &QAction::toggled, this, [this](bool on) {
        if (on) {
            const QSignalBlocker blocker(m_filterUnread);
            m_filterUnread->setChecked(false);
        }
    });
    m_filterFlagged = checkable(i18nc("@option:check search filter", "Flagged"));
    m_filterImportant = checkable(i18nc("@option:check search filter", "Important"));
    m_filterToMe = checkable(i18nc("@option:check search filter", "Sent to Me"));
    m_filterCcMe = checkable(i18nc("@option:check search filter", "CC to Me"));
    filterMenu->addSeparator();
    QMenu *received = filterMenu->addMenu(QIcon::fromTheme(QStringLiteral("view-calendar")), i18nc("@title:menu search filter", "Received"));
    m_filterReceived = new QActionGroup(received);
    const QStringList periods{i18nc("@option:radio received", "Any Time"),    i18nc("@option:radio received", "Today"),     i18nc("@option:radio received", "This Week"),
                              i18nc("@option:radio received", "Last 30 Days"), i18nc("@option:radio received", "This Year"), i18nc("@option:radio received", "Between...")};
    for (int i = 0; i < periods.size(); ++i) {
        QAction *a = received->addAction(periods.at(i));
        a->setCheckable(true);
        a->setData(i);
        a->setChecked(i == 0);
        m_filterReceived->addAction(a);
    }
    connect(m_filterReceived, &QActionGroup::triggered, this, [this](QAction *a) {
        // Between...: two dates, asked
        if (a->data().toInt() == 5) {
            QDialog dialog(this);
            dialog.setWindowTitle(i18nc("@title:window", "Received Between"));
            auto *from = new QDateEdit(m_rangeFrom.isValid() ? m_rangeFrom : QDate::currentDate().addMonths(-1), &dialog);
            auto *to = new QDateEdit(m_rangeTo.isValid() ? m_rangeTo : QDate::currentDate(), &dialog);
            from->setCalendarPopup(true);
            to->setCalendarPopup(true);
            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            auto *form = new QFormLayout(&dialog);
            form->addRow(i18nc("@label:chooser", "From:"), from);
            form->addRow(i18nc("@label:chooser", "Up to and including:"), to);
            form->addRow(buttons);
            connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            if (dialog.exec() == QDialog::Accepted) {
                m_rangeFrom = std::min(from->date(), to->date());
                m_rangeTo = std::max(from->date(), to->date());
            } else if (!m_rangeFrom.isValid()) {
                m_filterReceived->actions().constFirst()->setChecked(true);
            }
        }
        search();
    });
    // Categories: the mailbox's own, read when the menu opens
    m_filterCategories = filterMenu->addMenu(QIcon::fromTheme(QStringLiteral("tag")), i18nc("@title:menu search filter", "Category"));
    connect(m_filterCategories, &QMenu::aboutToShow, this, [this] {
        const QString key = m_folderMailboxKey.isEmpty() ? m_mailboxKey : m_folderMailboxKey;
        m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("masterCategories"), QStringLiteral("{}"), this,
                        [this](const QString &json) {
                            m_filterCategories->clear();
                            const QJsonArray value = QJsonDocument::fromJson(json.toUtf8()).object().value(QLatin1String("value")).toArray();
                            for (const QJsonValue &v : value) {
                                const QString name = v.toObject().value(QLatin1String("displayName")).toString();
                                QAction *a = m_filterCategories->addAction(name);
                                a->setCheckable(true);
                                a->setChecked(m_categoryFilters.contains(name));
                                connect(a, &QAction::toggled, this, [this, name](bool on) {
                                    m_categoryFilters.removeAll(name);
                                    if (on) {
                                        m_categoryFilters.append(name);
                                    }
                                    search();
                                });
                            }
                            if (value.isEmpty()) {
                                m_filterCategories->addAction(i18nc("@item:inmenu", "(no categories)"))->setEnabled(false);
                            }
                        });
    });
    filterMenu->addSeparator();
    // Ask for a name or word; ticked while one is set
    const struct {
        const char *field;
        KLocalizedString text;
        KLocalizedString question;
    } asked[] = {
        {"from", ki18nc("@option:check search filter", "From..."), ki18nc("@label:textbox", "From (name or address):")},
        {"to", ki18nc("@option:check search filter", "To..."), ki18nc("@label:textbox", "To (name or address):")},
        {"cc", ki18nc("@option:check search filter", "Cc..."), ki18nc("@label:textbox", "Cc (name or address):")},
        {"subject", ki18nc("@option:check search filter", "Subject Contains..."), ki18nc("@label:textbox", "Subject contains:")},
        {"body", ki18nc("@option:check search filter", "Message Text Contains..."), ki18nc("@label:textbox", "Message text contains:")},
        {"attachment", ki18nc("@option:check search filter", "Attachment Name..."), ki18nc("@label:textbox", "Attachment name contains:")},
    };
    for (const auto &f : asked) {
        const QString field = QString::fromLatin1(f.field);
        const QString question = f.question.toString();
        QAction *a = filterMenu->addAction(f.text.toString());
        a->setCheckable(true);
        m_textFilterActions.insert(field, a);
        connect(a, &QAction::triggered, this, [this, a, field, question](bool on) {
            if (on) {
                bool ok = false;
                const QString answer = QInputDialog::getText(this, i18nc("@title:window", "Filter"), question, QLineEdit::Normal, m_textFilters.value(field), &ok).trimmed();
                if (ok) {
                    m_textFilters.insert(field, answer);
                }
            } else {
                m_textFilters.remove(field);
            }
            if (m_textFilters.value(field).isEmpty()) {
                m_textFilters.remove(field);
            }
            a->setChecked(m_textFilters.contains(field));
            search();
        });
    }
    filterMenu->addSeparator();
    filterMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-clear")), i18nc("@action:inmenu", "Clear Filters"), this, [this] {
        clearFilters();
        search();
    });
    filters->setMenu(filterMenu);
    boxLayout->addWidget(filters);
    boxLayout->addStretch(1);
    m_searchField->setToolTip(i18nc("@info:tooltip",
                                    "Words search everything. Narrow it down with from:, to:, cc:, subject:, body:, hasattachments:yes, isread:no, "
                                    "isflagged:yes, importance:high and received>=2026-11-01, or use Filters."));
    m_searchField->setMaximumWidth(640);
    auto *searchAction = new QWidgetAction(this);
    searchAction->setText(i18nc("@action", "Search"));
    searchAction->setDefaultWidget(box);
    actions->addAction(QStringLiteral("search_box"), searchAction);
    connect(m_searchScope, &QComboBox::activated, this, [this] {
        KConfigGroup group = KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Search"));
        group.writeEntry("Scope", m_searchScope->currentData().toString());
        group.sync();
        if (!m_searchField->text().trimmed().isEmpty()) {
            search();
        }
    });
    // Graph's search after a longer pause: not for every letter typed
    m_onlineTimer.setSingleShot(true);
    m_onlineTimer.setInterval(900);
    connect(&m_onlineTimer, &QTimer::timeout, this, &MainWindow::searchOnline);

    // Writing (phase 4)
    QAction *newMessage = actions->addAction(QStringLiteral("new_message"));
    newMessage->setText(i18nc("@action", "New Message"));
    newMessage->setIconText(i18nc("@action short toolbar text", "New"));
    newMessage->setIcon(QIcon::fromTheme(QStringLiteral("mail-message-new")));
    actions->setDefaultShortcut(newMessage, QKeySequence(Qt::CTRL | Qt::Key_N));
    connect(newMessage, &QAction::triggered, this, [this] {
        openComposer(QStringLiteral("new"), 0);
    });
    const struct {
        const char *name;
        const char *kind;
        const char *icon;
        QKeySequence shortcut;
    } answers[] = {{"reply", "reply", "mail-reply-sender", QKeySequence(Qt::CTRL | Qt::Key_R)},
                   {"reply_all", "replyAll", "mail-reply-all", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R)},
                   {"forward", "forward", "mail-forward", QKeySequence(Qt::CTRL | Qt::Key_F)},
                   {"forward_as_attachment", "forwardAsAttachment", "mail-forward", QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_F)},
                   {"edit_as_new", "editAsNew", "mail-message-new", QKeySequence()}};
    for (const auto &answer : answers) {
        QAction *action = actions->addAction(QLatin1String(answer.name));
        action->setIcon(QIcon::fromTheme(QLatin1String(answer.icon)));
        actions->setDefaultShortcut(action, answer.shortcut);
        const QString kind = QLatin1String(answer.kind);
        connect(action, &QAction::triggered, this, [this, kind] {
            const QList<qint64> ids = selectedMessages();
            if (!ids.isEmpty()) {
                openComposer(kind, m_reading->message() != 0 ? m_reading->message() : ids.constFirst());
            }
        });
    }
    actions->action(QStringLiteral("reply"))->setText(i18nc("@action", "Reply"));
    actions->action(QStringLiteral("reply_all"))->setText(i18nc("@action", "Reply All"));
    actions->action(QStringLiteral("forward"))->setText(i18nc("@action", "Forward"));
    actions->action(QStringLiteral("forward_as_attachment"))->setText(i18nc("@action", "Forward as Attachment"));
    actions->action(QStringLiteral("edit_as_new"))->setText(i18nc("@action Outlook's Resend", "Edit as New Message"));
    QAction *meeting = actions->addAction(QStringLiteral("reply_with_meeting"));
    meeting->setText(i18nc("@action", "Reply with Meeting"));
    meeting->setIcon(QIcon::fromTheme(QStringLiteral("appointment-new")));
    actions->setDefaultShortcut(meeting, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_R));
    connect(meeting, &QAction::triggered, this, [this] {
        replyWithMeeting(m_mailboxKey, shownMessage());
    });

    QAction *calendar = actions->addAction(QStringLiteral("go_calendar"));
    calendar->setText(i18nc("@action", "Calendar"));
    calendar->setIcon(QIcon::fromTheme(QStringLiteral("view-calendar")));
    actions->setDefaultShortcut(calendar, QKeySequence(Qt::CTRL | Qt::Key_2));
    connect(calendar, &QAction::triggered, this, &MainWindow::openCalendar);
    QAction *contacts = actions->addAction(QStringLiteral("go_contacts"));
    contacts->setText(i18nc("@action", "Contacts"));
    contacts->setIcon(QIcon::fromTheme(QStringLiteral("view-pim-contacts")));
    actions->setDefaultShortcut(contacts, QKeySequence(Qt::CTRL | Qt::Key_3));
    connect(contacts, &QAction::triggered, this, &MainWindow::openContacts);
    QAction *tasks = actions->addAction(QStringLiteral("go_tasks"));
    tasks->setText(i18nc("@action", "Tasks"));
    tasks->setIcon(QIcon::fromTheme(QStringLiteral("view-task")));
    actions->setDefaultShortcut(tasks, QKeySequence(Qt::CTRL | Qt::Key_4));
    connect(tasks, &QAction::triggered, this, &MainWindow::openTasks);
    // View > Zoom and the dark message background (docs/PHASE9B.md 9b.25)
    KStandardAction::zoomIn(this, [this] {
        m_reading->zoom(1);
    }, actions);
    KStandardAction::zoomOut(this, [this] {
        m_reading->zoom(-1);
    }, actions);
    KStandardAction::actualSize(this, [this] {
        m_reading->zoom(0);
    }, actions);
    auto *dark = new KToggleAction(QIcon::fromTheme(QStringLiteral("color-mode-invert-text")), i18nc("@action", "Dark Background for Messages"), this);
    actions->addAction(QStringLiteral("dark_messages"), dark);
    dark->setChecked(MailView::darkMessages());
    dark->setVisible(MailView::darkTheme());
    connect(dark, &KToggleAction::toggled, this, [this](bool on) {
        MailView::setDarkMessages(on);
        m_reading->reload();
    });
    // Optional: Office previews run LibreOffice, which not every office uses
    auto *officePreview = new KToggleAction(QIcon::fromTheme(QStringLiteral("document-preview")), i18nc("@action", "Preview Office Documents with LibreOffice"), this);
    actions->addAction(QStringLiteral("office_preview"), officePreview);
    officePreview->setChecked(AttachmentPreview::officePreview());
    if (!AttachmentPreview::hasLibreOffice()) {
        officePreview->setText(i18nc("@action", "Preview Office Documents with LibreOffice (not installed)"));
    }
    connect(officePreview, &KToggleAction::toggled, this, [](bool on) {
        AttachmentPreview::setOfficePreview(on);
    });
    QAction *sweepAction = actions->addAction(QStringLiteral("sweep"));
    sweepAction->setText(i18nc("@action Outlook's Sweep", "Sweep..."));
    sweepAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-clear-all")));
    connect(sweepAction, &QAction::triggered, this, &MainWindow::sweep);
    QAction *formatting = actions->addAction(QStringLiteral("conditional_formatting"));
    formatting->setText(i18nc("@action", "Conditional Formatting..."));
    formatting->setIcon(QIcon::fromTheme(QStringLiteral("format-text-color")));
    connect(formatting, &QAction::triggered, this, [this] {
        ConditionalFormat::Dialog dialog(this);
        if (dialog.exec() == QDialog::Accepted) {
            m_delegate->reloadFormatting();
            m_messages->viewport()->update();
        }
    });
    // Settings > Read Receipts: Outlook's tracking option
    auto *receipts = new KSelectAction(QIcon::fromTheme(QStringLiteral("mail-receipt")), i18nc("@action", "Read Receipts"), this);
    actions->addAction(QStringLiteral("read_receipts"), receipts);
    receipts->setItems({i18nc("@item:inmenu read receipts", "Ask Each Time"), i18nc("@item:inmenu read receipts", "Always Send"),
                        i18nc("@item:inmenu read receipts", "Never Send")});
    receipts->setCurrentItem(int(ReadReceipts::mode()));
    connect(receipts, &KSelectAction::indexTriggered, this, [this](int index) {
        ReadReceipts::setMode(ReadReceipts::Mode(index));
        m_reading->reload();
    });
    QAction *cleanup = actions->addAction(QStringLiteral("cleanup"));
    cleanup->setText(i18nc("@action", "Clean Up Old Mail..."));
    cleanup->setIcon(QIcon::fromTheme(QStringLiteral("edit-clear-history")));
    connect(cleanup, &QAction::triggered, this, [this] {
        auto *dialog = new Cleanup::Dialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        connect(dialog, &Cleanup::Dialog::runRequested, this, [this] {
            QTimer::singleShot(0, this, [this] {
                runCleanup(true);
            });
        });
        dialog->open();
    });
    m_undoAction = KStandardAction::undo(this, &MainWindow::undoLast, actions);
    m_undoAction->setEnabled(false);
    QAction *onlineArchive = actions->addAction(QStringLiteral("open_online_archive"));
    onlineArchive->setText(i18nc("@action", "Open Online Archive in Browser"));
    onlineArchive->setIcon(QIcon::fromTheme(QStringLiteral("folder-html")));
    connect(onlineArchive, &QAction::triggered, this, [this] {
        openOnlineArchive(mailboxKey());
    });

    QAction *replyOptions = actions->addAction(QStringLiteral("reply_options"));
    replyOptions->setText(i18nc("@action", "Reply and Forward..."));
    replyOptions->setIcon(QIcon::fromTheme(QStringLiteral("mail-reply-sender")));
    connect(replyOptions, &QAction::triggered, this, [this] {
        ReplyOptions::Dialog(this).exec();
    });
    QAction *signatures = actions->addAction(QStringLiteral("signatures"));
    signatures->setText(i18nc("@action", "Signatures..."));
    signatures->setIcon(QIcon::fromTheme(QStringLiteral("document-sign")));
    connect(signatures, &QAction::triggered, this, [this] {
        SignatureDialog dialog(m_mailboxes, this);
        dialog.exec();
    });

    QAction *waiting = actions->addAction(QStringLiteral("waiting"));
    waiting->setText(i18nc("@action", "Waiting to Be Sent..."));
    waiting->setIcon(QIcon::fromTheme(QStringLiteral("mail-queue")));
    connect(waiting, &QAction::triggered, this, &MainWindow::openWaiting);

    QAction *offline = actions->addAction(QStringLiteral("offline_settings"));
    offline->setText(i18nc("@action", "Offline..."));
    offline->setIcon(QIcon::fromTheme(QStringLiteral("folder-sync")));
    connect(offline, &QAction::triggered, this, &MainWindow::openOfflineSettings);

    QAction *accountsAction = actions->addAction(QStringLiteral("accounts"));
    accountsAction->setText(i18nc("@action", "Microsoft 365 Accounts..."));
    accountsAction->setIcon(QIcon::fromTheme(QStringLiteral("system-users")));
    connect(accountsAction, &QAction::triggered, this, [] {
        GraphCore::AccountService::openSettings();
    });

    QAction *updates = actions->addAction(QStringLiteral("updates"));
    updates->setText(i18nc("@action", "Updates..."));
    updates->setIcon(QIcon::fromTheme(QStringLiteral("system-software-update")));
    // Builds without the check (distributions that ship updates themselves) do not offer it
    updates->setVisible(Updates::compiledIn());
    connect(updates, &QAction::triggered, this, &MainWindow::openUpdates);

    QAction *importPstAction = actions->addAction(QStringLiteral("import_pst"));
    importPstAction->setText(i18nc("@action", "Import Outlook Data File..."));
    importPstAction->setIcon(QIcon::fromTheme(QStringLiteral("document-import")));
    connect(importPstAction, &QAction::triggered, this, [this] {
        importPst(m_mailboxKey);
    });
    QAction *exportPstAction = actions->addAction(QStringLiteral("export_pst"));
    exportPstAction->setText(i18nc("@action", "Export to Outlook Data File..."));
    exportPstAction->setIcon(QIcon::fromTheme(QStringLiteral("document-export")));
    connect(exportPstAction, &QAction::triggered, this, [this] {
        exportPst(m_mailboxKey, {});
    });

    // File > Save as PDF and Message Details for the message shown (or the first selected)
    QAction *savePdf = actions->addAction(QStringLiteral("save_pdf"));
    savePdf->setText(i18nc("@action", "Save as PDF..."));
    savePdf->setIcon(QIcon::fromTheme(QStringLiteral("application-pdf")));
    connect(savePdf, &QAction::triggered, this, [this] {
        const qint64 message = shownMessage();
        const Store *store = m_mailboxes->store(m_mailboxKey);
        const std::optional<MessageRow> row = store && message ? store->message(message) : std::nullopt;
        if (!row) {
            return;
        }
        if (const QString file = MimeExport::askPdfFile(this, row->subject); !file.isEmpty()) {
            printMessage(file, [this, file](bool ok) {
                m_status->setText(ok ? i18nc("@info:status %1 a file", "Saved as %1.", file) : i18nc("@info:status %1 a file", "Could not save %1.", file));
            });
        }
    });
    QAction *details = actions->addAction(QStringLiteral("message_details"));
    details->setText(i18nc("@action", "Message Details..."));
    details->setIcon(QIcon::fromTheme(QStringLiteral("document-properties")));
    connect(details, &QAction::triggered, this, &MainWindow::openMessageDetails);

    QAction *openMessageAction = actions->addAction(QStringLiteral("open_message_file"));
    openMessageAction->setText(i18nc("@action", "Open Message File..."));
    openMessageAction->setIcon(QIcon::fromTheme(QStringLiteral("document-open")));
    connect(openMessageAction, &QAction::triggered, this, [this] {
        openMessageFile();
    });
    QAction *importMessagesAction = actions->addAction(QStringLiteral("import_message_files"));
    importMessagesAction->setText(i18nc("@action", "Import Message Files..."));
    importMessagesAction->setIcon(QIcon::fromTheme(QStringLiteral("document-import")));
    connect(importMessagesAction, &QAction::triggered, this, [this] {
        importMessageFiles(m_mailboxKey, m_folders->currentIndex().data(FolderModel::FolderIdRole).toLongLong());
    });

    QAction *autoReplies = actions->addAction(QStringLiteral("automatic_replies"));
    autoReplies->setText(i18nc("@action", "Automatic Replies..."));
    autoReplies->setIcon(QIcon::fromTheme(QStringLiteral("mail-reply-sender")));
    connect(autoReplies, &QAction::triggered, this, [this] {
        openAutoReplies();
    });

    KStandardAction::print(
        this,
        [this] {
            printMessage();
        },
        actions);
    KStandardAction::saveAs(
        this,
        [this] {
            exportMessages(selectedMessages());
        },
        actions);

    QAction *fromTemplate = actions->addAction(QStringLiteral("new_from_template"));
    fromTemplate->setText(i18nc("@action", "New from Template..."));
    fromTemplate->setIcon(QIcon::fromTheme(QStringLiteral("document-new-from-template")));
    connect(fromTemplate, &QAction::triggered, this, &MainWindow::openTemplates);

    // Undo send: how long a sent message waits (graphmailrc General/UndoSendSeconds)
    auto *undoSend = new KSelectAction(QIcon::fromTheme(QStringLiteral("edit-undo")), i18nc("@action", "Undo Send"), this);
    const int seconds[] = {0, 5, 10, 30};
    QStringList undoItems{i18nc("@item undo send", "Off")};
    for (int i = 1; i < 4; ++i) {
        undoItems.append(i18ncp("@item undo send", "%1 second", "%1 seconds", seconds[i]));
    }
    undoSend->setItems(undoItems);
    KConfigGroup general = KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("General"));
    const int current = general.readEntry("UndoSendSeconds", 0);
    undoSend->setCurrentItem(int(std::find(std::begin(seconds), std::end(seconds), current) - std::begin(seconds)) % 4);
    connect(undoSend, &KSelectAction::indexTriggered, this, [seconds](int index) {
        KConfigGroup group = KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("General"));
        group.writeEntry("UndoSendSeconds", seconds[std::clamp(index, 0, 3)]);
        group.sync();
    });
    actions->addAction(QStringLiteral("undo_send"), undoSend);

    QAction *mailboxSettings = actions->addAction(QStringLiteral("mailbox_settings"));
    mailboxSettings->setText(i18nc("@action", "Mailbox Settings..."));
    mailboxSettings->setIcon(QIcon::fromTheme(QStringLiteral("preferences-system-time")));
    connect(mailboxSettings, &QAction::triggered, this, [this] {
        openMailboxSettings();
    });

    QAction *archiveFolderAction = actions->addAction(QStringLiteral("archive_folder"));
    archiveFolderAction->setText(i18nc("@action", "Archive Folder..."));
    archiveFolderAction->setIcon(QIcon::fromTheme(QStringLiteral("archive-insert")));
    connect(archiveFolderAction, &QAction::triggered, this, [this] {
        chooseArchiveFolder(m_mailboxKey);
    });

    QAction *rules = actions->addAction(QStringLiteral("rules"));
    rules->setText(i18nc("@action", "Rules..."));
    rules->setIcon(QIcon::fromTheme(QStringLiteral("view-filter")));
    connect(rules, &QAction::triggered, this, [this] {
        openRules();
    });

    QAction *mailboxesAction = actions->addAction(QStringLiteral("mailboxes"));
    mailboxesAction->setText(i18nc("@action", "Mailboxes..."));
    mailboxesAction->setIcon(QIcon::fromTheme(QStringLiteral("folder-mail")));
    connect(mailboxesAction, &QAction::triggered, this, &MainWindow::openMailboxes);

    KStandardAction::quit(qApp, &QCoreApplication::quit, actions);
    // Settings > Configure Notifications: new mail, failed sending, reminders and the like
    // (the events of plasma-graphmail-linux.notifyrc), as other KDE programs offer it
    KStandardAction::configureNotifications(
        this,
        [this] {
            KNotifyConfigWidget::configure(this);
        },
        actions);
}

WaitingWindow *MainWindow::openWaiting()
{
    auto *window = new WaitingWindow(m_mailboxes, m_daemon, this);
    window->setAttribute(Qt::WA_DeleteOnClose);
    connect(window, &WaitingWindow::openOutgoing, this, [this](const QString &mailboxKey, const QString &uuid) {
        openOutgoing(mailboxKey, uuid);
    });
    window->show();
    return window;
}

void MainWindow::updateAccountsBar()
{
    QString error;
    const QList<GraphCore::ServiceAccount> all = m_accounts->accounts(&error);
    if (!error.isEmpty()) {
        // The service does not answer: the mailboxes say so themselves
        m_accountsBar->hide();
        return;
    }
    QStringList waiting;
    for (const GraphCore::ServiceAccount &a : all) {
        if (a.state == QLatin1String("signInNeeded") || a.state == QLatin1String("signedOut")) {
            waiting.append(a.userPrincipalName.isEmpty() ? a.displayName : a.userPrincipalName);
        }
    }
    if (all.isEmpty()) {
        m_accountsBar->setMessageType(KMessageWidget::Information);
        m_accountsBar->setText(i18nc("@info", "No work account yet. Add one under Microsoft 365 Accounts to see your mail, calendar and contacts."));
    } else if (!waiting.isEmpty()) {
        m_accountsBar->setMessageType(KMessageWidget::Warning);
        m_accountsBar->setText(i18ncp("@info %2 accounts", "%2 has to sign in again: its mail waits until then.", "These accounts have to sign in again: %2. Their mail waits until then.", waiting.size(),
                                      waiting.join(QStringLiteral(", "))));
    } else {
        if (m_accountsBar->isVisible()) {
            m_accountsBar->animatedHide();
        }
        return;
    }
    if (!m_accountsBar->isVisible()) {
        m_accountsBar->animatedShow();
    }
}

UpdatesDialog *MainWindow::openUpdates()
{
    auto *dialog = new UpdatesDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    return dialog;
}

OfflineDialog *MainWindow::openOfflineSettings()
{
    auto *dialog = new OfflineDialog(m_mailboxes, m_daemon, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    return dialog;
}

MailboxesDialog *MainWindow::openMailboxes()
{
    auto *dialog = new MailboxesDialog(m_mailboxes, m_daemon, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    return dialog;
}

CalendarWindow *MainWindow::openCalendar()
{
    calendarModule();
    showModule(2, m_calendar);
    return m_calendar;
}

CalendarWindow *MainWindow::calendarModule()
{
    if (!m_calendar) {
        m_calendar = new CalendarWindow(m_mailboxes, m_daemon);
        m_calendar->setCategoryList(m_categoryList);
        connect(m_calendar, &CalendarWindow::mailRequested, this, [this] {
            navigate(1);
        });
        connect(m_calendar, &CalendarWindow::navigate, this, &MainWindow::navigate);
        if (m_shell) {
            m_shell->embed(2, m_calendar);
        } else {
            m_calendar->setAttribute(Qt::WA_DeleteOnClose);
        }
    }
    return m_calendar;
}

ContactsWindow *MainWindow::openContacts()
{
    if (!m_contacts) {
        m_contacts = new ContactsWindow(m_mailboxes, m_daemon);
        m_contacts->setCategoryList(m_categoryList);
        connect(m_contacts, &ContactsWindow::navigate, this, &MainWindow::navigate);
        if (m_shell) {
            m_shell->embed(3, m_contacts);
        } else {
            m_contacts->setAttribute(Qt::WA_DeleteOnClose);
        }
    }
    showModule(3, m_contacts);
    return m_contacts;
}

TasksWindow *MainWindow::openTasks()
{
    if (!m_tasks) {
        m_tasks = new TasksWindow(m_mailboxes, m_daemon);
        m_tasks->setCategoryList(m_categoryList);
        connect(m_tasks, &TasksWindow::navigate, this, &MainWindow::navigate);
        connect(m_tasks, &TasksWindow::openMail, this, &MainWindow::openMessageById);
        if (m_shell) {
            m_shell->embed(4, m_tasks);
        } else {
            m_tasks->setAttribute(Qt::WA_DeleteOnClose);
        }
    }
    showModule(4, m_tasks);
    return m_tasks;
}

void MainWindow::showModule(int module, QWidget *page)
{
    // In the shell: its page, the window to the front; on their own: the module's window
    QWidget *top = m_shell ? static_cast<QWidget *>(m_shell) : page;
    if (m_shell) {
        m_shell->showModule(module);
    }
    top->show();
    top->raise();
    top->activateWindow();
}

void MainWindow::navigate(int module)
{
    switch (module) {
    case 1:
        showModule(1, this);
        break;
    case 2:
        openCalendar();
        break;
    case 3:
        openContacts();
        break;
    case 4:
        openTasks();
        break;
    default:
        break;
    }
}

QWidget *MainWindow::replyWithMeeting(const QString &mailboxKey, qint64 message)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<MessageRow> original = store && message ? store->message(message) : std::nullopt;
    if (!original) {
        return nullptr;
    }
    // The people of a Reply All (the sender and To required, Cc optional), without the user
    QStringList own{m_mailboxes->mailbox(mailboxKey).value_or(MailboxInfo()).address};
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        if (box.own) {
            own.append(box.address);
        }
    }
    QList<OutgoingRecipient> required;
    QList<OutgoingRecipient> optional;
    Quote::recipients(*original, true, own, &required, &optional);
    // The message below, as Outlook puts it in the meeting
    const QString text = Quote::headerText(*original) + Quote::originalText(*original, m_mailboxes->blobs(mailboxKey));
    return calendarModule()->newMeeting(mailboxKey, original->subject, required, optional, QStringLiteral("\n\n") + text);
}

QWidget *MainWindow::newAppointmentFromMail(const QString &mailboxKey, qint64 message)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<MessageRow> original = store && message ? store->message(message) : std::nullopt;
    if (!original) {
        return nullptr;
    }
    // Outlook's drag to Calendar: an appointment (no attendees) with the message as its text
    const QString text = Quote::headerText(*original) + Quote::originalText(*original, m_mailboxes->blobs(mailboxKey));
    return calendarModule()->newMeeting(mailboxKey, original->subject, {}, {}, text);
}

void MainWindow::newTaskFromMail(const QString &mailboxKey, qint64 message)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<MessageRow> original = store && message ? store->message(message) : std::nullopt;
    if (!original) {
        return;
    }
    const QString text = Quote::headerText(*original) + Quote::originalText(*original, m_mailboxes->blobs(mailboxKey));
    if (openTasks()->newTask(mailboxKey, original->subject, text)) {
        m_status->setText(i18nc("@info:status %1 a subject", "Task \"%1\" added to Tasks.", original->subject));
    }
}

QWidget *MainWindow::openMessageDetails()
{
    const qint64 message = shownMessage();
    return message ? MessageDetailsDialog::open(this, m_mailboxes, m_daemon, m_mailboxKey, message) : nullptr;
}

qint64 MainWindow::shownMessage() const
{
    if (const qint64 message = m_reading->message()) {
        return message;
    }
    const QList<qint64> ids = selectedMessages();
    return ids.isEmpty() ? 0 : ids.constFirst();
}

void MainWindow::printMessage(const QString &pdfFile, const std::function<void(bool)> &done)
{
    const qint64 message = shownMessage();
    const std::optional<MailboxInfo> box = m_mailboxes->mailbox(m_mailboxKey);
    if (message == 0 || !box) {
        if (done) {
            done(false);
        }
        return;
    }
    // Pictures as the reading pane shows them (Load Images)
    const bool remote = m_reading->message() == message && m_reading->remoteAllowed();
    Printing::printMessage(this, m_mailboxes->store(m_mailboxKey), m_mailboxes->blobs(m_mailboxKey), message, remote,
                           box->displayName.isEmpty() ? box->address : box->displayName, pdfFile, done);
}

void MainWindow::exportMessages(const QList<qint64> &messages, const QString &givenFile, const std::function<void(bool)> &done)
{
    Store *store = m_mailboxes->store(m_mailboxKey);
    if (messages.isEmpty() || !store) {
        if (done) {
            done(false);
        }
        return;
    }
    const bool mbox = messages.size() > 1;
    QString file = givenFile;
    if (file.isEmpty()) {
        const std::optional<MessageRow> first = store->message(messages.constFirst());
        QString name = mbox ? m_folders->currentIndex().data(Qt::DisplayRole).toString() : (first ? first->subject : QString());
        file = MimeExport::askFile(this, name, mbox);
        if (file.isEmpty()) {
            return;
        }
    }
    runExport(m_mailboxKey, messages, file, mbox, done);
}

void MainWindow::exportFolder(const QString &mailboxKey, qint64 folder, const QString &file, const std::function<void(bool)> &done)
{
    Store *store = m_mailboxes->store(mailboxKey);
    QList<qint64> ids;
    std::optional<PageCursor> cursor;
    while (store) {
        const QList<ListRow> page = store->listPage(folder, cursor, 1000);
        for (const ListRow &row : page) {
            ids.append(row.id);
        }
        if (page.size() < 1000) {
            break;
        }
        cursor = PageCursor{page.constLast().received, page.constLast().id};
    }
    // Oldest first, as mail programs read an mbox
    std::reverse(ids.begin(), ids.end());
    runExport(mailboxKey, ids, file, true, done);
}

QList<qint64> MainWindow::selectedMessageIds() const
{
    return selectedMessages();
}

void MainWindow::exportPst(const QString &mailboxKey, const QList<qint64> &folders, const QString &givenFile, const std::function<void(bool)> &done, bool itemsOnly)
{
    const std::optional<MailboxInfo> box = m_mailboxes->mailbox(mailboxKey);
    if (!box || !m_mailboxes->store(mailboxKey)) {
        if (done) {
            done(false);
        }
        return;
    }
    QString file = givenFile;
    if (file.isEmpty()) {
        static const QRegularExpression unsafe(QStringLiteral("[/\\\\:*?\"<>|]"));
        QString name = folders.size() == 1 ? m_mailboxes->store(mailboxKey)->folder(folders.first()).value_or(FolderRow()).name : box->label();
        name.replace(unsafe, QStringLiteral("_"));
        file = QFileDialog::getSaveFileName(this,
                                            i18nc("@title:window", "Export to Outlook Data File"),
                                            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QLatin1Char('/') + name + QStringLiteral(".pst"),
                                            i18nc("@item:inlistbox file type", "Outlook data file (*.pst)"));
        if (file.isEmpty()) {
            return;
        }
    }
    auto *exporter = new PstExporter(m_mailboxes, m_daemon, mailboxKey, folders, file, this);
    exporter->setItemsOnly(itemsOnly);
    connect(exporter, &PstExporter::finished, this, [this, done](bool ok, int original, int rebuilt, int items, const QString &path, const QString &error) {
        if (!ok) {
            m_status->setText(error.isEmpty() ? i18nc("@info:status %1 a file", "Could not save %1.", path)
                                              : i18nc("@info:status %1 a file, %2 the reason", "Could not save %1: %2", path, error));
        } else {
            QString text = rebuilt > 0 ? i18ncp("@info:status %2 a file",
                                                "Saved in %2; %1 message was rebuilt from this computer's copy (Microsoft 365 did not answer).",
                                                "Saved in %2; %1 messages were rebuilt from this computer's copy (Microsoft 365 did not answer).",
                                                rebuilt,
                                                path)
                                       : i18ncp("@info:status %2 a file", "%1 message saved in %2.", "%1 messages saved in %2.", original, path);
            if (items > 0) {
                // Contacts, tasks and series are whole; single events: those on this computer
                text += QLatin1Char(' ')
                    + i18ncp("@info:status", "Also %1 contact, calendar item or task (calendar: the events this computer has).",
                             "Also %1 contacts, calendar items and tasks (calendar: the events this computer has).", items);
            }
            m_status->setText(text);
        }
        if (done) {
            done(ok);
        }
    });
    exporter->start();
}

void MainWindow::importPst(const QString &mailboxKey, const QString &givenFile, bool merge, bool ask, const std::function<void(bool)> &done)
{
    const auto fail = [done] {
        if (done) {
            done(false);
        }
    };
    const std::optional<MailboxInfo> box = m_mailboxes->mailbox(mailboxKey);
    if (!box) {
        fail();
        return;
    }
    QString file = givenFile;
    if (file.isEmpty()) {
        file = QFileDialog::getOpenFileName(this,
                                            i18nc("@title:window", "Import Outlook Data File"),
                                            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                            i18nc("@item:inlistbox file type", "Outlook data file (*.pst *.ost)"));
        if (file.isEmpty()) {
            return;
        }
    }
    auto pst = std::make_shared<Pst::PstFile>();
    QString error;
    if (!pst->open(file, &error)) {
        if (ask) {
            KMessageBox::error(this, i18nc("@info %1 a file, %2 the reason", "%1 could not be opened as an Outlook data file: %2", file, error));
        }
        fail();
        return;
    }
    // A password protects the file in Outlook's interface only (MS-PST 4.2), but it MUST be asked
    if (const quint32 crc = pst->passwordCrc(); crc != 0) {
        bool accepted = false;
        const QString password = ask ? QInputDialog::getText(this, i18nc("@title:window", "Outlook Data File Password"),
                                                             i18nc("@label:textbox %1 a file", "The password of %1:", file), QLineEdit::Password, {}, &accepted)
                                     : QString();
        if (!accepted || !PstImport::passwordMatches(password, crc)) {
            if (accepted) {
                KMessageBox::error(this, i18nc("@info", "The password is not correct."));
            }
            fail();
            return;
        }
    }
    QList<quint32> folders;
    QHash<quint32, quint32> parents;
    QString newFolder;
    PstImportDialog dialog(pst.get(), file, box->label(), this);
    if (ask) {
        if (dialog.exec() != QDialog::Accepted) {
            fail();
            return;
        }
        newFolder = dialog.newFolderName();
    } else {
        newFolder = merge ? QString() : QFileInfo(file).completeBaseName();
    }
    folders = dialog.folders();
    parents = dialog.parents();
    auto *importer = new PstImporter(m_daemon, mailboxKey, pst, folders, parents, QStringLiteral("msgfolderroot"), newFolder, this);
    connect(importer, &PstImporter::finished, this, [this, done, ask](bool ok, int imported, int skipped, int failed, const QStringList &log) {
        const QString summary = i18ncp("@info %2 skipped, %3 failed", "%1 message imported; %2 left out, %3 failed.", "%1 messages imported; %2 left out, %3 failed.", imported,
                                       skipped, failed);
        m_status->setText(summary);
        if (ask) {
            if (log.isEmpty()) {
                KMessageBox::information(this, summary, i18nc("@title:window", "Import Outlook Data File"));
            } else {
                KMessageBox::informationList(this, summary, log, i18nc("@title:window", "Import Outlook Data File"));
            }
        }
        for (const QString &line : log) {
            qWarning() << "pst import log:" << line;
        }
        if (done) {
            done(ok);
        }
    });
    importer->start();
}

QWidget *MainWindow::openMessageFile(const QString &givenFile)
{
    QString file = givenFile;
    if (file.isEmpty()) {
        file = QFileDialog::getOpenFileName(this,
                                            i18nc("@title:window", "Open Message File"),
                                            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                            i18nc("@item:inlistbox file type", "Messages (*.msg *.eml)"));
        if (file.isEmpty()) {
            return nullptr;
        }
    }
    QString shown = file;
    QTemporaryDir *dir = nullptr;
    if (file.endsWith(QLatin1String(".msg"), Qt::CaseInsensitive)) {
        // Outlook's format: as MIME into KDE's message viewer, from a private temporary folder
        dir = new QTemporaryDir(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("graphmail-XXXXXX")));
        shown = dir->filePath(QFileInfo(file).completeBaseName() + QStringLiteral(".eml"));
        QString error;
        if (!MimeMapi::msgToEml(file, shown, &error)) {
            delete dir;
            KMessageBox::error(this, i18nc("@info %1 a file, %2 the reason", "%1 could not be opened: %2", file, error));
            return nullptr;
        }
    }
    auto *viewer = new MimeTreeParser::Widgets::MessageViewerDialog(shown, this);
    viewer->setAttribute(Qt::WA_DeleteOnClose);
    if (dir) {
        connect(viewer, &QObject::destroyed, this, [dir] {
            delete dir;
        });
    }
    viewer->show();
    return viewer;
}

QWidget *MainWindow::showFileForTests(const QString &file)
{
    if (!file.endsWith(QLatin1String(".pst"), Qt::CaseInsensitive)) {
        QWidget *viewer = openMessageFile(file);
        if (viewer) {
            viewer->resize(900, 700);
        }
        return viewer;
    }
    auto pst = std::make_shared<Pst::PstFile>();
    QString error;
    if (!pst->open(file, &error)) {
        return nullptr;
    }
    const std::optional<MailboxInfo> box = m_mailboxes->mailbox(m_mailboxKey);
    auto *dialog = new PstImportDialog(pst.get(), file, box ? box->label() : m_mailboxKey, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    // The file stays open as long as the dialog shows it
    connect(dialog, &QObject::destroyed, this, [pst] {});
    dialog->show();
    return dialog;
}

void MainWindow::openFiles(const QStringList &files)
{
    for (const QString &file : files) {
        const QString lower = file.toLower();
        if (lower.endsWith(QLatin1String(".pst")) || lower.endsWith(QLatin1String(".ost"))) {
            importPst(m_mailboxKey, file);
        } else if (QFileInfo::exists(file)) {
            openMessageFile(file);
        }
    }
}

QWidget *MainWindow::openMailto(const QString &link)
{
    const Mailto::Link mail = Mailto::parse(QUrl(link));
    QStringList files;
    for (const QString &file : mail.attachments) {
        if (QFileInfo(file).isFile()) {
            files.append(file);
        }
    }
    // A link on a web page could name any file: attach only what the user agrees to
    if (!files.isEmpty()
        && KMessageBox::questionTwoActionsList(this,
                                               i18nc("@info", "Another program asks to attach these files to the new message:"),
                                               files,
                                               i18nc("@title:window", "Attach Files"),
                                               KGuiItem(i18nc("@action:button", "Attach"), QStringLiteral("mail-attachment")),
                                               KGuiItem(i18nc("@action:button", "Leave Out"), QStringLiteral("dialog-cancel")))
            != KMessageBox::PrimaryAction) {
        files.clear();
    }
    auto *composer = static_cast<ComposerWindow *>(openComposer(QStringLiteral("new"), 0));
    composer->prefill(mail.to, mail.cc, mail.bcc, mail.subject, mail.body, files);
    return composer;
}

QList<QUrl> MainWindow::writeMessageFiles(const QString &mailboxKey, const QList<qint64> &messages)
{
    // In the app's cache: a message dropped on the composer is read by the daemon too
    if (!m_dragFiles) {
        const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir().mkpath(cache);
        m_dragFiles = std::make_unique<QTemporaryDir>(cache + QStringLiteral("/drag-XXXXXX"));
    }
    // A folder per drag: the same subject twice in one drag gets a number, not another drag's file
    const QDir dir(m_dragFiles->path() + QLatin1Char('/') + QString::number(++m_drags));
    if (!m_dragFiles->isValid() || !QDir().mkpath(dir.path())) {
        return {};
    }
    const Store *store = m_mailboxes->store(mailboxKey);
    QList<QUrl> urls;
    QElapsedTimer waited;
    waited.start();
    for (qint64 message : messages.mid(0, 200)) {
        const std::optional<MessageRow> row = store ? store->message(message) : std::nullopt;
        if (!row) {
            continue;
        }
        // The drop waits for the files: the original from Exchange while that is quick, the copy
        // rebuilt from the cache for the rest
        QByteArray mime;
        if (waited.elapsed() < 15000) {
            QEventLoop loop;
            auto guard = std::make_unique<QObject>();
            MimeExport::fetch(m_mailboxes, m_daemon, mailboxKey, message, guard.get(), [&mime, &loop](const QByteArray &data, bool) {
                mime = data;
                loop.quit();
            });
            if (mime.isEmpty()) {
                QTimer::singleShot(10000, &loop, &QEventLoop::quit);
                loop.exec(QEventLoop::ExcludeUserInputEvents);
            }
        }
        if (mime.isEmpty()) {
            mime = MimeExport::rebuild(m_mailboxes->store(mailboxKey), m_mailboxes->blobs(mailboxKey), message);
        }
        const QString base = MimeExport::safeFileName(row->subject);
        QString path = dir.filePath(base + QStringLiteral(".eml"));
        for (int n = 2; QFileInfo::exists(path); ++n) {
            path = dir.filePath(QStringLiteral("%1 (%2).eml").arg(base).arg(n));
        }
        QFile file(path);
        if (file.open(QIODevice::WriteOnly) && file.write(mime) == mime.size()) {
            urls.append(QUrl::fromLocalFile(path));
        }
    }
    return urls;
}

void MainWindow::importMessageFiles(const QString &mailboxKey, qint64 folder, QStringList files, const std::function<void(bool)> &done)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<FolderRow> row = store && folder != 0 ? store->folder(folder) : std::nullopt;
    if (!row) {
        if (done) {
            done(false);
        }
        return;
    }
    const bool ask = files.isEmpty();
    if (ask) {
        files = QFileDialog::getOpenFileNames(this,
                                              i18nc("@title:window %1 a folder", "Import Message Files into %1", row->name),
                                              QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                              i18nc("@item:inlistbox file type", "Messages (*.msg *.eml)"));
        if (files.isEmpty()) {
            return;
        }
    }
    // The log is for people: tests (which pass done) read the journal instead
    const bool showLog = ask || !done;
    auto *importer = new MessageFileImporter(m_daemon, mailboxKey, files, row->graphId, this);
    connect(importer, &MessageFileImporter::finished, this, [this, done, showLog](bool ok, int imported, int skipped, int failed, const QStringList &log) {
        const QString summary = i18ncp("@info %2 skipped, %3 failed", "%1 message imported; %2 left out, %3 failed.", "%1 messages imported; %2 left out, %3 failed.", imported,
                                       skipped, failed);
        m_status->setText(summary);
        if (showLog && !log.isEmpty()) {
            KMessageBox::informationList(this, summary, log, i18nc("@title:window", "Import Message Files"));
        }
        for (const QString &line : log) {
            qWarning() << "message import log:" << line;
        }
        if (done) {
            done(ok);
        }
    });
    importer->start();
}

void MainWindow::runExport(const QString &mailboxKey, const QList<qint64> &messages, const QString &file, bool mbox, const std::function<void(bool)> &done)
{
    if (messages.isEmpty()) {
        if (done) {
            done(false);
        }
        return;
    }
    auto *exporter = new MimeExporter(m_mailboxes, m_daemon, mailboxKey, messages, file, mbox, this);
    connect(exporter, &MimeExporter::finished, this, [this, done](bool ok, int original, int rebuilt, const QString &path) {
        if (!ok) {
            m_status->setText(i18nc("@info:status %1 a file", "Could not save %1.", path));
        } else if (rebuilt > 0) {
            m_status->setText(i18ncp("@info:status %2 a file",
                                     "Saved in %2; %1 message was rebuilt from this computer's copy (Microsoft 365 did not answer).",
                                     "Saved in %2; %1 messages were rebuilt from this computer's copy (Microsoft 365 did not answer).",
                                     rebuilt,
                                     path));
        } else {
            m_status->setText(i18ncp("@info:status %2 a file", "%1 message saved in %2.", "%1 messages saved in %2.", original, path));
        }
        if (done) {
            done(ok);
        }
    });
    exporter->start();
}

void MainWindow::openInNewWindow(int module)
{
    KXmlGuiWindow *window = nullptr;
    if (module == 2) {
        auto *calendar = new CalendarWindow(m_mailboxes, m_daemon);
        calendar->setCategoryList(m_categoryList);
        connect(calendar, &CalendarWindow::mailRequested, this, [this] {
            navigate(1);
        });
        connect(calendar, &CalendarWindow::navigate, this, &MainWindow::navigate);
        window = calendar;
    } else if (module == 3) {
        auto *contacts = new ContactsWindow(m_mailboxes, m_daemon);
        contacts->setCategoryList(m_categoryList);
        connect(contacts, &ContactsWindow::navigate, this, &MainWindow::navigate);
        window = contacts;
    } else if (module == 4) {
        auto *tasks = new TasksWindow(m_mailboxes, m_daemon);
        tasks->setCategoryList(m_categoryList);
        connect(tasks, &TasksWindow::navigate, this, &MainWindow::navigate);
        connect(tasks, &TasksWindow::openMail, this, &MainWindow::openMessageById);
        window = tasks;
    }
    if (window) {
        window->setAttribute(Qt::WA_DeleteOnClose);
        m_detached.append(window);
        window->show();
    }
}

void MainWindow::openMessageById(const QString &mailboxKey, qint64 message)
{
    auto *window = new MessageWindow(m_mailboxes, m_daemon, m_folderModel, mailboxKey, message, m_markReadEnabled);
    connect(window, &MessageWindow::replyWithMeetingRequested, this, &MainWindow::replyWithMeeting);
    connect(window, &MessageWindow::mailtoRequested, this, &MainWindow::openMailto);
    m_messageWindows.append(window);
    window->show();
}

QWidget *MainWindow::openComposer(const QString &kind, qint64 message, const QString &outgoing)
{
    ComposeRequest request;
    request.mailboxKey = m_mailboxKey;
    request.kind = kind;
    request.message = message;
    request.outgoing = outgoing;
    auto *composer = new ComposerWindow(m_mailboxes, m_daemon, request);
    composer->show();
    return composer;
}

QWidget *MainWindow::composeWithCloudLinks(const QStringList &paths)
{
    QString key = m_mailboxKey;
    for (const QString &path : paths) {
        const GraphSyncLink::Location location = GraphSyncLink::locationOf(path);
        if (!location.id.isEmpty()) {
            // That account's own mailbox, when Graph Mail has it
            const QString own = location.accountId + QStringLiteral("/me");
            if (m_mailboxes->mailbox(own)) {
                key = own;
            }
            break;
        }
    }
    ComposeRequest request;
    request.mailboxKey = key;
    request.kind = QStringLiteral("new");
    auto *composer = new ComposerWindow(m_mailboxes, m_daemon, request);
    composer->show();
    composer->attachCloudFiles(paths);
    return composer;
}

QWidget *MainWindow::openOutgoing(const QString &mailboxKey, const QString &uuid)
{
    selectFolder(mailboxKey);
    ComposeRequest request;
    request.mailboxKey = mailboxKey;
    request.kind = QStringLiteral("draft");
    request.outgoing = uuid;
    auto *composer = new ComposerWindow(m_mailboxes, m_daemon, request);
    composer->show();
    return composer;
}

QString MainWindow::account() const
{
    return m_mailboxKey.section(QLatin1Char('/'), 0, 0);
}

QString MainWindow::mailbox() const
{
    return m_mailboxKey.section(QLatin1Char('/'), 1);
}

QList<QPair<QString, QList<qint64>>> MainWindow::searchTargets() const
{
    const QString scope = m_searchScope ? m_searchScope->currentData().toString() : QStringLiteral("mailbox");
    const QString home = m_folderMailboxKey.isEmpty() ? m_mailboxKey : m_folderMailboxKey;
    QList<QPair<QString, QList<qint64>>> targets;
    if (scope == QLatin1String("folder") && m_folder != 0) {
        targets.append({home, {m_folder}});
    } else if (scope == QLatin1String("all")) {
        const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
        for (const MailboxInfo &box : boxes) {
            if (box.mail && m_mailboxes->store(box.key())) {
                targets.append({box.key(), {}});
            }
        }
    } else {
        targets.append({home, {}});
    }
    return targets;
}

void MainWindow::search()
{
    m_searchTimer.stop();
    m_onlineTimer.stop();
    ++m_searchRun;
    const QString text = m_searchField->text().trimmed();
    m_reading->clear();
    if (text.isEmpty() && !filtersActive()) {
        // Back to the folder the pane shows (a result from another mailbox may have switched)
        if (!m_folderMailboxKey.isEmpty()) {
            m_mailboxKey = m_folderMailboxKey;
        }
        Store *store = m_mailboxes->store(m_mailboxKey);
        if (!store) {
            return;
        }
        if (m_conversations) {
            m_conversationModel->setFolder(m_mailboxKey, store, m_folder, m_showRecipients);
        } else {
            m_messageModel->setFolder(m_mailboxKey, store, m_folder, m_showRecipients);
        }
        applyListModel();
        m_messages->scrollToTop();
        statusBar()->clearMessage();
        showResults(-1, false);
        return;
    }
    const SearchQuery query = searchQuery();
    const QList<QPair<QString, QList<qint64>>> targets = searchTargets();
    m_onlineHits = std::make_shared<QHash<QString, QList<qint64>>>();
    const auto online = m_onlineHits;
    MailboxSet *mailboxes = m_mailboxes;
    // The cache's rows for the query, plus Graph's results once they are in, newest first
    m_messageModel->setResults([mailboxes, query, targets, online]() {
        QList<SearchHit> hits;
        QSet<QString> seen;
        for (const auto &[key, folders] : targets) {
            const Store *store = mailboxes->store(key);
            if (!store) {
                continue;
            }
            QList<ListRow> rows = store->search(query, folders, 500);
            if (const QList<qint64> ids = online->value(key); !ids.isEmpty()) {
                // Graph matched the words already (also in text the cache does not hold): only
                // the filters it cannot apply (read, flagged) stay
                SearchQuery filters = query;
                filters.words.clear();
                filters.fields.clear();
                rows += store->search(filters, folders, 1000, ids);
            }
            QHash<qint64, QString> folderNames;
            for (const ListRow &row : std::as_const(rows)) {
                const QString id = key + QLatin1Char('/') + QString::number(row.id);
                if (seen.contains(id)) {
                    continue;
                }
                seen.insert(id);
                if (!folderNames.contains(row.folder)) {
                    const std::optional<FolderRow> folder = store->folder(row.folder);
                    folderNames.insert(row.folder, folder ? folder->name : QString());
                }
                hits.append({key, row, folderNames.value(row.folder)});
            }
        }
        std::stable_sort(hits.begin(), hits.end(), [](const SearchHit &a, const SearchHit &b) {
            return a.row.received != b.row.received ? a.row.received > b.row.received : a.row.id > b.row.id;
        });
        if (hits.size() > 1000) {
            hits.resize(1000);
        }
        return hits;
    });
    applyListModel();
    m_messages->scrollToTop();
    // Graph searches the message text of older mail too; the filters alone need no Graph
    const bool askGraph = !query.kql().isEmpty() && m_daemon->isRunning();
    showResults(m_messageModel->rowCount(), askGraph);
    if (askGraph) {
        m_onlineTimer.start();
    }
}

void MainWindow::loadSearchFolders()
{
    if (!m_daemon->isRunning()) {
        return;
    }
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        if (!box.mail) {
            continue;
        }
        const QString key = box.key();
        m_daemon->query(box.account, box.mailbox, QStringLiteral("searchFolders"), QStringLiteral("{}"), this, [this, key](const QString &json) {
            const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
            if (!answer.value(QLatin1String("ok")).toBool()) {
                return;
            }
            QList<SearchFolderInfo> folders;
            const QJsonArray value = answer.value(QLatin1String("body")).toObject().value(QLatin1String("value")).toArray();
            for (const QJsonValue &v : value) {
                const QJsonObject f = v.toObject();
                const QString name = f.value(QLatin1String("displayName")).toString();
                // Outlook on the web's own (Focused Inbox) search folders stay hidden
                if (name.startsWith(QLatin1String("OwaFV15"))) {
                    continue;
                }
                folders.append({f.value(QLatin1String("id")).toString(), name, f.value(QLatin1String("filterQuery")).toString()});
            }
            std::sort(folders.begin(), folders.end(), [](const SearchFolderInfo &a, const SearchFolderInfo &b) {
                return a.name.localeAwareCompare(b.name) < 0;
            });
            // Through reloadFolders: what was open and selected stays so
            m_folderModel->setSearchFolders(key, folders);
            reloadFolders();
        });
    }
}

void MainWindow::showSearchFolder(const QString &mailboxKey, const QString &id, const QString &name)
{
    const QString caption = m_mailboxes->mailbox(mailboxKey) ? i18nc("@title:window folder - mailbox", "%1 - %2", name, m_mailboxes->mailbox(mailboxKey)->label()) : name;
    setCaption(caption);
    {
        const QSignalBlocker blocker(m_searchField);
        m_searchField->clear();
    }
    ++m_searchRun;
    const quint64 run = m_searchRun;
    m_messageModel->clear();
    applyListModel();
    statusBar()->showMessage(i18nc("@info:status", "Asking Microsoft 365 what %1 holds...", name));
    m_daemon->query(mailboxKey.section(QLatin1Char('/'), 0, 0), mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("searchFolderMessages"),
                    QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("max"), 1000}}).toJson(QJsonDocument::Compact)), this,
                    [this, run, mailboxKey, name](const QString &json) {
                        if (run != m_searchRun) {
                            return;
                        }
                        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                        const Store *store = m_mailboxes->store(mailboxKey);
                        if (!store || !answer.value(QLatin1String("ok")).toBool()) {
                            statusBar()->showMessage(i18nc("@info:status", "%1 could not be read from Microsoft 365 (offline?)", name));
                            qWarning() << "search folder test: open failed";
                            return;
                        }
                        QStringList graphIds;
                        const QJsonArray ids = answer.value(QLatin1String("ids")).toArray();
                        for (const QJsonValue &v : ids) {
                            graphIds.append(v.toString());
                        }
                        const QList<qint64> rows = store->messageIdsByGraphIds(graphIds).values();
                        MailboxSet *mailboxes = m_mailboxes;
                        m_messageModel->setResults([mailboxes, mailboxKey, rows]() {
                            QList<SearchHit> hits;
                            const Store *store = mailboxes->store(mailboxKey);
                            if (!store || rows.isEmpty()) {
                                return hits;
                            }
                            QHash<qint64, QString> folderNames;
                            const QList<ListRow> found = store->search(SearchQuery(), {}, 1000, rows);
                            for (const ListRow &row : found) {
                                if (!folderNames.contains(row.folder)) {
                                    const std::optional<FolderRow> folder = store->folder(row.folder);
                                    folderNames.insert(row.folder, folder ? folder->name : QString());
                                }
                                hits.append({mailboxKey, row, folderNames.value(row.folder)});
                            }
                            return hits;
                        });
                        applyListModel();
                        qWarning() << "search folder test: opened" << name << m_messageModel->rowCount();
                        statusBar()->showMessage(i18ncp("@info:status", "%1 message", "%1 messages", m_messageModel->rowCount()));
                    });
}

void MainWindow::folderMenu(const QPoint &pos)
{
    const QModelIndex index = m_folders->indexAt(pos);
    const QString key = index.data(FolderModel::MailboxKeyRole).toString();
    QMenu menu(this);
    // Entries that follow Favorites
    std::function<void(QMenu *)> tail;
    if (index.data(FolderModel::IsSearchGroupRole).toBool()) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("folder-new")), i18nc("@action:inmenu", "New Search Folder..."), this, [this, key] {
            editSearchFolder(key, {}, {}, {});
        });
    } else if (const QString id = index.data(FolderModel::SearchFolderIdRole).toString(); !id.isEmpty()) {
        const QString name = index.data(Qt::DisplayRole).toString();
        const QString filter = index.data(FolderModel::SearchFilterRole).toString();
        menu.addAction(QIcon::fromTheme(QStringLiteral("document-edit")), i18nc("@action:inmenu", "Change Search Folder..."), this, [this, key, id, name, filter] {
            editSearchFolder(key, id, name, filter);
        });
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action:inmenu", "Delete Search Folder"), this, [this, key, id, name] {
            deleteSearchFolder(key, id, name, true);
        });
    } else if (!key.isEmpty() && index.data(FolderModel::FolderIdRole).toLongLong() != 0) {
        // Outlook on the web's folder menu: counts, new/rename/count, move/delete/empty, mark read
        const qint64 folder = index.data(FolderModel::FolderIdRole).toLongLong();
        const std::optional<FolderRow> row = m_mailboxes->store(key) ? m_mailboxes->store(key)->folder(folder) : std::nullopt;
        if (!row) {
            return;
        }
        const int unread = index.data(FolderModel::UnreadRole).toInt();
        const bool own = row->wellKnown.isEmpty(); // Outlook's own folders keep their name and place
        QAction *counts = menu.addAction(i18ncp("@info folder menu, %2 unread", "%1 item (%2 unread)", "%1 items (%2 unread)", row->total, unread));
        counts->setEnabled(false);
        menu.addSeparator();
        menu.addAction(QIcon::fromTheme(QStringLiteral("folder-new")), i18nc("@action:inmenu", "New Folder..."), this, [this, key, folder] {
            newFolder(key, folder);
        });
        if (own) {
            menu.addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), i18nc("@action:inmenu", "Rename Folder..."), this, [this, key, folder] {
                renameFolder(key, folder);
            });
        }
        QMenu *countMenu = menu.addMenu(QIcon::fromTheme(QStringLiteral("format-number-percent")), i18nc("@action:inmenu", "Change Folder Count"));
        const bool total = FolderModel::showsTotal(key, row->graphId, row->wellKnown);
        auto *countGroup = new QActionGroup(countMenu);
        QAction *showUnread = countMenu->addAction(i18nc("@action:inmenu", "Show Unread Count"), this, [this, key, row] {
            FolderModel::setShowsTotal(key, row->graphId, false);
            m_folders->viewport()->update();
        });
        QAction *showTotal = countMenu->addAction(i18nc("@action:inmenu", "Show Total Count"), this, [this, key, row] {
            FolderModel::setShowsTotal(key, row->graphId, true);
            m_folders->viewport()->update();
        });
        for (QAction *choice : {showUnread, showTotal}) {
            choice->setCheckable(true);
            countGroup->addAction(choice);
        }
        (total ? showTotal : showUnread)->setChecked(true);
        menu.addSeparator();
        if (own) {
            menu.addAction(QIcon::fromTheme(QStringLiteral("go-jump")), i18nc("@action:inmenu", "Move Folder..."), this, [this, key, folder] {
                moveFolder(key, folder);
            });
            menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action:inmenu", "Delete Folder"), this, [this, key, folder] {
                deleteFolder(key, folder);
            });
        }
        QAction *empty = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete-shred")), i18nc("@action:inmenu", "Empty Folder"), this, [this, key, folder] {
            emptyFolder(key, folder);
        });
        empty->setEnabled(row->total > 0);
        menu.addSeparator();
        QAction *read = menu.addAction(QIcon::fromTheme(QStringLiteral("mail-mark-read")), i18nc("@action:inmenu", "Mark All as Read"), this, [this, key, folder] {
            markAllRead(key, folder);
        });
        read->setEnabled(unread > 0);
        // Graph Mail's own extras, after Favorites
        const QString name = row->name;
        const std::optional<MailboxInfo> owner = m_mailboxes->mailbox(key);
        tail = [this, key, folder, name, row, owner](QMenu *m) {
            m->addSeparator();
            // No Graph API for folder permissions: Outlook on the web has them
            m->addAction(QIcon::fromTheme(QStringLiteral("system-users")), i18nc("@action:inmenu", "Sharing and Permissions (Outlook on the web)..."), this, [this, key] {
                openOnlineArchive(key);
                statusBar()->showMessage(i18nc("@info:status", "In Outlook on the web: right-click the folder, Sharing and permissions"), 10000);
            });
            m->addSeparator();
            m->addAction(QIcon::fromTheme(QStringLiteral("folder-saved-search")), i18nc("@action:inmenu", "New Search Folder..."), this, [this, key] {
                editSearchFolder(key, {}, {}, {});
            });
            m->addAction(QIcon::fromTheme(QStringLiteral("document-export")), i18nc("@action:inmenu", "Export to mbox..."), this, [this, key, folder, name] {
                if (const QString file = MimeExport::askFile(this, name, true); !file.isEmpty()) {
                    exportFolder(key, folder, file);
                }
            });
            m->addAction(QIcon::fromTheme(QStringLiteral("document-export")), i18nc("@action:inmenu", "Export to Outlook Data File..."), this, [this, key, folder] {
                exportPst(key, {folder});
            });
            // A folder's own cleanup setting (own mailboxes; never the folders cleanup leaves alone)
            if (owner && owner->own && !Cleanup::excluded(m_mailboxes->store(key), *row)) {
                m->addAction(QIcon::fromTheme(QStringLiteral("edit-clear-history")), i18nc("@action:inmenu", "Clean Up Settings..."), this, [this, key, row] {
                    Cleanup::FolderDialog dialog(key, row->graphId, row->name, this);
                    dialog.exec();
                });
            }
        };
    } else if (const std::optional<MailboxInfo> box = m_mailboxes->mailbox(key); box && !box->own && index.data(FolderModel::IsMailboxRole).toBool()) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("folder-new")), i18nc("@action:inmenu", "New Folder..."), this, [this, key] {
            newFolder(key, 0);
        });
    } else if (box && box->own && index.data(FolderModel::IsMailboxRole).toBool()) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("folder-new")), i18nc("@action:inmenu", "New Folder..."), this, [this, key] {
            newFolder(key, 0);
        });
        menu.addSeparator();
        menu.addAction(QIcon::fromTheme(QStringLiteral("document-import")), i18nc("@action:inmenu", "Import Outlook Data File..."), this, [this, key] {
            importPst(key);
        });
        menu.addAction(QIcon::fromTheme(QStringLiteral("document-export")), i18nc("@action:inmenu", "Export to Outlook Data File..."), this, [this, key] {
            exportPst(key, {});
        });
        menu.addSeparator();
        // The own mailbox's photo, as Outlook's account picture
        menu.addAction(QIcon::fromTheme(QStringLiteral("camera-photo")), i18nc("@action:inmenu", "Change Photo..."), this, [this, key] {
            m_photos->change(this, key);
        });
        if (!m_mailboxes->photo(key).isNull()) {
            menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action:inmenu", "Remove Photo"), this, [this, key] {
                m_photos->remove(this, key);
            });
        }
    }
    // Favorites (docs/PHASE9B.md 9b.27)
    if (const QString graphId = index.data(FolderModel::FolderIdRole).toLongLong() != 0 && !key.isEmpty() && m_mailboxes->store(key)
            ? m_mailboxes->store(key)->folder(index.data(FolderModel::FolderIdRole).toLongLong()).value_or(FolderRow()).graphId
            : QString();
        !graphId.isEmpty()) {
        const bool favorite = FolderModel::favorites().contains(key + QLatin1Char('|') + graphId);
        if (!menu.isEmpty()) {
            menu.addSeparator();
        }
        menu.addAction(QIcon::fromTheme(favorite ? QStringLiteral("non-starred-symbolic") : QStringLiteral("starred-symbolic")),
                       favorite ? i18nc("@action:inmenu", "Remove from Favorites") : i18nc("@action:inmenu", "Add to Favorites"), this, [this, key, graphId, favorite] {
                           m_folderModel->setFavorite(key, graphId, !favorite);
                           reloadFolders();
                       });
    }
    if (tail) {
        tail(&menu);
    }
    if (index.data(FolderModel::IsMailboxRole).toBool() && m_mailboxes->mailbox(key)) {
        if (!menu.isEmpty()) {
            menu.addSeparator();
        }
        menu.addAction(QIcon::fromTheme(QStringLiteral("folder-html")), i18nc("@action:inmenu", "Open Online Archive in Browser"), this, [this, key] {
            openOnlineArchive(key);
        });
    }
    if (!menu.isEmpty()) {
        menu.exec(m_folders->viewport()->mapToGlobal(pos));
    }
}

void MainWindow::openOnlineArchive(const QString &mailboxKey)
{
    // Graph gives no content of the online archive (docs/DESIGN.md), Outlook on the web shows it
    // as "In-Place Archive" in the folder pane. A shared mailbox opens by its address.
    const std::optional<MailboxInfo> box = m_mailboxes->mailbox(mailboxKey);
    QUrl url(QStringLiteral("https://outlook.office.com/mail/"));
    if (box && !box->own && !box->address.isEmpty()) {
        url.setPath(QStringLiteral("/mail/") + box->address + QLatin1Char('/'));
    }
    QDesktopServices::openUrl(url);
    statusBar()->showMessage(i18nc("@info:status", "Outlook on the web shows the online archive as \"In-Place Archive\" in its folder list"), 8000);
}

void MainWindow::editSearchFolder(const QString &mailboxKey, const QString &id, const QString &name, const QString &filter)
{
    // "Look in" offers the folder shown now when it belongs to this mailbox
    const QModelIndex current = m_folders->currentIndex();
    const bool here = current.data(FolderModel::MailboxKeyRole).toString() == mailboxKey && current.data(FolderModel::FolderIdRole).toLongLong() != 0;
    SearchFolderDialog dialog(name, filter, here ? current.data(Qt::DisplayRole).toString() : QString(), !id.isEmpty(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    QJsonObject args{{QStringLiteral("name"), dialog.name()}, {QStringLiteral("filter"), dialog.filter()}};
    if (!id.isEmpty()) {
        args.insert(QStringLiteral("id"), id);
    } else if (dialog.currentFolderOnly()) {
        args.insert(QStringLiteral("folder"), current.data(FolderModel::FolderIdRole).toLongLong());
    }
    m_daemon->query(mailboxKey.section(QLatin1Char('/'), 0, 0), mailboxKey.section(QLatin1Char('/'), 1),
                    id.isEmpty() ? QStringLiteral("searchFolderCreate") : QStringLiteral("searchFolderUpdate"),
                    QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this, [this](const QString &json) {
                        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                        qWarning() << "search folder test: saved" << answer.value(QLatin1String("ok")).toBool() << answer.value(QLatin1String("detail")).toString();
                        statusBar()->showMessage(answer.value(QLatin1String("ok")).toBool()
                                                     ? i18nc("@info:status", "Search folder saved; Exchange fills it in a few seconds")
                                                     : i18nc("@info:status", "Exchange did not accept the search folder: %1", answer.value(QLatin1String("detail")).toString()),
                                                 8000);
                        loadSearchFolders();
                    });
}

void MainWindow::deleteSearchFolder(const QString &mailboxKey, const QString &id, const QString &name, bool ask)
{
    if (ask
        && KMessageBox::warningContinueCancel(this,
                                              i18nc("@info %1 a search folder", "Delete the search folder %1? The messages in it stay where they are.", name),
                                              i18nc("@title:window", "Delete Search Folder"),
                                              KStandardGuiItem::del())
            != KMessageBox::Continue) {
        return;
    }
    m_daemon->query(mailboxKey.section(QLatin1Char('/'), 0, 0), mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("searchFolderDelete"),
                    QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("id"), id}}).toJson(QJsonDocument::Compact)), this, [this](const QString &json) {
                        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                        qWarning() << "search folder test: deleted" << answer.value(QLatin1String("ok")).toBool() << answer.value(QLatin1String("detail")).toString();
                        loadSearchFolders();
                    });
}

void MainWindow::searchFolderForTests(const QString &action, const QString &name, const QString &filter)
{
    if (!name.contains(QLatin1String("[TEST Graph Mail]"))) {
        qWarning() << "search folder test: not a test name";
        return;
    }
    const QString key = m_mailboxes->primaryAccount() + QStringLiteral("/me");
    if (action == QLatin1String("create")) {
        m_daemon->query(key.section(QLatin1Char('/'), 0, 0), QStringLiteral("me"), QStringLiteral("searchFolderCreate"),
                        QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("filter"), filter}}).toJson(QJsonDocument::Compact)), this,
                        [this](const QString &json) {
                            qWarning() << "search folder test: saved" << QJsonDocument::fromJson(json.toUtf8()).object().value(QLatin1String("ok")).toBool();
                            loadSearchFolders();
                        });
        return;
    }
    // The others act on the folder by name once the list is in
    QTimer::singleShot(2500, this, [this, key, action, name, filter] {
        const QModelIndex found = m_folderModel->match(m_folderModel->index(0, 0), Qt::DisplayRole, name, 1, Qt::MatchExactly | Qt::MatchRecursive).value(0);
        const QString id = found.data(FolderModel::SearchFolderIdRole).toString();
        if (id.isEmpty()) {
            qWarning() << "search folder test: not found" << name;
            return;
        }
        if (action == QLatin1String("open")) {
            m_folders->setCurrentIndex(found);
        } else if (action == QLatin1String("delete")) {
            deleteSearchFolder(key, id, name, false);
        } else if (action == QLatin1String("change")) {
            m_daemon->query(key.section(QLatin1Char('/'), 0, 0), QStringLiteral("me"), QStringLiteral("searchFolderUpdate"),
                            QString::fromUtf8(
                                QJsonDocument(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("name"), name}, {QStringLiteral("filter"), filter}}).toJson(QJsonDocument::Compact)),
                            this, [this](const QString &json) {
                                qWarning() << "search folder test: changed" << QJsonDocument::fromJson(json.toUtf8()).object().value(QLatin1String("ok")).toBool();
                                loadSearchFolders();
                            });
        }
    });
}

void MainWindow::loadCategories()
{
    if (!m_daemon->isRunning()) {
        return;
    }
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        if (box.mail) {
            m_categoryList->load(box.key());
        }
    }
}

void MainWindow::loadAutoReplies()
{
    if (!m_daemon->isRunning()) {
        return;
    }
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        if (!box.own) {
            continue;
        }
        const QString key = box.key();
        m_daemon->query(box.account, box.mailbox, QStringLiteral("mailboxSettings"), QStringLiteral("{}"), this, [this, key](const QString &json) {
            const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
            // Unreadable (offline): the last known state stays
            if (answer.value(QLatin1String("ok")).toBool()) {
                m_autoReplies.insert(key, answer.value(QLatin1String("body")).toObject().value(QLatin1String("automaticRepliesSetting")).toObject());
                showAutoRepliesBar();
            }
        });
    }
}

void MainWindow::showAutoRepliesBar()
{
    QStringList active = m_autoRepliesForTests;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        if (box.own && !active.contains(box.key()) && AutomaticReplies::fromJson(m_autoReplies.value(box.key())).activeAt(now)) {
            active.append(box.key());
        }
    }
    if (active.isEmpty()) {
        m_autoRepliesBar->animatedHide();
        return;
    }
    QStringList names;
    for (const QString &key : std::as_const(active)) {
        const std::optional<MailboxInfo> box = m_mailboxes->mailbox(key);
        if (box) {
            names.append(box->address.isEmpty() ? box->label() : box->address);
        }
    }
    m_autoRepliesBar->setText(i18ncp("@info %2 one or more mailboxes", "Automatic replies are being sent for %2.", "Automatic replies are being sent for %2.",
                                     int(names.size()), QLocale().createSeparatedList(names)));
    m_autoRepliesBar->setProperty("mailboxKey", active.constFirst());
    if (!m_autoRepliesBar->isVisible()) {
        m_autoRepliesBar->animatedShow();
    }
}

void MainWindow::showAutoRepliesBarForTests()
{
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        if (box.own) {
            m_autoRepliesForTests = {box.key()};
            break;
        }
    }
    m_autoRepliesBar->show();
    showAutoRepliesBar();
}

void MainWindow::turnOffAutoReplies()
{
    for (auto it = m_autoReplies.cbegin(); it != m_autoReplies.cend(); ++it) {
        if (!AutomaticReplies::fromJson(it.value()).activeAt(QDateTime::currentMSecsSinceEpoch())) {
            continue;
        }
        const QString key = it.key();
        const QJsonObject args{{QStringLiteral("settings"),
                                QJsonObject{{QStringLiteral("automaticRepliesSetting"), QJsonObject{{QStringLiteral("status"), QStringLiteral("disabled")}}}}}};
        m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("mailboxSettingsUpdate"),
                        QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this, [this, key](const QString &json) {
                            const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                            if (!answer.value(QLatin1String("ok")).toBool()) {
                                m_status->setText(accessProblemText(answer.isEmpty() ? QStringLiteral("network") : answer.value(QLatin1String("problem")).toString(), key));
                            }
                            qWarning() << "auto replies test: turned off" << answer.value(QLatin1String("ok")).toBool();
                            loadAutoReplies();
                        });
    }
}

MailboxSettingsDialog *MainWindow::openMailboxSettings(const QString &mailboxKey)
{
    auto *dialog = new MailboxSettingsDialog(m_mailboxes, m_daemon, mailboxKey.isEmpty() ? m_mailboxKey : mailboxKey, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    // The calendar shades the primary mailbox's working hours
    connect(dialog, &MailboxSettingsDialog::saved, this, [this](const QString &key, const QJsonObject &settings) {
        if (m_calendar && key == m_mailboxes->primaryAccount() + QStringLiteral("/me")) {
            m_calendar->setWorkingHours(WorkingHours::fromJson(settings.value(QLatin1String("workingHours")).toObject()));
        }
    });
    dialog->show();
    return dialog;
}

RulesDialog *MainWindow::openRules(const QString &mailboxKey)
{
    auto *dialog = new RulesDialog(m_mailboxes, m_daemon, m_folderModel, m_categoryList, mailboxKey.isEmpty() ? m_mailboxKey : mailboxKey, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    return dialog;
}

void MainWindow::offerUndoSend(const QString &mailboxKey, const QString &uuid, const QString &subject, int seconds)
{
    m_undoMailboxKey = mailboxKey;
    m_undoUuid = uuid;
    m_undoBar->setText(subject.trimmed().isEmpty() ? i18nc("@info", "Sending the message...") : i18nc("@info %1 a subject", "Sending \"%1\"...", subject.trimmed()));
    m_undoBar->animatedShow();
    // A moment less than the wait: an Undo at the last second would come too late anyway
    m_undoTimer.start(std::max(1, seconds) * 1000 - 300);
}

void MainWindow::undoSendForTests(const QString &record, int afterSeconds)
{
    offerUndoSend(record.section(QLatin1Char('|'), 0, 0), record.section(QLatin1Char('|'), 1, 1), record.section(QLatin1Char('|'), 2), 30);
    QTimer::singleShot(afterSeconds * 1000, this, [this] {
        const QList<QAction *> actions = m_undoBar->actions();
        if (!actions.isEmpty()) {
            actions.constFirst()->trigger();
        }
    });
}

void MainWindow::flagSelected(const QString &when)
{
    const QString account = m_mailboxKey.section(QLatin1Char('/'), 0, 0);
    const QString mailbox = m_mailboxKey.section(QLatin1Char('/'), 1);
    const QList<qint64> ids = selectedMessages();
    if (ids.isEmpty()) {
        return;
    }
    if (when == QLatin1String("complete") || when == QLatin1String("clear") || when == QLatin1String("none")) {
        for (qint64 id : ids) {
            m_daemon->setFlag(account, mailbox, id, when == QLatin1String("complete") ? 2 : when == QLatin1String("clear") ? 0 : 1);
        }
        return;
    }
    // Outlook's quick dates: the work week ends on Friday
    const QDate today = QDate::currentDate();
    const QDate friday = today.addDays(5 - today.dayOfWeek());
    const QDate nextMonday = today.addDays(8 - today.dayOfWeek());
    QDate start = today;
    QDate due = today;
    if (when == QLatin1String("tomorrow")) {
        start = due = today.addDays(1);
    } else if (when == QLatin1String("thisweek")) {
        due = friday < today ? today : friday;
    } else if (when == QLatin1String("nextweek")) {
        start = nextMonday;
        due = nextMonday.addDays(4);
    } else if (when == QLatin1String("custom")) {
        QDialog dialog(this);
        dialog.setWindowTitle(i18nc("@title:window", "Custom Follow-Up"));
        auto *startEdit = new QDateEdit(today, &dialog);
        auto *dueEdit = new QDateEdit(today, &dialog);
        for (QDateEdit *edit : {startEdit, dueEdit}) {
            edit->setCalendarPopup(true);
        }
        auto *form = new QFormLayout(&dialog);
        form->addRow(i18nc("@label", "Start date:"), startEdit);
        form->addRow(i18nc("@label", "Due date:"), dueEdit);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        form->addRow(buttons);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        start = std::min(startEdit->date(), dueEdit->date());
        due = std::max(startEdit->date(), dueEdit->date());
    }
    for (qint64 id : ids) {
        m_daemon->setFlagDates(account, mailbox, id, QDateTime(start, QTime(12, 0)).toMSecsSinceEpoch(), QDateTime(due, QTime(12, 0)).toMSecsSinceEpoch());
    }
}

void MainWindow::openTemplates()
{
    QDialog dialog(this);
    dialog.setWindowTitle(i18nc("@title:window", "New from Template"));
    auto *list = new QListWidget(&dialog);
    auto fill = [list] {
        list->clear();
        const QList<QJsonObject> all = Templates::all();
        for (const QJsonObject &t : all) {
            auto *item = new QListWidgetItem(QIcon::fromTheme(QStringLiteral("text-html")), t.value(QLatin1String("name")).toString(), list);
            item->setData(Qt::UserRole, t);
            item->setToolTip(t.value(QLatin1String("subject")).toString());
        }
    };
    fill();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Close, &dialog);
    auto *remove = buttons->addButton(i18nc("@action:button", "Delete"), QDialogButtonBox::ActionRole);
    remove->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete")));
    connect(remove, &QPushButton::clicked, &dialog, [list, fill, &dialog] {
        if (QListWidgetItem *item = list->currentItem()) {
            const QJsonObject t = item->data(Qt::UserRole).toJsonObject();
            if (KMessageBox::warningContinueCancel(&dialog, i18nc("@info %1 a template", "Delete the template %1?", t.value(QLatin1String("name")).toString()),
                                                   i18nc("@title:window", "Delete Template"), KStandardGuiItem::del())
                == KMessageBox::Continue) {
                Templates::remove(t.value(QLatin1String("id")).toString());
                fill();
            }
        }
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
    auto *note = new QLabel(i18nc("@info", "Save a message as a template from its Message menu."), &dialog);
    note->setWordWrap(true);
    note->setVisible(list->count() == 0);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(list);
    layout->addWidget(note);
    layout->addWidget(buttons);
    dialog.resize(420, 360);
    if (dialog.exec() != QDialog::Accepted || !list->currentItem()) {
        return;
    }
    const QJsonObject t = list->currentItem()->data(Qt::UserRole).toJsonObject();
    if (auto *composer = qobject_cast<ComposerWindow *>(openComposer(QStringLiteral("new"), 0))) {
        composer->applyTemplate(t);
    }
}

AutoRepliesDialog *MainWindow::openAutoReplies(const QString &mailboxKey)
{
    QString key = mailboxKey;
    if (key.isEmpty()) {
        key = m_autoRepliesBar->isVisible() ? m_autoRepliesBar->property("mailboxKey").toString() : m_mailboxKey;
    }
    auto *dialog = new AutoRepliesDialog(m_mailboxes, m_daemon, key, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &AutoRepliesDialog::saved, this, [this](const QString &key, const QJsonObject &setting) {
        m_autoReplies.insert(key, setting);
        showAutoRepliesBar();
    });
    dialog->show();
    return dialog;
}

void MainWindow::fillCategorizeMenu(QMenu *menu)
{
    menu->clear();
    const QList<qint64> ids = selectedMessages();
    const Store *store = m_mailboxes->store(m_mailboxKey);
    // Ticked: every selected message has it
    QHash<QString, int> counts;
    for (qint64 id : ids) {
        if (const std::optional<MessageRow> m = store ? store->message(id) : std::nullopt) {
            for (const QString &c : m->categories) {
                ++counts[c];
            }
        }
    }
    const QList<CategoryList::Category> categories = m_categoryList->categories(m_mailboxKey);
    for (const CategoryList::Category &c : categories) {
        QPixmap swatch(14, 14);
        swatch.fill(CategoryList::presetColor(c.preset));
        QAction *a = menu->addAction(QIcon(swatch), c.name);
        a->setCheckable(true);
        a->setChecked(!ids.isEmpty() && counts.value(c.name) == ids.size());
        a->setEnabled(!ids.isEmpty());
        const QString name = c.name;
        connect(a, &QAction::toggled, this, [this, name](bool on) {
            toggleCategory(name, on);
        });
    }
    if (categories.isEmpty()) {
        menu->addAction(i18nc("@item:inmenu", "(no categories)"))->setEnabled(false);
    }
    menu->addSeparator();
    QAction *clear = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-clear")), i18nc("@action:inmenu", "Clear All Categories"), this, &MainWindow::clearCategories);
    clear->setEnabled(!counts.isEmpty());
    menu->addAction(QIcon::fromTheme(QStringLiteral("configure")), i18nc("@action:inmenu", "All Categories..."), this, [this] {
        auto *dialog = new CategoriesDialog(m_categoryList, m_daemon, m_mailboxKey, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });
}

void MainWindow::toggleCategory(const QString &name, bool on)
{
    const Store *store = m_mailboxes->store(m_mailboxKey);
    const QList<qint64> ids = selectedMessages();
    for (qint64 id : ids) {
        const std::optional<MessageRow> m = store ? store->message(id) : std::nullopt;
        if (!m) {
            continue;
        }
        QStringList categories = m->categories;
        categories.removeAll(name);
        if (on) {
            categories.append(name);
        }
        if (categories != m->categories) {
            m_daemon->setCategories(account(), mailbox(), id, categories);
        }
    }
}

void MainWindow::clearCategories()
{
    const Store *store = m_mailboxes->store(m_mailboxKey);
    const QList<qint64> ids = selectedMessages();
    for (qint64 id : ids) {
        if (const std::optional<MessageRow> m = store ? store->message(id) : std::nullopt; m && !m->categories.isEmpty()) {
            m_daemon->setCategories(account(), mailbox(), id, {});
        }
    }
}

void MainWindow::showCategoryLabels()
{
    const Store *store = m_mailboxes->store(m_mailboxKey);
    const std::optional<MessageRow> m = store && m_reading->message() != 0 ? store->message(m_reading->message()) : std::nullopt;
    QList<QPair<QString, QColor>> labels;
    if (m) {
        for (const QString &c : m->categories) {
            labels.append({c, m_categoryList->color(m_mailboxKey, c)});
        }
    }
    m_reading->setCategoryLabels(labels);
}

void MainWindow::messageMenu(const QPoint &pos)
{
    if (!m_messages->indexAt(pos).isValid()) {
        return;
    }
    // In the order of Outlook on the web's menu
    QMenu menu(this);
    const auto add = [this](QMenu *to, const char *name) {
        if (QAction *a = actionCollection()->action(QLatin1String(name)); a && a->isVisible()) {
            to->addAction(a);
        }
    };
    for (const char *name : {"reply", "reply_all", "forward"}) {
        add(&menu, name);
    }
    menu.addSeparator();
    add(&menu, "delete");
    add(&menu, "archive");
    QMenu *move = menu.addMenu(QIcon::fromTheme(QStringLiteral("go-jump")), i18nc("@title:menu", "Move"));
    fillFolderMenu(move, false);
    QMenu *copy = menu.addMenu(QIcon::fromTheme(QStringLiteral("edit-copy")), i18nc("@title:menu", "Copy"));
    fillFolderMenu(copy, true);
    menu.addSeparator();
    add(&menu, "toggle_read");
    QMenu *categorize = menu.addMenu(QIcon::fromTheme(QStringLiteral("tag")), i18nc("@title:menu", "Categorize"));
    fillCategorizeMenu(categorize);
    add(&menu, "flag");
    QMenu *rules = menu.addMenu(QIcon::fromTheme(QStringLiteral("view-filter")), i18nc("@title:menu", "Rules"));
    add(rules, "create_rule");
    add(rules, "rules");
    menu.addSeparator();
    add(&menu, "report_junk");
    add(&menu, "report_phishing");
    add(&menu, "ignore_conversation");
    add(&menu, "sweep");
    menu.addSeparator();
    QMenu *related = menu.addMenu(QIcon::fromTheme(QStringLiteral("edit-find")), i18nc("@title:menu", "Find Related"));
    related->addAction(i18nc("@action:inmenu", "Messages from This Sender"), this, [this] {
        findRelated(false);
    });
    related->addAction(i18nc("@action:inmenu", "Messages in This Conversation"), this, [this] {
        findRelated(true);
    });
    QMenu *view = menu.addMenu(QIcon::fromTheme(QStringLiteral("view-preview")), i18nc("@title:menu", "View"));
    view->addAction(QIcon::fromTheme(QStringLiteral("window-new")), i18nc("@action:inmenu", "Open in a New Window"), this, [this] {
        openMessageWindow(m_messages->currentIndex());
    });
    add(view, "message_details");
    QMenu *more = menu.addMenu(i18nc("@title:menu", "More Actions"));
    for (const char *name : {"forward_as_attachment", "edit_as_new", "reply_with_meeting", "focus_move", "focus_always", "file_save_as", "save_pdf"}) {
        add(more, name);
    }
    menu.exec(m_messages->viewport()->mapToGlobal(pos));
}

void MainWindow::fillFolderMenu(QMenu *menu, bool copy)
{
    // The folders used last in this mailbox, then any other
    const Store *store = m_mailboxes->store(m_mailboxKey);
    const QList<qint64> recent = m_recentFolders.value(m_mailboxKey);
    for (qint64 folder : recent) {
        const std::optional<FolderRow> row = store ? store->folder(folder) : std::nullopt;
        if (!row || folder == m_folder) {
            continue;
        }
        menu->addAction(QIcon::fromTheme(QStringLiteral("folder")), row->name, this, [this, folder, copy] {
            copy ? copySelected(folder) : moveSelected(folder);
        });
    }
    if (!menu->isEmpty()) {
        menu->addSeparator();
    }
    menu->addAction(i18nc("@action:inmenu", "Other Folder..."), this, [this, copy] {
        if (copy) {
            actionCollection()->action(QStringLiteral("copy_to"))->trigger();
        } else {
            moveToFolder();
        }
    });
}

void MainWindow::rememberFolder(const QString &mailboxKey, qint64 folder)
{
    QList<qint64> &recent = m_recentFolders[mailboxKey];
    recent.removeAll(folder);
    recent.prepend(folder);
    while (recent.size() > 5) {
        recent.removeLast();
    }
}

void MainWindow::copySelected(qint64 folder)
{
    rememberFolder(m_mailboxKey, folder);
    const QString account = m_mailboxKey.section(QLatin1Char('/'), 0, 0);
    const QString mailbox = m_mailboxKey.section(QLatin1Char('/'), 1);
    const QList<qint64> ids = selectedMessages();
    auto failed = std::make_shared<int>(0);
    auto left = std::make_shared<int>(int(ids.size()));
    for (qint64 id : ids) {
        const QJsonObject args{{QStringLiteral("message"), id}, {QStringLiteral("folder"), folder}};
        m_daemon->query(account, mailbox, QStringLiteral("copy"), QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this,
                        [this, failed, left, total = int(ids.size())](const QString &json) {
                            if (!QJsonDocument::fromJson(json.toUtf8()).object().value(QLatin1String("ok")).toBool()) {
                                ++*failed;
                            }
                            if (--*left == 0) {
                                m_status->setText(*failed == 0 ? i18ncp("@info:status", "%1 message copied.", "%1 messages copied.", total)
                                                               : i18ncp("@info:status", "%1 message could not be copied.", "%1 messages could not be copied.", *failed));
                            }
                        });
    }
}

void MainWindow::reportPhishing()
{
    const QList<qint64> ids = selectedMessages();
    if (ids.isEmpty()
        || KMessageBox::warningContinueCancel(this,
                                              i18ncp("@info",
                                                     "Report this message to Microsoft as phishing? It moves to Deleted Items.",
                                                     "Report these %1 messages to Microsoft as phishing? They move to Deleted Items.",
                                                     int(ids.size())),
                                              i18nc("@title:window", "Report Phishing"),
                                              KGuiItem(i18nc("@action:button", "Report"), QStringLiteral("security-low")))
            != KMessageBox::Continue) {
        return;
    }
    const QString account = m_mailboxKey.section(QLatin1Char('/'), 0, 0);
    const QString mailbox = m_mailboxKey.section(QLatin1Char('/'), 1);
    for (qint64 id : ids) {
        m_daemon->query(account, mailbox, QStringLiteral("phish"), QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("message"), id}}).toJson(QJsonDocument::Compact)), this,
                        [this](const QString &json) {
                            const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                            m_status->setText(answer.value(QLatin1String("ok")).toBool()
                                                  ? i18nc("@info:status", "Reported as phishing to Microsoft; the message moved to Deleted Items.")
                                                  : accessProblemText(answer.isEmpty() ? QStringLiteral("network") : answer.value(QLatin1String("problem")).toString(), m_mailboxKey));
                        });
    }
}

void MainWindow::findRelated(bool conversation)
{
    const Store *store = m_mailboxes->store(m_mailboxKey);
    const QList<qint64> ids = selectedMessages();
    const std::optional<MessageRow> row = store && !ids.isEmpty() ? store->message(ids.constFirst()) : std::nullopt;
    if (!row) {
        return;
    }
    if (!conversation) {
        // The search field, as typed: from the sender, in the whole mailbox
        m_searchField->setText(QStringLiteral("from:") + row->senderAddress);
        search();
        return;
    }
    // The conversation in every folder of the mailbox, newest first
    MailboxSet *mailboxes = m_mailboxes;
    const QString key = m_mailboxKey;
    const QString conversationId = row->conversation;
    m_messageModel->setResults([mailboxes, key, conversationId]() {
        QList<SearchHit> hits;
        const Store *s = mailboxes->store(key);
        if (!s || conversationId.isEmpty()) {
            return hits;
        }
        const QList<ListRow> rows = s->search(SearchQuery(), {}, 1000, s->conversation(conversationId));
        for (const ListRow &r : rows) {
            const std::optional<FolderRow> folder = s->folder(r.folder);
            hits.append({key, r, folder ? folder->name : QString()});
        }
        std::stable_sort(hits.begin(), hits.end(), [](const SearchHit &a, const SearchHit &b) {
            return a.row.received > b.row.received;
        });
        return hits;
    });
    applyListModel();
    m_messages->scrollToTop();
    showResults(m_messageModel->rowCount(), false, i18nc("@info", "the conversation, in every folder"));
}

void MainWindow::createRuleFromMessage()
{
    const Store *store = m_mailboxes->store(m_mailboxKey);
    const QList<qint64> ids = selectedMessages();
    const std::optional<MessageRow> row = store && !ids.isEmpty() ? store->message(ids.constFirst()) : std::nullopt;
    if (!row) {
        return;
    }
    // Outlook's Create rule: from this sender, the rest chosen in the editor
    const QJsonObject from{{QStringLiteral("emailAddress"), QJsonObject{{QStringLiteral("name"), row->senderName}, {QStringLiteral("address"), row->senderAddress}}}};
    const QJsonObject draft{{QStringLiteral("displayName"), row->senderName.isEmpty() ? row->senderAddress : row->senderName},
                            {QStringLiteral("isEnabled"), true},
                            {QStringLiteral("conditions"), QJsonObject{{QStringLiteral("fromAddresses"), QJsonArray{from}}}}};
    openRules(m_mailboxKey)->createFrom(draft);
}

void MainWindow::categoryForTests(const QString &action, const QString &name, const QString &preset)
{
    if (!name.contains(QLatin1String("[TEST Graph Mail]"))) {
        qWarning() << "categories test: not a test name";
        return;
    }
    if (action == QLatin1String("set") || action == QLatin1String("unset")) {
        toggleCategory(name, action == QLatin1String("set"));
        qWarning() << "categories test:" << action << selectedMessages().size() << "messages";
        return;
    }
    auto *dialog = new CategoriesDialog(m_categoryList, m_daemon, m_mailboxes->primaryAccount() + QStringLiteral("/me"), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    if (action == QLatin1String("create")) {
        dialog->createForTests(name, preset);
    } else if (action == QLatin1String("recolor")) {
        QTimer::singleShot(2000, dialog, [dialog, name, preset] {
            dialog->recolorForTests(name, preset);
        });
    } else if (action == QLatin1String("delete")) {
        QTimer::singleShot(2000, dialog, [dialog, name] {
            dialog->deleteForTests(name);
        });
    }
}

void MainWindow::searchForTests(const QString &text, const QString &scope)
{
    if (!scope.isEmpty()) {
        m_searchScope->setCurrentIndex(std::max(0, m_searchScope->findData(scope)));
    }
    m_searchField->setText(text);
    QElapsedTimer timer;
    timer.start();
    search();
    qWarning() << "search test: local search took" << timer.elapsed() << "ms," << m_messageModel->rowCount() << "rows";
}

void MainWindow::showResults(int count, bool busy, const QString &note)
{
    // The button says how many filters are on, so a filtered list is never a surprise
    const int on = int(filtersText().isEmpty() ? 0 : filtersText().count(QStringLiteral(", ")) + 1);
    m_filtersButton->setText(on > 0 ? i18nc("@action:button %1 filters on", "Filters (%1)", on) : i18nc("@action:button", "Filters"));
    if (count < 0) {
        m_resultsBar->hide();
        return;
    }
    const QString text = m_searchField->text().trimmed();
    const QString where = m_searchScope->currentText();
    // What is searched: the words typed, the filters ticked, or both
    QString what = text.isEmpty() ? QString() : QLatin1Char('"') + text + QLatin1Char('"');
    if (filtersActive()) {
        what = what.isEmpty() ? filtersText() : i18nc("@info %1 the words, %2 the filters", "%1 with %2", what, filtersText());
    }
    QString line = count == 0 && !busy ? i18nc("@info %1 the search, %2 the scope", "No results for %1 in %2.", what, where)
                                       : i18ncp("@info %2 the search, %3 the scope", "%1 result for %2 in %3", "%1 results for %2 in %3", count, what, where);
    if (busy) {
        line += QLatin1Char(' ') + i18nc("@info", "(searching Microsoft 365 too...)");
    }
    if (!note.isEmpty()) {
        line += QLatin1Char(' ') + note;
    }
    m_resultsText->setText(line);
    m_resultsBusy->setVisible(busy);
    m_resultsBusy->setRunning(busy);
    m_resultsBar->show();
    m_focusTabs->hide();
}

SearchQuery MainWindow::searchQuery() const
{
    SearchQuery query = SearchQuery::parse(m_searchField->text());
    const bool typedFields = !query.fields.isEmpty();
    if (m_filterAttachments && m_filterAttachments->isChecked()) {
        query.attachments = true;
    }
    if (m_filterUnread && m_filterUnread->isChecked()) {
        query.unread = true;
    }
    if (m_filterRead && m_filterRead->isChecked()) {
        query.unread = false;
    }
    if (m_filterFlagged && m_filterFlagged->isChecked()) {
        query.flagged = true;
    }
    if (m_filterImportant && m_filterImportant->isChecked()) {
        query.highImportance = true;
    }
    // Sent to me / CC to me: the address of the mailbox searched
    const std::optional<MailboxInfo> box = m_mailboxes->mailbox(m_folderMailboxKey.isEmpty() ? m_mailboxKey : m_folderMailboxKey);
    if (box && !box->address.isEmpty()) {
        if (m_filterToMe && m_filterToMe->isChecked()) {
            query.fields[QStringLiteral("to")].append(box->address);
        }
        if (m_filterCcMe && m_filterCcMe->isChecked()) {
            query.fields[QStringLiteral("cc")].append(box->address);
        }
    }
    const int period = m_filterReceived && m_filterReceived->checkedAction() ? m_filterReceived->checkedAction()->data().toInt() : 0;
    const QDate today = QDate::currentDate();
    QDate from;
    QDate to;
    switch (period) {
    case 1:
        from = today;
        break;
    case 2:
        from = today.addDays(1 - today.dayOfWeek());
        break;
    case 3:
        from = today.addDays(-30);
        break;
    case 4:
        from = QDate(today.year(), 1, 1);
        break;
    case 5:
        from = m_rangeFrom;
        to = m_rangeTo;
        break;
    default:
        break;
    }
    if (from.isValid()) {
        query.receivedFrom = std::max(query.receivedFrom, QDateTime(from, QTime(0, 0)).toMSecsSinceEpoch());
    }
    if (to.isValid()) {
        const qint64 end = QDateTime(to.addDays(1), QTime(0, 0)).toMSecsSinceEpoch();
        query.receivedTo = query.receivedTo > 0 ? std::min(query.receivedTo, end) : end;
    }
    for (auto it = m_textFilters.cbegin(); it != m_textFilters.cend(); ++it) {
        if (it.key() == QLatin1String("attachment")) {
            query.attachmentNames.append(it.value());
        } else {
            query.fields[it.key()].append(it.value());
        }
    }
    query.categories += m_categoryFilters;
    // A filter's value is complete, not being typed: no prefix match for it
    if (query.words.isEmpty() && !typedFields && !m_textFilters.isEmpty()) {
        query.lastIsPrefix = false;
    }
    return query;
}

bool MainWindow::filtersActive() const
{
    return !filtersText().isEmpty();
}

QString MainWindow::filtersText() const
{
    QStringList on;
    for (QAction *a : {m_filterAttachments, m_filterUnread, m_filterRead, m_filterFlagged, m_filterImportant, m_filterToMe, m_filterCcMe}) {
        if (a && a->isChecked()) {
            on.append(a->text());
        }
    }
    if (m_filterReceived && m_filterReceived->checkedAction()) {
        const int period = m_filterReceived->checkedAction()->data().toInt();
        if (period == 5 && m_rangeFrom.isValid()) {
            // In the C locale Qt's short date is "1 10 2026": the day-month-year Outlook shows then
            const QLocale locale;
            auto date = [&locale](QDate d) {
                return locale.language() == QLocale::C ? d.toString(QStringLiteral("dd-MM-yyyy")) : locale.toString(d, QLocale::ShortFormat);
            };
            on.append(i18nc("@item search filter, %1 and %2 dates", "Received: %1 to %2", date(m_rangeFrom), date(m_rangeTo)));
        } else if (period != 0 && period != 5) {
            on.append(i18nc("@item search filter, %1 a period", "Received: %1", m_filterReceived->checkedAction()->text()));
        }
    }
    static const QHash<QString, KLocalizedString> labels{{QStringLiteral("from"), ki18nc("@item search filter", "From: %1")},
                                                        {QStringLiteral("to"), ki18nc("@item search filter", "To: %1")},
                                                        {QStringLiteral("cc"), ki18nc("@item search filter", "Cc: %1")},
                                                        {QStringLiteral("subject"), ki18nc("@item search filter", "Subject: %1")},
                                                        {QStringLiteral("body"), ki18nc("@item search filter", "Text: %1")},
                                                        {QStringLiteral("attachment"), ki18nc("@item search filter", "Attachment: %1")}};
    for (const QString &field : {QStringLiteral("from"), QStringLiteral("to"), QStringLiteral("cc"), QStringLiteral("subject"), QStringLiteral("body"), QStringLiteral("attachment")}) {
        if (m_textFilters.contains(field)) {
            on.append(labels.value(field).subs(m_textFilters.value(field)).toString());
        }
    }
    for (const QString &category : m_categoryFilters) {
        on.append(i18nc("@item search filter", "Category: %1", category));
    }
    return on.join(QStringLiteral(", "));
}

void MainWindow::clearFilters()
{
    for (QAction *a : {m_filterAttachments, m_filterUnread, m_filterRead, m_filterFlagged, m_filterImportant, m_filterToMe, m_filterCcMe}) {
        if (a) {
            const QSignalBlocker blocker(a);
            a->setChecked(false);
        }
    }
    for (QAction *a : std::as_const(m_textFilterActions)) {
        a->setChecked(false);
    }
    if (m_filterReceived && !m_filterReceived->actions().isEmpty()) {
        m_filterReceived->actions().constFirst()->setChecked(true);
    }
    m_textFilters.clear();
    m_categoryFilters.clear();
    m_rangeFrom = {};
    m_rangeTo = {};
}

void MainWindow::filterForTests(const QStringList &filters, const QString &scope)
{
    if (!scope.isEmpty()) {
        m_searchScope->setCurrentIndex(std::max(0, m_searchScope->findData(scope)));
    }
    const QHash<QString, QAction *> toggles{{QStringLiteral("attachments"), m_filterAttachments}, {QStringLiteral("unread"), m_filterUnread},
                                            {QStringLiteral("read"), m_filterRead},             {QStringLiteral("flagged"), m_filterFlagged},
                                            {QStringLiteral("important"), m_filterImportant},   {QStringLiteral("tome"), m_filterToMe},
                                            {QStringLiteral("ccme"), m_filterCcMe}};
    for (const QString &f : filters) {
        const QString key = f.section(QLatin1Char(':'), 0, 0);
        const QString value = f.section(QLatin1Char(':'), 1);
        if (QAction *toggle = toggles.value(f)) {
            const QSignalBlocker blocker(toggle);
            toggle->setChecked(true);
        } else if (key == QLatin1String("received")) {
            m_filterReceived->actions().value(value.toInt())->setChecked(true);
        } else if (key == QLatin1String("between")) {
            m_rangeFrom = QDate::fromString(value.section(QLatin1Char(':'), 0, 0), Qt::ISODate);
            m_rangeTo = QDate::fromString(value.section(QLatin1Char(':'), 1, 1), Qt::ISODate);
            m_filterReceived->actions().value(5)->setChecked(true);
        } else if (key == QLatin1String("category")) {
            m_categoryFilters.append(value);
        } else if (m_textFilterActions.contains(key)) {
            m_textFilters.insert(key, value);
            m_textFilterActions.value(key)->setChecked(true);
        }
    }
    QElapsedTimer timer;
    timer.start();
    search();
    qWarning() << "search test: filters" << filtersText() << "took" << timer.elapsed() << "ms," << m_messageModel->rowCount() << "rows";
}

void MainWindow::searchOnline()
{
    const SearchQuery query = searchQuery();
    if (query.kql().isEmpty()) {
        return;
    }
    const quint64 run = m_searchRun;
    const QList<QPair<QString, QList<qint64>>> targets = searchTargets();
    const auto online = m_onlineHits;
    auto pending = std::make_shared<int>(int(targets.size()));
    showResults(m_messageModel->rowCount(), true);
    const int local = m_messageModel->rowCount();
    for (const auto &[key, folders] : targets) {
        QJsonObject args{{QStringLiteral("kql"), query.kql()}, {QStringLiteral("max"), 1000}};
        if (folders.size() == 1) {
            args.insert(QStringLiteral("folder"), folders.first());
        }
        m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("search"), QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this,
                        [this, run, key, online, pending, local](const QString &json) {
                            if (run != m_searchRun) {
                                return; // the user typed on
                            }
                            const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                            if (const Store *store = m_mailboxes->store(key); store && answer.value(QLatin1String("ok")).toBool()) {
                                QStringList graphIds;
                                const QJsonArray ids = answer.value(QLatin1String("ids")).toArray();
                                for (const QJsonValue &v : ids) {
                                    graphIds.append(v.toString());
                                }
                                online->insert(key, store->messageIdsByGraphIds(graphIds).values());
                            }
                            if (--*pending > 0) {
                                return;
                            }
                            m_messageModel->refresh();
                            const int total = m_messageModel->rowCount();
                            qWarning() << "search test:" << local << "local," << total << "with Microsoft 365";
                            showResults(total, false,
                                        json.isEmpty() || !answer.value(QLatin1String("ok")).toBool()
                                            ? i18nc("@info", "(only this computer's copy: Microsoft 365 could not be searched)")
                                            : QString());
                        });
    }
}

void MainWindow::moveSelected(qint64 folder)
{
    rememberFolder(m_mailboxKey, folder);
    const QList<qint64> ids = selectedMessages();
    rememberForUndo(m_mailboxKey, ids, false);
    for (qint64 id : ids) {
        m_daemon->move(account(), mailbox(), id, folder);
    }
}

void MainWindow::moveToFolder()
{
    MoveDialog dialog(m_folderModel, m_mailboxKey, this);
    if (dialog.exec() == QDialog::Accepted && dialog.folder() != 0) {
        moveSelected(dialog.folder());
    }
}

qint64 MainWindow::archiveFolder(const QString &mailboxKey) const
{
    const QString chosen = KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("ArchiveFolder")).readEntry(mailboxKey, QString());
    const Store *store = m_mailboxes->store(mailboxKey);
    if (!chosen.isEmpty() && store) {
        if (const std::optional<FolderRow> row = store->folderByGraphId(chosen)) {
            return row->id;
        }
    }
    const QModelIndex archive = m_folderModel->indexForWellKnown(mailboxKey, QStringLiteral("archive"));
    return archive.isValid() ? archive.data(FolderModel::FolderIdRole).toLongLong() : 0;
}

void MainWindow::chooseArchiveFolder(const QString &mailboxKey, qint64 givenFolder, const std::function<void(bool)> &done)
{
    qint64 folder = givenFolder;
    if (folder == 0) {
        MoveDialog dialog(m_folderModel, mailboxKey, this);
        dialog.setWindowTitle(i18nc("@title:window", "Archive Folder"));
        if (dialog.exec() != QDialog::Accepted || dialog.folder() == 0) {
            return;
        }
        folder = dialog.folder();
    }
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<FolderRow> row = store ? store->folder(folder) : std::nullopt;
    if (!row) {
        if (done) {
            done(false);
        }
        return;
    }
    const QString graphId = row->graphId;
    const QString name = row->name;
    const QJsonObject args{{QStringLiteral("folder"), graphId}};
    m_daemon->query(mailboxKey.section(QLatin1Char('/'), 0, 0), mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("archiveFolderSet"),
                    QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this, [this, mailboxKey, graphId, name, done](const QString &json) {
                        const bool taken = QJsonDocument::fromJson(json.toUtf8()).object().value(QLatin1String("ok")).toBool();
                        KConfigGroup group = KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("ArchiveFolder"));
                        if (taken) {
                            // The mailbox knows it now, on every device: no local choice needed
                            group.deleteEntry(mailboxKey);
                            m_status->setText(i18nc("@info:status %1 a folder", "Archive moves messages to %1.", name));
                        } else {
                            group.writeEntry(mailboxKey, graphId);
                            m_status->setText(i18nc("@info:status %1 a folder", "Archive moves messages to %1 (on this computer: Microsoft 365 did not take the setting).", name));
                        }
                        group.sync();
                        if (done) {
                            done(taken);
                        }
                    });
}

void MainWindow::archiveSelected()
{
    const qint64 folder = archiveFolder(m_mailboxKey);
    if (folder == 0) {
        // Outlook asks for one the first time; here: make "Archive" at the top of the mailbox
        if (KMessageBox::questionTwoActions(this,
                                            i18nc("@info", "This mailbox has no archive folder yet. Create a folder named Archive for it?"),
                                            i18nc("@title:window", "Archive"),
                                            KGuiItem(i18nc("@action:button", "Create Archive Folder"), QStringLiteral("folder-new")),
                                            KStandardGuiItem::cancel())
            != KMessageBox::PrimaryAction) {
            return;
        }
        const QString key = m_mailboxKey;
        const QJsonObject args{{QStringLiteral("parent"), QStringLiteral("msgfolderroot")}, {QStringLiteral("name"), i18nc("@item folder name", "Archive")}};
        m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("folderCreate"),
                        QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this, [this, key](const QString &json) {
                            const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                            if (!answer.value(QLatin1String("ok")).toBool()) {
                                m_status->setText(i18nc("@info:status %1 the reason", "The Archive folder could not be made: %1", answer.value(QLatin1String("detail")).toString()));
                                return;
                            }
                            // Kept on this computer at once; the mailbox setting is tried as well
                            KConfigGroup group = KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("ArchiveFolder"));
                            group.writeEntry(key, answer.value(QLatin1String("id")).toString());
                            group.sync();
                            const QJsonObject set{{QStringLiteral("folder"), answer.value(QLatin1String("id")).toString()}};
                            m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("archiveFolderSet"),
                                            QString::fromUtf8(QJsonDocument(set).toJson(QJsonDocument::Compact)), this, [](const QString &) {});
                            m_status->setText(i18nc("@info:status", "The Archive folder was made; Archive works as soon as it appears in the folder list."));
                        });
        return;
    }
    if (folder != m_folder) {
        moveSelected(folder);
    }
}

void MainWindow::selectRow(int row)
{
    const QModelIndex index = m_messages->model()->index(row, 0);
    if (index.isValid()) {
        m_messages->setCurrentIndex(index);
    }
}

ReadingPane *MainWindow::readingPane() const
{
    return m_reading;
}

void MainWindow::loadImages()
{
    m_reading->loadImages();
}

bool MainWindow::printBodyToPdf(const QString &file)
{
    return m_reading->printBodyToPdf(file);
}

void MainWindow::setMarkReadEnabled(bool enabled)
{
    m_markReadEnabled = enabled;
}

void MainWindow::selectFolder(const QString &path)
{
    QModelIndex index = m_folderModel->indexForPath(path);
    if (index.isValid() && index.data(FolderModel::IsMailboxRole).toBool()) {
        // A mailbox stands for its Inbox
        index = m_folderModel->indexForWellKnown(path, QStringLiteral("inbox"));
    }
    if (!index.isValid()) {
        return;
    }
    m_folders->expand(index.parent());
    m_folders->setCurrentIndex(index);
    m_folders->scrollTo(index);
}

void MainWindow::showMessage(const QString &mailboxKey, qint64 message)
{
    Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<MessageRow> m = store ? store->message(message) : std::nullopt;
    if (!m) {
        return;
    }
    selectFolder(mailboxKey + QLatin1Char('/') + QString::number(m->folder));
    if (m_messages->model() != m_messageModel) {
        m_reading->showMessage(store, m_mailboxes->blobs(mailboxKey), message);
        return;
    }
    // New mail is near the top; look a few screens deep before giving up on the list position
    for (int round = 0; round < 10 && m_messageModel->rowOfId(message) < 0 && m_messageModel->canFetchMore({}); ++round) {
        m_messageModel->fetchMore({});
    }
    const int row = m_messageModel->rowOfId(message);
    if (row >= 0) {
        m_messages->setCurrentIndex(m_messageModel->index(row));
        m_messages->scrollTo(m_messageModel->index(row));
    } else {
        m_reading->showMessage(store, m_mailboxes->blobs(mailboxKey), message);
    }
}

void MainWindow::folderChanged()
{
    const QModelIndex index = m_folders->currentIndex();
    const QString account = m_mailboxKey.section(QLatin1Char('/'), 0, 0);
    const QString mailbox = m_mailboxKey.section(QLatin1Char('/'), 1);
    if (m_folder != 0) {
        // The previous folder goes back to its normal poll class
        m_daemon->setFolderOpen(account, mailbox, m_folder, false);
    }
    m_reading->clear();
    // A search folder: Exchange's current contents; the group itself shows nothing
    if (const QString searchFolder = index.data(FolderModel::SearchFolderIdRole).toString(); !searchFolder.isEmpty()) {
        m_mailboxKey = index.data(FolderModel::MailboxKeyRole).toString();
        m_folderMailboxKey = m_mailboxKey;
        m_folder = 0;
        showSearchFolder(m_mailboxKey, searchFolder, index.data(Qt::DisplayRole).toString());
        updateActions();
        return;
    }
    if (index.data(FolderModel::IsAllInboxesRole).toBool()) {
        showAllInboxes();
        return;
    }
    if (!index.isValid() || index.data(FolderModel::IsMailboxRole).toBool() || index.data(FolderModel::IsSearchGroupRole).toBool()
        || index.data(FolderModel::IsFavoritesGroupRole).toBool()) {
        m_mailboxKey = index.data(FolderModel::MailboxKeyRole).toString();
        m_folder = 0;
        m_messageModel->clear();
        m_conversationModel->clear();
        setCaption(index.data(Qt::DisplayRole).toString());
        updateActions();
        return;
    }
    m_mailboxKey = index.data(FolderModel::MailboxKeyRole).toString();
    m_folderMailboxKey = m_mailboxKey;
    m_folder = index.data(FolderModel::FolderIdRole).toLongLong();
    // Remembered for the next start (screenshots and tests leave it alone)
    if (!m_testMode && !m_mailboxKey.isEmpty()) {
        KConfigGroup group = windowGroup();
        if (group.readEntry("LastMailbox") != m_mailboxKey) {
            group.writeEntry("LastMailbox", m_mailboxKey);
            group.sync();
        }
    }
    {
        const QSignalBlocker blocker(m_searchField);
        m_searchField->clear();
    }
    const QString wellKnown = index.data(FolderModel::WellKnownRole).toString();
    if (wellKnown != m_folderWellKnown || m_folderMailboxKey != m_mailboxKey) {
        loadIgnored();
    }
    m_folderWellKnown = wellKnown;
    m_showRecipients = wellKnown == QLatin1String("sentitems") || wellKnown == QLatin1String("drafts") || wellKnown == QLatin1String("outbox");
    updateFocusTabs();
    m_messageModel->setFocusFilter(focusFilter());
    m_conversationModel->setFocusFilter(focusFilter());
    if (conversationView()) {
        m_conversationModel->setFolder(m_mailboxKey, m_mailboxes->store(m_mailboxKey), m_folder, m_showRecipients);
    } else {
        m_messageModel->setFolder(m_mailboxKey, m_mailboxes->store(m_mailboxKey), m_folder, m_showRecipients);
    }
    applyListModel();
    m_messages->scrollToTop();
    // Open folders are checked every minute (the hot poll class)
    m_daemon->setFolderOpen(m_mailboxKey.section(QLatin1Char('/'), 0, 0), m_mailboxKey.section(QLatin1Char('/'), 1), m_folder, true);
    const std::optional<MailboxInfo> info = m_mailboxes->mailbox(m_mailboxKey);
    setCaption(info ? i18nc("@title:window folder - mailbox", "%1 - %2", index.data(Qt::DisplayRole).toString(), info->label())
                    : index.data(Qt::DisplayRole).toString());
    updateActions();
}

void MainWindow::showAllInboxes()
{
    // The Inboxes of every mailbox with mail in one list, newest first (Outlook's All Inboxes);
    // a row's actions work in its own mailbox, as with search results
    MailboxSet *mailboxes = m_mailboxes;
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    QStringList keys;
    for (const MailboxInfo &box : boxes) {
        if (box.mail) {
            keys.append(box.key());
        }
    }
    m_folder = 0;
    m_mailboxKey = keys.value(0);
    m_folderMailboxKey = m_mailboxKey;
    m_folderWellKnown = QStringLiteral("inbox");
    m_showRecipients = false;
    m_messageModel->setResults([mailboxes, keys]() {
        QList<SearchHit> hits;
        for (const QString &key : keys) {
            const Store *store = mailboxes->store(key);
            const std::optional<FolderRow> inbox = store ? store->folderByWellKnown(QStringLiteral("inbox")) : std::nullopt;
            if (!inbox) {
                continue;
            }
            const std::optional<MailboxInfo> box = mailboxes->mailbox(key);
            const QList<ListRow> rows = store->listPage(inbox->id, std::nullopt, 300);
            for (const ListRow &row : rows) {
                hits.append({key, row, box ? box->label() : inbox->name});
            }
        }
        std::stable_sort(hits.begin(), hits.end(), [](const SearchHit &a, const SearchHit &b) {
            return a.row.received != b.row.received ? a.row.received > b.row.received : a.row.id > b.row.id;
        });
        if (hits.size() > 1000) {
            hits.resize(1000);
        }
        return hits;
    });
    applyListModel();
    m_messages->scrollToTop();
    setCaption(i18nc("@item folder pane, the Inboxes of every mailbox", "All Inboxes"));
    updateActions();
}

void MainWindow::messageChanged()
{
    // One row: show it (a conversation row shows its newest message)
    const QModelIndexList rows = m_messages->selectionModel()->selectedRows();
    m_markReadTimer.stop();
    if (rows.size() != 1) {
        m_reading->clear();
        updateActions();
        return;
    }
    // A search result from another mailbox: the actions (read, move, reply) work there
    if (const QString key = rows.constFirst().data(MessageListModel::MailboxKeyRole).toString(); !key.isEmpty() && key != m_mailboxKey) {
        m_mailboxKey = key;
    }
    m_reading->showMessage(m_mailboxes->store(m_mailboxKey), m_mailboxes->blobs(m_mailboxKey), rows.constFirst().data(MessageListModel::IdRole).toLongLong());
    showCategoryLabels();
    const int seconds = markReadSeconds();
    if (m_markReadEnabled && seconds >= 0) {
        m_markReadTimer.start(seconds * 1000);
    }
    updateActions();
}

void MainWindow::collectionChanged(const QString &account, const QString &mailbox, const QString &collection)
{
    const QString key = account + QLatin1Char('/') + mailbox;
    if (collection == QLatin1String("tree")) {
        reloadFolders();
        return;
    }
    if (collection.startsWith(QLatin1String("folder:"))) {
        m_folderModel->updateCounts(key);
        m_badgeTimer.start();
        if (key == m_mailboxKey && collection.mid(7).toLongLong() == m_folder) {
            refreshList();
        }
        if (key == m_mailboxKey) {
            showCategoryLabels();
        }
    }
}

void MainWindow::updateUnreadBadge()
{
    // The unread messages in the Inboxes of the own mailboxes on the task manager's icon, as KMail
    // shows them (Unity's LauncherEntry, which Plasma's task manager follows)
    int unread = 0;
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        const Store *store = box.own ? m_mailboxes->store(box.key()) : nullptr;
        if (const std::optional<FolderRow> inbox = store ? store->folderByWellKnown(QStringLiteral("inbox")) : std::nullopt) {
            unread += inbox->unread;
        }
    }
    if (unread == m_badge) {
        return;
    }
    m_badge = unread;
    QDBusMessage update = QDBusMessage::createSignal(QStringLiteral("/io/github/nvaert1986/graphmail/UnityLauncher"), QStringLiteral("com.canonical.Unity.LauncherEntry"),
                                                     QStringLiteral("Update"));
    update << QStringLiteral("application://io.github.nvaert1986.graphmail.desktop")
           << QVariantMap{{QStringLiteral("count"), qint64(unread)}, {QStringLiteral("count-visible"), unread > 0}};
    QDBusConnection::sessionBus().send(update);
}

void MainWindow::reloadFolders()
{
    // Keep what was expanded and selected across the rebuild
    QStringList expanded;
    std::function<void(const QModelIndex &)> collect = [&](const QModelIndex &parent) {
        for (int row = 0; row < m_folderModel->rowCount(parent); ++row) {
            const QModelIndex index = m_folderModel->index(row, 0, parent);
            if (m_folders->isExpanded(index)) {
                expanded.append(index.data(FolderModel::PathKeyRole).toString());
            }
            collect(index);
        }
    };
    collect({});
    const QString current = m_folders->currentIndex().data(FolderModel::PathKeyRole).toString();
    // Favorites stay open unless the user closed them
    if (!m_favoritesClosed) {
        expanded.append(QStringLiteral("favorites"));
    }
    {
        // The same folder stays selected: no need to reload the list
        const QSignalBlocker blocker(m_folders->selectionModel());
        m_folderModel->reload();
        for (const QString &path : std::as_const(expanded)) {
            m_folders->expand(m_folderModel->indexForPath(path));
        }
        const QModelIndex again = m_folderModel->indexForPath(current);
        if (again.isValid()) {
            m_folders->setCurrentIndex(again);
        }
    }
    if (!m_folderModel->indexForPath(current).isValid() && !current.isEmpty()) {
        folderChanged();
    }
}

void MainWindow::updateStatus()
{
    if (!m_daemon->isRunning()) {
        m_status->setText(i18nc("@info:status", "The sync service is not running: showing the mail downloaded so far"));
        return;
    }
    const QJsonArray all = m_daemon->status();
    bool offline = false;
    bool initial = false;
    bool syncing = false;
    int pending = 0;
    int outgoing = 0;
    int notSent = 0;
    qint64 lastSync = 0;
    QStringList errors;
    for (const QJsonValue &value : all) {
        const QJsonObject m = value.toObject();
        const QString state = m.value(QLatin1String("state")).toString();
        offline |= state == QLatin1String("offline");
        initial |= state == QLatin1String("initialSync");
        syncing |= state == QLatin1String("syncing");
        pending += m.value(QLatin1String("pendingOperations")).toInt();
        outgoing += m.value(QLatin1String("outgoing")).toInt();
        notSent += m.value(QLatin1String("outgoingFailed")).toInt();
        lastSync = std::max(lastSync, qint64(m.value(QLatin1String("lastSync")).toInteger()));
        if (state == QLatin1String("noAccess")) {
            const std::optional<MailboxInfo> box =
                m_mailboxes->mailbox(m.value(QLatin1String("account")).toString() + QLatin1Char('/') + m.value(QLatin1String("mailbox")).toString());
            errors.append(accessProblemText(QStringLiteral("noAccess"), box ? box->label() : m.value(QLatin1String("mailbox")).toString()));
        }
    }
    QString text;
    if (offline) {
        text = i18nc("@info:status", "Offline: changes are sent when the connection is back");
    } else if (initial) {
        text = i18nc("@info:status", "Downloading mail...");
    } else if (syncing) {
        text = i18nc("@info:status", "Checking for new mail...");
    } else if (lastSync > 0) {
        text = i18nc("@info:status time of the last sync", "Up to date (%1)", QLocale().toString(QDateTime::fromMSecsSinceEpoch(lastSync).time(), QLocale::ShortFormat));
    } else {
        text = i18nc("@info:status", "Connected");
    }
    if (pending > 0) {
        text += QStringLiteral("  -  ") + i18ncp("@info:status", "%1 change waiting to be sent", "%1 changes waiting to be sent", pending);
    }
    if (outgoing > 0) {
        text += QStringLiteral("  -  ") + i18ncp("@info:status", "%1 message being sent", "%1 messages being sent", outgoing);
    }
    if (notSent > 0) {
        text += QStringLiteral("  -  ") + i18ncp("@info:status", "%1 message not sent (in Drafts)", "%1 messages not sent (in Drafts)", notSent);
    }
    // Access taken away from another mailbox: said in the bar, the details on hover
    if (!errors.isEmpty()) {
        text += QStringLiteral("  -  ") + i18ncp("@info:status", "No access to %1 mailbox", "No access to %1 mailboxes", int(errors.size()));
    }
    m_status->setText(text);
    m_status->setToolTip(errors.join(QLatin1Char('\n')));
    // The indicator: how Graph Mail is connected, and what waits (it opens the list)
    const int waiting = pending + outgoing + notSent;
    QString icon = QStringLiteral("network-connect");
    QString tip = i18nc("@info:tooltip", "Connected to Microsoft 365");
    if (offline) {
        icon = QStringLiteral("network-offline");
        tip = i18nc("@info:tooltip", "Offline: working with the copy on this computer");
    } else if (!errors.isEmpty() || notSent > 0) {
        icon = QStringLiteral("dialog-warning");
        tip = notSent > 0 ? i18nc("@info:tooltip", "Some messages were not sent") : errors.join(QLatin1Char('\n'));
    } else if (initial || syncing) {
        icon = QStringLiteral("view-refresh");
        tip = i18nc("@info:tooltip", "Synchronising with Microsoft 365");
    }
    m_connection->setIcon(QIcon::fromTheme(icon));
    m_connection->setText(waiting > 0 ? i18ncp("@action:button", "%1 waiting", "%1 waiting", waiting) : QString());
    m_connection->setToolTip(tip + QLatin1Char('\n') + i18nc("@info:tooltip", "Click for what is waiting to be sent"));
}

QList<qint64> MainWindow::selectedMessages() const
{
    QList<qint64> ids;
    const QModelIndexList rows = m_messages->selectionModel()->selectedRows();
    for (const QModelIndex &index : rows) {
        // Search results from other mailboxes than the current one stay out of an action
        if (const QString key = index.data(MessageListModel::MailboxKeyRole).toString(); !key.isEmpty() && key != m_mailboxKey) {
            continue;
        }
        // A conversation row stands for all its messages, as in Outlook
        const QList<qint64> messages = m_messages->model() == m_conversationModel ? m_conversationModel->messagesOf(index)
                                                                                  : QList<qint64>{index.data(MessageListModel::IdRole).toLongLong()};
        for (qint64 id : messages) {
            if (!ids.contains(id)) {
                ids.append(id);
            }
        }
    }
    return ids;
}

bool MainWindow::conversationView() const
{
    return m_conversations && m_searchField->text().trimmed().isEmpty();
}

void MainWindow::refreshList()
{
    // The list reloads (a message came, went or changed): the selected message stays selected;
    // when it left the folder (deleted, moved), the one that takes its place is selected, as in
    // Outlook, so Delete can be pressed again for the next one
    QAbstractItemModel *model = m_messages->model();
    QModelIndex current = m_messages->currentIndex();
    while (current.parent().isValid()) {
        current = current.parent();
    }
    const bool hadSelection = m_messages->selectionModel() && m_messages->selectionModel()->hasSelection() && current.isValid();
    const int row = current.row();
    const qint64 id = current.data(MessageListModel::IdRole).toLongLong();
    const int scroll = m_messages->verticalScrollBar()->value();
    if (model == m_conversationModel) {
        m_conversationModel->refresh();
    } else {
        m_messageModel->refresh();
    }
    const int rows = model->rowCount();
    if (!hadSelection || rows == 0 || (m_messages->selectionModel()->hasSelection() && m_messages->currentIndex().isValid())) {
        return;
    }
    // The same message if it is still there, else the row where it was
    int target = std::min(row, rows - 1);
    for (int r = 0; r < rows; ++r) {
        if (model->index(r, 0).data(MessageListModel::IdRole).toLongLong() == id) {
            target = r;
            break;
        }
    }
    m_messages->verticalScrollBar()->setValue(scroll);
    const QModelIndex next = model->index(target, 0);
    m_messages->selectionModel()->setCurrentIndex(next, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_messages->scrollTo(next);
}

void MainWindow::applyListModel()
{
    QAbstractItemModel *wanted = conversationView() ? static_cast<QAbstractItemModel *>(m_conversationModel) : m_messageModel;
    if (m_messages->model() == wanted) {
        return;
    }
    m_messages->setModel(wanted);
    m_messages->setRootIsDecorated(wanted == m_conversationModel);
    connect(m_messages->selectionModel(), &QItemSelectionModel::selectionChanged, this, &MainWindow::messageChanged);
}

void MainWindow::updateActions()
{
    const bool any = !selectedMessages().isEmpty();
    // The toolbar follows the folder, as Outlook's does: Drafts discard and do not answer, Deleted
    // Items and Junk Email can be emptied
    const bool drafts = m_folderWellKnown == QLatin1String("drafts") && m_folder != 0;
    const bool outgoing = drafts || (m_folderWellKnown == QLatin1String("outbox") && m_folder != 0);
    if (QAction *a = actionCollection()->action(QStringLiteral("delete"))) {
        a->setText(drafts ? i18nc("@action", "Discard Draft") : i18nc("@action", "Delete"));
    }
    for (const QString &name : {QStringLiteral("reply"), QStringLiteral("reply_all"), QStringLiteral("forward")}) {
        if (QAction *a = actionCollection()->action(name)) {
            a->setVisible(!outgoing);
        }
    }
    if (QAction *a = actionCollection()->action(QStringLiteral("toggle_read"))) {
        const Store *store = m_mailboxes->store(m_mailboxKey);
        const QList<qint64> ids = selectedMessages();
        const std::optional<MessageRow> first = store && !ids.isEmpty() ? store->message(ids.constFirst()) : std::nullopt;
        const bool read = first && first->isRead;
        a->setEnabled(any);
        a->setText(read ? i18nc("@action", "Mark as Unread") : i18nc("@action", "Mark as Read"));
        a->setIcon(QIcon::fromTheme(read ? QStringLiteral("mail-mark-unread") : QStringLiteral("mail-mark-read")));
    }
    for (const QString &name : {QStringLiteral("report_phishing"), QStringLiteral("copy_to"), QStringLiteral("create_rule"), QStringLiteral("sweep")}) {
        if (QAction *a = actionCollection()->action(name)) {
            a->setEnabled(any);
        }
    }
    if (QAction *a = actionCollection()->action(QStringLiteral("report_phishing"))) {
        a->setVisible(m_folderWellKnown != QLatin1String("sentitems") && m_folderWellKnown != QLatin1String("drafts") && m_folderWellKnown != QLatin1String("outbox"));
    }
    if (QAction *a = actionCollection()->action(QStringLiteral("empty_folder"))) {
        const bool emptiable = m_folder != 0 && (m_folderWellKnown == QLatin1String("deleteditems") || m_folderWellKnown == QLatin1String("junkemail"));
        a->setVisible(emptiable);
        a->setEnabled(emptiable);
    }
    for (const QString &name :
         {QStringLiteral("mark_read"), QStringLiteral("mark_unread"), QStringLiteral("flag"), QStringLiteral("delete"), QStringLiteral("move"), QStringLiteral("archive")}) {
        if (QAction *action = actionCollection()->action(name)) {
            action->setEnabled(any);
        }
    }
    const bool one = m_messages->selectionModel() && m_messages->selectionModel()->selectedRows().size() == 1;
    for (const QString &name : {QStringLiteral("reply"), QStringLiteral("reply_all"), QStringLiteral("forward"), QStringLiteral("forward_as_attachment"),
                                QStringLiteral("edit_as_new"), QStringLiteral("reply_with_meeting")}) {
        if (QAction *action = actionCollection()->action(name)) {
            action->setEnabled(one);
        }
    }
    // Focused Inbox: from the tab the message is in to the other one
    const bool tabs = m_focusTabs && !m_focusTabs->isHidden();
    const bool inFocused = !tabs || m_focusTabs->currentIndex() == 0;
    if (QAction *a = actionCollection()->action(QStringLiteral("focus_move"))) {
        a->setVisible(tabs);
        a->setEnabled(any);
        a->setText(inFocused ? i18nc("@action", "Move to Other") : i18nc("@action", "Move to Focused"));
    }
    if (QAction *a = actionCollection()->action(QStringLiteral("focus_always"))) {
        a->setVisible(tabs);
        a->setEnabled(one);
        a->setText(inFocused ? i18nc("@action", "Always Move to Other") : i18nc("@action", "Always Move to Focused"));
    }
    if (QAction *a = actionCollection()->action(QStringLiteral("report_junk"))) {
        a->setEnabled(any && m_folderWellKnown != QLatin1String("sentitems") && m_folderWellKnown != QLatin1String("drafts"));
        a->setVisible(!outgoing && m_folderWellKnown != QLatin1String("sentitems"));
        a->setText(m_folderWellKnown == QLatin1String("junkemail") ? i18nc("@action", "Not Junk") : i18nc("@action", "Report Junk"));
    }
    if (QAction *a = actionCollection()->action(QStringLiteral("ignore_conversation"))) {
        Store *store = m_mailboxes->store(m_mailboxKey);
        const QList<qint64> ids = selectedMessages();
        const std::optional<MessageRow> m = store && !ids.isEmpty() ? store->message(ids.constFirst()) : std::nullopt;
        const bool ignored = m && m_ignored.contains(m->conversation);
        a->setEnabled(m && !m->conversation.isEmpty());
        a->setText(ignored ? i18nc("@action", "Stop Ignoring Conversation") : i18nc("@action", "Ignore Conversation"));
    }
}

void MainWindow::setReadingPane(const QString &where)
{
    if (where == m_readingPane) {
        return;
    }
    KConfigGroup group = windowGroup();
    // Keep the sizes of the layout being left
    if (m_readingPane == QLatin1String("bottom")) {
        group.writeEntry("SplitterBottom", m_splitter->saveState());
        group.writeEntry("ReadingSplitter", m_readingSplitter->saveState());
    } else if (!m_readingPane.isEmpty()) {
        group.writeEntry("Splitter", m_splitter->saveState());
    }
    m_readingPane = where;
    group.writeEntry("ReadingPane", where);
    group.sync();
    if (where == QLatin1String("bottom")) {
        m_splitter->insertWidget(1, m_readingSplitter);
        m_readingSplitter->addWidget(m_listPane);
        m_readingSplitter->addWidget(m_reading);
        m_readingSplitter->show();
        m_reading->show();
        if (!m_splitter->restoreState(group.readEntry("SplitterBottom", QByteArray()))) {
            m_splitter->setSizes({240, 1060});
        }
        if (!m_readingSplitter->restoreState(group.readEntry("ReadingSplitter", QByteArray()))) {
            m_readingSplitter->setSizes({320, 480});
        }
    } else {
        m_splitter->insertWidget(1, m_listPane);
        m_splitter->insertWidget(2, m_reading);
        m_readingSplitter->hide();
        m_reading->setVisible(where != QLatin1String("off"));
        m_splitter->setStretchFactor(1, where == QLatin1String("off") ? 1 : 0);
        m_splitter->restoreState(group.readEntry("Splitter", QByteArray()));
    }
}

void MainWindow::runQuickStep(const QJsonObject &step)
{
    const QList<qint64> ids = selectedMessages();
    if (ids.isEmpty()) {
        return;
    }
    const QJsonArray actions = step.value(QLatin1String("actions")).toArray();
    auto has = [&actions](const char *type) {
        return std::any_of(actions.begin(), actions.end(), [type](const QJsonValue &a) {
            return a.toObject().value(QLatin1String("type")).toString() == QLatin1String(type);
        });
    };
    // Marks first, then the answers (they need the message where it is), the move or delete last
    for (const QJsonValue &v : actions) {
        const QJsonObject a = v.toObject();
        const QString type = a.value(QLatin1String("type")).toString();
        if (type == QLatin1String("read") || type == QLatin1String("unread")) {
            markSelected(type == QLatin1String("read"));
        } else if (type == QLatin1String("flag")) {
            flagSelected(QStringLiteral("none"));
        } else if (type == QLatin1String("complete")) {
            flagSelected(QStringLiteral("complete"));
        } else if (type == QLatin1String("clearflag")) {
            flagSelected(QStringLiteral("clear"));
        } else if (type == QLatin1String("categorize")) {
            toggleCategory(a.value(QLatin1String("category")).toString(), true);
        }
    }
    for (const QJsonValue &v : actions) {
        const QJsonObject a = v.toObject();
        const QString type = a.value(QLatin1String("type")).toString();
        if (type == QLatin1String("reply") || type == QLatin1String("replyAll") || type == QLatin1String("forward")) {
            auto *composer = qobject_cast<ComposerWindow *>(openComposer(type, ids.constFirst()));
            if (composer && !a.value(QLatin1String("to")).toString().isEmpty()) {
                composer->addTo(a.value(QLatin1String("to")).toString());
            }
        }
    }
    if (has("move")) {
        for (const QJsonValue &v : actions) {
            const QJsonObject a = v.toObject();
            if (a.value(QLatin1String("type")).toString() != QLatin1String("move")) {
                continue;
            }
            Store *store = m_mailboxes->store(m_mailboxKey);
            const std::optional<FolderRow> folder = store ? store->folderByGraphId(a.value(QLatin1String("folder")).toString()) : std::nullopt;
            if (a.value(QLatin1String("mailbox")).toString() != m_mailboxKey || !folder) {
                m_status->setText(i18nc("@info:status %1 a folder", "This Quick Step moves to %1, a folder of another mailbox.", a.value(QLatin1String("folderName")).toString()));
            } else {
                moveSelected(folder->id);
            }
            break;
        }
    } else if (has("archive")) {
        archiveSelected();
    } else if (has("delete")) {
        deleteSelected();
    }
    qCInfo(GRAPHMAIL) << "quick step:" << step.value(QLatin1String("name")).toString() << ids.size() << "messages";
}

bool MainWindow::runQuickStepForTests(const QString &name)
{
    const QJsonArray steps = QuickSteps::all();
    for (const QJsonValue &v : steps) {
        if (v.toObject().value(QLatin1String("name")).toString() == name) {
            runQuickStep(v.toObject());
            return true;
        }
    }
    return false;
}

void MainWindow::reading9ForTests(const QString &focusMove, const QString &junk, const QString &ignore)
{
    if (!focusMove.isEmpty()) {
        moveFocus(focusMove.startsWith(QLatin1String("always-")), focusMove.endsWith(QLatin1String("focused")) ? 1 : 0);
    }
    if (!junk.isEmpty()) {
        reportJunk(junk == QLatin1String("junk") ? 1 : 0);
    }
    if (!ignore.isEmpty()) {
        ignoreConversation(ignore == QLatin1String("off") ? 0 : 1);
    }
}

void MainWindow::setFocusTabForTests(int tab)
{
    m_focusTabs->setCurrentIndex(tab);
}

int MainWindow::focusFilter() const
{
    // isHidden, not isVisible: the folder may open before the window is shown
    return !m_focusTabs->isHidden() ? (m_focusTabs->currentIndex() == 0 ? 1 : 0) : -1;
}

void MainWindow::updateFocusTabs()
{
    m_focusTabs->setVisible(m_focusedInbox && m_focusedInbox->isChecked() && m_folderWellKnown == QLatin1String("inbox") && m_resultsBar->isHidden());
}

void MainWindow::moveFocus(bool always, int target)
{
    const QString account = m_mailboxKey.section(QLatin1Char('/'), 0, 0);
    const QString mailbox = m_mailboxKey.section(QLatin1Char('/'), 1);
    const bool toFocused = target >= 0 ? target == 1 : m_focusTabs->currentIndex() == 1;
    const QList<qint64> ids = selectedMessages();
    Store *store = m_mailboxes->store(m_mailboxKey);
    for (qint64 id : ids) {
        m_daemon->setFocused(account, mailbox, id, toFocused);
    }
    if (!always || ids.isEmpty() || !store) {
        return;
    }
    // Always: Exchange sorts this sender's next messages the same way
    const std::optional<MessageRow> m = store->message(ids.constFirst());
    if (!m || m->senderAddress.isEmpty()) {
        return;
    }
    const QJsonObject args{{QStringLiteral("address"), m->senderAddress}, {QStringLiteral("name"), m->senderName}, {QStringLiteral("focused"), toFocused}};
    const QString sender = m->senderAddress;
    m_daemon->query(account, mailbox, QStringLiteral("focusOverride"), QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this,
                    [this, sender, toFocused](const QString &json) {
                        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                        qWarning() << "focus test: override" << answer.value(QLatin1String("ok")).toBool() << answer.value(QLatin1String("detail")).toString();
                        m_status->setText(answer.value(QLatin1String("ok")).toBool()
                                              ? (toFocused ? i18nc("@info:status %1 an address", "Mail from %1 now always goes to Focused.", sender)
                                                           : i18nc("@info:status %1 an address", "Mail from %1 now always goes to Other.", sender))
                                              : accessProblemText(answer.isEmpty() ? QStringLiteral("network") : answer.value(QLatin1String("problem")).toString(), m_mailboxKey));
                    });
}

void MainWindow::reportJunk(int junkWanted)
{
    const QString account = m_mailboxKey.section(QLatin1Char('/'), 0, 0);
    const QString mailbox = m_mailboxKey.section(QLatin1Char('/'), 1);
    const bool junk = junkWanted >= 0 ? junkWanted == 1 : m_folderWellKnown != QLatin1String("junkemail");
    const QList<qint64> ids = selectedMessages();
    for (qint64 id : ids) {
        const QJsonObject args{{QStringLiteral("message"), id}, {QStringLiteral("junk"), junk}};
        m_daemon->query(account, mailbox, QStringLiteral("junk"), QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this, [this, junk](const QString &json) {
            const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
            qWarning() << "junk test:" << junk << answer.value(QLatin1String("ok")).toBool() << answer.value(QLatin1String("detail")).toString();
            m_status->setText(answer.value(QLatin1String("ok")).toBool()
                                  ? (junk ? i18nc("@info:status", "Reported as junk: moved to Junk Email, the sender is blocked.")
                                          : i18nc("@info:status", "Not junk: moved to the Inbox, the sender is trusted."))
                                  : accessProblemText(answer.isEmpty() ? QStringLiteral("network") : answer.value(QLatin1String("problem")).toString(), m_mailboxKey));
        });
    }
}

void MainWindow::loadIgnored()
{
    m_ignored.clear();
    const QString key = m_mailboxKey;
    m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("ignoredConversations"), QStringLiteral("{}"), this,
                    [this, key](const QString &json) {
                        if (key != m_mailboxKey) {
                            return;
                        }
                        m_ignored.clear();
                        const QJsonArray list = QJsonDocument::fromJson(json.toUtf8()).array();
                        for (const QJsonValue &v : list) {
                            m_ignored.append(v.toString());
                        }
                        updateActions();
                    });
}

void MainWindow::ignoreConversation(int onWanted)
{
    Store *store = m_mailboxes->store(m_mailboxKey);
    const QList<qint64> ids = selectedMessages();
    const std::optional<MessageRow> m = store && !ids.isEmpty() ? store->message(ids.constFirst()) : std::nullopt;
    if (!m || m->conversation.isEmpty()) {
        return;
    }
    const bool on = onWanted >= 0 ? onWanted == 1 : !m_ignored.contains(m->conversation);
    if (on && !m_testMode
        && KMessageBox::warningContinueCancel(this,
                                              i18nc("@info", "The selected conversation and all its future messages will be moved to Deleted Items. Graph Mail does this "
                                                             "while it runs: Microsoft 365 does not offer it to other apps."),
                                              i18nc("@title:window", "Ignore Conversation"),
                                              KGuiItem(i18nc("@action:button", "Ignore Conversation"), QStringLiteral("mail-thread-ignored")))
            != KMessageBox::Continue) {
        return;
    }
    const QString key = m_mailboxKey;
    const QJsonObject args{{QStringLiteral("message"), m->id}, {QStringLiteral("on"), on}};
    m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("ignoreConversation"),
                    QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this, [this, on](const QString &json) {
                        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                        qWarning() << "ignore test:" << on << answer.value(QLatin1String("ok")).toBool() << answer.value(QLatin1String("moved")).toInt();
                        m_status->setText(on ? i18ncp("@info:status", "Conversation ignored: %1 message moved to Deleted Items.",
                                                      "Conversation ignored: %1 messages moved to Deleted Items.", answer.value(QLatin1String("moved")).toInt())
                                             : i18nc("@info:status", "The conversation is no longer ignored."));
                        loadIgnored();
                    });
}

void MainWindow::markSelected(bool read)
{
    const QString account = m_mailboxKey.section(QLatin1Char('/'), 0, 0);
    const QString mailbox = m_mailboxKey.section(QLatin1Char('/'), 1);
    const QList<qint64> ids = selectedMessages();
    const Store *store = m_mailboxes->store(m_mailboxKey);
    for (qint64 id : ids) {
        const bool receipt = read && store
            && ReadReceipts::decide(ReadReceipts::mode(), store->readReceiptRequested(id), true) == ReadReceipts::Decision::Send;
        m_daemon->markRead(account, mailbox, id, read, receipt || !read);
    }
    // Marking by hand wins over the automatic mark-as-read
    m_markReadTimer.stop();
}

void MainWindow::toggleFlag()
{
    const QString account = m_mailboxKey.section(QLatin1Char('/'), 0, 0);
    const QString mailbox = m_mailboxKey.section(QLatin1Char('/'), 1);
    const QModelIndexList rows = m_messages->selectionModel()->selectedRows();
    // Like Outlook: flag them all, unless all are flagged already
    const bool allFlagged = std::all_of(rows.cbegin(), rows.cend(), [](const QModelIndex &index) {
        return index.data(MessageListModel::FlagRole).toInt() == 1;
    });
    const QList<qint64> ids = selectedMessages();
    for (qint64 id : ids) {
        m_daemon->setFlag(account, mailbox, id, allFlagged ? 0 : 1);
    }
}

void MainWindow::deleteSelected()
{
    const QString account = m_mailboxKey.section(QLatin1Char('/'), 0, 0);
    const QString mailbox = m_mailboxKey.section(QLatin1Char('/'), 1);
    const QList<qint64> ids = selectedMessages();
    // In Deleted Items a delete is for good (Ctrl+Z cannot bring it back): asked first, as Outlook does
    const Store *store = m_mailboxes->store(m_mailboxKey);
    const std::optional<FolderRow> deleted = store ? store->folderByWellKnown(QStringLiteral("deleteditems")) : std::nullopt;
    const int permanent = int(std::count_if(ids.cbegin(), ids.cend(), [store, &deleted](qint64 id) {
        const std::optional<MessageRow> row = store ? store->message(id) : std::nullopt;
        return row && deleted && row->folder == deleted->id;
    }));
    if (permanent > 0
        && KMessageBox::warningContinueCancel(this,
                                              i18ncp("@info", "Permanently delete this message?", "Permanently delete these %1 messages?", permanent),
                                              i18nc("@title:window", "Delete Permanently"),
                                              KStandardGuiItem::del())
            != KMessageBox::Continue) {
        return;
    }
    rememberForUndo(m_mailboxKey, ids, true);
    for (qint64 id : ids) {
        m_daemon->remove(account, mailbox, id);
    }
}

void MainWindow::sweep()
{
    const Store *store = m_mailboxes->store(m_mailboxKey);
    const std::optional<MessageRow> row = store ? store->message(shownMessage()) : std::nullopt;
    if (!row || row->senderAddress.isEmpty()) {
        return;
    }
    const std::optional<FolderRow> folder = store->folder(row->folder);
    const QList<qint64> ids = store->messagesFrom(row->folder, row->senderAddress);
    const std::optional<MailboxInfo> box = m_mailboxes->mailbox(m_mailboxKey);
    const QString sender = row->senderName.isEmpty() ? row->senderAddress : QStringLiteral("%1 <%2>").arg(row->senderName, row->senderAddress);
    SweepDialog dialog(m_folderModel, m_mailboxKey, sender, folder ? folder->name : QString(), int(ids.size()), box && box->own, this);
    if (dialog.exec() == QDialog::Accepted) {
        sweepMessages(m_mailboxKey, shownMessage(), int(dialog.scope()), dialog.target());
    }
}

void MainWindow::sweepMessages(const QString &mailboxKey, qint64 message, int scope, qint64 target)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<MessageRow> row = store ? store->message(message) : std::nullopt;
    if (!row || row->senderAddress.isEmpty()) {
        return;
    }
    QList<qint64> ids = store->messagesFrom(row->folder, row->senderAddress);
    if (SweepDialog::Scope(scope) == SweepDialog::Scope::KeepNewest && !ids.isEmpty()) {
        ids.removeFirst();
    }
    const QString account = mailboxKey.section(QLatin1Char('/'), 0, 0);
    const QString mailbox = mailboxKey.section(QLatin1Char('/'), 1);
    rememberForUndo(mailboxKey, ids, target == 0);
    for (qint64 id : std::as_const(ids)) {
        if (target == 0) {
            m_daemon->remove(account, mailbox, id);
        } else if (id != 0) {
            m_daemon->move(account, mailbox, id, target);
        }
    }
    m_status->setText(i18ncp("@info:status", "Swept %1 message.", "Swept %1 messages.", int(ids.size())));
    if (SweepDialog::Scope(scope) != SweepDialog::Scope::AllAndFuture) {
        return;
    }
    // Future messages: an Inbox rule after the existing ones
    const std::optional<FolderRow> targetFolder = target != 0 ? store->folder(target) : std::nullopt;
    const QString address = row->senderAddress;
    m_daemon->query(account, mailbox, QStringLiteral("messageRules"), QStringLiteral("{}"), this, [this, account, mailbox, address, targetFolder](const QString &json) {
        int last = 0;
        const QJsonArray rules = QJsonDocument::fromJson(json.toUtf8()).object().value(QLatin1String("body")).toObject().value(QLatin1String("value")).toArray();
        for (const QJsonValue &v : rules) {
            last = std::max(last, v.toObject().value(QLatin1String("sequence")).toInt());
        }
        const QJsonObject args{{QStringLiteral("rule"), SweepDialog::rule(address, targetFolder ? targetFolder->graphId : QString(), last + 1)}};
        m_daemon->query(account, mailbox, QStringLiteral("messageRuleCreate"), QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this,
                        [this, address](const QString &answer) {
                            const bool ok = QJsonDocument::fromJson(answer.toUtf8()).object().value(QLatin1String("ok")).toBool();
                            m_status->setText(ok ? i18nc("@info:status", "Future messages from %1 will be swept too (a rule in Rules).", address)
                                                 : i18nc("@info:status", "The rule for future messages from %1 could not be made.", address));
                        });
    });
}

void MainWindow::runCleanup(bool interactive, bool notify)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QList<Cleanup::Candidate> candidates = Cleanup::plan(m_mailboxes, now);
    if (interactive) {
        Cleanup::ReportDialog report(candidates, this);
        if (report.exec() != QDialog::Accepted) {
            return;
        }
    }
    Cleanup::setLastRun(now);
    const Cleanup::Settings settings = Cleanup::settings();
    int moved = 0;
    int deleted = 0;
    int reported = 0;
    QStringList problems;
    QHash<QString, QList<Cleanup::Candidate>> exports;
    for (const Cleanup::Candidate &c : candidates) {
        const QString account = c.mailboxKey.section(QLatin1Char('/'), 0, 0);
        const QString mailbox = c.mailboxKey.section(QLatin1Char('/'), 1);
        switch (c.action) {
        case Cleanup::Action::Archive: {
            const qint64 target = archiveFolder(c.mailboxKey);
            if (target == 0 || target == c.folder) {
                problems.append(i18nc("@info %1 a folder", "%1: no Archive folder to move to", c.folderName));
                break;
            }
            rememberForUndo(c.mailboxKey, c.messages, false);
            for (qint64 id : c.messages) {
                m_daemon->move(account, mailbox, id, target);
            }
            moved += int(c.messages.size());
            break;
        }
        case Cleanup::Action::Delete:
            rememberForUndo(c.mailboxKey, c.messages, true);
            for (qint64 id : c.messages) {
                m_daemon->remove(account, mailbox, id);
            }
            deleted += int(c.messages.size());
            break;
        case Cleanup::Action::ExportPst:
            exports[c.mailboxKey].append(c);
            break;
        case Cleanup::Action::Report:
            reported += int(c.messages.size());
            break;
        }
    }
    QStringList done;
    if (moved > 0) {
        done.append(i18ncp("@info", "%1 message moved to the Archive folder.", "%1 messages moved to the Archive folder.", moved));
    }
    if (deleted > 0) {
        done.append(i18ncp("@info", "%1 message deleted.", "%1 messages deleted.", deleted));
    }
    if (reported > 0) {
        done.append(i18ncp("@info", "%1 old message could be cleaned up (Settings > Clean Up Old Mail).", "%1 old messages could be cleaned up (Settings > Clean Up Old Mail).", reported));
    }
    const auto tell = [this, interactive, notify](const QString &text) {
        m_status->setText(text);
        // A scheduled run happens unseen: the desktop says what it did
        if (!interactive && notify && !text.isEmpty()) {
            auto *notification = new KNotification(QStringLiteral("cleanup"));
            notification->setComponentName(QStringLiteral("plasma-graphmail-linux"));
            notification->setTitle(i18nc("@title notification", "Old mail cleaned up"));
            notification->setText(text);
            notification->setIconName(QStringLiteral("edit-clear-history"));
            notification->sendEvent();
        }
    };
    tell((done + problems).join(QLatin1Char(' ')));
    // Export to PST, then remove: one file per mailbox; only what is in the file goes
    for (auto it = exports.cbegin(); it != exports.cend(); ++it) {
        const QString key = it.key();
        const std::optional<MailboxInfo> box = m_mailboxes->mailbox(key);
        QList<qint64> folders;
        QSet<qint64> messages;
        for (const Cleanup::Candidate &c : it.value()) {
            folders.append(c.folder);
            for (qint64 id : c.messages) {
                messages.insert(id);
            }
        }
        QDir().mkpath(settings.pstFolder);
        static const QRegularExpression unsafe(QStringLiteral("[/\\\\:*?\"<>|]"));
        QString name = i18nc("@info file name, %1 a mailbox, %2 a date", "Old mail %1 %2", box ? box->label() : key, QDate::currentDate().toString(Qt::ISODate));
        name.replace(unsafe, QStringLiteral("_"));
        QString file = QDir(settings.pstFolder).filePath(name + QStringLiteral(".pst"));
        for (int n = 2; QFileInfo::exists(file); ++n) {
            file = QDir(settings.pstFolder).filePath(QStringLiteral("%1 (%2).pst").arg(name).arg(n));
        }
        auto *exporter = new PstExporter(m_mailboxes, m_daemon, key, folders, file, interactive ? this : nullptr);
        exporter->setMessages(messages);
        const int planned = int(messages.size());
        connect(exporter, &PstExporter::finished, this, [this, key, exporter, planned, tell](bool ok, int, int, int, const QString &path, const QString &error) {
            if (!ok) {
                tell(i18nc("@info %1 a file, %2 the reason", "Old mail was not removed: %1 could not be written (%2).", path, error));
                return;
            }
            // Only messages whose full original is in the file go; one rebuilt from the cache (it may
            // lack its body or attachments) or skipped stays in the mailbox
            const QList<qint64> ids = exporter->savedOriginals();
            if (ids.size() < planned) {
                tell(i18ncp("@info", "%1 old message could not be saved completely and stays in the mailbox.", "%1 old messages could not be saved completely and stay in the mailbox.",
                            planned - int(ids.size())));
            }
            if (ids.isEmpty()) {
                return;
            }
            rememberForUndo(key, ids, true);
            for (qint64 id : ids) {
                m_daemon->remove(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), id);
            }
            tell(i18ncp("@info %2 a file", "%1 old message saved in %2 and removed from the mailbox.", "%1 old messages saved in %2 and removed from the mailbox.",
                        int(ids.size()), path));
        });
        exporter->start();
    }
}

void MainWindow::emptyFolder(const QString &mailboxKey, qint64 folder)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<FolderRow> row = store && folder != 0 ? store->folder(folder) : std::nullopt;
    if (!row) {
        return;
    }
    QList<qint64> ids;
    std::optional<PageCursor> cursor;
    for (;;) {
        const QList<ListRow> page = store->listPage(folder, cursor, 1000);
        for (const ListRow &r : page) {
            ids.append(r.id);
        }
        if (page.size() < 1000) {
            break;
        }
        cursor = PageCursor{page.constLast().received, page.constLast().id};
    }
    // Graph lists only mail: calendar items, tasks, contacts and some other kinds in the folder stay
    // (its total counts them); Outlook on the web can remove those
    const int hidden = std::max(0, row->total - int(ids.size()));
    const auto offerWeb = [this, mailboxKey, row, hidden] {
        const QString text = i18ncp("@info %2 a folder",
                                    "%2 still holds %1 item that Graph Mail cannot see or remove (a calendar item, task, contact or another kind Microsoft Graph does not show). Empty the folder in Outlook on the web to remove it.",
                                    "%2 still holds %1 items that Graph Mail cannot see or remove (calendar items, tasks, contacts or other kinds Microsoft Graph does not show). Empty the folder in Outlook on the web to remove them.",
                                    hidden,
                                    row->name);
        if (KMessageBox::questionTwoActions(this, text, i18nc("@title:window", "Empty Folder"), KGuiItem(i18nc("@action:button", "Open Outlook on the web"), QStringLiteral("internet-web-browser")), KStandardGuiItem::close())
            == KMessageBox::PrimaryAction) {
            const std::optional<MailboxInfo> box = m_mailboxes->mailbox(mailboxKey);
            QUrl url(QStringLiteral("https://outlook.office.com/mail/"));
            const QString folder = row->wellKnown.isEmpty() ? QString() : row->wellKnown;
            url.setPath(QStringLiteral("/mail/") + (box && !box->own && !box->address.isEmpty() ? box->address + QLatin1Char('/') : QString()) + folder);
            QDesktopServices::openUrl(url);
        }
    };
    if (ids.isEmpty()) {
        m_status->setText(i18nc("@info:status %1 a folder", "%1 is empty.", row->name));
        if (hidden > 0) {
            offerWeb();
        }
        return;
    }
    // Deleted Items: gone for good (Exchange keeps them a while in Recoverable Items); any other
    // folder (Junk Email): to Deleted Items, as Outlook's Empty Folder does, and Ctrl+Z works
    const bool permanent = row->wellKnown == QLatin1String("deleteditems");
    const QString question = permanent
        ? i18ncp("@info %2 a folder", "Permanently delete the %1 message in %2?", "Permanently delete all %1 messages in %2?", int(ids.size()), row->name)
        : i18ncp("@info %2 a folder", "Move the %1 message in %2 to Deleted Items?", "Move all %1 messages in %2 to Deleted Items?", int(ids.size()), row->name);
    if (KMessageBox::warningContinueCancel(this, question, i18nc("@title:window", "Empty Folder"), KGuiItem(i18nc("@action:button", "Empty Folder"), QStringLiteral("edit-delete-shred")))
        != KMessageBox::Continue) {
        return;
    }
    if (!permanent) {
        rememberForUndo(mailboxKey, ids, true);
    }
    for (qint64 id : std::as_const(ids)) {
        m_daemon->remove(mailboxKey.section(QLatin1Char('/'), 0, 0), mailboxKey.section(QLatin1Char('/'), 1), id);
    }
    m_status->setText(permanent ? i18ncp("@info:status", "%1 message deleted for good.", "%1 messages deleted for good.", int(ids.size()))
                                : i18ncp("@info:status", "%1 message moved to Deleted Items.", "%1 messages moved to Deleted Items.", int(ids.size())));
    if (hidden > 0) {
        offerWeb();
    }
}

void MainWindow::folderChange(const QString &mailboxKey, const QString &kind, const QJsonObject &args, const QString &done)
{
    m_daemon->query(mailboxKey.section(QLatin1Char('/'), 0, 0), mailboxKey.section(QLatin1Char('/'), 1), kind, QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this,
                    [this, mailboxKey, done](const QString &json) {
                        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                        if (answer.value(QLatin1String("ok")).toBool()) {
                            m_status->setText(done);
                            return;
                        }
                        const QString problem = answer.isEmpty() ? QStringLiteral("network") : answer.value(QLatin1String("problem")).toString();
                        const QString detail = answer.value(QLatin1String("detail")).toString();
                        // ErrorFolderExists, a move into its own subfolder, ...: Exchange's words
                        KMessageBox::error(this,
                                           problem.isEmpty() || problem == QLatin1String("other") ? detail : accessProblemText(problem, mailboxKey),
                                           i18nc("@title:window", "Folder"));
                    });
}

void MainWindow::newFolder(const QString &mailboxKey, qint64 parent)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<FolderRow> under = store && parent != 0 ? store->folder(parent) : std::nullopt;
    if (parent != 0 && !under) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this,
                                               i18nc("@title:window", "New Folder"),
                                               under ? i18nc("@label:textbox %1 a folder", "Name of the new folder in %1:", under->name)
                                                     : i18nc("@label:textbox", "Name of the new folder:"),
                                               QLineEdit::Normal,
                                               {},
                                               &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    folderChange(mailboxKey,
                 QStringLiteral("folderCreate"),
                 {{QStringLiteral("parent"), under ? under->graphId : QStringLiteral("msgfolderroot")}, {QStringLiteral("name"), name}, {QStringLiteral("unique"), true}},
                 i18nc("@info:status %1 a folder", "Folder %1 created.", name));
}

void MainWindow::renameFolder(const QString &mailboxKey, qint64 folder)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<FolderRow> row = store ? store->folder(folder) : std::nullopt;
    if (!row || !row->wellKnown.isEmpty()) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, i18nc("@title:window", "Rename Folder"), i18nc("@label:textbox", "New name:"), QLineEdit::Normal, row->name, &ok).trimmed();
    if (!ok || name.isEmpty() || name == row->name) {
        return;
    }
    folderChange(mailboxKey,
                 QStringLiteral("folderChange"),
                 {{QStringLiteral("folder"), folder}, {QStringLiteral("action"), QStringLiteral("rename")}, {QStringLiteral("name"), name}},
                 i18nc("@info:status %1 a folder", "Folder renamed to %1.", name));
}

void MainWindow::moveFolder(const QString &mailboxKey, qint64 folder)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<FolderRow> row = store ? store->folder(folder) : std::nullopt;
    if (!row || !row->wellKnown.isEmpty()) {
        return;
    }
    MoveDialog dialog(m_folderModel, mailboxKey, this);
    dialog.setWindowTitle(i18nc("@title:window %1 a folder", "Move Folder %1", row->name));
    if (dialog.exec() != QDialog::Accepted || (dialog.folder() == 0 && !dialog.mailboxChosen()) || dialog.folder() == folder) {
        return;
    }
    const std::optional<FolderRow> target = dialog.folder() != 0 ? store->folder(dialog.folder()) : std::nullopt;
    folderChange(mailboxKey,
                 QStringLiteral("folderChange"),
                 {{QStringLiteral("folder"), folder}, {QStringLiteral("action"), QStringLiteral("move")}, {QStringLiteral("target"), dialog.folder()}},
                 target ? i18nc("@info:status %1 a folder, %2 another", "Folder %1 moved to %2.", row->name, target->name)
                        : i18nc("@info:status %1 a folder", "Folder %1 moved to the top of the mailbox.", row->name));
}

void MainWindow::deleteFolder(const QString &mailboxKey, qint64 folder)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<FolderRow> row = store ? store->folder(folder) : std::nullopt;
    if (!row || !row->wellKnown.isEmpty()) {
        return;
    }
    // Inside Deleted Items it goes for good (Exchange keeps it a while in Recoverable Items)
    bool inDeleted = false;
    for (std::optional<FolderRow> up = store->folderByGraphId(row->parentGraphId); up && !inDeleted; up = store->folderByGraphId(up->parentGraphId)) {
        inDeleted = up->wellKnown == QLatin1String("deleteditems");
    }
    const QString question = inDeleted ? i18nc("@info %1 a folder", "Permanently delete the folder %1 and everything in it?", row->name)
                                       : i18nc("@info %1 a folder", "Move the folder %1 and everything in it to Deleted Items?", row->name);
    if (KMessageBox::warningContinueCancel(this, question, i18nc("@title:window", "Delete Folder"), KStandardGuiItem::del()) != KMessageBox::Continue) {
        return;
    }
    folderChange(mailboxKey,
                 QStringLiteral("folderChange"),
                 {{QStringLiteral("folder"), folder}, {QStringLiteral("action"), QStringLiteral("delete")}},
                 inDeleted ? i18nc("@info:status %1 a folder", "Folder %1 deleted for good.", row->name)
                           : i18nc("@info:status %1 a folder", "Folder %1 moved to Deleted Items.", row->name));
}

void MainWindow::markAllRead(const QString &mailboxKey, qint64 folder)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<FolderRow> row = store ? store->folder(folder) : std::nullopt;
    if (!row) {
        return;
    }
    folderChange(mailboxKey,
                 QStringLiteral("folderChange"),
                 {{QStringLiteral("folder"), folder}, {QStringLiteral("action"), QStringLiteral("markAllRead")}},
                 i18nc("@info:status %1 a folder", "Everything in %1 is marked as read.", row->name));
}

void MainWindow::rememberForUndo(const QString &mailboxKey, const QList<qint64> &messages, bool deleting)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    if (!store) {
        return;
    }
    const std::optional<FolderRow> deleted = store->folderByWellKnown(QStringLiteral("deleteditems"));
    UndoStep step{mailboxKey, {}};
    for (qint64 message : messages) {
        const std::optional<MessageRow> row = store->message(message);
        if (row && !(deleting && deleted && row->folder == deleted->id)) {
            step.moves.append({message, row->folder});
        }
    }
    if (step.moves.isEmpty()) {
        return;
    }
    m_undoSteps.append(step);
    // Outlook keeps a few steps; older ones fall off
    while (m_undoSteps.size() > 20) {
        m_undoSteps.removeFirst();
    }
    m_undoAction->setEnabled(true);
}

void MainWindow::undoLast()
{
    if (m_undoSteps.isEmpty()) {
        return;
    }
    const UndoStep step = m_undoSteps.takeLast();
    m_undoAction->setEnabled(!m_undoSteps.isEmpty());
    const Store *store = m_mailboxes->store(step.mailboxKey);
    int moved = 0;
    for (const auto &[message, folder] : step.moves) {
        // Still in the cache and somewhere else: back to where it was
        const std::optional<MessageRow> row = store ? store->message(message) : std::nullopt;
        if (row && row->folder != folder && store->folder(folder)) {
            m_daemon->move(step.mailboxKey.section(QLatin1Char('/'), 0, 0), step.mailboxKey.section(QLatin1Char('/'), 1), message, folder);
            ++moved;
        }
    }
    m_status->setText(moved > 0 ? i18ncp("@info:status", "Undone: %1 message moved back.", "Undone: %1 messages moved back.", moved)
                                : i18nc("@info:status", "Nothing to undo: the messages are no longer there."));
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_messages->viewport() && (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop)) {
        // Only files from outside; messages of the list itself are not dropped back on it
        auto *drop = static_cast<QDropEvent *>(event);
        const qint64 folder = m_folders->currentIndex().data(FolderModel::FolderIdRole).toLongLong();
        const bool files = folder != 0 && drop->mimeData()->hasUrls() && !drop->mimeData()->hasFormat(MessageListModel::mimeType());
        const QStringList paths = files ? FolderModel::messageFiles(drop->mimeData()) : QStringList();
        if (paths.isEmpty()) {
            drop->ignore();
            return true;
        }
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        if (event->type() == QEvent::Drop) {
            importMessageFiles(m_mailboxKey, folder, paths);
        }
        return true;
    }
    if (watched == m_messages && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if ((key == Qt::Key_Return || key == Qt::Key_Enter) && m_messages->currentIndex().isValid()) {
            openMessageWindow(m_messages->currentIndex());
            return true;
        }
    }
    return KXmlGuiWindow::eventFilter(watched, event);
}

QWidget *MainWindow::openSelectedMessage()
{
    openMessageWindow(m_messages->currentIndex());
    m_messageWindows.removeAll(nullptr);
    return m_messageWindows.isEmpty() ? nullptr : m_messageWindows.constLast().data();
}

void MainWindow::openMessageWindow(const QModelIndex &index)
{
    // A conversation row stands for its newest message
    const qint64 message = index.data(MessageListModel::IdRole).toLongLong();
    if (message == 0) {
        return;
    }
    m_messageWindows.removeAll(nullptr);
    for (MessageWindow *window : std::as_const(m_messageWindows)) {
        if (window->message() == message && window->mailboxKey() == m_mailboxKey) {
            window->show();
            window->raise();
            window->activateWindow();
            return;
        }
    }
    // A draft is written further, not read
    const QModelIndex drafts = m_folderModel->indexForWellKnown(m_mailboxKey, QStringLiteral("drafts"));
    if (drafts.isValid() && drafts.data(FolderModel::FolderIdRole).toLongLong() == m_folder && m_searchField->text().trimmed().isEmpty()) {
        openComposer(QStringLiteral("draft"), message);
        return;
    }
    // The pane's timer is not needed any more: the window marks it read
    m_markReadTimer.stop();
    auto *window = new MessageWindow(m_mailboxes, m_daemon, m_folderModel, m_mailboxKey, message, m_markReadEnabled);
    connect(window, &MessageWindow::replyWithMeetingRequested, this, &MainWindow::replyWithMeeting);
    connect(window, &MessageWindow::mailtoRequested, this, &MainWindow::openMailto);
    m_messageWindows.append(window);
    window->show();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    KConfigGroup group = windowGroup();
    // The three-pane layout's sizes and the bottom layout's apart: their widgets differ
    if (m_readingPane == QLatin1String("bottom")) {
        group.writeEntry("SplitterBottom", m_splitter->saveState());
        group.writeEntry("ReadingSplitter", m_readingSplitter->saveState());
    } else {
        group.writeEntry("Splitter", m_splitter->saveState());
    }
    group.sync();
    // Quitting closes the message windows and the calendar too
    for (MessageWindow *window : std::as_const(m_messageWindows)) {
        if (window) {
            window->close();
        }
    }
    if (m_calendar) {
        m_calendar->close();
    }
    if (m_contacts) {
        m_contacts->close();
    }
    if (m_tasks) {
        m_tasks->close();
    }
    for (const QPointer<QWidget> &window : std::as_const(m_detached)) {
        if (window) {
            window->close();
        }
    }
    KXmlGuiWindow::closeEvent(event);
}

}
