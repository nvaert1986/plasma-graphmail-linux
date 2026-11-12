// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "autorepliesdialog.h"
#include "accesstext.h"
#include "daemonclient.h"
#include "mailboxset.h"
#include "mailboxsettings.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KPIMTextEdit/RichTextComposer>
#include <KPIMTextEdit/RichTextComposerActions>
#include <KPIMTextEdit/RichTextComposerControler>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTimeZone>
#include <QToolBar>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
// The part between <body> and </body> of what QTextDocument writes, as Outlook stores a reply;
// an empty editor gives an empty message
QString bodyOf(KPIMTextEdit::RichTextComposer *editor)
{
    if (editor->document()->isEmpty()) {
        return {};
    }
    static const QRegularExpression body(QStringLiteral("<body[^>]*>(.*)</body>"), QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    const QString html = editor->composerControler()->toCleanHtml();
    const QRegularExpressionMatch match = body.match(html);
    return match.hasMatch() ? match.captured(1).trimmed() : html;
}
}

AutoRepliesDialog::AutoRepliesDialog(MailboxSet *mailboxes, DaemonClient *daemon, const QString &mailboxKey, QWidget *parent)
    : QDialog(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
    , m_mailbox(new QComboBox(this))
    , m_off(new QRadioButton(i18nc("@option:radio", "Do not send automatic replies"), this))
    , m_on(new QRadioButton(i18nc("@option:radio", "Send automatic replies"), this))
    , m_scheduled(new QCheckBox(i18nc("@option:check", "Only send during this time range:"), this))
    , m_start(new QDateTimeEdit(this))
    , m_end(new QDateTimeEdit(this))
    , m_tabs(new QTabWidget(this))
    , m_outside(new QCheckBox(i18nc("@option:check", "Auto-reply to people outside my organization"), this))
    , m_contactsOnly(new QRadioButton(i18nc("@option:radio", "My contacts only"), this))
    , m_anyone(new QRadioButton(i18nc("@option:radio", "Anyone outside my organization"), this))
    , m_status(new QLabel(this))
    , m_buttons(new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this))
{
    setWindowTitle(i18nc("@title:window", "Automatic Replies"));

    // Only the own mailboxes: Graph does not give another mailbox's settings
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        if (box.own) {
            m_mailbox->addItem(box.address.isEmpty() ? box.label() : box.address, box.key());
        }
    }
    m_mailbox->setCurrentIndex(std::max(0, m_mailbox->findData(mailboxKey)));
    connect(m_mailbox, &QComboBox::currentIndexChanged, this, &AutoRepliesDialog::load);

    auto *onOff = new QButtonGroup(this);
    onOff->addButton(m_off);
    onOff->addButton(m_on);
    auto *outsideAudience = new QButtonGroup(this);
    outsideAudience->addButton(m_contactsOnly);
    outsideAudience->addButton(m_anyone);
    for (QDateTimeEdit *edit : {m_start, m_end}) {
        edit->setCalendarPopup(true);
        edit->setDisplayFormat(QLocale().dateTimeFormat(QLocale::ShortFormat));
    }
    // A sensible range to start from: today 17:00 to tomorrow 08:00
    const QDate today = QDate::currentDate();
    m_start->setDateTime(QDateTime(today, QTime(17, 0)));
    m_end->setDateTime(QDateTime(today.addDays(1), QTime(8, 0)));

    auto *range = new QFormLayout;
    range->setContentsMargins(24, 0, 0, 0);
    range->addRow(i18nc("@label:textbox", "Start time:"), m_start);
    range->addRow(i18nc("@label:textbox", "End time:"), m_end);

    auto *inside = new QWidget(m_tabs);
    auto *insideLayout = new QVBoxLayout(inside);
    m_inside = editor(inside, insideLayout);
    m_tabs->addTab(inside, i18nc("@title:tab", "Inside My Organization"));

    auto *outside = new QWidget(m_tabs);
    auto *outsideLayout = new QVBoxLayout(outside);
    outsideLayout->addWidget(m_outside);
    auto *audience = new QHBoxLayout;
    audience->setContentsMargins(24, 0, 0, 0);
    audience->addWidget(m_contactsOnly);
    audience->addWidget(m_anyone);
    audience->addStretch();
    outsideLayout->addLayout(audience);
    m_outsideText = editor(outside, outsideLayout);
    m_tabs->addTab(outside, i18nc("@title:tab", "Outside My Organization"));

    for (QAbstractButton *b : {static_cast<QAbstractButton *>(m_off), static_cast<QAbstractButton *>(m_on), static_cast<QAbstractButton *>(m_scheduled),
                               static_cast<QAbstractButton *>(m_outside)}) {
        connect(b, &QAbstractButton::toggled, this, &AutoRepliesDialog::updateEnabled);
    }
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    m_status->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    if (m_mailbox->count() > 1) {
        auto *form = new QFormLayout;
        form->addRow(i18nc("@label:listbox", "Mailbox:"), m_mailbox);
        layout->addLayout(form);
    } else {
        m_mailbox->hide();
    }
    layout->addWidget(m_off);
    layout->addWidget(m_on);
    auto *scheduled = new QVBoxLayout;
    scheduled->setContentsMargins(24, 0, 0, 0);
    scheduled->addWidget(m_scheduled);
    scheduled->addLayout(range);
    layout->addLayout(scheduled);
    layout->addWidget(m_tabs, 1);
    layout->addWidget(m_status);
    layout->addWidget(m_buttons);
    resize(640, 560);

    load();
}

KPIMTextEdit::RichTextComposer *AutoRepliesDialog::editor(QWidget *parent, QLayout *layout)
{
    auto *text = new KPIMTextEdit::RichTextComposer(parent);
    auto *actions = new KActionCollection(text);
    text->composerActions()->createActions(actions);
    text->activateRichText();
    auto *formatting = new QToolBar(parent);
    for (const char *name : {"format_text_bold", "format_text_italic", "format_text_underline", "format_text_foreground_color", "format_font_family", "format_font_size",
                             "manage_link"}) {
        if (QAction *action = actions->action(QLatin1String(name))) {
            formatting->addAction(action);
        }
    }
    layout->addWidget(formatting);
    layout->addWidget(text);
    return text;
}

void AutoRepliesDialog::load()
{
    m_loaded = false;
    updateEnabled();
    m_status->setText(i18nc("@info:status", "Reading the automatic replies..."));
    const QString key = m_mailbox->currentData().toString();
    m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("mailboxSettings"), QStringLiteral("{}"), this,
                    [this, key](const QString &json) {
                        if (key != m_mailbox->currentData().toString()) {
                            return;
                        }
                        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                        if (!answer.value(QLatin1String("ok")).toBool()) {
                            m_status->setText(accessProblemText(answer.isEmpty() ? QStringLiteral("network") : answer.value(QLatin1String("problem")).toString(), key));
                            return;
                        }
                        showReplies(AutomaticReplies::fromJson(answer.value(QLatin1String("body")).toObject().value(QLatin1String("automaticRepliesSetting")).toObject()));
                        qWarning().noquote() << "auto replies test: setting"
                                             << QString::fromUtf8(QJsonDocument(answer.value(QLatin1String("body")).toObject().value(QLatin1String("automaticRepliesSetting")).toObject())
                                                                      .toJson(QJsonDocument::Compact));
                        m_status->clear();
                        m_loaded = true;
                        updateEnabled();
                        Q_EMIT loadedForTests();
                    });
}

void AutoRepliesDialog::showReplies(const AutomaticReplies &replies)
{
    m_on->setChecked(replies.status != AutomaticReplies::Status::Disabled);
    m_off->setChecked(replies.status == AutomaticReplies::Status::Disabled);
    m_scheduled->setChecked(replies.status == AutomaticReplies::Status::Scheduled);
    // Exchange keeps the last range also when the replies are off or always on
    if (replies.start > 0 && replies.end > replies.start) {
        m_start->setDateTime(QDateTime::fromMSecsSinceEpoch(replies.start).toLocalTime());
        m_end->setDateTime(QDateTime::fromMSecsSinceEpoch(replies.end).toLocalTime());
    }
    m_outside->setChecked(replies.external != AutomaticReplies::External::None);
    m_contactsOnly->setChecked(replies.external == AutomaticReplies::External::ContactsOnly);
    m_anyone->setChecked(replies.external != AutomaticReplies::External::ContactsOnly);
    m_inside->setHtml(replies.internalMessage);
    m_outsideText->setHtml(replies.externalMessage);
}

AutomaticReplies AutoRepliesDialog::current() const
{
    AutomaticReplies r;
    if (m_on->isChecked()) {
        r.status = m_scheduled->isChecked() ? AutomaticReplies::Status::Scheduled : AutomaticReplies::Status::AlwaysEnabled;
    }
    r.external = !m_outside->isChecked() ? AutomaticReplies::External::None
        : m_contactsOnly->isChecked()    ? AutomaticReplies::External::ContactsOnly
                                         : AutomaticReplies::External::All;
    r.start = m_start->dateTime().toMSecsSinceEpoch();
    r.end = m_end->dateTime().toMSecsSinceEpoch();
    r.internalMessage = bodyOf(m_inside);
    r.externalMessage = bodyOf(m_outsideText);
    return r;
}

void AutoRepliesDialog::updateEnabled()
{
    const bool on = m_loaded && m_on->isChecked();
    m_off->setEnabled(m_loaded);
    m_on->setEnabled(m_loaded);
    m_scheduled->setEnabled(on);
    m_start->setEnabled(on && m_scheduled->isChecked());
    m_end->setEnabled(on && m_scheduled->isChecked());
    m_tabs->setEnabled(on);
    m_contactsOnly->setEnabled(m_outside->isChecked());
    m_anyone->setEnabled(m_outside->isChecked());
    m_outsideText->setEnabled(m_outside->isChecked());
    m_tabs->setTabText(1, m_outside->isChecked() ? i18nc("@title:tab", "Outside My Organization (On)") : i18nc("@title:tab", "Outside My Organization"));
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(m_loaded);
}

void AutoRepliesDialog::accept()
{
    if (m_on->isChecked() && m_scheduled->isChecked() && m_end->dateTime() <= m_start->dateTime()) {
        m_status->setText(i18nc("@info", "The end time must be after the start time."));
        return;
    }
    save(true);
}

void AutoRepliesDialog::save(bool close)
{
    const QString key = m_mailbox->currentData().toString();
    const QJsonObject setting = current().toJson();
    m_status->setText(i18nc("@info:status", "Saving..."));
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    const QJsonObject args{{QStringLiteral("settings"), QJsonObject{{QStringLiteral("automaticRepliesSetting"), setting}}}};
    m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("mailboxSettingsUpdate"),
                    QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this, [this, key, setting, close](const QString &json) {
                        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                        const bool ok = answer.value(QLatin1String("ok")).toBool();
                        qWarning() << "auto replies test:" << ok << answer.value(QLatin1String("detail")).toString();
                        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
                        if (!ok) {
                            m_status->setText(accessProblemText(answer.isEmpty() ? QStringLiteral("network") : answer.value(QLatin1String("problem")).toString(), key));
                            return;
                        }
                        m_status->clear();
                        Q_EMIT saved(key, setting);
                        if (close) {
                            QDialog::accept();
                        }
                    });
}

void AutoRepliesDialog::saveForTests(const QString &spec, const QString &text)
{
    const QString kind = spec.section(QLatin1Char('|'), 0, 0);
    m_on->setChecked(kind != QLatin1String("off"));
    m_off->setChecked(kind == QLatin1String("off"));
    m_scheduled->setChecked(kind == QLatin1String("scheduled"));
    if (kind == QLatin1String("scheduled")) {
        m_start->setDateTime(QDateTime::fromString(spec.section(QLatin1Char('|'), 1, 1), Qt::ISODate));
        m_end->setDateTime(QDateTime::fromString(spec.section(QLatin1Char('|'), 2, 2), Qt::ISODate));
    }
    if (text == QLatin1String("-")) {
        // Back to Exchange's default: no texts, outside replies to everyone
        m_inside->clear();
        m_outsideText->clear();
        m_outside->setChecked(true);
        m_anyone->setChecked(true);
    } else if (!text.isEmpty()) {
        m_inside->setPlainText(text);
        m_outside->setChecked(true);
        m_contactsOnly->setChecked(true);
        m_outsideText->setPlainText(text);
    }
    save(false);
}

}
