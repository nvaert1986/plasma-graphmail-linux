// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "composer.h"
#include "accesstext.h"
#include "avatar.h"
#include "blobstore.h"
#include "daemonclient.h"
#include "mailboxset.h"
#include "mailboxworker.h"
#include "mailview.h"
#include "mainwindow.h"
#include "quote.h"
#include "recipientedit.h"
#include "signaturedialog.h"
#include "signatures.h"
#include "store.h"
#include "templates.h"

#include <KActionCollection>
#include <KActionMenu>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KMessageBox>
#include <KMessageWidget>
#include <KPIMTextEdit/RichTextComposer>
#include <KPIMTextEdit/RichTextComposerActions>
#include <KPIMTextEdit/RichTextComposerControler>
#include <KPIMTextEdit/RichTextComposerImages>
#include <KSharedConfig>
#include <KStandardAction>
#include <KSelectAction>
#include <KToggleAction>

#include <QActionGroup>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSplitter>
#include <QTextBlock>
#include <QTextDocument>
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
    , m_subject(new QLineEdit(this))
    , m_attachmentList(new QListWidget(this))
    , m_editor(new KPIMTextEdit::RichTextComposer(this))
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
    m_editor->document()->setDefaultFont(mailFont());
    m_editor->activateRichText();

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

    setupFrom();

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
    for (QLineEdit *edit : {static_cast<QLineEdit *>(m_to), static_cast<QLineEdit *>(m_cc), static_cast<QLineEdit *>(m_bcc), m_subject}) {
        connect(edit, &QLineEdit::textEdited, this, &ComposerWindow::setDirty);
    }
    for (RecipientEdit *edit : {m_to, m_cc, m_bcc}) {
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

    // Send, with an arrow for Send As / Send on Behalf when writing from another mailbox
    m_send = new KActionMenu(QIcon::fromTheme(QStringLiteral("mail-send")), i18nc("@action", "Send"), this);
    m_send->setPopupMode(QToolButton::MenuButtonPopup);
    actions->addAction(QStringLiteral("send"), m_send);
    actions->setDefaultShortcut(m_send, QKeySequence(Qt::CTRL | Qt::Key_Return));
    connect(m_send, &QAction::triggered, this, [this] {
        if (validate()) {
            save(true);
            m_closing = true;
            // Undo send: the main window offers Undo while the message waits
            if (m_record.sendAfter > 0) {
                if (MainWindow *main = MainWindow::instance()) {
                    main->offerUndoSend(m_mailboxKey, m_record.uuid, m_subject->text(), undoSendSeconds());
                }
            }
            close();
        }
    });
    m_sendMenu = new QMenu(this);
    m_sendModes = new QActionGroup(this);
    m_sendModes->setExclusive(true);

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

void ComposerWindow::startFrom(const MessageRow *original)
{
    m_record.kind = m_request.kind;
    const bool answer = original && m_request.kind != QLatin1String("new") && m_request.kind != QLatin1String("draft");
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
            // The original's attachments come along; the user may remove them
            const QList<AttachmentRow> rows = m_store ? m_store->attachments(original->id) : QList<AttachmentRow>();
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
        m_quoteSource = original->id;
        buildQuote();
        showQuote();
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
    m_quoteView->showBody(m_store, m_blobs, m_quoteSource, false);
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
    if (!m_signatureStart.isNull() && !m_signatureEnd.isNull() && m_signatureEnd.position() > m_signatureStart.position()) {
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
    o.ui = ui;
    return o;
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
    m_delay->setText(m_deferUntil > 0 ? i18nc("@action %1 a date and time", "Delay Delivery (until %1)...",
                                              QLocale().toString(QDateTime::fromMSecsSinceEpoch(m_deferUntil), QLocale::ShortFormat))
                                      : i18nc("@action", "Delay Delivery..."));
}

void ComposerWindow::askMailTips()
{
    QStringList addresses;
    for (RecipientEdit *edit : {m_to, m_cc, m_bcc}) {
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
