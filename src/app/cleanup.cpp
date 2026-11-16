// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cleanup.h"
#include "mailboxset.h"
#include "store.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace GraphMail::Cleanup
{

namespace
{
KConfigGroup group()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Cleanup"));
}

QString actionName(Action action)
{
    switch (action) {
    case Action::ExportPst:
        return QStringLiteral("pst");
    case Action::Delete:
        return QStringLiteral("delete");
    case Action::Report:
        return QStringLiteral("report");
    case Action::Archive:
        break;
    }
    return QStringLiteral("archive");
}

Action actionOf(const QString &name)
{
    if (name == QLatin1String("pst")) {
        return Action::ExportPst;
    }
    if (name == QLatin1String("delete")) {
        return Action::Delete;
    }
    return name == QLatin1String("report") ? Action::Report : Action::Archive;
}

void fillActions(QComboBox *box)
{
    for (Action a : {Action::Archive, Action::ExportPst, Action::Delete, Action::Report}) {
        box->addItem(actionText(a), int(a));
    }
}

QSpinBox *daysBox(QWidget *parent)
{
    auto *box = new QSpinBox(parent);
    box->setRange(1, 3650);
    box->setSuffix(i18nc("@item:valuesuffix days", " days"));
    return box;
}

QString ruleKey(const QString &mailboxKey, const QString &graphId)
{
    return QStringLiteral("Folder|") + mailboxKey + QLatin1Char('|') + graphId;
}
}

QString actionText(Action action)
{
    switch (action) {
    case Action::ExportPst:
        return i18nc("@item:inlistbox cleanup action", "Export to an Outlook data file (.pst), then remove");
    case Action::Delete:
        return i18nc("@item:inlistbox cleanup action", "Delete (to Deleted Items)");
    case Action::Report:
        return i18nc("@item:inlistbox cleanup action", "Only report");
    case Action::Archive:
        break;
    }
    return i18nc("@item:inlistbox cleanup action", "Move to the Archive folder");
}

Settings settings()
{
    const KConfigGroup g = group();
    Settings s;
    s.enabled = g.readEntry("Enabled", false);
    s.action = actionOf(g.readEntry("Action", QString()));
    s.days = std::clamp(g.readEntry("Days", 365), 1, 3650);
    s.everyDays = std::clamp(g.readEntry("EveryDays", 0), 0, 365);
    s.pstFolder = g.readEntry("PstFolder", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    s.lastRun = g.readEntry("LastRun", qint64(0));
    return s;
}

void setSettings(const Settings &s)
{
    KConfigGroup g = group();
    g.writeEntry("Enabled", s.enabled);
    g.writeEntry("Action", actionName(s.action));
    g.writeEntry("Days", s.days);
    g.writeEntry("EveryDays", s.everyDays);
    g.writeEntry("PstFolder", s.pstFolder);
    g.sync();
}

void setLastRun(qint64 ms)
{
    KConfigGroup g = group();
    g.writeEntry("LastRun", ms);
    g.sync();
}

FolderRule folderRule(const QString &mailboxKey, const QString &graphId)
{
    // "default", "off" or "own|<action>|<days>"
    const QStringList parts = group().readEntry(ruleKey(mailboxKey, graphId), QString()).split(QLatin1Char('|'));
    FolderRule rule;
    if (parts.value(0) == QLatin1String("off")) {
        rule.mode = FolderRule::Mode::Off;
    } else if (parts.value(0) == QLatin1String("own")) {
        rule.mode = FolderRule::Mode::Own;
        rule.action = actionOf(parts.value(1));
        rule.days = std::clamp(parts.value(2).toInt(), 1, 3650);
    }
    return rule;
}

void setFolderRule(const QString &mailboxKey, const QString &graphId, const FolderRule &rule)
{
    KConfigGroup g = group();
    switch (rule.mode) {
    case FolderRule::Mode::Default:
        g.deleteEntry(ruleKey(mailboxKey, graphId));
        break;
    case FolderRule::Mode::Off:
        g.writeEntry(ruleKey(mailboxKey, graphId), QStringLiteral("off"));
        break;
    case FolderRule::Mode::Own:
        g.writeEntry(ruleKey(mailboxKey, graphId), QStringLiteral("own|%1|%2").arg(actionName(rule.action)).arg(rule.days));
        break;
    }
    g.sync();
}

bool excluded(const QString &wellKnown)
{
    static const QStringList never{QStringLiteral("deleteditems"), QStringLiteral("drafts"), QStringLiteral("outbox"), QStringLiteral("junkemail"),
                                   QStringLiteral("archive"), QStringLiteral("conversationhistory"), QStringLiteral("syncissues"),
                                   QStringLiteral("conflicts"), QStringLiteral("localfailures"), QStringLiteral("serverfailures")};
    return never.contains(wellKnown);
}

bool excluded(const Store *store, const FolderRow &folder)
{
    if (excluded(folder.wellKnown)) {
        return true;
    }
    QString parent = folder.parentGraphId;
    for (int depth = 0; depth < 64 && !parent.isEmpty() && store; ++depth) {
        const std::optional<FolderRow> up = store->folderByGraphId(parent);
        if (!up) {
            break;
        }
        if (excluded(up->wellKnown)) {
            return true;
        }
        parent = up->parentGraphId;
    }
    return false;
}

std::optional<FolderRule> effective(const Settings &s, const FolderRule &rule)
{
    switch (rule.mode) {
    case FolderRule::Mode::Off:
        return std::nullopt;
    case FolderRule::Mode::Own:
        // A folder's own setting counts even while the default is off
        return rule;
    case FolderRule::Mode::Default:
        break;
    }
    if (!s.enabled) {
        return std::nullopt;
    }
    FolderRule result;
    result.mode = FolderRule::Mode::Own;
    result.action = s.action;
    result.days = s.days;
    return result;
}

QList<Candidate> plan(MailboxSet *mailboxes, qint64 nowMs)
{
    QList<Candidate> result;
    const Settings s = settings();
    const QList<MailboxInfo> boxes = mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        const Store *store = box.own && box.mail ? mailboxes->store(box.key()) : nullptr;
        if (!store) {
            continue;
        }
        const QList<FolderRow> folders = store->folders();
        for (const FolderRow &f : folders) {
            const std::optional<FolderRule> rule = excluded(store, f) ? std::nullopt : effective(s, folderRule(box.key(), f.graphId));
            if (!rule) {
                continue;
            }
            const QList<qint64> old = store->messagesBefore(f.id, nowMs - qint64(rule->days) * 24 * 3600 * 1000);
            if (!old.isEmpty()) {
                result.append({box.key(), f.id, f.name, rule->action, rule->days, old});
            }
        }
    }
    return result;
}

Dialog::Dialog(QWidget *parent)
    : QDialog(parent)
    , m_enabled(new QCheckBox(i18nc("@option:check", "Clean up old mail"), this))
    , m_action(new QComboBox(this))
    , m_days(daysBox(this))
    , m_scheduled(new QCheckBox(i18nc("@option:check", "Clean up automatically every"), this))
    , m_every(daysBox(this))
    , m_pstFolder(new QLineEdit(this))
    , m_lastRun(new QLabel(this))
{
    setWindowTitle(i18nc("@title:window", "Clean Up Old Mail"));
    const Settings s = settings();
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(i18nc("@info", "Mail older than the chosen age is cleaned up in every mail folder of your own mailboxes. A folder can have its own "
                                            "setting or be left alone (Clean Up Settings in its menu). Messages flagged for follow-up stay. "
                                            "Deleted Items, Drafts, Outbox, Junk Email and the Archive folder are never touched."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    layout->addWidget(m_enabled);
    auto *form = new QFormLayout;
    fillActions(m_action);
    m_action->setCurrentIndex(m_action->findData(int(s.action)));
    form->addRow(i18nc("@label:listbox", "What happens:"), m_action);
    m_days->setValue(s.days);
    form->addRow(i18nc("@label:spinbox", "With mail older than:"), m_days);
    auto *pstRow = new QHBoxLayout;
    m_pstFolder->setText(s.pstFolder);
    m_browse = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open-folder")), QString(), this);
    connect(m_browse, &QPushButton::clicked, this, [this] {
        const QString folder = QFileDialog::getExistingDirectory(this, i18nc("@title:window", "Folder for the Outlook Data Files"), m_pstFolder->text());
        if (!folder.isEmpty()) {
            m_pstFolder->setText(folder);
        }
    });
    pstRow->addWidget(m_pstFolder, 1);
    pstRow->addWidget(m_browse);
    form->addRow(i18nc("@label:textbox", "Outlook data files go to:"), pstRow);
    auto *scheduleRow = new QHBoxLayout;
    m_scheduled->setChecked(s.everyDays > 0);
    m_every->setValue(s.everyDays > 0 ? s.everyDays : 14);
    scheduleRow->addWidget(m_scheduled);
    scheduleRow->addWidget(m_every);
    scheduleRow->addStretch();
    form->addRow(QString(), scheduleRow);
    layout->addLayout(form);
    m_lastRun->setText(s.lastRun > 0 ? i18nc("@info %1 a date and time", "Last cleanup: %1", QLocale().toString(QDateTime::fromMSecsSinceEpoch(s.lastRun), QLocale::ShortFormat))
                                     : i18nc("@info", "Not cleaned up yet."));
    m_lastRun->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(m_lastRun);
    m_enabled->setChecked(s.enabled);
    connect(m_enabled, &QCheckBox::toggled, this, &Dialog::updateEnabled);
    connect(m_scheduled, &QCheckBox::toggled, this, &Dialog::updateEnabled);
    connect(m_action, &QComboBox::currentIndexChanged, this, &Dialog::updateEnabled);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    QPushButton *now = buttons->addButton(i18nc("@action:button", "Clean Up Now..."), QDialogButtonBox::ActionRole);
    now->setIcon(QIcon::fromTheme(QStringLiteral("edit-clear-history")));
    connect(now, &QPushButton::clicked, this, [this] {
        save();
        Q_EMIT runRequested();
        QDialog::accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    updateEnabled();
}

void Dialog::updateEnabled()
{
    const bool on = m_enabled->isChecked();
    for (QWidget *w : std::initializer_list<QWidget *>{m_action, m_days, m_scheduled, m_pstFolder}) {
        w->setEnabled(on);
    }
    m_every->setEnabled(on && m_scheduled->isChecked());
    m_pstFolder->setEnabled(on && Action(m_action->currentData().toInt()) == Action::ExportPst);
    m_browse->setEnabled(m_pstFolder->isEnabled());
}

void Dialog::save()
{
    Settings s = settings();
    s.enabled = m_enabled->isChecked();
    s.action = Action(m_action->currentData().toInt());
    s.days = m_days->value();
    s.everyDays = m_scheduled->isChecked() ? m_every->value() : 0;
    s.pstFolder = m_pstFolder->text().trimmed();
    setSettings(s);
}

void Dialog::accept()
{
    save();
    QDialog::accept();
}

FolderDialog::FolderDialog(const QString &mailboxKey, const QString &graphId, const QString &folderName, QWidget *parent)
    : QDialog(parent)
    , m_mailboxKey(mailboxKey)
    , m_graphId(graphId)
    , m_mode(new QComboBox(this))
    , m_action(new QComboBox(this))
    , m_days(daysBox(this))
{
    setWindowTitle(i18nc("@title:window %1 a folder", "Clean Up Settings for %1", folderName));
    const FolderRule rule = folderRule(mailboxKey, graphId);
    m_mode->addItem(i18nc("@item:inlistbox folder cleanup", "Use the default (Settings > Clean Up Old Mail)"), int(FolderRule::Mode::Default));
    m_mode->addItem(i18nc("@item:inlistbox folder cleanup", "Never clean up this folder"), int(FolderRule::Mode::Off));
    m_mode->addItem(i18nc("@item:inlistbox folder cleanup", "Its own setting"), int(FolderRule::Mode::Own));
    m_mode->setCurrentIndex(m_mode->findData(int(rule.mode)));
    fillActions(m_action);
    m_action->setCurrentIndex(m_action->findData(int(rule.action)));
    m_days->setValue(rule.days);
    auto *form = new QFormLayout(this);
    form->addRow(i18nc("@label:listbox", "Cleanup:"), m_mode);
    form->addRow(i18nc("@label:listbox", "What happens:"), m_action);
    form->addRow(i18nc("@label:spinbox", "With mail older than:"), m_days);
    const auto update = [this] {
        const bool own = FolderRule::Mode(m_mode->currentData().toInt()) == FolderRule::Mode::Own;
        m_action->setEnabled(own);
        m_days->setEnabled(own);
    };
    connect(m_mode, &QComboBox::currentIndexChanged, this, update);
    update();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);
}

void FolderDialog::accept()
{
    FolderRule rule;
    rule.mode = FolderRule::Mode(m_mode->currentData().toInt());
    rule.action = Action(m_action->currentData().toInt());
    rule.days = m_days->value();
    setFolderRule(m_mailboxKey, m_graphId, rule);
    QDialog::accept();
}

ReportDialog::ReportDialog(const QList<Candidate> &candidates, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(i18nc("@title:window", "Clean Up Old Mail"));
    resize(640, 420);
    auto *layout = new QVBoxLayout(this);
    auto *tree = new QTreeWidget(this);
    tree->setRootIsDecorated(false);
    tree->setHeaderLabels({i18nc("@title:column", "Folder"), i18nc("@title:column", "Messages"), i18nc("@title:column", "Older than"), i18nc("@title:column", "What happens")});
    int total = 0;
    bool anything = false;
    for (const Candidate &c : candidates) {
        tree->addTopLevelItem(new QTreeWidgetItem(tree,
                                                  {c.folderName, QString::number(c.messages.size()),
                                                   i18ncp("@item older than", "%1 day", "%1 days", c.days), actionText(c.action)}));
        total += int(c.messages.size());
        anything = anything || c.action != Action::Report;
    }
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    auto *summary = new QLabel(candidates.isEmpty() ? i18nc("@info", "Nothing to clean up: no folder has mail older than its age, or cleanup is off.")
                                                    : i18ncp("@info", "%1 message can be cleaned up.", "%1 messages can be cleaned up.", total),
                               this);
    summary->setWordWrap(true);
    layout->addWidget(summary);
    layout->addWidget(tree, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    if (anything) {
        QPushButton *run = buttons->addButton(i18nc("@action:button", "Clean Up"), QDialogButtonBox::AcceptRole);
        run->setIcon(QIcon::fromTheme(QStringLiteral("edit-clear-history")));
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

}
