// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "composer.h"
#include "graphsynclink.h"
#include "maileditor.h"
#include "pastedimages.h"
#include "spelllanguage.h"
#include "fontfamilybox.h"
#include "coloraction.h"
#include "accesstext.h"
#include "avatar.h"
#include "blobstore.h"
#include "daemonclient.h"
#include "mailboxset.h"
#include "mailboxworker.h"
#include "mailview.h"
#include "mainwindow.h"
#include "mimeexport.h"
#include "quote.h"
#include "recipientedit.h"
#include "replyoptions.h"
#include "signaturedialog.h"
#include "signatures.h"
#include "store.h"
#include "templates.h"

#include <KActionCollection>
#include <KActionMenu>
#include <KConfigGroup>
#include <KFontAction>
#include <KLocalizedString>
#include <KMessageBox>
#include <KMessageWidget>
#include <KPIMTextEdit/RichTextComposer>
#include <KPIMTextEdit/RichTextComposerActions>
#include <KPIMTextEdit/RichTextComposerControler>
#include <KPIMTextEdit/RichTextComposerImages>
#include <KSharedConfig>
#include <KStandardAction>
#include <KStandardShortcut>
#include <TextEmoticonsWidgets/EmoticonTextEditAction>
#include <KSelectAction>
#include <KToggleAction>

#include <QActionGroup>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QFile>
#include <QInputDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QKeyEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPointer>
#include <QPrinter>
#include <QPrintDialog>
#include <QRegularExpression>
#include <QSplitter>
#include <QStandardPaths>
#include <QStringListModel>
#include <QTextBlock>
#include <QTextDocument>
#include <QApplication>
#include <QAbstractItemView>
#include <QCompleter>
#include <QDialog>
#include <QTextTable>
#include <QWidgetAction>
#include <QMouseEvent>
#include <QAbstractTextDocumentLayout>
#include <QScrollBar>
#include <KToolBar>
#include <QUuid>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
constexpr int autosaveMs = 60 * 1000;

QString bodyOf(const QString &html)
{
    static const QRegularExpression body(QStringLiteral("<body[^>]*>(.*)</body>"), QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = body.match(html);
    return match.hasMatch() ? match.captured(1).trimmed() : html;
}

KConfigGroup mailboxGroup(const QString &key)
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals);
    // The daemon writes what it learned into the same file
    config->reparseConfiguration();
    return config->group(QStringLiteral("Account ") + key.section(QLatin1Char('/'), 0, 0)).group(QStringLiteral("Mailbox ") + key.section(QLatin1Char('/'), 1));
}

// The composer's own settings: the other From addresses used before
KConfigGroup composerGroup()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Composer"));
}

// Undo send (docs/PHASE9.md): how long a sent message waits; 0 = sent at once
int undoSendSeconds()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals);
    config->reparseConfiguration();
    return std::clamp(config->group(QStringLiteral("General")).readEntry("UndoSendSeconds", 0), 0, 60);
}

// The font Outlook writes in; the next one when it is not installed
QFont mailFont()
{
    QFont font;
    font.setFamilies({QStringLiteral("Aptos"), QStringLiteral("Calibri"), QStringLiteral("Carlito"), QFont().family()});
    font.setPointSize(11);
    return font;
}
}

ComposerWindow::ComposerWindow(MailboxSet *mailboxes, DaemonClient *daemon, const ComposeRequest &request, QWidget *parent)
    : KXmlGuiWindow(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
    , m_request(request)
    , m_mailboxKey(request.mailboxKey)
    , m_book(new AddressBook(mailboxes, this))
    , m_notice(new KMessageWidget(this))
    , m_from(new QComboBox(this))
    , m_to(new RecipientEdit(m_book, this))
    , m_cc(new RecipientEdit(m_book, this))
    , m_bcc(new RecipientEdit(m_book, this))
    , m_bccLabel(new QLabel(i18nc("@label:textbox", "Bcc:"), this))
    , m_replyTo(new RecipientEdit(m_book, this))
    , m_replyToLabel(new QLabel(i18nc("@label:textbox", "Reply to:"), this))
    , m_subject(new QLineEdit(this))
    , m_attachmentList(new QListWidget(this))
    , m_editor(new MailEditor(this))
    , m_splitter(new QSplitter(Qt::Vertical, this))
    , m_quoteBox(new QWidget(this))
    , m_quoteHeader(new QLabel(this))
    , m_quoteView(new MailView(this))
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::Window);
    setAcceptDrops(true);
    MailView::prepareWindow(this);
    m_store = m_mailboxes->store(m_mailboxKey);
    m_blobs = m_mailboxes->blobs(m_mailboxKey);
    m_record.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);

    m_notice->setVisible(false);
    m_notice->setCloseButtonVisible(true);
    m_notice->setWordWrap(true);

    // The header fields, as in Outlook
    auto *fields = new QWidget(this);
    auto *form = new QFormLayout(fields);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(i18nc("@label:listbox", "From:"), m_from);
    form->addRow(i18nc("@label:textbox", "To:"), m_to);
    form->addRow(i18nc("@label:textbox", "Cc:"), m_cc);
    form->addRow(m_bccLabel, m_bcc);
    form->addRow(m_replyToLabel, m_replyTo);
    form->addRow(i18nc("@label:textbox", "Subject:"), m_subject);

    m_attachmentList->setFlow(QListView::LeftToRight);
    m_attachmentList->setWrapping(true);
    m_attachmentList->setMaximumHeight(64);
    m_attachmentList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_attachmentList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_attachmentList->setFrameShape(QFrame::NoFrame);
    m_attachmentList->setVisible(false);
    connect(m_attachmentList, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (m_attachmentList->selectedItems().isEmpty()) {
            return;
        }
        QMenu menu;
        menu.addAction(QIcon::fromTheme(QStringLiteral("list-remove")), i18nc("@action:inmenu", "Remove Attachment"), this, &ComposerWindow::removeSelectedAttachments);
        menu.exec(m_attachmentList->viewport()->mapToGlobal(pos));
    });

    // The editor: rich text in Outlook's font, spell checking while typing
    m_editor->setSpellCheckingSupport(true);
    m_editor->setCheckSpellingEnabled(true);
    m_spell = new SpellLanguage(m_editor, this);
    m_editor->document()->setDefaultFont(mailFont());
    m_editor->activateRichText();
    // Pictures pasted from a web page (Outlook on the web) point at the web: brought into the mail
    auto *pasted = new PastedImages(m_editor);
    pasted->setContentIdResolver([this](const QString &contentId) {
        return m_mailboxes->inlinePicture(contentId);
    });
    connect(pasted, &PastedImages::unavailable, this, [this](int count) {
        KMessageBox::information(this,
                                 i18ncp("@info",
                                        "%1 picture could not be copied. The browser copies some pictures as links that only work inside it: save the picture there and add it with Insert Image.",
                                        "%1 pictures could not be copied. The browser copies some pictures as links that only work inside it: save them there and add them with Insert Image.",
                                        count),
                                 i18nc("@title:window", "Pasted Pictures"));
    });
    // @mentions
    connect(m_editor, &QTextEdit::cursorPositionChanged, this, &ComposerWindow::updateMention);

    // The original under an answer: shown as it is, not edited
    auto *quoteLayout = new QVBoxLayout(m_quoteBox);
    quoteLayout->setContentsMargins(0, 0, 0, 0);
    m_quoteHeader->setTextFormat(Qt::RichText);
    m_quoteHeader->setWordWrap(true);
    m_quoteHeader->setTextInteractionFlags(Qt::TextSelectableByMouse);
    quoteLayout->addWidget(m_quoteHeader);
    quoteLayout->addWidget(m_quoteView, 1);
    m_quoteBox->setVisible(false);
    m_splitter->addWidget(m_editor);
    m_splitter->addWidget(m_quoteBox);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 2);
    m_splitter->setChildrenCollapsible(false);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    // MailTips (docs/PHASE9.md): what Exchange knows about the recipients, as under Outlook's To
    m_mailTips = new KMessageWidget(this);
    m_mailTips->setMessageType(KMessageWidget::Information);
    m_mailTips->setCloseButtonVisible(true);
    m_mailTips->setWordWrap(true);
    m_mailTips->hide();
    m_mailTipsTimer.setSingleShot(true);
    m_mailTipsTimer.setInterval(1200);
    connect(&m_mailTipsTimer, &QTimer::timeout, this, &ComposerWindow::askMailTips);
    layout->addWidget(m_notice);
    layout->addWidget(fields);
    layout->addWidget(m_mailTips);
    layout->addWidget(m_attachmentList);
    layout->addWidget(m_splitter, 1);
    setCentralWidget(central);

    setupActions();
    setupGUI(QSize(900, 760), ToolBar | Keys | Save | Create, QStringLiteral("graphmailcomposerui.rc"));
    setAutoSaveSettings(QStringLiteral("ComposerWindow"));
    // The formatting on a row of its own, as in Outlook: next to Send it went into the toolbar's
    // overflow arrow as soon as the window was a little narrow (also over a saved layout)
    if (KToolBar *format = toolBar(QStringLiteral("formatToolBar")); format && !toolBarBreak(format)) {
        insertToolBarBreak(format);
    }
    // KDE's standard Print, Cut, Copy and Paste land in front of Send on their own: menus only
    if (KToolBar *main = toolBar(QStringLiteral("mainToolBar"))) {
        for (const char *name : {"file_print", "edit_cut", "edit_copy", "edit_paste"}) {
            if (QAction *action = actionCollection()->action(QLatin1String(name))) {
                main->removeAction(action);
            }
        }
    }
    // The main row stays short: the less used buttons show only their icon (the tooltip names them)
    for (const char *name : {"importance_high", "importance_low", "save_draft", "discard", "manage_link", "add_image", "insert_table_dialog", "insert_emoji", "edit_redo"}) {
        if (QAction *action = actionCollection()->action(QLatin1String(name))) {
            action->setPriority(QAction::LowPriority);
        }
    }

    setupFrom();
    m_spell->setMailbox(fromKey());

    // What the window starts with
    std::optional<Outgoing> stored;
    if (!m_request.outgoing.isEmpty() && m_store) {
        stored = m_store->outgoing(m_request.outgoing);
    }
    std::optional<MessageRow> original = (m_store && m_request.message != 0) ? m_store->message(m_request.message) : std::nullopt;
    if (!stored && m_request.kind == QLatin1String("draft") && original && m_store) {
        // A draft saved by Graph Mail opens the way it was left
        stored = m_store->outgoingForDraft(original->graphId);
    }
    if (stored) {
        restore(*stored);
    } else if (m_request.kind == QLatin1String("draft") && original) {
        // Made elsewhere (Outlook, the phone): read it whole, the cache lacks Bcc and attachments
        m_record.kind = QStringLiteral("draft");
        m_record.draftId = original->graphId;
        startFrom(&*original);
        m_editor->setEnabled(false);
        const MessageRow draft = *original;
        m_daemon->loadMessage(m_mailboxKey.section(QLatin1Char('/'), 0, 0), m_mailboxKey.section(QLatin1Char('/'), 1), draft.id, this, [this, draft](const QString &json) {
            restoreFromServer(json, draft);
        });
    } else {
        startFrom(original ? &*original : nullptr);
    }

    connect(m_editor, &QTextEdit::textChanged, this, &ComposerWindow::setDirty);
    m_editor->installEventFilter(this);
    // Files dropped on the text: the editor would take them as text
    m_editor->viewport()->installEventFilter(this);
    for (QLineEdit *edit : {static_cast<QLineEdit *>(m_to), static_cast<QLineEdit *>(m_cc), static_cast<QLineEdit *>(m_bcc), static_cast<QLineEdit *>(m_replyTo), m_subject}) {
        connect(edit, &QLineEdit::textEdited, this, &ComposerWindow::setDirty);
    }
    for (RecipientEdit *edit : {m_to, m_cc, m_bcc, m_replyTo}) {
        connect(edit, &QLineEdit::textChanged, &m_mailTipsTimer, qOverload<>(&QTimer::start));
    }
    connect(m_subject, &QLineEdit::textChanged, this, &ComposerWindow::updateTitle);
    connect(m_from, &QComboBox::activated, this, [this] {
        if (m_from->currentData().toString() == QLatin1String("other-new")) {
            askOtherFrom();
            return;
        }
        fromChanged();
    });
    connect(m_daemon, &DaemonClient::bodyReady, this, [this](const QString &account, const QString &mailbox, qint64 message) {
        if (message == m_quoteSource && account + QLatin1Char('/') + mailbox == m_mailboxKey && m_quoteHtml.isEmpty()) {
            buildQuote();
            showQuote();
        }
        if (message != 0 && message == m_asNewSource && account + QLatin1Char('/') + mailbox == m_mailboxKey) {
            fillAsNew();
        }
    });
    connect(m_daemon, &DaemonClient::sendRightLearned, this, &ComposerWindow::updateSendMenu);
    connect(m_daemon, &DaemonClient::outgoingChanged, this, [this](const QString &, const QString &, const QString &uuid, const QString &state, const QString &error) {
        if (uuid == m_record.uuid && state == QLatin1String("failed")) {
            m_notice->setMessageType(KMessageWidget::Error);
            m_notice->setText(error.startsWith(QLatin1String("sendAsDenied:")) ? accessProblemText(QStringLiteral("sendAsDenied"), error.mid(13))
                                                                                : i18nc("@info", "Exchange did not accept this message: %1", error));
            m_notice->animatedShow();
        }
    });

    m_autosave.setInterval(autosaveMs);
    connect(&m_autosave, &QTimer::timeout, this, [this] {
        if (m_dirty) {
            save(false);
        }
    });
    m_autosave.start();
    m_dirty = false;
    updateTitle();
    updateSendMenu();
    if (m_to->text().isEmpty()) {
        m_to->setFocus();
    } else {
        m_editor->setFocus();
    }
}

ComposerWindow::~ComposerWindow() = default;

void ComposerWindow::setupActions()
{
    KActionCollection *actions = actionCollection();

    // The spelling language, on the formatting row (0.14.4)
    auto *spellLanguage = new QWidgetAction(this);
    spellLanguage->setText(i18nc("@action", "Spelling Language"));
    spellLanguage->setIcon(QIcon::fromTheme(QStringLiteral("tools-check-spelling")));
    spellLanguage->setDefaultWidget(m_spell->comboBox());
    actions->addAction(QStringLiteral("spell_language"), spellLanguage);

    // Send, with an arrow for Send As / Send on Behalf when writing from another mailbox
    m_send = new KActionMenu(QIcon::fromTheme(QStringLiteral("mail-send")), i18nc("@action", "Send"), this);
    m_send->setPopupMode(QToolButton::MenuButtonPopup);
    actions->addAction(QStringLiteral("send"), m_send);
    actions->setDefaultShortcut(m_send, QKeySequence(Qt::CTRL | Qt::Key_Return));
    connect(m_send, &QAction::triggered, this, [this] {
        // Names typed without an address are checked first, as Outlook does on Send
        QStringList invalid;
        m_to->recipients(&invalid);
        m_cc->recipients(&invalid);
        m_bcc->recipients(&invalid);
        if (!invalid.isEmpty()) {
            checkNames([this](bool resolved) {
                if (resolved) {
                    m_send->trigger();
                } else {
                    validate();
                }
            });
            return;
        }
        // The checks and their questions (attachment, subject) once per Send (0.14.5: they came
        // twice), also when the OneDrive step below sends by itself later
        if (!m_sendChecked) {
            if (!validate()) {
                return;
            }
            m_sendChecked = true;
        }
        // OneDrive/SharePoint links: uploads finished and the recipients given access first; this
        // runs again by itself when that is done
        if (!cloudReadyToSend()) {
            return;
        }
        m_sendChecked = false;
        save(true);
        m_closing = true;
        // Undo send: the main window offers Undo while the message waits
        if (m_record.sendAfter > 0) {
            if (MainWindow *main = MainWindow::instance()) {
                main->offerUndoSend(m_mailboxKey, m_record.uuid, m_subject->text(), undoSendSeconds());
            }
        }
        close();
    });
    m_sendMenu = new QMenu(this);
    m_sendModes = new QActionGroup(this);
    m_sendModes->setExclusive(true);

    QAction *checkNamesAction = actions->addAction(QStringLiteral("check_names"));
    checkNamesAction->setText(i18nc("@action", "Check Names"));
    checkNamesAction->setIcon(QIcon::fromTheme(QStringLiteral("im-user")));
    actions->setDefaultShortcut(checkNamesAction, QKeySequence(Qt::CTRL | Qt::Key_K));
    connect(checkNamesAction, &QAction::triggered, this, [this] {
        checkNames();
    });

    QAction *saveDraft = actions->addAction(QStringLiteral("save_draft"));
    saveDraft->setText(i18nc("@action", "Save Draft"));
    saveDraft->setIcon(QIcon::fromTheme(QStringLiteral("document-save")));
    actions->setDefaultShortcut(saveDraft, QKeySequence::Save);
    connect(saveDraft, &QAction::triggered, this, [this] {
        save(false);
    });

    QAction *attach = actions->addAction(QStringLiteral("attach"));
    attach->setText(i18nc("@action", "Attach File..."));
    attach->setIconText(i18nc("@action short toolbar text", "Attach"));
    attach->setIcon(QIcon::fromTheme(QStringLiteral("mail-attachment")));
    actions->setDefaultShortcut(attach, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    connect(attach, &QAction::triggered, this, [this] {
        addFiles(QFileDialog::getOpenFileNames(this, i18nc("@title:window", "Attach Files")));
    });
    // Synced OneDrive/SharePoint files as links (composercloud.cpp): only with Graph Sync installed
    // and syncing locations of this account
    QAction *attachCloud = actions->addAction(QStringLiteral("attach_cloud"));
    attachCloud->setText(i18nc("@action", "Attach from OneDrive or SharePoint..."));
    attachCloud->setIcon(QIcon::fromTheme(QStringLiteral("folder-cloud")));
    attachCloud->setVisible(GraphSyncLink::available() && !GraphSyncLink::locations(m_mailboxKey.section(QLatin1Char('/'), 0, 0)).isEmpty());
    connect(attachCloud, &QAction::triggered, this, &ComposerWindow::attachFromCloud);

    QAction *discardAction = actions->addAction(QStringLiteral("discard"));
    discardAction->setText(i18nc("@action", "Discard"));
    discardAction->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete")));
    connect(discardAction, &QAction::triggered, this, [this] {
        if (KMessageBox::warningContinueCancel(this,
                                               i18nc("@info", "Discard this message? It is deleted from Drafts too."),
                                               i18nc("@title:window", "Discard Message"),
                                               KStandardGuiItem::discard())
            == KMessageBox::Continue) {
            discard();
        }
    });

    m_high = new KToggleAction(QIcon::fromTheme(QStringLiteral("emblem-important")), i18nc("@action", "High Importance"), this);
    actions->addAction(QStringLiteral("importance_high"), m_high);
    m_low = new KToggleAction(QIcon::fromTheme(QStringLiteral("arrow-down")), i18nc("@action", "Low Importance"), this);
    actions->addAction(QStringLiteral("importance_low"), m_low);
    connect(m_high, &QAction::toggled, this, [this](bool on) {
        if (on) {
            m_low->setChecked(false);
        }
        setDirty();
    });
    connect(m_low, &QAction::toggled, this, [this](bool on) {
        if (on) {
            m_high->setChecked(false);
        }
        setDirty();
    });

    // Options (docs/PHASE9.md): receipts, sensitivity, delay delivery, as Outlook's Options tab
    m_deliveryReceipt = new KToggleAction(i18nc("@action", "Request a Delivery Receipt"), this);
    actions->addAction(QStringLiteral("delivery_receipt"), m_deliveryReceipt);
    m_readReceipt = new KToggleAction(i18nc("@action", "Request a Read Receipt"), this);
    actions->addAction(QStringLiteral("read_receipt"), m_readReceipt);
    m_sensitivity = new KSelectAction(QIcon::fromTheme(QStringLiteral("security-medium")), i18nc("@action", "Sensitivity"), this);
    m_sensitivity->setItems({i18nc("@item sensitivity", "Normal"), i18nc("@item sensitivity", "Personal"), i18nc("@item sensitivity", "Private"),
                             i18nc("@item sensitivity", "Confidential")});
    m_sensitivity->setCurrentItem(0);
    actions->addAction(QStringLiteral("sensitivity"), m_sensitivity);
    m_delay = actions->addAction(QStringLiteral("delay_delivery"));
    m_delay->setIcon(QIcon::fromTheme(QStringLiteral("mail-send-delayed"), QIcon::fromTheme(QStringLiteral("chronometer"))));
    m_delay->setCheckable(true);
    connect(m_delay, &QAction::triggered, this, &ComposerWindow::askDelay);
    showDelay();
    QAction *saveTemplate = actions->addAction(QStringLiteral("save_template"));
    saveTemplate->setText(i18nc("@action", "Save as Template..."));
    saveTemplate->setIcon(QIcon::fromTheme(QStringLiteral("document-save-as-template")));
    connect(saveTemplate, &QAction::triggered, this, &ComposerWindow::saveAsTemplate);
    for (QAction *a : {static_cast<QAction *>(m_deliveryReceipt), static_cast<QAction *>(m_readReceipt), static_cast<QAction *>(m_sensitivity)}) {
        connect(a, &QAction::triggered, this, &ComposerWindow::setDirty);
    }

    m_plainText = new KToggleAction(i18nc("@action", "Plain Text"), this);
    m_plainText->setToolTip(i18nc("@info:tooltip", "Write this message without formatting"));
    actions->addAction(QStringLiteral("plain_text"), m_plainText);
    connect(m_plainText, &QAction::toggled, this, [this](bool plain) {
        if (plain) {
            m_editor->switchToPlainText();
        } else {
            m_editor->activateRichText();
        }
        setDirty();
    });

    m_showBcc = new KToggleAction(i18nc("@action", "Show Bcc"), this);
    actions->addAction(QStringLiteral("show_bcc"), m_showBcc);
    connect(m_showBcc, &QAction::toggled, this, [this](bool on) {
        m_bcc->setVisible(on);
        m_bccLabel->setVisible(on);
    });
    m_bcc->setVisible(false);
    m_bccLabel->setVisible(false);
    m_showReplyTo = new KToggleAction(i18nc("@action", "Show Reply To"), this);
    actions->addAction(QStringLiteral("show_reply_to"), m_showReplyTo);
    connect(m_showReplyTo, &QAction::toggled, this, [this](bool on) {
        m_replyTo->setVisible(on);
        m_replyToLabel->setVisible(on);
    });
    m_replyTo->setVisible(false);
    m_replyToLabel->setVisible(false);

    m_signatureMenu = new KActionMenu(QIcon::fromTheme(QStringLiteral("document-sign")), i18nc("@action", "Signature"), this);
    m_signatureMenu->setPopupMode(QToolButton::InstantPopup);
    actions->addAction(QStringLiteral("signature"), m_signatureMenu);
    connect(m_signatureMenu->menu(), &QMenu::aboutToShow, this, [this] {
        QMenu *menu = m_signatureMenu->menu();
        menu->clear();
        menu->addAction(i18nc("@item:inmenu no signature", "(None)"), this, [this] {
            insertSignature({});
        });
        const QList<Signature> all = Signatures::all();
        for (const Signature &s : all) {
            QAction *a = menu->addAction(s.name, this, [this, id = s.id] {
                insertSignature(id);
            });
            a->setCheckable(true);
            a->setChecked(s.id == m_signatureId);
        }
        menu->addSeparator();
        menu->addAction(QIcon::fromTheme(QStringLiteral("configure")), i18nc("@action:inmenu", "Signatures..."), this, [this] {
            SignatureDialog dialog(m_mailboxes, this);
            dialog.exec();
        });
    });

    // The editor's formatting (bold, lists, fonts, links, pictures, ...)
    m_editor->composerActions()->createActions(actions);
    // The editor was switched to rich text before its actions existed; the table menu only learns the
    // mode from that switch and does nothing in plain text
    m_editor->composerActions()->textModeChanged(m_editor->textMode());
    // The font box applies a font on Enter or a pick from its list, not at every letter typed
    if (auto *fontFamily = qobject_cast<KFontAction *>(actions->action(QStringLiteral("format_font_family")))) {
        new FontFamilyBox(fontFamily, m_editor);
    }
    // Insert Table opens the table dialog at once: the editor's own Table is a menu of menus
    // (Insert > Table..., rows, columns, joining), kept for working on a table
    QAction *insertTable = actions->addAction(QStringLiteral("insert_table_dialog"));
    insertTable->setText(i18nc("@action", "Insert Table..."));
    insertTable->setIconText(i18nc("@action short toolbar text", "Table"));
    insertTable->setIcon(QIcon::fromTheme(QStringLiteral("insert-table")));
    connect(insertTable, &QAction::triggered, this, [this] {
        auto *tables = qobject_cast<KActionMenu *>(actionCollection()->action(QStringLiteral("insert_table")));
        QAction *insert = tables ? tables->menu()->actions().value(0) : nullptr;
        if (QAction *table = insert && insert->menu() ? insert->menu()->actions().value(0) : nullptr) {
            const auto tablesNow = [this] {
                QList<QTextTable *> list;
                for (QTextFrame *frame : m_editor->document()->rootFrame()->childFrames()) {
                    if (auto *t = qobject_cast<QTextTable *>(frame)) {
                        list.append(t);
                    }
                }
                return list;
            };
            const QList<QTextTable *> before = tablesNow();
            table->trigger();
            // The cursor into the first cell of the new table, as in Outlook
            for (QTextTable *inserted : tablesNow()) {
                if (!before.contains(inserted)) {
                    m_editor->setTextCursor(inserted->cellAt(0, 0).firstCursorPosition());
                    m_editor->setFocus();
                    break;
                }
            }
        }
    });
    // Outlook's Font Color and Highlight: the last colour on the button, a palette under the arrow
    // (KPIMTextEdit's own buttons show only a square of the colour)
    for (const ColorAction::Kind kind : {ColorAction::Kind::Text, ColorAction::Kind::Highlight}) {
        auto *color = new ColorAction(kind, this);
        actions->addAction(kind == ColorAction::Kind::Text ? QStringLiteral("text_color") : QStringLiteral("highlight_color"), color);
        connect(color, &ColorAction::colorChosen, this, [this, kind](const QColor &chosen) {
            const bool text = kind == ColorAction::Kind::Text;
            if (!chosen.isValid()) {
                clearTextColor(!text);
            } else if (text) {
                m_editor->composerControler()->setTextForegroundColor(chosen);
            } else {
                m_editor->composerControler()->setTextBackgroundColor(chosen);
            }
            m_editor->setFocus();
        });
    }

    // Outlook's emoji button: KDE's picker, the emoji goes in where the cursor is
    auto *emoji = new TextEmoticonsWidgets::EmoticonTextEditAction(this);
    emoji->setText(i18nc("@action", "Insert Emoji"));
    emoji->setIcon(QIcon::fromTheme(QStringLiteral("smiley-add")));
    actions->addAction(QStringLiteral("insert_emoji"), emoji);
    connect(emoji, &TextEmoticonsWidgets::EmoticonTextEditAction::insertEmoticon, this, [this](const QString &text) {
        m_editor->insertPlainText(text);
        m_editor->setFocus();
    });

    // Undo and Redo of the text, Print Draft (Outlook on the web's Format text and Options)
    QAction *undo = KStandardAction::undo(this, [this] {
        undoText(true);
    }, actions);
    QAction *redo = KStandardAction::redo(this, [this] {
        undoText(false);
    }, actions);
    undo->setEnabled(false);
    redo->setEnabled(false);
    connect(m_editor, &QTextEdit::undoAvailable, undo, &QAction::setEnabled);
    connect(m_editor, &QTextEdit::redoAvailable, redo, &QAction::setEnabled);
    KStandardAction::cut(m_editor, &QTextEdit::cut, actions);
    KStandardAction::copy(m_editor, &QTextEdit::copy, actions);
    KStandardAction::paste(m_editor, &QTextEdit::paste, actions);
    QAction *printDraft = KStandardAction::print(this, [this] {
        QPrinter printer;
        QPrintDialog dialog(&printer, this);
        dialog.setWindowTitle(i18nc("@title:window", "Print Draft"));
        if (dialog.exec() == QDialog::Accepted) {
            m_editor->document()->print(&printer);
        }
    }, actions);
    printDraft->setText(i18nc("@action", "Print Draft..."));

    KStandardAction::close(this, &QWidget::close, actions);
}

void ComposerWindow::setupFrom()
{
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        if (!box.mail) {
            continue;
        }
        m_from->addItem(QIcon(Avatar::pixmap(m_mailboxes->photo(box.key()), 16, box.displayName, box.address)),
                        box.displayName.isEmpty() || box.own ? box.address : box.displayName + QStringLiteral(" <") + box.address + QLatin1Char('>'), box.key());
    }
    // Addresses used before through "Other Address...", then the entry to add one
    const QJsonArray others = QJsonDocument::fromJson(composerGroup().readEntry("OtherFrom", QString()).toUtf8()).array();
    for (const QJsonValue &v : others) {
        const QJsonObject o = v.toObject();
        if (m_mailboxes->mailbox(o.value(QLatin1String("account")).toString() + QStringLiteral("/me"))) {
            addOtherFrom(o.value(QLatin1String("account")).toString(), o.value(QLatin1String("address")).toString(), o.value(QLatin1String("name")).toString(), false);
        }
    }
    m_from->addItem(QIcon::fromTheme(QStringLiteral("list-add-user")), i18nc("@item:inlistbox", "Other Address..."), QStringLiteral("other-new"));
    m_from->setCurrentIndex(std::max(0, m_from->findData(m_mailboxKey)));
    m_fromBefore = m_from->currentIndex();
}

QString ComposerWindow::fromKey() const
{
    const QString data = m_from->currentData().toString();
    if (data.startsWith(QLatin1String("other|"))) {
        return data.section(QLatin1Char('|'), 1, 1) + QStringLiteral("/me");
    }
    return data == QLatin1String("other-new") ? m_from->itemData(m_fromBefore).toString() : data;
}

QString ComposerWindow::otherFrom() const
{
    const QString data = m_from->currentData().toString();
    return data.startsWith(QLatin1String("other|")) ? data : QString();
}

void ComposerWindow::addOtherFrom(const QString &account, const QString &address, const QString &name, bool remember)
{
    const QString data = QStringLiteral("other|%1|%2|%3").arg(account, address, name);
    int index = m_from->findData(data);
    if (index < 0) {
        // Before "Other Address...", when it is there already
        const int add = m_from->findData(QStringLiteral("other-new"));
        index = add < 0 ? m_from->count() : add;
        m_from->insertItem(index, name.isEmpty() ? address : name + QStringLiteral(" <") + address + QLatin1Char('>'), data);
    }
    m_from->setCurrentIndex(index);
    if (remember) {
        QJsonArray others = QJsonDocument::fromJson(composerGroup().readEntry("OtherFrom", QString()).toUtf8()).array();
        for (qsizetype i = others.size() - 1; i >= 0; --i) {
            if (others.at(i).toObject().value(QLatin1String("address")).toString().compare(address, Qt::CaseInsensitive) == 0) {
                others.removeAt(i);
            }
        }
        others.prepend(QJsonObject{{QStringLiteral("account"), account}, {QStringLiteral("address"), address}, {QStringLiteral("name"), name}});
        while (others.size() > 10) {
            others.removeLast();
        }
        KConfigGroup group = composerGroup();
        group.writeEntry("OtherFrom", QString::fromUtf8(QJsonDocument(others).toJson(QJsonDocument::Compact)));
        group.sync();
    }
}

void ComposerWindow::askOtherFrom()
{
    QDialog dialog(this);
    dialog.setWindowTitle(i18nc("@title:window", "Other From Address"));
    auto *address = new RecipientEdit(m_book, &dialog);
    address->setPlaceholderText(i18nc("@info:placeholder", "Address to send from"));
    auto *note = new QLabel(i18nc("@info",
                                  "Microsoft 365 sends as this address if you have Send As or Send on Behalf permission for it. "
                                  "The sent copy stays in your own Sent Items."),
                            &dialog);
    note->setWordWrap(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(address);
    layout->addWidget(note);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(460, 150);
    const QList<OutgoingRecipient> people = dialog.exec() == QDialog::Accepted ? address->recipients() : QList<OutgoingRecipient>();
    if (people.isEmpty()) {
        m_from->setCurrentIndex(m_fromBefore);
        return;
    }
    addOtherFrom(fromKey().section(QLatin1Char('/'), 0, 0), people.first().address, people.first().name, true);
    fromChanged();
}

void ComposerWindow::setOtherFromForTests(const QString &address, const QString &name)
{
    addOtherFrom(fromKey().section(QLatin1Char('/'), 0, 0), address, name, false);
    fromChanged();
}

void ComposerWindow::addOriginalAttachments(const MessageRow &original)
{
    // The original's attachments come along; the user may remove them
    const QList<AttachmentRow> rows = m_store ? m_store->attachments(original.id) : QList<AttachmentRow>();
    for (const AttachmentRow &row : rows) {
        if (row.isInline) {
            continue;
        }
        Item item;
        item.attachment.key = QStringLiteral("original:") + row.name;
        item.attachment.name = row.name;
        item.attachment.size = row.size;
        item.fromOriginal = true;
        m_items.append(item);
    }
    refreshAttachmentList();
}

void ComposerWindow::startFrom(const MessageRow *original)
{
    m_record.kind = m_request.kind;
    if (original && m_request.kind == QLatin1String("editAsNew")) {
        startAsNew(*original);
        fromChanged();
        return;
    }
    // How the original comes along (Settings > Reply and Forward)
    const bool forward = m_request.kind == QLatin1String("forward");
    const ReplyOptions::Mode mode = forward ? ReplyOptions::forForwards() : ReplyOptions::forReplies();
    if (original && (m_request.kind == QLatin1String("forwardAsAttachment") || (forward && mode == ReplyOptions::Mode::Attach))) {
        // A new message with the original attached as a mail file (.eml), as Outlook's Forward
        // as Attachment
        m_record.kind = QStringLiteral("new");
        m_subject->setText(Quote::subject(QStringLiteral("forward"), original->subject));
        attachOriginal(*original);
    }
    const bool answer = original && m_record.kind != QLatin1String("new")
        && (m_request.kind == QLatin1String("reply") || m_request.kind == QLatin1String("replyAll") || forward);
    if (answer) {
        m_record.source = original->graphId;
        m_subject->setText(Quote::subject(m_request.kind, original->subject));
        if (m_request.kind != QLatin1String("forward")) {
            QStringList own{m_mailboxes->mailbox(m_mailboxKey).value_or(MailboxInfo()).address};
            const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
            for (const MailboxInfo &box : boxes) {
                if (box.own) {
                    own.append(box.address);
                }
            }
            QList<OutgoingRecipient> to;
            QList<OutgoingRecipient> cc;
            Quote::recipients(*original, m_request.kind == QLatin1String("replyAll"), own, &to, &cc);
            m_to->setRecipients(to);
            m_cc->setRecipients(cc);
        } else {
            addOriginalAttachments(*original);
        }
        if (mode == ReplyOptions::Mode::Attach) {
            attachOriginal(*original);
        } else if (mode != ReplyOptions::Mode::None) {
            m_quotePrefixed = mode == ReplyOptions::Mode::Prefix;
            m_quoteSource = original->id;
            buildQuote();
            showQuote();
        }
    } else if (!m_request.to.isEmpty()) {
        m_to->setRecipients(m_request.to);
    } else if (original && m_request.kind == QLatin1String("draft")) {
        m_subject->setText(original->subject);
        m_to->setRecipients(parseRecipients(original->toList));
        m_cc->setRecipients(parseRecipients(original->ccList));
    }

    // The signature of the From mailbox, for new messages or for answers
    const bool reply = m_request.kind != QLatin1String("new");
    if (m_request.kind != QLatin1String("draft")) {
        // A line to write on and an empty one before the signature, as Outlook starts
        QTextCursor cursor(m_editor->document());
        cursor.insertBlock();
        cursor.insertBlock();
        const QString id = Signatures::defaultFor(fromKey().section(QLatin1Char('/'), 0, 0), fromKey().section(QLatin1Char('/'), 1), reply);
        if (!id.isEmpty()) {
            insertSignature(id);
        }
        m_editor->moveCursor(QTextCursor::Start);
    }
    fromChanged();
    // The prepared text (empty lines, signature, quote) is where undo stops, as in Outlook
    m_editor->document()->clearUndoRedoStacks();
}

void ComposerWindow::attachOriginal(const MessageRow &original)
{
    // The original as a mail file (.eml); the daemon keeps the file once the draft is saved
    const QString name = MimeExport::safeFileName(original.subject).left(80);
    QPointer<ComposerWindow> self(this);
    MimeExport::fetch(m_mailboxes, m_daemon, m_mailboxKey, original.id, this, [self, name](const QByteArray &mime, bool) {
        if (!self || mime.isEmpty()) {
            return;
        }
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/attached-") + QUuid::createUuid().toString(QUuid::Id128);
        QDir().mkpath(dir);
        QFile file(dir + QLatin1Char('/') + name + QStringLiteral(".eml"));
        if (file.open(QIODevice::WriteOnly) && file.write(mime) == mime.size()) {
            file.close();
            self->addFiles({file.fileName()});
        }
    });
}

void ComposerWindow::startAsNew(const MessageRow &original)
{
    // Edit as New Message (Outlook's Resend): the original's subject, people, text and attachments
    // in a new message. Sent as a forward of the original, so Exchange copies its attachments;
    // the whole content is then replaced by what is written here
    m_record.kind = QStringLiteral("forward");
    m_record.source = original.graphId;
    m_subject->setText(original.subject);
    m_to->setRecipients(parseRecipients(original.toList));
    m_cc->setRecipients(parseRecipients(original.ccList));
    addOriginalAttachments(original);
    m_asNewSource = original.id;
    fillAsNew();
}

void ComposerWindow::fillAsNew()
{
    const std::optional<MessageRow> original = m_store ? m_store->message(m_asNewSource) : std::nullopt;
    const QString html = original ? Quote::originalHtml(*original, m_blobs) : QString();
    if (html.isEmpty()) {
        // Not cached yet: the daemon fetches it and bodyReady comes back here
        m_daemon->fetchBody(m_mailboxKey.section(QLatin1Char('/'), 0, 0), m_mailboxKey.section(QLatin1Char('/'), 1), m_asNewSource);
        return;
    }
    m_editor->setHtml(html);
    m_editor->moveCursor(QTextCursor::Start);
    m_asNewSource = 0;
}

void ComposerWindow::buildQuote()
{
    if (!m_store || m_quoteSource == 0) {
        return;
    }
    const std::optional<MessageRow> original = m_store->message(m_quoteSource);
    if (!original) {
        return;
    }
    const QString html = Quote::originalHtml(*original, m_blobs);
    if (html.isEmpty()) {
        // Not cached (older than the body window): the daemon fetches it, bodyReady rebuilds
        m_daemon->fetchBody(m_mailboxKey.section(QLatin1Char('/'), 0, 0), m_mailboxKey.section(QLatin1Char('/'), 1), m_quoteSource);
    }
    m_quoteHtml = html.isEmpty() ? QString() : Quote::headerHtml(*original) + html;
    m_quoteText = html.isEmpty() ? QString() : Quote::headerText(*original) + Quote::originalText(*original, m_blobs);
    if (m_quotePrefixed && !html.isEmpty()) {
        // Prefix each line: "> " in text, a quote bar in HTML (as mail programs show it)
        m_quoteHtml = Quote::headerHtml(*original) + QStringLiteral("<blockquote style=\"margin:0 0 0 4px;padding-left:8px;border-left:2px solid #a0a0a0\">") + html
            + QStringLiteral("</blockquote>");
        m_quoteText = ReplyOptions::prefixLines(m_quoteText);
    }
}

void ComposerWindow::showQuote()
{
    if (m_quoteSource == 0 || !m_store) {
        return;
    }
    const std::optional<MessageRow> original = m_store->message(m_quoteSource);
    if (!original) {
        return;
    }
    m_quoteHeader->setText(Quote::headerHtml(*original));
    if (m_quotePrefixed) {
        // As it is sent: every line prefixed
        m_quoteView->showText(ReplyOptions::prefixLines(Quote::originalText(*original, m_blobs)));
    } else {
        m_quoteView->showBody(m_store, m_blobs, m_quoteSource, false);
    }
    m_quoteBox->setVisible(true);
}

void ComposerWindow::restore(const Outgoing &record)
{
    m_record = record;
    m_saved = true;
    m_mailboxKey = m_request.mailboxKey;
    const QJsonObject ui = record.ui;
    m_subject->setText(record.subject);
    m_to->setRecipients(record.to);
    m_cc->setRecipients(record.cc);
    m_bcc->setRecipients(record.bcc);
    m_showBcc->setChecked(!record.bcc.isEmpty());
    m_replyTo->setRecipients(record.replyTo);
    m_showReplyTo->setChecked(!record.replyTo.isEmpty());
    m_high->setChecked(record.importance == 2);
    m_low->setChecked(record.importance == 0);
    m_readReceipt->setChecked(record.readReceipt);
    m_deliveryReceipt->setChecked(record.deliveryReceipt);
    m_sensitivity->setCurrentItem(qBound(0, record.sensitivity, 3));
    m_deferUntil = record.deferUntil;
    showDelay();
    m_dropped = record.dropSourceAttachments;
    m_quoteHtml = ui.value(QLatin1String("quoteHtml")).toString();
    m_quoteText = ui.value(QLatin1String("quoteText")).toString();
    m_quoteSource = ui.value(QLatin1String("quoteSource")).toInteger();
    m_quotePrefixed = ui.value(QLatin1String("quotePrefixed")).toBool();
    m_signatureId = ui.value(QLatin1String("signature")).toString();
    // Pictures in the text: back into the editor from the blob store
    const QJsonObject images = ui.value(QLatin1String("images")).toObject();
    for (auto it = images.constBegin(); it != images.constEnd(); ++it) {
        m_imageCids.insert(it.key(), it.value().toString());
    }
    if (record.html) {
        m_editor->setHtml(ui.value(QLatin1String("editor")).toString(record.body));
        for (const OutgoingAttachment &a : record.attachments) {
            const QString name = m_imageCids.key(a.contentId);
            if (a.isInline && !name.isEmpty() && m_blobs) {
                QString error;
                if (const std::optional<QByteArray> data = m_blobs->get(a.blob, &error)) {
                    m_editor->composerControler()->composerImages()->loadImage(QImage::fromData(*data), name, name);
                }
            }
        }
    } else {
        m_plainText->setChecked(true);
        m_editor->setPlainText(ui.value(QLatin1String("editor")).toString(record.body));
    }
    for (const OutgoingAttachment &a : record.attachments) {
        if (a.removed || a.isInline) {
            continue;
        }
        m_items.append({a, false});
        m_given.insert(a.key);
    }
    for (const OutgoingAttachment &a : record.attachments) {
        if (a.isInline) {
            m_given.insert(a.key);
        }
    }
    // OneDrive/SharePoint links of the draft (uploads still running are followed again)
    restoreCloud(ui.value(QLatin1String("cloudFiles")).toArray());
    refreshAttachmentList();
    showQuote();
    const QString other = ui.value(QLatin1String("otherFrom")).toString();
    if (!other.isEmpty()) {
        addOtherFrom(other.section(QLatin1Char('|'), 1, 1), other.section(QLatin1Char('|'), 2, 2), other.section(QLatin1Char('|'), 3), false);
        m_fromBefore = m_from->currentIndex();
    }
    const int from = other.isEmpty() ? m_from->findData(ui.value(QLatin1String("fromKey")).toString()) : -1;
    if (from >= 0) {
        m_from->setCurrentIndex(from);
    }
    if (record.state == Outgoing::State::Failed && !record.lastError.isEmpty()) {
        m_notice->setMessageType(KMessageWidget::Error);
        m_notice->setText(record.lastError.contains(QLatin1String("ErrorSendAsDenied"))
                              ? accessProblemText(QStringLiteral("sendAsDenied"), record.fromAddress)
                              : i18nc("@info", "This message was not sent: %1", record.lastError));
        m_notice->show();
    }
    fromChanged();
}

void ComposerWindow::restoreFromServer(const QString &json, const MessageRow &draft)
{
    m_editor->setEnabled(true);
    const QJsonObject m = QJsonDocument::fromJson(json.toUtf8()).object();
    if (m.isEmpty()) {
        // Offline: what the cache has
        QString error;
        const std::optional<QByteArray> body = (m_blobs && !draft.bodyBlob.isEmpty()) ? m_blobs->get(draft.bodyBlob, &error) : std::nullopt;
        if (body) {
            if (draft.bodyType == 1) {
                m_editor->setHtml(QString::fromUtf8(*body));
            } else {
                m_editor->setPlainText(QString::fromUtf8(*body));
            }
        }
        m_dirty = false;
        return;
    }
    auto list = [](const QJsonValue &value) {
        QList<OutgoingRecipient> result;
        const QJsonArray array = value.toArray();
        for (const QJsonValue &v : array) {
            const QJsonObject a = v.toObject().value(QLatin1String("emailAddress")).toObject();
            result.append({a.value(QLatin1String("name")).toString(), a.value(QLatin1String("address")).toString()});
        }
        return result;
    };
    m_subject->setText(m.value(QLatin1String("subject")).toString());
    m_to->setRecipients(list(m.value(QLatin1String("toRecipients"))));
    m_cc->setRecipients(list(m.value(QLatin1String("ccRecipients"))));
    m_bcc->setRecipients(list(m.value(QLatin1String("bccRecipients"))));
    m_showBcc->setChecked(!m.value(QLatin1String("bccRecipients")).toArray().isEmpty());
    m_replyTo->setRecipients(list(m.value(QLatin1String("replyTo"))));
    m_showReplyTo->setChecked(!m.value(QLatin1String("replyTo")).toArray().isEmpty());
    const QString importance = m.value(QLatin1String("importance")).toString();
    m_high->setChecked(importance == QLatin1String("high"));
    m_low->setChecked(importance == QLatin1String("low"));
    m_readReceipt->setChecked(m.value(QLatin1String("isReadReceiptRequested")).toBool());
    m_deliveryReceipt->setChecked(m.value(QLatin1String("isDeliveryReceiptRequested")).toBool());
    m_record.internetMessageId = m.value(QLatin1String("internetMessageId")).toString();
    const QJsonObject body = m.value(QLatin1String("body")).toObject();
    if (body.value(QLatin1String("contentType")).toString().compare(QLatin1String("html"), Qt::CaseInsensitive) == 0) {
        m_editor->setHtml(body.value(QLatin1String("content")).toString());
    } else {
        m_plainText->setChecked(true);
        m_editor->setPlainText(body.value(QLatin1String("content")).toString());
    }
    // Attachments already on the draft: listed, removable, never uploaded again
    const QJsonArray attachments = m.value(QLatin1String("attachments")).toArray();
    for (const QJsonValue &v : attachments) {
        const QJsonObject a = v.toObject();
        if (a.value(QLatin1String("isInline")).toBool()) {
            continue;
        }
        Item item;
        item.attachment.key = QStringLiteral("server:") + a.value(QLatin1String("id")).toString();
        item.attachment.graphId = a.value(QLatin1String("id")).toString();
        item.attachment.name = a.value(QLatin1String("name")).toString();
        item.attachment.contentType = a.value(QLatin1String("contentType")).toString();
        item.attachment.size = a.value(QLatin1String("size")).toInteger();
        m_items.append(item);
        m_given.insert(item.attachment.key);
    }
    refreshAttachmentList();
    m_dirty = false;
}

void ComposerWindow::fromChanged()
{
    if (m_from->currentData().toString() != QLatin1String("other-new")) {
        m_fromBefore = m_from->currentIndex();
    }
    const QString key = fromKey();
    m_spell->setMailbox(key);
    // A new message lives in the mailbox it is sent from: move it when From changes
    if (m_record.kind == QLatin1String("new") && key != m_mailboxKey && !key.isEmpty()) {
        if (m_saved) {
            m_daemon->discardOutgoing(m_mailboxKey.section(QLatin1Char('/'), 0, 0), m_mailboxKey.section(QLatin1Char('/'), 1), m_record.uuid);
            m_record.uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_record.draftId.clear();
            m_saved = false;
            m_given.clear();
            for (Item &item : m_items) {
                item.attachment.graphId.clear();
            }
            setDirty();
        }
        // The other mailbox's default signature, as Outlook switches it
        const QString signature = Signatures::defaultFor(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), false);
        if (signature != m_signatureId) {
            insertSignature(signature);
        }
        m_mailboxKey = key;
        m_store = m_mailboxes->store(m_mailboxKey);
        m_blobs = m_mailboxes->blobs(m_mailboxKey);
    }
    updateSendMenu();
}

QString ComposerWindow::learnedRight(QString *date) const
{
    const KConfigGroup group = mailboxGroup(fromKey());
    if (date) {
        const QDateTime when = group.readEntry("SendRightDate", QDateTime());
        // The C locale's short format ("11 11 2026") reads badly: the message list's style then
        const QString format = QLocale().name() == QLatin1String("C") ? QStringLiteral("dd-MM-yyyy HH:mm") : QLocale().dateTimeFormat(QLocale::ShortFormat);
        *date = when.isValid() ? QLocale().toString(when.toLocalTime(), format) : QString();
    }
    return group.readEntry("SendRight");
}

void ComposerWindow::updateSendMenu()
{
    QMenu *menu = m_sendMenu;
    menu->clear();
    const QList<QAction *> modes = m_sendModes->actions();
    for (QAction *a : modes) {
        m_sendModes->removeAction(a);
        a->deleteLater();
    }
    const std::optional<MailboxInfo> from = m_mailboxes->mailbox(fromKey());
    if (!from || from->own) {
        // The user's own mailbox: just Send, without an arrow
        m_send->setMenu(nullptr);
        m_send->setPopupMode(QToolButton::DelayedPopup);
        m_send->setToolTip(i18nc("@info:tooltip", "Send this message"));
        m_notice->hide();
        return;
    }
    m_send->setMenu(m_sendMenu);
    m_send->setPopupMode(QToolButton::MenuButtonPopup);
    QString date;
    const QString learned = learnedRight(&date);
    const QString chosen = mailboxGroup(fromKey()).readEntry("SendDefault", learned == QLatin1String("behalf") ? QStringLiteral("behalf") : QStringLiteral("as"));

    QAction *as = menu->addAction(i18nc("@action:inmenu %1 a mailbox address", "Send As %1", from->address));
    as->setData(QStringLiteral("as"));
    QAction *behalf = menu->addAction(i18nc("@action:inmenu %1 a mailbox address", "Send on Behalf of %1", from->address));
    behalf->setData(QStringLiteral("behalf"));
    for (QAction *a : {as, behalf}) {
        a->setCheckable(true);
        m_sendModes->addAction(a);
        connect(a, &QAction::triggered, this, [this] {
            setDirty();
        });
    }
    // What Exchange did before decides what can happen: it sends as the mailbox whenever the
    // user has Send As, and refuses without either right
    // The reason is in the item itself: a tooltip on a menu item is easily missed
    if (learned == QLatin1String("as")) {
        behalf->setEnabled(false);
        behalf->setText(i18nc("@action:inmenu %1 a mailbox address", "Send on Behalf of %1 (not possible: you have Send As)", from->address));
        behalf->setToolTip(i18nc("@info:tooltip", "You have Send As on this mailbox, so Exchange sends as the mailbox (learned %1)", date));
    } else if (learned == QLatin1String("behalf")) {
        as->setEnabled(false);
        as->setText(i18nc("@action:inmenu %1 a mailbox address", "Send As %1 (not possible: you only have Send on Behalf)", from->address));
        as->setToolTip(i18nc("@info:tooltip", "You only have Send on Behalf on this mailbox (learned %1)", date));
    } else if (learned == QLatin1String("denied")) {
        as->setEnabled(false);
        behalf->setEnabled(false);
        as->setText(i18nc("@action:inmenu %1 a mailbox address, %2 a date", "Send As %1 (refused by Exchange on %2)", from->address, date));
        behalf->setText(i18nc("@action:inmenu %1 a mailbox address, %2 a date", "Send on Behalf of %1 (refused by Exchange on %2)", from->address, date));
    }
    QAction *pick = chosen == QLatin1String("behalf") && behalf->isEnabled() ? behalf : (as->isEnabled() ? as : behalf);
    pick->setChecked(true);
    menu->setToolTipsVisible(true);
    menu->addSeparator();
    menu->addAction(i18nc("@action:inmenu", "Make This the Default"), this, [this] {
        if (QAction *checked = m_sendModes->checkedAction()) {
            KConfigGroup group = mailboxGroup(fromKey());
            group.writeEntry("SendDefault", checked->data().toString());
            group.sync();
        }
    });
    if (!learned.isEmpty()) {
        menu->addAction(i18nc("@action:inmenu", "Forget What Exchange Allowed"), this, [this] {
            KConfigGroup group = mailboxGroup(fromKey());
            group.deleteEntry("SendRight");
            group.deleteEntry("SendRightDate");
            group.sync();
            updateSendMenu();
        });
    }
    m_send->setToolTip(i18nc("@info:tooltip", "Send this message from %1", from->address));
    if (learned == QLatin1String("denied")) {
        m_notice->setMessageType(KMessageWidget::Warning);
        m_notice->setText(i18nc("@info", "Exchange refused mail from %1 on %2: you have neither Send As nor Send on Behalf there. Choose another From address, or use \"Forget What Exchange Allowed\" on the Send arrow to try again.",
                                from->address,
                                date));
        m_notice->show();
    } else {
        m_notice->hide();
    }
}

void ComposerWindow::insertSignature(const QString &id)
{
    const std::optional<Signature> signature = Signatures::find(id);
    QTextCursor cursor(m_editor->document());
    // The old signature's place, if it still belongs to this text: the editor can rebuild its
    // document (plain text and back), and a stale range would cut the wrong text
    const int length = m_editor->document()->characterCount();
    const bool known = !m_signatureStart.isNull() && !m_signatureEnd.isNull() && m_signatureStart.document() == m_editor->document()
        && m_signatureEnd.document() == m_editor->document() && m_signatureEnd.position() > m_signatureStart.position() && m_signatureEnd.position() < length;
    if (known) {
        // Swap the one there
        cursor.setPosition(m_signatureStart.position());
        cursor.setPosition(m_signatureEnd.position(), QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
    } else {
        cursor.movePosition(QTextCursor::End);
        if (!cursor.block().text().isEmpty()) {
            cursor.insertBlock();
        }
    }
    m_signatureId = signature ? id : QString();
    if (!signature) {
        m_signatureStart = QTextCursor();
        m_signatureEnd = QTextCursor();
        return;
    }
    const int start = cursor.position();
    cursor.insertHtml(signature->html);
    for (const Signature::Image &image : signature->images) {
        m_editor->composerControler()->composerImages()->loadImage(QImage::fromData(image.png), image.name, image.name);
    }
    m_signatureStart = QTextCursor(m_editor->document());
    m_signatureStart.setPosition(start);
    m_signatureEnd = QTextCursor(m_editor->document());
    m_signatureEnd.setPosition(cursor.position());
}

void ComposerWindow::addFiles(const QStringList &paths)
{
    QMimeDatabase mimes;
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (!info.isFile() || !info.isReadable()) {
            continue;
        }
        // Over 25 MB: into OneDrive's Attachments folder and sent as a link, as Outlook does (only
        // with Graph Sync and a OneDrive of this account; otherwise attached as it is)
        if (info.size() > largeAttachmentBytes && putInOneDrive(info.absoluteFilePath())) {
            continue;
        }
        Item item;
        item.attachment.key = QUuid::createUuid().toString(QUuid::WithoutBraces);
        item.attachment.name = info.fileName();
        item.attachment.path = info.absoluteFilePath();
        item.attachment.size = info.size();
        item.attachment.contentType = mimes.mimeTypeForFile(info).name();
        m_items.append(item);
    }
    refreshAttachmentList();
    setDirty();
}

void ComposerWindow::removeSelectedAttachments()
{
    const QList<QListWidgetItem *> selected = m_attachmentList->selectedItems();
    QStringList keys;
    for (QListWidgetItem *item : selected) {
        keys.append(item->data(Qt::UserRole).toString());
    }
    // OneDrive links: off the list (a link already in the text stays; the user removes it there)
    m_cloud.removeIf([&keys](const CloudFile &file) {
        return keys.contains(QStringLiteral("cloud:") + file.path);
    });
    m_items.removeIf([this, &keys](const Item &item) {
        if (!keys.contains(item.attachment.key)) {
            return false;
        }
        if (item.fromOriginal) {
            m_dropped.append(item.attachment.name);
        }
        return true;
    });
    refreshAttachmentList();
    setDirty();
}

void ComposerWindow::refreshAttachmentList()
{
    m_attachmentList->clear();
    QMimeDatabase mimes;
    for (const Item &item : std::as_const(m_items)) {
        if (item.fromOriginal && m_dropped.contains(item.attachment.name)) {
            continue;
        }
        const QMimeType type = mimes.mimeTypeForFile(item.attachment.name, QMimeDatabase::MatchExtension);
        auto *row = new QListWidgetItem(QIcon::fromTheme(type.iconName(), QIcon::fromTheme(QStringLiteral("unknown"))),
                                        i18nc("@item attachment name, size", "%1 (%2)", item.attachment.name, QLocale().formattedDataSize(item.attachment.size)));
        row->setData(Qt::UserRole, item.attachment.key);
        m_attachmentList->addItem(row);
    }
    for (const CloudFile &file : std::as_const(m_cloud)) {
        const QString state = !file.error.isEmpty() ? i18nc("@item a OneDrive link that failed", "no link: %1", file.error)
            : file.uploading                        ? i18nc("@item", "uploading to OneDrive...")
                                                    : i18nc("@item", "OneDrive link");
        auto *row = new QListWidgetItem(QIcon::fromTheme(file.error.isEmpty() ? QStringLiteral("folder-cloud") : QStringLiteral("dialog-warning")),
                                        i18nc("@item attachment name, state", "%1 (%2)", file.name, state));
        row->setToolTip(file.url.isEmpty() ? file.path : file.url);
        row->setData(Qt::UserRole, QString(QStringLiteral("cloud:") + file.path));
        m_attachmentList->addItem(row);
    }
    m_attachmentList->setVisible(m_attachmentList->count() > 0);
}

void ComposerWindow::setDirty()
{
    m_dirty = true;
}

void ComposerWindow::updateTitle()
{
    const QString subject = m_subject->text().trimmed();
    setWindowTitle(subject.isEmpty() ? i18nc("@title:window a message without a subject yet", "New Message") : subject);
}

Outgoing ComposerWindow::record(bool send)
{
    Outgoing o = m_record;
    o.send = send;
    o.to = m_to->recipients();
    o.cc = m_cc->recipients();
    o.bcc = m_bcc->recipients();
    o.replyTo = m_replyTo->recipients();
    o.mentions = m_plainText->isChecked() ? QList<OutgoingRecipient>() : mentionsInText();
    o.subject = m_subject->text();
    o.importance = m_high->isChecked() ? 2 : m_low->isChecked() ? 0 : 1;
    o.html = !m_plainText->isChecked();
    o.dropSourceAttachments = m_dropped;
    o.readReceipt = m_readReceipt->isChecked();
    o.deliveryReceipt = m_deliveryReceipt->isChecked();
    o.sensitivity = std::max(0, m_sensitivity->currentItem());
    o.deferUntil = m_deferUntil;
    const int undo = send ? undoSendSeconds() : 0;
    o.sendAfter = undo > 0 ? QDateTime::currentMSecsSinceEpoch() + undo * 1000LL : 0;

    const std::optional<MailboxInfo> from = m_mailboxes->mailbox(fromKey());
    const bool otherMailbox = from && (fromKey() != QStringLiteral("%1/me").arg(from->account));
    o.fromAddress = otherMailbox || fromKey() != m_mailboxKey ? from->address : QString();
    o.fromName = otherMailbox ? from->displayName : QString();
    o.fromMailbox = otherMailbox ? from->mailbox : QString();
    if (const QString other = otherFrom(); !other.isEmpty()) {
        // Through the own mailbox with "from" set: Exchange decides by the rights (no learning,
        // there is no mailbox of it to read the sent copy from)
        o.fromAddress = other.section(QLatin1Char('|'), 2, 2);
        o.fromName = other.section(QLatin1Char('|'), 3);
        o.fromMailbox.clear();
    }
    o.sendMode = m_sendModes->checkedAction() ? m_sendModes->checkedAction()->data().toString() : QString();

    o.attachments.clear();
    for (const Item &item : std::as_const(m_items)) {
        if (item.fromOriginal) {
            continue;
        }
        OutgoingAttachment a = item.attachment;
        // Handed over once; later saves name it by its key only
        if (m_given.contains(a.key)) {
            a.path.clear();
        }
        o.attachments.append(a);
    }

    QJsonObject ui{{QStringLiteral("quoteHtml"), m_quoteHtml},
                   {QStringLiteral("quoteText"), m_quoteText},
                   {QStringLiteral("quoteSource"), m_quoteSource},
                   {QStringLiteral("quotePrefixed"), m_quotePrefixed},
                   {QStringLiteral("signature"), m_signatureId},
                   {QStringLiteral("fromKey"), fromKey()},
                   {QStringLiteral("otherFrom"), otherFrom()}};
    if (o.html) {
        QString text = bodyOf(m_editor->composerControler()->toCleanHtml());
        // Pictures in the text go along as inline attachments
        const auto images = m_editor->composerControler()->composerImages()->embeddedImages();
        QJsonObject imageMap;
        for (const auto &image : images) {
            const QString reference = QStringLiteral("src=\"") + image->imageName + QLatin1Char('"');
            if (!text.contains(reference)) {
                continue;
            }
            QString cid = m_imageCids.value(image->imageName);
            if (cid.isEmpty()) {
                cid = QStringLiteral("img") + QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral("@graphmail");
                m_imageCids.insert(image->imageName, cid);
            }
            text.replace(reference, QStringLiteral("src=\"cid:") + cid + QLatin1Char('"'));
            imageMap.insert(image->imageName, cid);
            OutgoingAttachment a;
            a.key = QStringLiteral("img:") + cid;
            a.name = image->imageName.section(QLatin1Char('/'), -1) + (image->imageName.endsWith(QLatin1String(".png"), Qt::CaseInsensitive) ? QString() : QStringLiteral(".png"));
            a.contentType = QStringLiteral("image/png");
            a.isInline = true;
            a.contentId = cid;
            if (!m_given.contains(a.key)) {
                const QString dir = composeDirectory() + QLatin1Char('/') + o.uuid;
                QDir().mkpath(dir);
                QFile file(dir + QLatin1Char('/') + cid.section(QLatin1Char('@'), 0, 0) + QStringLiteral(".png"));
                if (file.open(QIODevice::WriteOnly)) {
                    file.write(QByteArray::fromBase64(image->image));
                    file.close();
                    a.path = file.fileName();
                }
            }
            o.attachments.append(a);
        }
        ui.insert(QStringLiteral("images"), imageMap);
        ui.insert(QStringLiteral("editor"), m_editor->toHtml());
        o.body = QStringLiteral("<html><head><meta charset=\"utf-8\"></head><body><div style=\"font-family:Aptos,Calibri,Carlito,sans-serif;font-size:11pt\">") + text
            + QStringLiteral("</div>") + m_quoteHtml + QStringLiteral("</body></html>");
    } else {
        const QString text = m_editor->toPlainText();
        ui.insert(QStringLiteral("editor"), text);
        o.body = m_quoteText.isEmpty() ? text : text + QStringLiteral("\n\n") + m_quoteText;
    }
    ui.insert(QStringLiteral("cloudFiles"), cloudJson());
    o.ui = ui;
    return o;
}

void ComposerWindow::checkNames(const std::function<void(bool)> &done)
{
    struct Part {
        QPointer<RecipientEdit> edit;
        QString text;
    };
    auto parts = std::make_shared<QList<Part>>();
    for (RecipientEdit *edit : {m_to, m_cc, m_bcc, m_replyTo}) {
        QStringList invalid;
        edit->recipients(&invalid);
        for (const QString &text : std::as_const(invalid)) {
            parts->append({edit, text});
        }
    }
    auto unresolved = std::make_shared<QStringList>();
    auto next = std::make_shared<std::function<void()>>();
    // One hit is taken, several are offered (in a test run the first), none is reported
    const auto take = [this, unresolved](const Part &part, const QStringList &hits) {
        QString chosen = hits.value(0);
        if (hits.size() > 1 && !m_testMode) {
            bool ok = false;
            chosen = QInputDialog::getItem(this, i18nc("@title:window", "Check Names"), i18nc("@label:listbox", "More than one person matches \"%1\":", part.text), hits, 0, false, &ok);
            if (!ok) {
                chosen.clear();
            }
        }
        const QList<OutgoingRecipient> parsed = parseRecipients(chosen);
        if (parsed.isEmpty() || !part.edit) {
            unresolved->append(part.text);
            return;
        }
        part.edit->replacePart(part.text, parsed.first());
    };
    *next = [this, parts, unresolved, next, take, done] {
        if (parts->isEmpty()) {
            if (!unresolved->isEmpty()) {
                m_notice->setMessageType(KMessageWidget::Error);
                m_notice->setText(i18nc("@info", "No one was found for: %1", unresolved->join(QStringLiteral(", "))));
                m_notice->animatedShow();
            } else {
                m_notice->animatedHide();
            }
            if (done) {
                done(unresolved->isEmpty());
            }
            *next = nullptr;
            return;
        }
        const Part part = parts->takeFirst();
        if (const QStringList local = AddressBook::matches(m_book->model()->stringList(), part.text); !local.isEmpty()) {
            take(part, local);
            (*next)();
            return;
        }
        // Not among the known people: the organisation's directory
        const QString account = m_mailboxKey.section(QLatin1Char('/'), 0, 0);
        m_daemon->query(account, QStringLiteral("me"), QStringLiteral("directory"),
                        QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("text"), part.text}}).toJson(QJsonDocument::Compact)), this,
                        [part, next, take](const QString &json) {
                            QStringList hits;
                            const QJsonArray value = QJsonDocument::fromJson(json.toUtf8()).object().value(QLatin1String("value")).toArray();
                            for (const QJsonValue &v : value) {
                                const QJsonObject o = v.toObject();
                                const QString address = o.value(QLatin1String("mail")).toString();
                                if (!address.isEmpty()) {
                                    hits.append(formatRecipient({o.value(QLatin1String("displayName")).toString(), address}));
                                }
                            }
                            take(part, hits);
                            (*next)();
                        });
    };
    (*next)();
}

QString ComposerWindow::recipientsForTests() const
{
    return m_to->text();
}

QStringList ComposerWindow::typeBodyForTests(const QString &text)
{
    m_editor->setFocus();
    for (const QChar c : text) {
        const int key = c == QLatin1Char(' ') ? Qt::Key_Space : c == QLatin1Char('\n') ? Qt::Key_Return : 0;
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, key == Qt::Key_Return ? QString() : QString(c));
        QCoreApplication::sendEvent(m_editor, &press);
    }
    QStringList links;
    for (QTextBlock block = m_editor->document()->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            if (it.fragment().charFormat().isAnchor()) {
                links.append(it.fragment().text() + QStringLiteral(" -> ") + it.fragment().charFormat().anchorHref());
            }
        }
    }
    return links;
}

QStringList ComposerWindow::keysForTests(const QString &script)
{
    // | is Enter, < is Shift+Left, # colours the selection red, ^ is the Font Color button, _ its
    // Automatic, % is Insert Table, } picks the @mention list's entry, ~ is Ctrl+Z (logs the text)
    m_editor->setFocus();
    QStringList log;
    const auto press = [this](int key, Qt::KeyboardModifiers modifiers, const QString &text) {
        QKeyEvent override(QEvent::ShortcutOverride, key, modifiers, text);
        QCoreApplication::sendEvent(m_editor, &override);
        QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
        QCoreApplication::sendEvent(m_editor, &event);
        QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
        QCoreApplication::sendEvent(m_editor, &release);
    };
    for (const QChar c : script) {
        if (c == QLatin1Char('|')) {
            press(Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
        } else if (c == QLatin1Char('<')) {
            press(Qt::Key_Left, Qt::ShiftModifier, QString());
        } else if (c == QLatin1Char('#')) {
            m_editor->composerControler()->setTextForegroundColor(Qt::red);
        } else if (c == QLatin1Char('^') || c == QLatin1Char('_')) {
            // ^ the Font Color button (its last colour), _ its "Automatic"; logs the selection's colour
            if (c == QLatin1Char('^')) {
                actionCollection()->action(QStringLiteral("text_color"))->trigger();
            } else {
                clearTextColor(false);
            }
            QTextCursor at = m_editor->textCursor();
            at.setPosition(std::min(at.selectionStart(), at.selectionEnd()) + 1);
            const QTextCharFormat format = at.charFormat();
            log.append(format.hasProperty(QTextFormat::ForegroundBrush) ? QStringLiteral("color %1").arg(format.foreground().color().name()) : QStringLiteral("color automatic"));
        } else if (c == QLatin1Char('}')) {
            // The @mention list's first entry, as Enter takes it
            const bool open = m_mention && m_mention->popup()->isVisible();
            if (open) {
                insertMention(m_mention->popup()->currentIndex().data().toString());
            }
            QStringList mentions;
            for (const OutgoingRecipient &r : mentionsInText()) {
                mentions.append(r.name + QStringLiteral(" <") + r.address + QLatin1Char('>'));
            }
            log.append(QStringLiteral("list %1; text [%2]; to [%3]; mentions [%4]")
                           .arg(open ? QStringLiteral("open") : QStringLiteral("closed"),
                                m_editor->toPlainText().section(QLatin1Char('\n'), 0, 0),
                                m_to->text(),
                                mentions.join(QStringLiteral(", "))));
        } else if (c == QLatin1Char('%')) {
            // Insert Table..., its dialog accepted as it opens
            QTimer::singleShot(300, this, [] {
                if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                    dialog->accept();
                }
            });
            actionCollection()->action(QStringLiteral("insert_table_dialog"))->trigger();
            const QList<QTextFrame *> frames = m_editor->document()->rootFrame()->childFrames();
            const auto found = std::find_if(frames.cbegin(), frames.cend(), [](QTextFrame *frame) {
                return qobject_cast<QTextTable *>(frame) != nullptr;
            });
            const QTextTable *table = found == frames.cend() ? nullptr : qobject_cast<QTextTable *>(*found);
            log.append(table ? QStringLiteral("table %1x%2, cursor in it: %3").arg(table->rows()).arg(table->columns()).arg(m_editor->textCursor().currentTable() == table)
                             : QStringLiteral("no table"));
        } else if (c == QLatin1Char('~')) {
            press(Qt::Key_Z, Qt::ControlModifier, QString());
            log.append(QStringLiteral("undo -> [%1] cursor %2")
                           .arg(m_editor->toPlainText().replace(QLatin1Char('\n'), QLatin1Char('|')))
                           .arg(m_editor->textCursor().position()));
        } else {
            press(c == QLatin1Char(' ') ? Qt::Key_Space : Qt::Key_A, Qt::NoModifier, QString(c));
        }
        QCoreApplication::processEvents();
    }
    return log;
}

void ComposerWindow::insertHtmlForTests(const QString &html)
{
    // As a paste does (MailEditor::insertFromMimeData)
    m_editor->textCursor().insertHtml(MailEditor::htmlForQt(html));
}

QStringList ComposerWindow::imagesForTests() const
{
    QStringList list;
    for (QTextBlock block = m_editor->document()->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextImageFormat format = it.fragment().charFormat().toImageFormat();
            if (format.isValid()) {
                const QImage image = m_editor->document()->resource(QTextDocument::ImageResource, QUrl(format.name())).value<QImage>();
                list.append(format.name().left(40) + (image.isNull() ? QStringLiteral(" (not held)") : QStringLiteral(" (held %1x%2)").arg(image.width()).arg(image.height())));
            }
        }
    }
    return list;
}

QStringList ComposerWindow::dropFilesForTests(const QStringList &paths)
{
    QStringList result{QStringLiteral("undo steps before: %1").arg(m_editor->document()->availableUndoSteps())};
    auto *data = new QMimeData;
    QList<QUrl> urls;
    for (const QString &path : paths) {
        urls.append(QUrl::fromLocalFile(path));
    }
    data->setUrls(urls);
    const QPointF at(20, 10);
    QDragEnterEvent enter(at.toPoint(), Qt::CopyAction, data, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(m_editor->viewport(), &enter);
    QDropEvent drop(at, Qt::CopyAction, data, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(m_editor->viewport(), &drop);
    delete data;
    for (const auto &image : m_editor->composerControler()->composerImages()->embeddedImages()) {
        result.append(QStringLiteral("inline: ") + image->imageName);
    }
    for (const Item &item : std::as_const(m_items)) {
        result.append(QStringLiteral("attached: ") + item.attachment.name);
    }
    result.append(QStringLiteral("accepted: %1").arg(drop.isAccepted()));
    // A click in the middle of the first picture selects it
    for (QTextBlock block = m_editor->document()->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            if (it.fragment().charFormat().isImageFormat()) {
                QTextCursor at(m_editor->document());
                at.setPosition(it.fragment().position());
                const QTextImageFormat image = it.fragment().charFormat().toImageFormat();
                const QRect line = m_editor->cursorRect(at);
                const QPoint middle(line.left() + int(image.width() / 2), line.bottom() - int(image.height() / 2));
                QMouseEvent press(QEvent::MouseButtonPress, middle, m_editor->viewport()->mapToGlobal(middle), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(m_editor->viewport(), &press);
                QMouseEvent release(QEvent::MouseButtonRelease, middle, m_editor->viewport()->mapToGlobal(middle), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(m_editor->viewport(), &release);
                const QTextCursor selected = m_editor->textCursor();
                result.append(QStringLiteral("click at %1,%2 selects %3-%4, a picture: %5")
                                  .arg(middle.x())
                                  .arg(middle.y())
                                  .arg(selected.selectionStart())
                                  .arg(selected.selectionEnd())
                                  .arg(selected.charFormat().isImageFormat()));
                return result;
            }
        }
    }
    return result;
}

void ComposerWindow::typeRecipientsForTests(const QString &text)
{
    m_to->setText(text);
}

bool ComposerWindow::validate()
{
    QStringList invalid;
    const QList<OutgoingRecipient> all = m_to->recipients(&invalid) + m_cc->recipients(&invalid) + m_bcc->recipients(&invalid);
    if (!invalid.isEmpty()) {
        KMessageBox::error(this, i18nc("@info", "These are not mail addresses: %1", invalid.join(QStringLiteral(", "))), i18nc("@title:window", "Check the Recipients"));
        return false;
    }
    if (all.isEmpty()) {
        KMessageBox::error(this, i18nc("@info", "Add at least one recipient."), i18nc("@title:window", "No Recipients"));
        m_to->setFocus();
        return false;
    }
    // The attachment reminder: the text speaks of an attachment and there is none
    const bool attached = std::any_of(m_items.cbegin(), m_items.cend(), [this](const Item &item) {
        return !item.attachment.isInline && !(item.fromOriginal && m_dropped.contains(item.attachment.name));
    });
    if (!attached && Quote::mentionsAttachment(m_subject->text() + QLatin1Char('\n') + m_editor->toPlainText())) {
        const auto answer = KMessageBox::questionTwoActionsCancel(this,
                                                                  i18nc("@info", "The message mentions an attachment, but nothing is attached."),
                                                                  i18nc("@title:window", "Forgot an Attachment?"),
                                                                  KGuiItem(i18nc("@action:button", "Attach a File..."), QStringLiteral("mail-attachment")),
                                                                  KGuiItem(i18nc("@action:button", "Send Anyway"), QStringLiteral("mail-send")));
        if (answer == KMessageBox::PrimaryAction) {
            if (QAction *attach = actionCollection()->action(QStringLiteral("attach"))) {
                attach->trigger();
            }
            return false;
        }
        if (answer == KMessageBox::Cancel) {
            return false;
        }
    }
    if (m_subject->text().trimmed().isEmpty()
        && KMessageBox::questionTwoActions(this,
                                           i18nc("@info", "This message has no subject. Send it anyway?"),
                                           i18nc("@title:window", "No Subject"),
                                           KGuiItem(i18nc("@action:button", "Send Anyway"), QStringLiteral("mail-send")),
                                           KStandardGuiItem::cancel())
            != KMessageBox::PrimaryAction) {
        m_subject->setFocus();
        return false;
    }
    return true;
}

void ComposerWindow::save(bool send)
{
    const Outgoing o = record(send);
    m_daemon->saveOutgoing(m_mailboxKey.section(QLatin1Char('/'), 0, 0), m_mailboxKey.section(QLatin1Char('/'), 1), QString::fromUtf8(QJsonDocument(o.toJson()).toJson(QJsonDocument::Compact)));
    for (const OutgoingAttachment &a : o.attachments) {
        m_given.insert(a.key);
    }
    m_record.uuid = o.uuid;
    m_saved = true;
    m_dirty = false;
}

void ComposerWindow::discard()
{
    if (m_saved || !m_record.draftId.isEmpty()) {
        if (!m_saved) {
            // A draft made elsewhere: the daemon only knows it once it is saved
            save(false);
        }
        m_daemon->discardOutgoing(m_mailboxKey.section(QLatin1Char('/'), 0, 0), m_mailboxKey.section(QLatin1Char('/'), 1), m_record.uuid);
    }
    m_closing = true;
    close();
}

void ComposerWindow::askDelay()
{
    QDialog dialog(this);
    dialog.setWindowTitle(i18nc("@title:window", "Delay Delivery"));
    auto *on = new QCheckBox(i18nc("@option:check", "Do not deliver before:"), &dialog);
    auto *when = new QDateTimeEdit(&dialog);
    when->setCalendarPopup(true);
    when->setDisplayFormat(QLocale().dateTimeFormat(QLocale::ShortFormat));
    when->setDateTime(m_deferUntil > 0 ? QDateTime::fromMSecsSinceEpoch(m_deferUntil) : QDateTime(QDate::currentDate().addDays(1), QTime(8, 0)));
    on->setChecked(true);
    connect(on, &QCheckBox::toggled, when, &QWidget::setEnabled);
    auto *note = new QLabel(i18nc("@info", "Microsoft 365 keeps the message and sends it then, also when this computer is off."), &dialog);
    note->setWordWrap(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    auto *row = new QHBoxLayout;
    row->addWidget(on);
    row->addWidget(when, 1);
    layout->addLayout(row);
    layout->addWidget(note);
    layout->addWidget(buttons);
    if (dialog.exec() == QDialog::Accepted) {
        // Once on the draft it cannot be removed, only moved to the past (Outgoing::deferUntil)
        m_deferUntil = on->isChecked() && when->dateTime() > QDateTime::currentDateTime() ? when->dateTime().toMSecsSinceEpoch() : (m_deferUntil != 0 ? -1 : 0);
        setDirty();
    }
    showDelay();
}

void ComposerWindow::showDelay()
{
    m_delay->setChecked(m_deferUntil > 0);
    // Outlook's Schedule send (classic Outlook: Delay Delivery)
    m_delay->setText(m_deferUntil > 0 ? i18nc("@action %1 a date and time", "Schedule Send (%1)...",
                                              QLocale().toString(QDateTime::fromMSecsSinceEpoch(m_deferUntil), QLocale::ShortFormat))
                                      : i18nc("@action", "Schedule Send..."));
}

void ComposerWindow::askMailTips()
{
    QStringList addresses;
    for (RecipientEdit *edit : {m_to, m_cc, m_bcc, m_replyTo}) {
        const QList<OutgoingRecipient> list = edit->recipients();
        for (const OutgoingRecipient &r : list) {
            if (!addresses.contains(r.address, Qt::CaseInsensitive)) {
                addresses.append(r.address);
            }
        }
    }
    const QString asked = addresses.join(QLatin1Char(';')).toLower();
    if (asked == m_mailTipsAsked) {
        return;
    }
    m_mailTipsAsked = asked;
    if (addresses.isEmpty()) {
        m_mailTips->animatedHide();
        return;
    }
    // From the mailbox the message goes out of: "outside the organisation" is relative to it
    const QString key = fromKey().isEmpty() ? m_mailboxKey : fromKey();
    m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("mailTips"),
                    QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("addresses"), QJsonArray::fromStringList(addresses)}}).toJson(QJsonDocument::Compact)), this,
                    [this, asked](const QString &json) {
                        if (asked == m_mailTipsAsked) {
                            showMailTips(QJsonDocument::fromJson(json.toUtf8()).object().value(QLatin1String("value")).toArray());
                        }
                    });
}

void ComposerWindow::showMailTips(const QJsonArray &tips)
{
    QStringList lines;
    m_maxMessageSize = 0;
    for (const QJsonValue &v : tips) {
        const QJsonObject tip = v.toObject();
        const QString address = tip.value(QLatin1String("emailAddress")).toObject().value(QLatin1String("address")).toString();
        const QString who = address.toHtmlEscaped();
        const QString reply = tip.value(QLatin1String("automaticReplies")).toObject().value(QLatin1String("message")).toString();
        if (!reply.isEmpty()) {
            QTextDocument text;
            text.setHtml(reply);
            QString plain = text.toPlainText().simplified();
            if (plain.size() > 200) {
                plain = plain.left(200) + QStringLiteral("...");
            }
            lines.append(i18nc("@info %1 an address, %2 its automatic reply", "<b>%1</b> has automatic replies on: %2", who, plain.toHtmlEscaped()));
        }
        if (tip.value(QLatin1String("mailboxFull")).toBool()) {
            lines.append(i18nc("@info %1 an address", "The mailbox of <b>%1</b> is full: the message may not be delivered.", who));
        }
        if (tip.value(QLatin1String("recipientScope")).toString().contains(QLatin1String("external"))) {
            lines.append(i18nc("@info %1 an address", "<b>%1</b> is outside your organization.", who));
        }
        if (tip.value(QLatin1String("deliveryRestricted")).toBool()) {
            lines.append(i18nc("@info %1 an address", "<b>%1</b> does not accept mail from you.", who));
        }
        if (tip.value(QLatin1String("isModerated")).toBool()) {
            lines.append(i18nc("@info %1 an address", "<b>%1</b> is moderated: the message waits for approval.", who));
        }
        const int members = tip.value(QLatin1String("totalMemberCount")).toInt();
        if (members > 1) {
            const int external = tip.value(QLatin1String("externalMemberCount")).toInt();
            lines.append(external > 0 ? i18ncp("@info %2 a group, %3 external members", "<b>%2</b> has %1 member, %3 outside your organization.",
                                               "<b>%2</b> has %1 members, %3 outside your organization.", members, who, external)
                                      : i18ncp("@info %2 a group", "<b>%2</b> has %1 member.", "<b>%2</b> has %1 members.", members, who));
        }
        const qint64 max = tip.value(QLatin1String("maxMessageSize")).toInteger();
        if (max > 0 && (m_maxMessageSize == 0 || max < m_maxMessageSize)) {
            m_maxMessageSize = max;
        }
    }
    qint64 size = 0;
    for (const Item &item : std::as_const(m_items)) {
        size += item.attachment.size;
    }
    // Attachments grow by a third in the mail (base64)
    if (m_maxMessageSize > 0 && size * 4 / 3 > m_maxMessageSize) {
        lines.append(i18nc("@info %1 a size", "The attachments are larger than a recipient accepts (%1).", QLocale().formattedDataSize(m_maxMessageSize)));
    }
    qWarning().noquote() << "mailtips test:" << (lines.isEmpty() ? QStringLiteral("(none)") : lines.join(QStringLiteral(" | ")));
    if (lines.isEmpty()) {
        m_mailTips->animatedHide();
        return;
    }
    m_mailTips->setText(lines.join(QStringLiteral("<br>")));
    m_mailTips->animatedShow();
}

void ComposerWindow::addTo(const QString &addresses)
{
    m_to->setRecipients(m_to->recipients() + parseRecipients(addresses));
    setDirty();
}

QString ComposerWindow::lastRecord() const
{
    return m_mailboxKey + QLatin1Char('|') + m_record.uuid + QLatin1Char('|') + m_subject->text();
}

QJsonObject ComposerWindow::templateJson() const
{
    auto recipients = [](const QList<OutgoingRecipient> &list) {
        QJsonArray out;
        for (const OutgoingRecipient &r : list) {
            out.append(QJsonObject{{QStringLiteral("name"), r.name}, {QStringLiteral("address"), r.address}});
        }
        return out;
    };
    return {{QStringLiteral("subject"), m_subject->text()},
            {QStringLiteral("to"), recipients(m_to->recipients())},
            {QStringLiteral("cc"), recipients(m_cc->recipients())},
            {QStringLiteral("bcc"), recipients(m_bcc->recipients())},
            {QStringLiteral("html"), !m_plainText->isChecked()},
            {QStringLiteral("text"), m_plainText->isChecked() ? m_editor->toPlainText() : bodyOf(m_editor->composerControler()->toCleanHtml())},
            {QStringLiteral("importance"), m_high->isChecked() ? 2 : m_low->isChecked() ? 0 : 1},
            {QStringLiteral("sensitivity"), std::max(0, m_sensitivity->currentItem())},
            {QStringLiteral("readReceipt"), m_readReceipt->isChecked()},
            {QStringLiteral("deliveryReceipt"), m_deliveryReceipt->isChecked()}};
}

void ComposerWindow::applyTemplate(const QJsonObject &t)
{
    auto recipients = [](const QJsonValue &value) {
        QList<OutgoingRecipient> out;
        const QJsonArray list = value.toArray();
        for (const QJsonValue &v : list) {
            out.append({v.toObject().value(QLatin1String("name")).toString(), v.toObject().value(QLatin1String("address")).toString()});
        }
        return out;
    };
    m_subject->setText(t.value(QLatin1String("subject")).toString());
    m_to->setRecipients(recipients(t.value(QLatin1String("to"))));
    m_cc->setRecipients(recipients(t.value(QLatin1String("cc"))));
    m_bcc->setRecipients(recipients(t.value(QLatin1String("bcc"))));
    m_showBcc->setChecked(!t.value(QLatin1String("bcc")).toArray().isEmpty());
    const int importance = t.value(QLatin1String("importance")).toInt(1);
    m_high->setChecked(importance == 2);
    m_low->setChecked(importance == 0);
    m_sensitivity->setCurrentItem(qBound(0, t.value(QLatin1String("sensitivity")).toInt(), 3));
    m_readReceipt->setChecked(t.value(QLatin1String("readReceipt")).toBool());
    m_deliveryReceipt->setChecked(t.value(QLatin1String("deliveryReceipt")).toBool());
    // The template's text above the signature the composer already put in
    QTextCursor cursor(m_editor->document());
    cursor.movePosition(QTextCursor::Start);
    if (t.value(QLatin1String("html")).toBool(true)) {
        m_plainText->setChecked(false);
        cursor.insertHtml(t.value(QLatin1String("text")).toString());
    } else {
        cursor.insertText(t.value(QLatin1String("text")).toString());
    }
    setDirty();
}

void ComposerWindow::saveAsTemplate()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, i18nc("@title:window", "Save as Template"), i18nc("@label:textbox", "Name:"), QLineEdit::Normal,
                                               m_subject->text().trimmed(), &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }
    QJsonObject t = templateJson();
    t.insert(QStringLiteral("name"), name.trimmed());
    if (!Templates::save(&t)) {
        KMessageBox::error(this, i18nc("@info %1 a folder", "The template could not be saved in %1.", Templates::directory()));
        return;
    }
    m_notice->setMessageType(KMessageWidget::Positive);
    m_notice->setText(i18nc("@info %1 a template's name", "Saved as the template \"%1\".", name.trimmed()));
    m_notice->animatedShow();
}

void ComposerWindow::setOptionsForTests(const QStringList &options)
{
    for (const QString &o : options) {
        if (o == QLatin1String("read")) {
            m_readReceipt->setChecked(true);
        } else if (o == QLatin1String("delivery")) {
            m_deliveryReceipt->setChecked(true);
        } else if (o.startsWith(QLatin1String("sensitivity="))) {
            m_sensitivity->setCurrentItem(o.section(QLatin1Char('='), 1).toInt());
        } else if (o.startsWith(QLatin1String("defer="))) {
            m_deferUntil = QDateTime::fromString(o.section(QLatin1Char('='), 1), Qt::ISODate).toMSecsSinceEpoch();
            showDelay();
        }
    }
}

void ComposerWindow::setTestMode()
{
    // An invisible dialog would block an off-screen run (and its sound reaches the desktop)
    m_testMode = true;
    m_autosave.stop();
}

void ComposerWindow::setTestInput(const QString &to, const QString &subject, const QString &text, const QStringList &files)
{
    if (!to.isEmpty()) {
        m_to->setRecipients(parseRecipients(to));
    }
    if (!subject.isEmpty()) {
        m_subject->setText(subject);
    }
    if (!text.isEmpty()) {
        QTextCursor cursor(m_editor->document());
        cursor.insertText(text);
    }
    addFiles(files);
}

void ComposerWindow::prefill(const QStringList &to, const QStringList &cc, const QStringList &bcc, const QString &subject, const QString &text, const QStringList &files)
{
    m_to->setRecipients(parseRecipients(to.join(QLatin1Char(';'))));
    m_cc->setRecipients(parseRecipients(cc.join(QLatin1Char(';'))));
    m_bcc->setRecipients(parseRecipients(bcc.join(QLatin1Char(';'))));
    if (!bcc.isEmpty()) {
        m_showBcc->setChecked(true);
    }
    if (!subject.isEmpty()) {
        m_subject->setText(subject);
    }
    if (!text.isEmpty()) {
        // Above the signature, where the user would start typing
        QTextCursor cursor(m_editor->document());
        cursor.movePosition(QTextCursor::Start);
        cursor.insertText(text + QLatin1Char('\n'));
    }
    addFiles(files);
}

QString ComposerWindow::saveNow()
{
    save(false);
    m_closing = true;
    return m_record.uuid;
}

bool ComposerWindow::sendNow()
{
    QStringList invalid;
    const QList<OutgoingRecipient> all = m_to->recipients(&invalid) + m_cc->recipients(&invalid) + m_bcc->recipients(&invalid);
    if (all.isEmpty() || !invalid.isEmpty()) {
        return false;
    }
    save(true);
    m_closing = true;
    return true;
}

void ComposerWindow::closeEvent(QCloseEvent *event)
{
    if (!m_closing && m_dirty && !m_testMode) {
        const auto answer = KMessageBox::warningTwoActionsCancel(this,
                                                                 i18nc("@info", "Save this message as a draft?"),
                                                                 i18nc("@title:window", "Close Message"),
                                                                 KStandardGuiItem::save(),
                                                                 KStandardGuiItem::discard());
        if (answer == KMessageBox::Cancel) {
            event->ignore();
            return;
        }
        if (answer == KMessageBox::PrimaryAction) {
            save(false);
        } else if (m_saved) {
            m_daemon->discardOutgoing(m_mailboxKey.section(QLatin1Char('/'), 0, 0), m_mailboxKey.section(QLatin1Char('/'), 1), m_record.uuid);
        }
    }
    m_autosave.stop();
    KXmlGuiWindow::closeEvent(event);
}

bool ComposerWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_editor->viewport() && (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop)) {
        auto *drop = static_cast<QDropEvent *>(event);
        QStringList paths;
        const QList<QUrl> urls = drop->mimeData()->hasUrls() ? drop->mimeData()->urls() : QList<QUrl>();
        for (const QUrl &url : urls) {
            if (url.isLocalFile()) {
                paths.append(url.toLocalFile());
            }
        }
        if (!paths.isEmpty()) {
            // The caret shows where an image lands
            m_editor->setTextCursor(m_editor->cursorForPosition(drop->position().toPoint()));
            if (event->type() == QEvent::Drop) {
                dropFiles(paths);
            }
            drop->acceptProposedAction();
            return true;
        }
    }
    if (watched == m_editor->viewport() && event->type() == QEvent::MouseButtonPress) {
        // A click on a picture selects it, as in Outlook; pressed again it can be dragged elsewhere
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton && mouse->modifiers() == Qt::NoModifier && selectImageAt(mouse->position().toPoint())) {
            m_clickedImage = m_editor->textCursor();
        }
    }
    if (watched == m_editor->viewport() && event->type() == QEvent::MouseButtonRelease && !m_clickedImage.isNull() && !m_replayingKey) {
        // Qt clears a selection that was clicked without dragging; the picture stays selected
        const QTextCursor image = m_clickedImage;
        m_clickedImage = QTextCursor();
        m_replayingKey = true;
        QCoreApplication::sendEvent(watched, event);
        m_replayingKey = false;
        QTextCursor check(m_editor->document());
        check.setPosition(image.selectionStart());
        check.setPosition(image.selectionEnd(), QTextCursor::KeepAnchor);
        if (!m_editor->textCursor().hasSelection() && check.charFormat().isImageFormat()) {
            m_editor->setTextCursor(check);
        }
        return true;
    }
    if (watched == m_editor && event->type() == QEvent::KeyPress && !m_replayingKey) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const int key = keyEvent->key();
        // The @mention list open: Enter and Tab pick, Escape closes (the list handles them)
        if (m_mention && m_mention->popup()->isVisible()
            && (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Tab || key == Qt::Key_Backtab || key == Qt::Key_Escape)) {
            if (key == Qt::Key_Escape) {
                endMention();
            } else if (const QModelIndex current = m_mention->popup()->currentIndex(); current.isValid()) {
                insertMention(current.data().toString());
            }
            return true;
        }
        const QKeySequence sequence(keyEvent->keyCombination());
        if (KStandardShortcut::undo().contains(sequence) || KStandardShortcut::redo().contains(sequence)) {
            undoText(KStandardShortcut::undo().contains(sequence));
            return true;
        }
        if (key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Tab) {
            linkTypedAddress();
        }
        const bool enter = key == Qt::Key_Return || key == Qt::Key_Enter;
        const bool typing = !(keyEvent->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && (enter || !keyEvent->text().isEmpty())
            && key != Qt::Key_Backspace && key != Qt::Key_Delete && key != Qt::Key_Escape;
        if (typing) {
            // Qt makes every Enter a step of its own. Here a typed line and its Enter are one step,
            // so one Ctrl+Z takes back a line, as in Outlook: the keys of a line share an edit block
            QTextDocument *document = m_editor->document();
            const bool continues = !m_editor->textCursor().hasSelection() && document->availableUndoSteps() == m_typingSteps
                && m_editor->textCursor().position() == m_typingEnd;
            QTextCursor block = m_editor->textCursor();
            if (continues && (enter || !m_lineEnded)) {
                block.joinPreviousEditBlock();
            } else {
                block.beginEditBlock();
            }
            m_replayingKey = true;
            QCoreApplication::sendEvent(m_editor, event);
            m_replayingKey = false;
            block.endEditBlock();
            m_typingSteps = document->availableUndoSteps();
            m_typingEnd = m_editor->textCursor().position();
            m_lineEnded = enter;
            return true;
        }
    }
    return KXmlGuiWindow::eventFilter(watched, event);
}

void ComposerWindow::linkTypedAddress()
{
    if (m_editor->textMode() != KPIMTextEdit::RichTextComposer::Rich) {
        return;
    }
    QTextCursor cursor = m_editor->textCursor();
    if (cursor.hasSelection()) {
        return;
    }
    // The word just typed, without punctuation after it
    const QTextBlock block = cursor.block();
    const QString text = block.text();
    const int end = cursor.positionInBlock();
    int start = end;
    while (start > 0 && !text.at(start - 1).isSpace()) {
        --start;
    }
    QString word = text.mid(start, end - start);
    while (!word.isEmpty() && QStringLiteral(".,;:!?)]}'\"").contains(word.back())) {
        word.chop(1);
    }
    static const QRegularExpression address(QStringLiteral(R"(^(https?://[^\s<>"]+|www\.[^\s<>"]+\.[^\s<>"]+)$)"), QRegularExpression::CaseInsensitiveOption);
    if (word.isEmpty() || !address.match(word).hasMatch()) {
        return;
    }
    QTextCursor link(m_editor->document());
    link.setPosition(block.position() + start);
    link.setPosition(block.position() + start + int(word.size()), QTextCursor::KeepAnchor);
    if (link.charFormat().isAnchor()) {
        return;
    }
    const QTextCharFormat before = cursor.charFormat();
    QTextCharFormat format;
    format.setAnchor(true);
    format.setAnchorHref(word.startsWith(QLatin1String("www."), Qt::CaseInsensitive) ? QStringLiteral("https://") + word : word);
    format.setForeground(m_editor->palette().color(QPalette::Link));
    format.setFontUnderline(true);
    link.mergeCharFormat(format);
    // What is typed next is ordinary text again
    m_editor->setCurrentCharFormat(before);
}

void ComposerWindow::updateMention()
{
    if (m_editor->textMode() != KPIMTextEdit::RichTextComposer::Rich) {
        return;
    }
    const QTextCursor cursor = m_editor->textCursor();
    if (cursor.hasSelection()) {
        endMention();
        return;
    }
    const QTextBlock block = cursor.block();
    const QString text = block.text();
    const int pos = cursor.positionInBlock();
    if (m_mentionStart < 0) {
        // An "@" just typed at the start of a word (not inside an address or a link)
        if (pos > 0 && text.at(pos - 1) == QLatin1Char('@') && (pos == 1 || text.at(pos - 2).isSpace()) && !cursor.charFormat().isAnchor()) {
            m_mentionStart = block.position() + pos - 1;
            startMention();
        } else {
            return;
        }
    }
    const int start = m_mentionStart - block.position();
    if (start < 0 || start >= text.size() || pos <= start || text.at(start) != QLatin1Char('@')) {
        endMention();
        return;
    }
    const QString typed = text.mid(start + 1, pos - start - 1);
    if (typed.size() > 40 || typed.contains(QStringLiteral("  "))) {
        endMention();
        return;
    }
    m_mention->setCompletionPrefix(typed);
    if (m_mention->completionCount() == 0) {
        m_mention->popup()->hide();
        return;
    }
    QRect rect = m_editor->cursorRect();
    rect.setWidth(380);
    m_mention->complete(rect);
    // Enter takes the first one, as in Outlook
    m_mention->popup()->setCurrentIndex(m_mention->completionModel()->index(0, 0));
}

void ComposerWindow::startMention()
{
    // The people of the conversation first (the original's sender and recipients, the To and Cc
    // lines), then the address book
    QStringList entries;
    QSet<QString> seen;
    const auto add = [&entries, &seen](const OutgoingRecipient &r) {
        if (!r.address.isEmpty() && !seen.contains(r.address.toLower())) {
            seen.insert(r.address.toLower());
            entries.append(r.name.isEmpty() ? r.address : QStringLiteral("%1 <%2>").arg(r.name, r.address));
        }
    };
    if (const std::optional<MessageRow> original = (m_store && m_request.message != 0) ? m_store->message(m_request.message) : std::nullopt) {
        add({original->senderName, original->senderAddress});
        for (const OutgoingRecipient &r : parseRecipients(original->toList) + parseRecipients(original->ccList)) {
            add(r);
        }
    }
    for (const OutgoingRecipient &r : m_to->recipients() + m_cc->recipients()) {
        add(r);
    }
    for (const QString &entry : m_book->model()->stringList()) {
        const QList<OutgoingRecipient> parsed = parseRecipients(entry);
        if (!parsed.isEmpty()) {
            add(parsed.constFirst());
        }
    }
    if (!m_mention) {
        m_mention = new QCompleter(this);
        m_mention->setWidget(m_editor);
        m_mention->setCompletionMode(QCompleter::PopupCompletion);
        m_mention->setCaseSensitivity(Qt::CaseInsensitive);
        m_mention->setFilterMode(Qt::MatchContains);
        m_mention->setMaxVisibleItems(8);
        connect(m_mention, qOverload<const QString &>(&QCompleter::activated), this, &ComposerWindow::insertMention);
    }
    m_mention->setModel(new QStringListModel(entries, m_mention));
}

void ComposerWindow::endMention()
{
    m_mentionStart = -1;
    if (m_mention) {
        m_mention->popup()->hide();
    }
}

void ComposerWindow::insertMention(const QString &entry)
{
    const QList<OutgoingRecipient> parsed = parseRecipients(entry);
    if (parsed.isEmpty() || m_mentionStart < 0) {
        endMention();
        return;
    }
    const OutgoingRecipient person = parsed.constFirst();
    QTextCursor cursor = m_editor->textCursor();
    const QTextCharFormat plain = [&cursor] {
        QTextCharFormat f = cursor.charFormat();
        f.setAnchor(false);
        f.setAnchorHref({});
        return f;
    }();
    cursor.beginEditBlock();
    cursor.setPosition(m_mentionStart, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    // "@Name" as a mailto link in the link colour, as Outlook marks a mention
    QTextCharFormat link = plain;
    link.setAnchor(true);
    link.setAnchorHref(QStringLiteral("mailto:") + person.address);
    link.setForeground(m_editor->palette().color(QPalette::Link));
    link.setFontUnderline(false);
    cursor.insertText(QLatin1Char('@') + (person.name.isEmpty() ? person.address : person.name), link);
    cursor.insertText(QStringLiteral(" "), plain);
    cursor.endEditBlock();
    m_editor->setTextCursor(cursor);
    m_editor->setCurrentCharFormat(plain);
    m_mentionStart = -1;
    if (m_mention) {
        m_mention->popup()->hide();
    }
    // Mentioned means addressed: on the To line unless already on the message
    const QList<OutgoingRecipient> on = m_to->recipients() + m_cc->recipients() + m_bcc->recipients();
    const bool present = std::any_of(on.cbegin(), on.cend(), [&person](const OutgoingRecipient &r) {
        return r.address.compare(person.address, Qt::CaseInsensitive) == 0;
    });
    if (!present) {
        m_to->setRecipients(m_to->recipients() + QList<OutgoingRecipient>{person});
        setDirty();
    }
}

QList<OutgoingRecipient> ComposerWindow::mentionsInText() const
{
    QList<OutgoingRecipient> list;
    QSet<QString> seen;
    for (QTextBlock block = m_editor->document()->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QTextCharFormat format = fragment.charFormat();
            const QString href = format.anchorHref();
            if (format.isAnchor() && href.startsWith(QLatin1String("mailto:"), Qt::CaseInsensitive) && fragment.text().startsWith(QLatin1Char('@'))) {
                const QString address = href.mid(7).section(QLatin1Char('?'), 0, 0);
                if (!address.isEmpty() && !seen.contains(address.toLower())) {
                    seen.insert(address.toLower());
                    list.append({fragment.text().mid(1), address});
                }
            }
        }
    }
    return list;
}

void ComposerWindow::clearTextColor(bool background)
{
    ColorAction::clearColor(m_editor, background);
}

void ComposerWindow::undoText(bool undo)
{
    QTextDocument *document = m_editor->document();
    const QTextCursor before = m_editor->textCursor();
    const QString text = document->toRawText();
    // Qt's own cursor place adds up the lengths of a change recorded twice (a colour) and can lie
    // past the end, which lands the cursor at the top; the place of the last change is used instead
    int changed = -1;
    const QMetaObject::Connection connection = connect(document, &QTextDocument::contentsChange, this, [&changed](int from, int removed, int added) {
        Q_UNUSED(removed)
        changed = from + added;
    });
    if (undo) {
        document->undo();
    } else {
        document->redo();
    }
    disconnect(connection);
    const int last = document->characterCount() - 1;
    QTextCursor cursor(document);
    if (document->toRawText() == text) {
        // Only the look changed: the selection stays
        cursor.setPosition(std::min(before.anchor(), last));
        cursor.setPosition(std::min(before.position(), last), QTextCursor::KeepAnchor);
    } else {
        cursor.setPosition(std::clamp(changed, 0, last));
    }
    m_editor->setTextCursor(cursor);
    m_editor->ensureCursorVisible();
    m_typingSteps = -1;
}

bool ComposerWindow::selectImageAt(const QPoint &viewportPoint)
{
    QTextDocument *document = m_editor->document();
    const QPointF point = viewportPoint + QPointF(m_editor->horizontalScrollBar()->value(), m_editor->verticalScrollBar()->value());
    const int hit = document->documentLayout()->hitTest(point, Qt::ExactHit);
    if (hit < 0) {
        return false;
    }
    // The character at the hit, the picture itself (a format belongs to the character before a cursor)
    QTextCursor image(document);
    image.setPosition(hit);
    image.setPosition(hit + 1, QTextCursor::KeepAnchor);
    if (!image.charFormat().isImageFormat()) {
        return false;
    }
    const QTextCursor current = m_editor->textCursor();
    if (current.selectionStart() != image.selectionStart() || current.selectionEnd() != image.selectionEnd()) {
        m_editor->setTextCursor(image);
    }
    return true;
}

void ComposerWindow::dropFiles(const QStringList &paths)
{
    // As Outlook: pictures go into the text (rich text only), everything else is attached
    QStringList attach;
    for (const QString &path : paths) {
        QImageReader reader(path);
        const QSize size = reader.size();
        if (m_editor->textMode() != KPIMTextEdit::RichTextComposer::Rich || !reader.canRead() || !size.isValid()) {
            attach.append(path);
            continue;
        }
        // A photo at full size would be wider than any mail window
        const int maxWidth = 800;
        if (size.width() > maxWidth) {
            m_editor->composerControler()->composerImages()->addImage(QUrl::fromLocalFile(path), maxWidth, size.height() * maxWidth / size.width());
        } else {
            m_editor->composerControler()->composerImages()->addImage(QUrl::fromLocalFile(path));
        }
    }
    addFiles(attach);
}

void ComposerWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void ComposerWindow::dropEvent(QDropEvent *event)
{
    QStringList paths;
    const QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) {
            paths.append(url.toLocalFile());
        }
    }
    addFiles(paths);
    event->acceptProposedAction();
}

}
