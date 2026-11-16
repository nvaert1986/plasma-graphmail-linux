// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "conditionalformat.h"

#include <KColorButton>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace GraphMail::ConditionalFormat
{

namespace
{
KConfigGroup group()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("ConditionalFormatting"));
}
}

bool Rule::matches(const QString &senderName, const QString &senderAddress, const QString &subject) const
{
    const QString wanted = text.trimmed();
    if (!enabled || wanted.isEmpty()) {
        return false;
    }
    if (field == QLatin1String("subject")) {
        return subject.contains(wanted, Qt::CaseInsensitive);
    }
    return senderName.contains(wanted, Qt::CaseInsensitive) || senderAddress.contains(wanted, Qt::CaseInsensitive);
}

QList<Rule> rules()
{
    QList<Rule> list;
    const QJsonArray array = QJsonDocument::fromJson(group().readEntry("Rules", QString()).toUtf8()).array();
    for (const QJsonValue &v : array) {
        const QJsonObject o = v.toObject();
        Rule r;
        r.name = o.value(QLatin1String("name")).toString();
        r.field = o.value(QLatin1String("field")).toString(QStringLiteral("sender"));
        r.text = o.value(QLatin1String("text")).toString();
        r.color = QColor(o.value(QLatin1String("color")).toString());
        r.enabled = o.value(QLatin1String("enabled")).toBool(true);
        if (r.color.isValid()) {
            list.append(r);
        }
    }
    return list;
}

void setRules(const QList<Rule> &list)
{
    QJsonArray array;
    for (const Rule &r : list) {
        array.append(QJsonObject{{QStringLiteral("name"), r.name},
                                 {QStringLiteral("field"), r.field},
                                 {QStringLiteral("text"), r.text},
                                 {QStringLiteral("color"), r.color.name()},
                                 {QStringLiteral("enabled"), r.enabled}});
    }
    KConfigGroup g = group();
    g.writeEntry("Rules", QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
    g.sync();
}

std::optional<QColor> colorFor(const QList<Rule> &list, const QString &senderName, const QString &senderAddress, const QString &subject)
{
    for (const Rule &r : list) {
        if (r.matches(senderName, senderAddress, subject)) {
            return r.color;
        }
    }
    return std::nullopt;
}

Dialog::Dialog(QWidget *parent)
    : QDialog(parent)
    , m_rules(rules())
    , m_list(new QListWidget(this))
    , m_name(new QLineEdit(this))
    , m_field(new QComboBox(this))
    , m_text(new QLineEdit(this))
    , m_color(new KColorButton(this))
    , m_enabled(new QCheckBox(i18nc("@option:check", "On"), this))
{
    setWindowTitle(i18nc("@title:window", "Conditional Formatting"));
    resize(560, 420);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(i18nc("@info", "Messages that match a rule get its colour in the message list. The first rule that matches wins."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *top = new QHBoxLayout;
    top->addWidget(m_list, 1);
    auto *side = new QVBoxLayout;
    auto *add = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), i18nc("@action:button", "Add"), this);
    auto *remove = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), i18nc("@action:button", "Remove"), this);
    side->addWidget(add);
    side->addWidget(remove);
    side->addStretch();
    top->addLayout(side);
    layout->addLayout(top, 1);
    m_field->addItem(i18nc("@item:inlistbox condition", "Sender contains"), QStringLiteral("sender"));
    m_field->addItem(i18nc("@item:inlistbox condition", "Subject contains"), QStringLiteral("subject"));
    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:textbox", "Name:"), m_name);
    auto *condition = new QHBoxLayout;
    condition->addWidget(m_field);
    condition->addWidget(m_text, 1);
    form->addRow(i18nc("@label", "Condition:"), condition);
    form->addRow(i18nc("@label:chooser", "Colour:"), m_color);
    form->addRow(QString(), m_enabled);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(add, &QPushButton::clicked, this, [this] {
        storeRule();
        Rule r;
        r.name = i18nc("@item a new conditional formatting rule", "New rule");
        m_rules.append(r);
        refreshList();
        m_list->setCurrentRow(int(m_rules.size()) - 1);
        m_text->setFocus();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = m_list->currentRow();
        if (row < 0 || row >= m_rules.size()) {
            return;
        }
        m_shown = -1;
        m_rules.removeAt(row);
        refreshList();
        m_list->setCurrentRow(std::min(row, int(m_rules.size()) - 1));
    });
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        storeRule();
        showRule(row);
    });
    // The list follows the name and colour while they are edited
    connect(m_name, &QLineEdit::textEdited, this, [this] {
        storeRule();
        if (QListWidgetItem *item = m_list->item(m_shown)) {
            item->setText(m_name->text());
        }
    });
    connect(m_color, &KColorButton::changed, this, [this] {
        storeRule();
        refreshList();
    });
    refreshList();
    m_list->setCurrentRow(m_rules.isEmpty() ? -1 : 0);
    showRule(m_list->currentRow());
}

void Dialog::refreshList()
{
    const QSignalBlocker blocker(m_list);
    const int row = m_list->currentRow();
    m_list->clear();
    for (const Rule &r : std::as_const(m_rules)) {
        QPixmap swatch(12, 12);
        swatch.fill(r.color);
        m_list->addItem(new QListWidgetItem(QIcon(swatch), r.name));
    }
    m_list->setCurrentRow(row);
}

void Dialog::showRule(int row)
{
    m_shown = row;
    const bool valid = row >= 0 && row < m_rules.size();
    for (QWidget *w : std::initializer_list<QWidget *>{m_name, m_field, m_text, m_color, m_enabled}) {
        w->setEnabled(valid);
    }
    const Rule r = valid ? m_rules.at(row) : Rule();
    const QSignalBlocker b1(m_color);
    m_name->setText(r.name);
    m_field->setCurrentIndex(std::max(0, m_field->findData(r.field)));
    m_text->setText(r.text);
    m_color->setColor(r.color);
    m_enabled->setChecked(r.enabled);
}

void Dialog::storeRule()
{
    if (m_shown < 0 || m_shown >= m_rules.size()) {
        return;
    }
    Rule &r = m_rules[m_shown];
    r.name = m_name->text();
    r.field = m_field->currentData().toString();
    r.text = m_text->text();
    r.color = m_color->color();
    r.enabled = m_enabled->isChecked();
}

void Dialog::accept()
{
    storeRule();
    setRules(m_rules);
    QDialog::accept();
}

}
