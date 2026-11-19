// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "signaturedialog.h"
#include "maileditor.h"
#include "coloraction.h"
#include "pastedimages.h"
#include "mailboxset.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KMessageBox>
#include <KPIMTextEdit/RichTextComposer>
#include <KPIMTextEdit/RichTextComposerActions>
#include <KPIMTextEdit/RichTextComposerControler>
#include <KPIMTextEdit/RichTextComposerImages>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QTextDocument>
#include <QToolBar>
#include <QUuid>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
// The part between <body> and </body> of what QTextDocument writes
QString bodyOf(const QString &html)
{
    static const QRegularExpression body(QStringLiteral("<body[^>]*>(.*)</body>"), QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = body.match(html);
    return match.hasMatch() ? match.captured(1).trimmed() : html;
}

// The font of a new message (composer.cpp mailFont): the signature editor shows text without its own
// font as the message will
QFont messageFont()
{
    QFont font;
    font.setFamilies({QStringLiteral("Aptos"), QStringLiteral("Calibri"), QStringLiteral("Carlito"), QFont().family()});
    font.setPointSize(11);
    return font;
}

// The signature's text in the font the signature editor shows it in. Qt writes no font for text in the
// document's default font: that default sits only in <body>, which saving drops, so such text took the
// new message's default when inserted (a size changed from 11 to 12 came back as 11)
QString withEditorFont(const QString &body, const QFont &font)
{
    QStringList families;
    for (const QString &family : font.families()) {
        families.append(QLatin1Char('\'') + family + QLatin1Char('\''));
    }
    return QStringLiteral("<div style=\"font-family:%1;font-size:%2pt\">%3</div>").arg(families.join(QLatin1Char(',')), QString::number(font.pointSizeF()), body);
}
}

void SignatureDialog::load(KPIMTextEdit::RichTextComposer *editor, const Signature &signature)
{
    editor->setHtml(signature.html);
    for (const Signature::Image &image : signature.images) {
        editor->composerControler()->composerImages()->loadImage(QImage::fromData(image.png), image.name, image.name);
    }
}

void SignatureDialog::store(KPIMTextEdit::RichTextComposer *editor, Signature *signature)
{
    signature->html = withEditorFont(bodyOf(editor->composerControler()->toCleanHtml()), editor->document()->defaultFont());
    signature->images.clear();
    const auto images = editor->composerControler()->composerImages()->embeddedImages();
    for (const auto &image : images) {
        if (signature->html.contains(image->imageName)) {
            signature->images.append({image->imageName, QByteArray::fromBase64(image->image)});
        }
    }
}

SignatureDialog::SignatureDialog(MailboxSet *mailboxes, QWidget *parent)
    : QDialog(parent)
    , m_mailboxes(mailboxes)
    , m_signatures(Signatures::all())
    , m_list(new QListWidget(this))
    , m_editor(new MailEditor(this))
    , m_actions(new KActionCollection(this))
    , m_mailbox(new QComboBox(this))
    , m_forNew(new QComboBox(this))
    , m_forReply(new QComboBox(this))
{
    setWindowTitle(i18nc("@title:window", "Signatures"));

    // The list with Add, Rename and Remove
    auto *add = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), i18nc("@action:button", "New"), this);
    auto *rename = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-rename")), i18nc("@action:button", "Rename"), this);
    auto *remove = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), i18nc("@action:button", "Delete"), this);
    connect(add, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, i18nc("@title:window", "New Signature"), i18nc("@label:textbox", "Name:"), QLineEdit::Normal, {}, &ok);
        if (!ok || name.trimmed().isEmpty()) {
            return;
        }
        keepCurrent();
        m_signatures.append({QUuid::createUuid().toString(QUuid::WithoutBraces), name.trimmed(), {}, {}});
        m_list->addItem(name.trimmed());
        m_list->setCurrentRow(int(m_signatures.size()) - 1);
        fillDefaults();
    });
    connect(rename, &QPushButton::clicked, this, [this] {
        const int row = m_list->currentRow();
        if (row < 0) {
            return;
        }
        bool ok = false;
        const QString name = QInputDialog::getText(this, i18nc("@title:window", "Rename Signature"), i18nc("@label:textbox", "Name:"), QLineEdit::Normal, m_signatures[row].name, &ok);
        if (ok && !name.trimmed().isEmpty()) {
            m_signatures[row].name = name.trimmed();
            m_list->item(row)->setText(name.trimmed());
            fillDefaults();
        }
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = m_list->currentRow();
        if (row < 0) {
            return;
        }
        m_current = -1;
        m_signatures.removeAt(row);
        delete m_list->takeItem(row);
        fillDefaults();
    });
    for (const Signature &s : std::as_const(m_signatures)) {
        m_list->addItem(s.name);
    }
    connect(m_list, &QListWidget::currentRowChanged, this, &SignatureDialog::showSignature);

    auto *listButtons = new QHBoxLayout;
    listButtons->addWidget(add);
    listButtons->addWidget(rename);
    listButtons->addWidget(remove);
    auto *left = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(m_list);
    leftLayout->addLayout(listButtons);

    // The editor with the usual formatting
    m_editor->composerActions()->createActions(m_actions);
    m_editor->document()->setDefaultFont(messageFont());
    m_editor->activateRichText();
    // A signature copied from Outlook on the web: its pictures point at the web
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
    auto *formatting = new QToolBar(this);
    // Font Color as in the composer (the "A" with the colour under it and a palette)
    auto *textColor = new ColorAction(ColorAction::Kind::Text, this);
    connect(textColor, &ColorAction::colorChosen, this, [this](const QColor &color) {
        if (color.isValid()) {
            m_editor->composerControler()->setTextForegroundColor(color);
        } else {
            ColorAction::clearColor(m_editor, false);
        }
        m_editor->setFocus();
    });
    for (const char *name : {"format_text_bold", "format_text_italic", "format_text_underline", "text_color", "format_font_family", "format_font_size", "manage_link", "add_image"}) {
        if (QLatin1String(name) == QLatin1String("text_color")) {
            formatting->addAction(textColor);
        } else if (QAction *action = m_actions->action(QLatin1String(name))) {
            formatting->addAction(action);
        }
    }
    auto *right = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(formatting);
    rightLayout->addWidget(m_editor);

    auto *splitter = new QSplitter(this);
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({200, 520});

    // Which signature each mailbox uses
    auto *defaults = new QGroupBox(i18nc("@title:group", "Default Signature"), this);
    auto *form = new QFormLayout(defaults);
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        if (!box.mail) {
            continue;
        }
        m_mailbox->addItem(box.address.isEmpty() ? box.label() : box.address, box.key());
        m_defaults.insert(box.key(), {Signatures::defaultFor(box.account, box.mailbox, false), Signatures::defaultFor(box.account, box.mailbox, true)});
    }
    // As wide as the longest signature name or address, never cut off ("Example Pe|")
    for (QComboBox *box : {m_mailbox, m_forNew, m_forReply}) {
        box->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        box->setMinimumContentsLength(24);
    }
    form->addRow(i18nc("@label:listbox", "Mailbox:"), m_mailbox);
    form->addRow(i18nc("@label:listbox", "New messages:"), m_forNew);
    form->addRow(i18nc("@label:listbox", "Replies and forwards:"), m_forReply);
    connect(m_mailbox, &QComboBox::currentIndexChanged, this, &SignatureDialog::fillDefaults);
    connect(m_forNew, &QComboBox::activated, this, [this] {
        defaultChanged(false);
    });
    connect(m_forReply, &QComboBox::activated, this, [this] {
        defaultChanged(true);
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(splitter, 1);
    layout->addWidget(defaults);
    layout->addWidget(buttons);
    resize(780, 560);

    fillDefaults();
    if (!m_signatures.isEmpty()) {
        m_list->setCurrentRow(0);
    } else {
        m_editor->setEnabled(false);
    }
}

void SignatureDialog::keepCurrent()
{
    if (m_current >= 0 && m_current < m_signatures.size()) {
        store(m_editor, &m_signatures[m_current]);
    }
}

void SignatureDialog::showSignature(int row)
{
    keepCurrent();
    m_current = row;
    m_editor->setEnabled(row >= 0);
    if (row < 0) {
        m_editor->clear();
        return;
    }
    load(m_editor, m_signatures.at(row));
}

void SignatureDialog::fillDefaults()
{
    const QPair<QString, QString> current = m_defaults.value(m_mailbox->currentData().toString());
    for (QComboBox *box : {m_forNew, m_forReply}) {
        box->clear();
        box->addItem(i18nc("@item:inlistbox no signature", "(none)"), QString());
        for (const Signature &s : std::as_const(m_signatures)) {
            box->addItem(s.name, s.id);
        }
        const QString id = box == m_forNew ? current.first : current.second;
        box->setCurrentIndex(std::max(0, box->findData(id)));
    }
}

void SignatureDialog::defaultChanged(bool reply)
{
    QPair<QString, QString> &entry = m_defaults[m_mailbox->currentData().toString()];
    (reply ? entry.second : entry.first) = (reply ? m_forReply : m_forNew)->currentData().toString();
}

void SignatureDialog::accept()
{
    keepCurrent();
    Signatures::save(m_signatures);
    for (auto it = m_defaults.cbegin(); it != m_defaults.cend(); ++it) {
        const QString account = it.key().section(QLatin1Char('/'), 0, 0);
        const QString mailbox = it.key().section(QLatin1Char('/'), 1);
        Signatures::setDefault(account, mailbox, false, it.value().first);
        Signatures::setDefault(account, mailbox, true, it.value().second);
    }
    QDialog::accept();
}

}
