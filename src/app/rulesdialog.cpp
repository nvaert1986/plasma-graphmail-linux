// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "rulesdialog.h"
#include "accesstext.h"
#include "categorylist.h"
#include "daemonclient.h"
#include "mailboxset.h"
#include "movedialog.h"
#include "store.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace GraphMail
{

namespace
{
// The conditions and actions the editor shows; anything else in a rule is kept as it is
const char *const knownConditions[] = {"fromAddresses", "sentToMe", "sentOnlyToMe", "subjectContains", "bodyOrSubjectContains", "senderContains", "hasAttachments", "importance"};
const char *const knownActions[] = {"moveToFolder", "copyToFolder", "delete", "markAsRead", "markImportance", "assignCategories", "forwardTo", "stopProcessingRules"};

QStringList splitList(const QString &text)
{
    static const QRegularExpression separators(QStringLiteral("[;,]"));
    QStringList out;
    const QStringList parts = text.split(separators, Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        if (!p.trimmed().isEmpty()) {
            out.append(p.trimmed());
        }
    }
    return out;
}

QString joinStrings(const QJsonArray &array)
{
    QStringList out;
    for (const QJsonValue &v : array) {
        out.append(v.toString());
    }
    return out.join(QLatin1String("; "));
}

QJsonArray recipients(const QString &text)
{
    QJsonArray out;
    const QStringList addresses = splitList(text);
    for (const QString &a : addresses) {
        out.append(QJsonObject{{QStringLiteral("emailAddress"), QJsonObject{{QStringLiteral("address"), a}}}});
    }
    return out;
}

QString joinAddresses(const QJsonArray &array)
{
    QStringList out;
    for (const QJsonValue &v : array) {
        const QJsonObject address = v.toObject().value(QLatin1String("emailAddress")).toObject();
        const QString a = address.value(QLatin1String("address")).toString();
        out.append(a.isEmpty() ? address.value(QLatin1String("name")).toString() : a);
    }
    return out.join(QLatin1String("; "));
}

QJsonArray toArray(const QStringList &list)
{
    QJsonArray out;
    for (const QString &s : list) {
        out.append(s);
    }
    return out;
}

QString importanceText(const QString &value)
{
    if (value == QLatin1String("high")) {
        return i18nc("@item importance", "High");
    }
    if (value == QLatin1String("low")) {
        return i18nc("@item importance", "Low");
    }
    return i18nc("@item importance", "Normal");
}

QComboBox *importanceCombo(QWidget *parent)
{
    auto *combo = new QComboBox(parent);
    for (const char *value : {"high", "normal", "low"}) {
        combo->addItem(importanceText(QLatin1String(value)), QLatin1String(value));
    }
    return combo;
}

QString folderName(MailboxSet *mailboxes, const QString &mailboxKey, const QString &graphId)
{
    Store *store = mailboxes->store(mailboxKey);
    const std::optional<FolderRow> folder = store ? store->folderByGraphId(graphId) : std::nullopt;
    return folder ? folder->name : i18nc("@item a folder that is not known here", "(another folder)");
}

// A line: [x] label [field]
QCheckBox *addRow(QGridLayout *grid, const QString &label, QWidget *field = nullptr)
{
    auto *check = new QCheckBox(label, grid->parentWidget());
    const int row = grid->rowCount();
    grid->addWidget(check, row, 0);
    if (field) {
        grid->addWidget(field, row, 1);
        field->setEnabled(false);
        QObject::connect(check, &QCheckBox::toggled, field, &QWidget::setEnabled);
    } else {
        grid->addWidget(new QWidget(grid->parentWidget()), row, 1);
    }
    return check;
}
}

RuleEditor::RuleEditor(MailboxSet *mailboxes, FolderModel *folders, CategoryList *categories, const QString &mailboxKey, const QJsonObject &rule, QWidget *parent)
    : QDialog(parent)
    , m_mailboxes(mailboxes)
    , m_folders(folders)
    , m_mailboxKey(mailboxKey)
    , m_rule(rule)
    , m_name(new QLineEdit(this))
    , m_enabled(new QCheckBox(i18nc("@option:check", "Turn on this rule"), this))
    , m_status(new QLabel(this))
{
    setWindowTitle(rule.isEmpty() ? i18nc("@title:window", "New Rule") : i18nc("@title:window", "Change Rule"));
    const QJsonObject conditions = rule.value(QLatin1String("conditions")).toObject();
    const QJsonObject actions = rule.value(QLatin1String("actions")).toObject();

    auto *when = new QGroupBox(i18nc("@title:group", "When a message arrives that matches all of these"), this);
    auto *whenGrid = new QGridLayout(when);
    m_fromText = new QLineEdit(when);
    m_fromText->setPlaceholderText(i18nc("@info:placeholder", "Addresses, separated by ;"));
    m_from = addRow(whenGrid, i18nc("@option:check rule condition", "From:"), m_fromText);
    m_sentToMe = addRow(whenGrid, i18nc("@option:check rule condition", "My name is in the To box"));
    m_sentOnlyToMe = addRow(whenGrid, i18nc("@option:check rule condition", "Sent only to me"));
    m_subjectText = new QLineEdit(when);
    m_subjectText->setPlaceholderText(i18nc("@info:placeholder", "Words, separated by ;"));
    m_subject = addRow(whenGrid, i18nc("@option:check rule condition", "Subject contains:"), m_subjectText);
    m_bodyOrSubjectText = new QLineEdit(when);
    m_bodyOrSubjectText->setPlaceholderText(m_subjectText->placeholderText());
    m_bodyOrSubject = addRow(whenGrid, i18nc("@option:check rule condition", "Subject or body contains:"), m_bodyOrSubjectText);
    m_senderContainsText = new QLineEdit(when);
    m_senderContainsText->setPlaceholderText(m_subjectText->placeholderText());
    m_senderContains = addRow(whenGrid, i18nc("@option:check rule condition", "Sender address contains:"), m_senderContainsText);
    m_hasAttachments = addRow(whenGrid, i18nc("@option:check rule condition", "Has attachments"));
    m_importanceValue = importanceCombo(when);
    m_importance = addRow(whenGrid, i18nc("@option:check rule condition", "Importance:"), m_importanceValue);
    whenGrid->setColumnStretch(1, 1);

    auto *what = new QGroupBox(i18nc("@title:group", "Do the following"), this);
    auto *whatGrid = new QGridLayout(what);
    for (FolderChoice *choice : {&m_move, &m_copy}) {
        choice->button = new QPushButton(i18nc("@action:button", "Choose a Folder..."), what);
        connect(choice->button, &QPushButton::clicked, this, [this, choice] {
            chooseFolder(choice);
        });
    }
    m_move.check = addRow(whatGrid, i18nc("@option:check rule action", "Move to folder:"), m_move.button);
    m_copy.check = addRow(whatGrid, i18nc("@option:check rule action", "Copy to folder:"), m_copy.button);
    m_delete = addRow(whatGrid, i18nc("@option:check rule action", "Delete it"));
    m_markRead = addRow(whatGrid, i18nc("@option:check rule action", "Mark as read"));
    m_markImportanceValue = importanceCombo(what);
    m_markImportance = addRow(whatGrid, i18nc("@option:check rule action", "Set importance to:"), m_markImportanceValue);
    m_category = new QComboBox(what);
    const QList<CategoryList::Category> list = categories->categories(mailboxKey);
    for (const CategoryList::Category &c : list) {
        m_category->addItem(c.name);
    }
    m_categorize = addRow(whatGrid, i18nc("@option:check rule action", "Categorize as:"), m_category);
    m_forwardText = new QLineEdit(what);
    m_forwardText->setPlaceholderText(m_fromText->placeholderText());
    m_forward = addRow(whatGrid, i18nc("@option:check rule action", "Forward to:"), m_forwardText);
    m_stop = addRow(whatGrid, i18nc("@option:check rule action", "Stop processing more rules"));
    whatGrid->setColumnStretch(1, 1);

    // What the rule holds now
    m_name->setText(rule.value(QLatin1String("displayName")).toString());
    m_enabled->setChecked(rule.isEmpty() || rule.value(QLatin1String("isEnabled")).toBool());
    auto set = [](QCheckBox *check, QLineEdit *edit, const QString &text) {
        check->setChecked(!text.isEmpty());
        edit->setText(text);
    };
    set(m_from, m_fromText, joinAddresses(conditions.value(QLatin1String("fromAddresses")).toArray()));
    m_sentToMe->setChecked(conditions.value(QLatin1String("sentToMe")).toBool());
    m_sentOnlyToMe->setChecked(conditions.value(QLatin1String("sentOnlyToMe")).toBool());
    set(m_subject, m_subjectText, joinStrings(conditions.value(QLatin1String("subjectContains")).toArray()));
    set(m_bodyOrSubject, m_bodyOrSubjectText, joinStrings(conditions.value(QLatin1String("bodyOrSubjectContains")).toArray()));
    set(m_senderContains, m_senderContainsText, joinStrings(conditions.value(QLatin1String("senderContains")).toArray()));
    m_hasAttachments->setChecked(conditions.value(QLatin1String("hasAttachments")).toBool());
    if (conditions.contains(QLatin1String("importance"))) {
        m_importance->setChecked(true);
        m_importanceValue->setCurrentIndex(std::max(0, m_importanceValue->findData(conditions.value(QLatin1String("importance")).toString())));
    }
    m_move.graphId = actions.value(QLatin1String("moveToFolder")).toString();
    m_copy.graphId = actions.value(QLatin1String("copyToFolder")).toString();
    for (FolderChoice *choice : {&m_move, &m_copy}) {
        choice->check->setChecked(!choice->graphId.isEmpty());
        showFolder(choice);
    }
    m_delete->setChecked(actions.value(QLatin1String("delete")).toBool());
    m_markRead->setChecked(actions.value(QLatin1String("markAsRead")).toBool());
    if (actions.contains(QLatin1String("markImportance"))) {
        m_markImportance->setChecked(true);
        m_markImportanceValue->setCurrentIndex(std::max(0, m_markImportanceValue->findData(actions.value(QLatin1String("markImportance")).toString())));
    }
    const QJsonArray assigned = actions.value(QLatin1String("assignCategories")).toArray();
    if (!assigned.isEmpty()) {
        const QString name = assigned.first().toString();
        if (m_category->findText(name) < 0) {
            m_category->addItem(name);
        }
        m_category->setCurrentText(name);
        m_categorize->setChecked(true);
    }
    set(m_forward, m_forwardText, joinAddresses(actions.value(QLatin1String("forwardTo")).toArray()));
    m_stop->setChecked(actions.value(QLatin1String("stopProcessingRules")).toBool());

    // Made in Outlook with more than the editor shows: say that it is kept
    bool more = !rule.value(QLatin1String("exceptions")).toObject().isEmpty() || assigned.size() > 1;
    for (auto it = conditions.begin(); it != conditions.end(); ++it) {
        more |= std::none_of(std::begin(knownConditions), std::end(knownConditions), [&it](const char *k) {
            return it.key() == QLatin1String(k);
        });
    }
    for (auto it = actions.begin(); it != actions.end(); ++it) {
        more |= std::none_of(std::begin(knownActions), std::end(knownActions), [&it](const char *k) {
            return it.key() == QLatin1String(k);
        });
    }
    auto *note = new QLabel(this);
    note->setWordWrap(true);
    if (rule.value(QLatin1String("isReadOnly")).toBool()) {
        note->setText(i18nc("@info", "This rule can only be changed in Outlook."));
    } else if (more) {
        note->setText(i18nc("@info", "This rule also has conditions, exceptions or actions that are not shown here; they are kept."));
    }
    note->setVisible(!note->text().isEmpty());
    m_status->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(!rule.value(QLatin1String("isReadOnly")).toBool());
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(i18nc("@label:textbox", "Name:"), this));
    top->addWidget(m_name, 1);
    top->addWidget(m_enabled);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(when);
    layout->addWidget(what);
    layout->addWidget(note);
    layout->addWidget(m_status);
    layout->addStretch();
    layout->addWidget(buttons);
    resize(620, sizeHint().height());
}

void RuleEditor::chooseFolder(FolderChoice *choice)
{
    MoveDialog dialog(m_folders, m_mailboxKey, this);
    dialog.setWindowTitle(i18nc("@title:window", "Choose a Folder"));
    if (dialog.exec() != QDialog::Accepted || dialog.folder() == 0) {
        return;
    }
    Store *store = m_mailboxes->store(m_mailboxKey);
    const std::optional<FolderRow> folder = store ? store->folder(dialog.folder()) : std::nullopt;
    if (folder) {
        choice->graphId = folder->graphId;
        choice->check->setChecked(true);
        showFolder(choice);
    }
}

void RuleEditor::showFolder(FolderChoice *choice)
{
    choice->button->setText(choice->graphId.isEmpty() ? i18nc("@action:button", "Choose a Folder...") : folderName(m_mailboxes, m_mailboxKey, choice->graphId));
}

QJsonObject RuleEditor::rule() const
{
    QJsonObject conditions = m_rule.value(QLatin1String("conditions")).toObject();
    QJsonObject actions = m_rule.value(QLatin1String("actions")).toObject();
    for (const char *k : knownConditions) {
        conditions.remove(QLatin1String(k));
    }
    for (const char *k : knownActions) {
        actions.remove(QLatin1String(k));
    }
    if (m_from->isChecked() && !splitList(m_fromText->text()).isEmpty()) {
        conditions.insert(QStringLiteral("fromAddresses"), recipients(m_fromText->text()));
    }
    if (m_sentToMe->isChecked()) {
        conditions.insert(QStringLiteral("sentToMe"), true);
    }
    if (m_sentOnlyToMe->isChecked()) {
        conditions.insert(QStringLiteral("sentOnlyToMe"), true);
    }
    const std::pair<QCheckBox *, QLineEdit *> lists[] = {{m_subject, m_subjectText}, {m_bodyOrSubject, m_bodyOrSubjectText}, {m_senderContains, m_senderContainsText}};
    const char *const listKeys[] = {"subjectContains", "bodyOrSubjectContains", "senderContains"};
    for (int i = 0; i < 3; ++i) {
        if (lists[i].first->isChecked() && !splitList(lists[i].second->text()).isEmpty()) {
            conditions.insert(QLatin1String(listKeys[i]), toArray(splitList(lists[i].second->text())));
        }
    }
    if (m_hasAttachments->isChecked()) {
        conditions.insert(QStringLiteral("hasAttachments"), true);
    }
    if (m_importance->isChecked()) {
        conditions.insert(QStringLiteral("importance"), m_importanceValue->currentData().toString());
    }
    if (m_move.check->isChecked() && !m_move.graphId.isEmpty()) {
        actions.insert(QStringLiteral("moveToFolder"), m_move.graphId);
    }
    if (m_copy.check->isChecked() && !m_copy.graphId.isEmpty()) {
        actions.insert(QStringLiteral("copyToFolder"), m_copy.graphId);
    }
    if (m_delete->isChecked()) {
        actions.insert(QStringLiteral("delete"), true);
    }
    if (m_markRead->isChecked()) {
        actions.insert(QStringLiteral("markAsRead"), true);
    }
    if (m_markImportance->isChecked()) {
        actions.insert(QStringLiteral("markImportance"), m_markImportanceValue->currentData().toString());
    }
    if (m_categorize->isChecked() && !m_category->currentText().isEmpty()) {
        // Outlook may have given more than one: those after the first stay
        QJsonArray assigned = m_rule.value(QLatin1String("actions")).toObject().value(QLatin1String("assignCategories")).toArray();
        if (assigned.isEmpty()) {
            assigned.append(m_category->currentText());
        } else {
            assigned[0] = m_category->currentText();
        }
        actions.insert(QStringLiteral("assignCategories"), assigned);
    }
    if (m_forward->isChecked() && !splitList(m_forwardText->text()).isEmpty()) {
        actions.insert(QStringLiteral("forwardTo"), recipients(m_forwardText->text()));
    }
    if (m_stop->isChecked()) {
        actions.insert(QStringLiteral("stopProcessingRules"), true);
    }
    QJsonObject out{{QStringLiteral("displayName"), m_name->text().trimmed()},
                    {QStringLiteral("isEnabled"), m_enabled->isChecked()},
                    {QStringLiteral("conditions"), conditions},
                    {QStringLiteral("actions"), actions}};
    if (m_rule.contains(QLatin1String("exceptions"))) {
        out.insert(QStringLiteral("exceptions"), m_rule.value(QLatin1String("exceptions")));
    }
    return out;
}

void RuleEditor::accept()
{
    if (m_name->text().trimmed().isEmpty()) {
        m_status->setText(i18nc("@info", "Give the rule a name."));
        return;
    }
    if ((m_move.check->isChecked() && m_move.graphId.isEmpty()) || (m_copy.check->isChecked() && m_copy.graphId.isEmpty())) {
        m_status->setText(i18nc("@info", "Choose the folder."));
        return;
    }
    if (rule().value(QLatin1String("actions")).toObject().isEmpty()) {
        m_status->setText(i18nc("@info", "Choose at least one thing to do."));
        return;
    }
    QDialog::accept();
}

void RuleEditor::fillForTests(const QString &name, const QString &subject)
{
    m_name->setText(name);
    m_subject->setChecked(true);
    m_subjectText->setText(subject);
    m_markRead->setChecked(true);
    m_markImportance->setChecked(true);
    m_markImportanceValue->setCurrentIndex(m_markImportanceValue->findData(QStringLiteral("high")));
}

RulesDialog::RulesDialog(MailboxSet *mailboxes, DaemonClient *daemon, FolderModel *folders, CategoryList *categories, const QString &mailboxKey, QWidget *parent)
    : QDialog(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
    , m_folders(folders)
    , m_categories(categories)
    , m_mailbox(new QComboBox(this))
    , m_list(new QTreeWidget(this))
    , m_new(new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), i18nc("@action:button", "New Rule..."), this))
    , m_change(new QPushButton(QIcon::fromTheme(QStringLiteral("document-edit")), i18nc("@action:button", "Change..."), this))
    , m_delete(new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action:button", "Delete"), this))
    , m_up(new QPushButton(QIcon::fromTheme(QStringLiteral("go-up")), i18nc("@action:button", "Move Up"), this))
    , m_down(new QPushButton(QIcon::fromTheme(QStringLiteral("go-down")), i18nc("@action:button", "Move Down"), this))
    , m_status(new QLabel(this))
{
    setWindowTitle(i18nc("@title:window", "Rules"));
    // Only the own mailboxes: Graph does not give another mailbox's rules
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        if (box.own) {
            m_mailbox->addItem(box.address.isEmpty() ? box.label() : box.address, box.key());
        }
    }
    m_mailbox->setCurrentIndex(std::max(0, m_mailbox->findData(mailboxKey)));
    connect(m_mailbox, &QComboBox::currentIndexChanged, this, &RulesDialog::load);

    m_list->setHeaderLabels({i18nc("@title:column", "Rule"), i18nc("@title:column", "What it does")});
    m_list->setRootIsDecorated(false);
    m_list->setColumnWidth(0, 240);
    m_list->header()->setStretchLastSection(true);
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &RulesDialog::updateButtons);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] {
        edit(selected());
    });
    connect(m_list, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        if (m_filling || column != 0) {
            return;
        }
        const QJsonObject rule = m_rules.at(m_list->indexOfTopLevelItem(item)).toObject();
        const bool on = item->checkState(0) == Qt::Checked;
        if (on != rule.value(QLatin1String("isEnabled")).toBool()) {
            call(QStringLiteral("messageRuleUpdate"),
                 {{QStringLiteral("id"), rule.value(QLatin1String("id"))}, {QStringLiteral("rule"), QJsonObject{{QStringLiteral("isEnabled"), on}}}});
        }
    });
    connect(m_new, &QPushButton::clicked, this, [this] {
        edit({});
    });
    connect(m_change, &QPushButton::clicked, this, [this] {
        edit(selected());
    });
    connect(m_delete, &QPushButton::clicked, this, [this] {
        remove(selected(), true);
    });
    connect(m_up, &QPushButton::clicked, this, [this] {
        move(selected(), -1);
    });
    connect(m_down, &QPushButton::clicked, this, [this] {
        move(selected(), 1);
    });

    auto *side = new QVBoxLayout;
    for (QPushButton *b : {m_new, m_change, m_delete, m_up, m_down}) {
        side->addWidget(b);
    }
    side->addStretch();
    auto *middle = new QHBoxLayout;
    middle->addWidget(m_list, 1);
    middle->addLayout(side);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    m_status->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    if (m_mailbox->count() > 1) {
        auto *top = new QHBoxLayout;
        top->addWidget(new QLabel(i18nc("@label:listbox", "Mailbox:"), this));
        top->addWidget(m_mailbox, 1);
        layout->addLayout(top);
    } else {
        m_mailbox->hide();
    }
    auto *intro = new QLabel(i18nc("@info", "Microsoft 365 applies these rules to new mail in the Inbox, in this order, also while Graph Mail is closed."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    layout->addLayout(middle, 1);
    layout->addWidget(m_status);
    layout->addWidget(buttons);
    resize(820, 440);
    load();
}

QString RulesDialog::summary(const QJsonObject &rule, MailboxSet *mailboxes, const QString &mailboxKey)
{
    const QJsonObject c = rule.value(QLatin1String("conditions")).toObject();
    const QJsonObject a = rule.value(QLatin1String("actions")).toObject();
    QStringList when;
    if (c.contains(QLatin1String("fromAddresses"))) {
        when.append(i18nc("@item rule condition, %1 addresses", "From %1", joinAddresses(c.value(QLatin1String("fromAddresses")).toArray())));
    }
    if (c.value(QLatin1String("sentToMe")).toBool()) {
        when.append(i18nc("@item rule condition", "my name in To"));
    }
    if (c.value(QLatin1String("sentOnlyToMe")).toBool()) {
        when.append(i18nc("@item rule condition", "sent only to me"));
    }
    if (c.contains(QLatin1String("subjectContains"))) {
        when.append(i18nc("@item rule condition, %1 words", "subject contains %1", joinStrings(c.value(QLatin1String("subjectContains")).toArray())));
    }
    if (c.contains(QLatin1String("bodyOrSubjectContains"))) {
        when.append(i18nc("@item rule condition, %1 words", "subject or body contains %1", joinStrings(c.value(QLatin1String("bodyOrSubjectContains")).toArray())));
    }
    if (c.contains(QLatin1String("senderContains"))) {
        when.append(i18nc("@item rule condition, %1 words", "sender contains %1", joinStrings(c.value(QLatin1String("senderContains")).toArray())));
    }
    if (c.value(QLatin1String("hasAttachments")).toBool()) {
        when.append(i18nc("@item rule condition", "has attachments"));
    }
    if (c.contains(QLatin1String("importance"))) {
        when.append(i18nc("@item rule condition, %1 high/normal/low", "importance %1", importanceText(c.value(QLatin1String("importance")).toString()).toLower()));
    }
    QStringList what;
    if (a.contains(QLatin1String("moveToFolder"))) {
        what.append(i18nc("@item rule action, %1 a folder", "move to %1", folderName(mailboxes, mailboxKey, a.value(QLatin1String("moveToFolder")).toString())));
    }
    if (a.contains(QLatin1String("copyToFolder"))) {
        what.append(i18nc("@item rule action, %1 a folder", "copy to %1", folderName(mailboxes, mailboxKey, a.value(QLatin1String("copyToFolder")).toString())));
    }
    if (a.value(QLatin1String("delete")).toBool() || a.value(QLatin1String("permanentDelete")).toBool()) {
        what.append(i18nc("@item rule action", "delete"));
    }
    if (a.value(QLatin1String("markAsRead")).toBool()) {
        what.append(i18nc("@item rule action", "mark as read"));
    }
    if (a.contains(QLatin1String("markImportance"))) {
        what.append(i18nc("@item rule action, %1 high/normal/low", "importance %1", importanceText(a.value(QLatin1String("markImportance")).toString()).toLower()));
    }
    if (a.contains(QLatin1String("assignCategories"))) {
        what.append(i18nc("@item rule action, %1 categories", "categorize as %1", joinStrings(a.value(QLatin1String("assignCategories")).toArray())));
    }
    for (const char *key : {"forwardTo", "forwardAsAttachmentTo", "redirectTo"}) {
        if (a.contains(QLatin1String(key))) {
            what.append(i18nc("@item rule action, %1 addresses", "forward to %1", joinAddresses(a.value(QLatin1String(key)).toArray())));
        }
    }
    if (a.value(QLatin1String("stopProcessingRules")).toBool()) {
        what.append(i18nc("@item rule action", "stop processing more rules"));
    }
    const QString conditions = when.isEmpty() ? i18nc("@item rule without conditions", "every message") : when.join(QLatin1String(", "));
    return i18nc("@item a rule, %1 its conditions, %2 its actions", "%1: %2", conditions, what.join(QLatin1String(", ")));
}

void RulesDialog::load()
{
    m_status->setText(i18nc("@info:status", "Reading the rules..."));
    m_new->setEnabled(false);
    const QString key = m_mailbox->currentData().toString();
    m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("messageRules"), QStringLiteral("{}"), this,
                    [this, key](const QString &json) {
                        if (key != m_mailbox->currentData().toString()) {
                            return;
                        }
                        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                        if (!answer.value(QLatin1String("ok")).toBool()) {
                            m_status->setText(accessProblemText(answer.isEmpty() ? QStringLiteral("network") : answer.value(QLatin1String("problem")).toString(), key));
                            return;
                        }
                        QList<QJsonObject> rules;
                        const QJsonArray value = answer.value(QLatin1String("body")).toObject().value(QLatin1String("value")).toArray();
                        for (const QJsonValue &v : value) {
                            rules.append(v.toObject());
                        }
                        std::stable_sort(rules.begin(), rules.end(), [](const QJsonObject &a, const QJsonObject &b) {
                            return a.value(QLatin1String("sequence")).toInt() < b.value(QLatin1String("sequence")).toInt();
                        });
                        m_rules = QJsonArray();
                        for (const QJsonObject &r : std::as_const(rules)) {
                            m_rules.append(r);
                        }
                        m_status->clear();
                        m_new->setEnabled(true);
                        fill();
                        Q_EMIT loadedForTests();
                    });
}

void RulesDialog::fill()
{
    const QString current = selected().value(QLatin1String("id")).toString();
    m_filling = true;
    m_list->clear();
    const QString key = m_mailbox->currentData().toString();
    for (const QJsonValue &v : std::as_const(m_rules)) {
        const QJsonObject rule = v.toObject();
        auto *item = new QTreeWidgetItem(m_list, {rule.value(QLatin1String("displayName")).toString(), summary(rule, m_mailboxes, key)});
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, rule.value(QLatin1String("isEnabled")).toBool() ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(1, item->text(1));
        if (rule.value(QLatin1String("hasError")).toBool()) {
            item->setIcon(0, QIcon::fromTheme(QStringLiteral("dialog-warning")));
            item->setToolTip(0, i18nc("@info:tooltip", "Exchange reports an error in this rule (for example a folder that no longer exists)"));
        }
        if (rule.value(QLatin1String("id")).toString() == current) {
            m_list->setCurrentItem(item);
        }
    }
    m_filling = false;
    updateButtons();
}

QJsonObject RulesDialog::selected() const
{
    const int row = m_list->currentItem() ? m_list->indexOfTopLevelItem(m_list->currentItem()) : -1;
    return row >= 0 && row < m_rules.size() ? m_rules.at(row).toObject() : QJsonObject();
}

QJsonObject RulesDialog::byName(const QString &name) const
{
    for (const QJsonValue &v : m_rules) {
        if (v.toObject().value(QLatin1String("displayName")).toString() == name) {
            return v.toObject();
        }
    }
    return {};
}

void RulesDialog::updateButtons()
{
    const int row = m_list->currentItem() ? m_list->indexOfTopLevelItem(m_list->currentItem()) : -1;
    m_change->setEnabled(row >= 0);
    m_delete->setEnabled(row >= 0);
    m_up->setEnabled(row > 0);
    m_down->setEnabled(row >= 0 && row < m_rules.size() - 1);
}

void RulesDialog::call(const QString &kind, const QJsonObject &args)
{
    m_status->setText(i18nc("@info:status", "Saving..."));
    const QString key = m_mailbox->currentData().toString();
    m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), kind, QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this,
                    [this, key, kind](const QString &json) {
                        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                        const bool ok = answer.value(QLatin1String("ok")).toBool();
                        qWarning() << "rules test:" << kind << ok << answer.value(QLatin1String("detail")).toString();
                        load();
                        if (!ok) {
                            const QString detail = answer.value(QLatin1String("detail")).toString();
                            const QString problem = answer.value(QLatin1String("problem")).toString();
                            // Exchange's own reason (a rule it does not accept) says more than "error"
                            m_status->setText(problem == QLatin1String("error") && !detail.isEmpty()
                                                  ? i18nc("@info %1 Exchange's reason", "Microsoft 365 did not accept the rule: %1", detail)
                                                  : accessProblemText(answer.isEmpty() ? QStringLiteral("network") : problem, key));
                        }
                    });
}

void RulesDialog::edit(const QJsonObject &rule, bool forTests, const QString &name, const QString &subject)
{
    RuleEditor editor(m_mailboxes, m_folders, m_categories, m_mailbox->currentData().toString(), rule, this);
    if (forTests) {
        editor.fillForTests(name, subject);
    } else if (editor.exec() != QDialog::Accepted) {
        return;
    }
    QJsonObject out = editor.rule();
    if (rule.isEmpty()) {
        // New rules go last, as in Outlook
        int last = 0;
        for (const QJsonValue &v : std::as_const(m_rules)) {
            last = std::max(last, v.toObject().value(QLatin1String("sequence")).toInt());
        }
        out.insert(QStringLiteral("sequence"), last + 1);
        call(QStringLiteral("messageRuleCreate"), {{QStringLiteral("rule"), out}});
    } else {
        call(QStringLiteral("messageRuleUpdate"), {{QStringLiteral("id"), rule.value(QLatin1String("id"))}, {QStringLiteral("rule"), out}});
    }
}

void RulesDialog::move(const QJsonObject &rule, int step)
{
    int row = -1;
    for (int i = 0; i < m_rules.size(); ++i) {
        if (m_rules.at(i).toObject().value(QLatin1String("id")) == rule.value(QLatin1String("id"))) {
            row = i;
        }
    }
    const int other = row + step;
    if (row < 0 || other < 0 || other >= m_rules.size()) {
        return;
    }
    // Swap the two sequence numbers (equal ones get distinct ones)
    const QJsonObject neighbour = m_rules.at(other).toObject();
    int mine = rule.value(QLatin1String("sequence")).toInt();
    int theirs = neighbour.value(QLatin1String("sequence")).toInt();
    if (mine == theirs) {
        theirs = mine + step;
    }
    call(QStringLiteral("messageRuleUpdate"), {{QStringLiteral("id"), rule.value(QLatin1String("id"))}, {QStringLiteral("rule"), QJsonObject{{QStringLiteral("sequence"), theirs}}}});
    call(QStringLiteral("messageRuleUpdate"), {{QStringLiteral("id"), neighbour.value(QLatin1String("id"))}, {QStringLiteral("rule"), QJsonObject{{QStringLiteral("sequence"), mine}}}});
}

void RulesDialog::remove(const QJsonObject &rule, bool ask)
{
    if (rule.isEmpty()) {
        return;
    }
    if (ask
        && KMessageBox::warningContinueCancel(this, i18nc("@info %1 a rule", "Delete the rule %1?", rule.value(QLatin1String("displayName")).toString()),
                                              i18nc("@title:window", "Delete Rule"), KStandardGuiItem::del())
            != KMessageBox::Continue) {
        return;
    }
    call(QStringLiteral("messageRuleDelete"), {{QStringLiteral("id"), rule.value(QLatin1String("id"))}});
}

RuleEditor *RulesDialog::editorForTests(const QString &name, const QString &subject)
{
    auto *editor = new RuleEditor(m_mailboxes, m_folders, m_categories, m_mailbox->currentData().toString(), {}, this);
    editor->setAttribute(Qt::WA_DeleteOnClose);
    editor->fillForTests(name, subject);
    editor->show();
    return editor;
}

void RulesDialog::runForTests(const QString &spec)
{
    const QString action = spec.section(QLatin1Char('|'), 0, 0);
    const QString name = spec.section(QLatin1Char('|'), 1, 1);
    if (action == QLatin1String("create")) {
        edit({}, true, name, spec.section(QLatin1Char('|'), 2));
    } else if (action == QLatin1String("toggle")) {
        const QJsonObject rule = byName(name);
        call(QStringLiteral("messageRuleUpdate"),
             {{QStringLiteral("id"), rule.value(QLatin1String("id"))}, {QStringLiteral("rule"), QJsonObject{{QStringLiteral("isEnabled"), !rule.value(QLatin1String("isEnabled")).toBool()}}}});
    } else if (action == QLatin1String("up")) {
        move(byName(name), -1);
    } else if (action == QLatin1String("delete")) {
        remove(byName(name), false);
    }
}

}
