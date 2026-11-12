// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "taskswindow.h"
#include "categorylist.h"
#include "daemonclient.h"
#include "graphmapper.h"
#include "mailboxset.h"
#include "store.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KMessageBox>
#include <KStandardAction>
#include <KToggleAction>

#include <QCheckBox>
#include <QDateEdit>
#include <QDateTimeEdit>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSplitter>
#include <QStatusBar>
#include <QTextEdit>
#include <QTimeZone>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>
#include <memory>

namespace GraphMail
{

namespace
{
QString text(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString();
}

QString windowsZone()
{
    const QByteArray windows = QTimeZone::ianaIdToWindowsId(QTimeZone::systemTimeZoneId());
    return windows.isEmpty() ? QStringLiteral("UTC") : QString::fromLatin1(windows);
}

// To Do's dateTimeTimeZone as a local time (To Do writes UTC unless told otherwise)
QDateTime localTime(const QJsonValue &value)
{
    const QJsonObject t = value.toObject();
    if (t.isEmpty()) {
        return {};
    }
    QDateTime dt = QDateTime::fromString(text(t, "dateTime").left(19), Qt::ISODate);
    const QString zone = text(t, "timeZone");
    QTimeZone tz = zone.isEmpty() || zone == QLatin1String("UTC") ? QTimeZone(QTimeZone::UTC) : QTimeZone(zone.toUtf8());
    if (!tz.isValid()) {
        tz = QTimeZone(QTimeZone::windowsIdToDefaultIanaId(zone.toUtf8()));
    }
    dt.setTimeZone(tz.isValid() ? tz : QTimeZone::systemTimeZone());
    return dt.toLocalTime();
}

QJsonObject graphTime(const QDateTime &local)
{
    return {{QStringLiteral("dateTime"), local.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))}, {QStringLiteral("timeZone"), windowsZone()}};
}

bool done(const QJsonObject &task)
{
    return text(task, "status") == QLatin1String("completed");
}

QDate dueDate(const QJsonObject &task)
{
    return Mapper::dueDate(task.value(QLatin1String("dueDateTime")).toObject());
}
}

TasksWindow::TasksWindow(MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent)
    : KXmlGuiWindow(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
    , m_lists(new QTreeWidget(this))
    , m_add(new QLineEdit(this))
    , m_tasks(new QTreeWidget(this))
    , m_details(new QWidget(this))
    , m_title(new QLineEdit(this))
    , m_hasDue(new QCheckBox(i18nc("@option:check", "Due:"), this))
    , m_due(new QDateEdit(this))
    , m_remind(new QCheckBox(i18nc("@option:check", "Remind me:"), this))
    , m_reminder(new QDateTimeEdit(this))
    , m_important(new QCheckBox(i18nc("@option:check", "Important"), this))
    , m_steps(new QListWidget(this))
    , m_newStep(new QLineEdit(this))
    , m_notes(new QTextEdit(this))
    , m_openMail(new QPushButton(QIcon::fromTheme(QStringLiteral("mail-message")), i18nc("@action:button", "Open the Mail"), this))
    , m_save(new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")), i18nc("@action:button", "Save"), this))
    , m_delete(new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action:button", "Delete"), this))
    , m_created(new QLabel(this))
{
    setWindowTitle(i18nc("@title:window", "Tasks"));
    m_lists->setHeaderHidden(true);
    m_lists->setFrameShape(QFrame::NoFrame);
    m_lists->setContextMenuPolicy(Qt::CustomContextMenu);
    m_add->setPlaceholderText(i18nc("@info:placeholder", "Add a task"));
    m_add->setClearButtonEnabled(true);
    m_tasks->setColumnCount(3);
    m_tasks->setHeaderLabels({i18nc("@title:column", "Task"), i18nc("@title:column", "Due"), QString()});
    m_tasks->setRootIsDecorated(false);
    m_tasks->setFrameShape(QFrame::NoFrame);
    m_tasks->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tasks->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_due->setCalendarPopup(true);
    m_due->setDisplayFormat(QStringLiteral("ddd dd-MM-yyyy"));
    m_reminder->setCalendarPopup(true);
    m_reminder->setDisplayFormat(QStringLiteral("ddd dd-MM-yyyy HH:mm"));
    m_newStep->setPlaceholderText(i18nc("@info:placeholder", "Add a step"));
    m_notes->setAcceptRichText(false);
    m_notes->setPlaceholderText(i18nc("@info:placeholder", "Notes"));
    m_created->setForegroundRole(QPalette::PlaceholderText);
    connect(m_hasDue, &QCheckBox::toggled, m_due, &QWidget::setEnabled);
    connect(m_remind, &QCheckBox::toggled, m_reminder, &QWidget::setEnabled);

    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:textbox", "Title:"), m_title);
    auto *dueRow = new QHBoxLayout;
    dueRow->addWidget(m_hasDue);
    dueRow->addWidget(m_due);
    dueRow->addStretch();
    auto *remindRow = new QHBoxLayout;
    remindRow->addWidget(m_remind);
    remindRow->addWidget(m_reminder);
    remindRow->addStretch();
    form->addRow(dueRow);
    form->addRow(remindRow);
    form->addRow(m_important);
    // Categories: labels and a button to tick them
    m_categoryLabels = new QLabel(this);
    m_categoryLabels->setTextFormat(Qt::RichText);
    m_categorize = new QToolButton(this);
    m_categorize->setIcon(QIcon::fromTheme(QStringLiteral("tag")));
    m_categorize->setText(i18nc("@action:button", "Categorize"));
    m_categorize->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_categorize->setPopupMode(QToolButton::InstantPopup);
    auto *categorizeMenu = new QMenu(m_categorize);
    m_categorize->setMenu(categorizeMenu);
    connect(categorizeMenu, &QMenu::aboutToShow, this, [this, categorizeMenu] {
        const std::optional<Task> t = currentTask();
        if (!t || !m_categoryList) {
            categorizeMenu->clear();
            return;
        }
        QStringList current;
        const QJsonArray array = t->json.value(QLatin1String("categories")).toArray();
        for (const QJsonValue &v : array) {
            current.append(v.toString());
        }
        const Task task = *t;
        m_categoryList->fillMenu(categorizeMenu, task.mailboxKey, current, [this, task](const QString &name, bool on) {
            setTaskCategory(task, name, on);
        });
    });
    auto *categoryRow = new QHBoxLayout;
    categoryRow->addWidget(m_categorize);
    categoryRow->addWidget(m_categoryLabels, 1);
    form->addRow(categoryRow);
    auto *detailLayout = new QVBoxLayout(m_details);
    detailLayout->addLayout(form);
    detailLayout->addWidget(new QLabel(i18nc("@label", "Steps:"), this));
    detailLayout->addWidget(m_steps, 1);
    detailLayout->addWidget(m_newStep);
    detailLayout->addWidget(m_notes, 1);
    detailLayout->addWidget(m_openMail);
    detailLayout->addWidget(m_created);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_save);
    buttons->addStretch();
    buttons->addWidget(m_delete);
    detailLayout->addLayout(buttons);
    m_details->setEnabled(false);

    auto *middle = new QWidget(this);
    auto *middleLayout = new QVBoxLayout(middle);
    middleLayout->setContentsMargins(0, 0, 0, 0);
    middleLayout->addWidget(m_add);
    middleLayout->addWidget(m_tasks, 1);
    auto *splitter = new QSplitter(this);
    splitter->addWidget(m_lists);
    splitter->addWidget(middle);
    splitter->addWidget(m_details);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({220, 600, 420});
    setCentralWidget(splitter);

    connect(m_lists, &QTreeWidget::currentItemChanged, this, &TasksWindow::listChanged);
    connect(m_lists, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu menu(this);
        menu.addAction(QIcon::fromTheme(QStringLiteral("list-add")), i18nc("@action:inmenu", "New List..."), this, &TasksWindow::newList);
        QTreeWidgetItem *item = m_lists->itemAt(pos);
        if (item && item->data(0, Qt::UserRole + 1).toString().isEmpty() && item->data(0, Qt::UserRole).toString().contains(QLatin1Char('|'))) {
            menu.addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), i18nc("@action:inmenu", "Rename List..."), this, &TasksWindow::renameList);
            menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action:inmenu", "Delete List"), this, &TasksWindow::deleteList);
        }
        menu.exec(m_lists->viewport()->mapToGlobal(pos));
    });
    connect(m_add, &QLineEdit::returnPressed, this, &TasksWindow::addTask);
    connect(m_tasks, &QTreeWidget::currentItemChanged, this, &TasksWindow::showDetails);
    connect(m_tasks, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        // The tick box: done or not, at once
        if (m_filling || column != 0) {
            return;
        }
        const int index = item->data(0, Qt::UserRole).toInt();
        // Only a tick that differs from the task: any other change of the row (its font, its
        // colour) also comes here, and must never send anything (phase 8: a loop sent the same
        // status 60 times each time the window opened)
        if (index >= 0 && index < m_shown.size() && (item->checkState(0) == Qt::Checked) != done(m_shown.at(index).json)) {
            setDone(m_shown.at(index), item->checkState(0) == Qt::Checked);
        }
    });
    connect(m_save, &QPushButton::clicked, this, &TasksWindow::saveDetails);
    connect(m_title, &QLineEdit::returnPressed, this, &TasksWindow::saveDetails);
    connect(m_delete, &QPushButton::clicked, this, [this] {
        if (const std::optional<Task> t = currentTask()) {
            if (KMessageBox::warningContinueCancel(this, i18nc("@info", "Delete the task \"%1\"?", text(t->json, "title")), i18nc("@title:window", "Delete Task"),
                                                   KStandardGuiItem::del())
                == KMessageBox::Continue) {
                deleteTask(*t);
            }
        }
    });
    connect(m_newStep, &QLineEdit::returnPressed, this, [this] {
        const std::optional<Task> t = currentTask();
        const QString step = m_newStep->text().trimmed();
        if (!t || step.isEmpty()) {
            return;
        }
        m_daemon->taskStep(t->mailboxKey.section(QLatin1Char('/'), 0, 0), t->mailboxKey.section(QLatin1Char('/'), 1), t->id, QStringLiteral("create"), {},
                           QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("displayName"), step}}).toJson(QJsonDocument::Compact)));
        auto *item = new QListWidgetItem(step, m_steps);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        m_newStep->clear();
        // Steps are not in the cache: read them again once Graph made it (for its id)
        QTimer::singleShot(4000, this, &TasksWindow::loadSteps);
    });
    connect(m_steps, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        const std::optional<Task> t = currentTask();
        const QString id = item->data(Qt::UserRole).toString();
        if (m_filling || !t || id.isEmpty()) {
            return;
        }
        m_daemon->taskStep(t->mailboxKey.section(QLatin1Char('/'), 0, 0), t->mailboxKey.section(QLatin1Char('/'), 1), t->id, QStringLiteral("update"), id,
                           QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("isChecked"), item->checkState() == Qt::Checked}}).toJson(QJsonDocument::Compact)));
    });
    m_steps->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_steps, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *item = m_steps->itemAt(pos);
        const std::optional<Task> t = currentTask();
        if (!item || !t || item->data(Qt::UserRole).toString().isEmpty()) {
            return;
        }
        QMenu menu(this);
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action:inmenu", "Delete Step"), this, [this, item, t] {
            m_daemon->taskStep(t->mailboxKey.section(QLatin1Char('/'), 0, 0), t->mailboxKey.section(QLatin1Char('/'), 1), t->id, QStringLiteral("delete"), item->data(Qt::UserRole).toString(), {});
            delete item;
        });
        menu.exec(m_steps->viewport()->mapToGlobal(pos));
    });
    connect(m_openMail, &QPushButton::clicked, this, [this] {
        const std::optional<Task> t = currentTask();
        if (!t) {
            return;
        }
        const QString externalId = t->json.value(QLatin1String("linkedResources")).toArray().first().toObject().value(QLatin1String("externalId")).toString();
        const Store *store = m_mailboxes->store(t->mailboxKey);
        if (const std::optional<MessageRow> m = store ? store->messageByGraphId(externalId) : std::nullopt) {
            Q_EMIT openMail(t->mailboxKey, m->id);
        } else {
            statusBar()->showMessage(i18nc("@info:status", "The mail is not in Graph Mail's copy of the mailbox (yet)"), 6000);
        }
    });

    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(300);
    connect(&m_reloadTimer, &QTimer::timeout, this, [this] {
        fillLists();
    });
    connect(m_daemon, &DaemonClient::collectionChanged, this, [this](const QString &, const QString &, const QString &collection, int) {
        if (collection == QLatin1String("todo")) {
            m_reloadTimer.start();
        }
    });

    setupActions();
    setupGUI(QSize(1280, 760), ToolBar | Keys | Save | Create | StatusBar, QStringLiteral("graphmailtasksui.rc"));
    setAutoSaveSettings(QStringLiteral("TasksWindow"));
    fillLists();
}

void TasksWindow::setupActions()
{
    KActionCollection *actions = actionCollection();
    QAction *newTask = actions->addAction(QStringLiteral("new_task"));
    newTask->setText(i18nc("@action", "New Task"));
    newTask->setIconText(i18nc("@action short toolbar text", "New"));
    newTask->setIcon(QIcon::fromTheme(QStringLiteral("view-task-add")));
    actions->setDefaultShortcut(newTask, QKeySequence(Qt::CTRL | Qt::Key_N));
    connect(newTask, &QAction::triggered, this, [this] {
        m_add->setFocus();
    });
    QAction *newListAction = actions->addAction(QStringLiteral("new_list"));
    newListAction->setText(i18nc("@action", "New List..."));
    newListAction->setIcon(QIcon::fromTheme(QStringLiteral("list-add")));
    connect(newListAction, &QAction::triggered, this, &TasksWindow::newList);
    m_showCompleted = new KToggleAction(QIcon::fromTheme(QStringLiteral("view-task")), i18nc("@action", "Show Completed Tasks"), this);
    actions->addAction(QStringLiteral("show_completed"), m_showCompleted);
    connect(m_showCompleted, &QAction::toggled, this, &TasksWindow::fillTasks);
    const struct {
        const char *name;
        int module;
        const char *icon;
        KLocalizedString text;
        Qt::Key key;
    } modules[] = {{"go_mail", 1, "mail-message", ki18nc("@action", "Mail"), Qt::Key_1},
                   {"go_calendar", 2, "view-calendar", ki18nc("@action", "Calendar"), Qt::Key_2},
                   {"go_contacts", 3, "view-pim-contacts", ki18nc("@action", "Contacts"), Qt::Key_3}};
    for (const auto &m : modules) {
        QAction *a = actions->addAction(QLatin1String(m.name));
        a->setText(m.text.toString());
        a->setIcon(QIcon::fromTheme(QLatin1String(m.icon)));
        actions->setDefaultShortcut(a, QKeySequence(Qt::CTRL | m.key));
        const int module = m.module;
        connect(a, &QAction::triggered, this, [this, module] {
            Q_EMIT navigate(module);
        });
    }
    // Close: the window it is in (the one application window when it is embedded)
    KStandardAction::close(
        this,
        [this] {
            window()->close();
        },
        actions);
}

void TasksWindow::fillLists()
{
    const QString was = m_lists->currentItem() ? m_lists->currentItem()->data(0, Qt::UserRole).toString() : QString();
    m_lists->blockSignals(true);
    m_lists->clear();
    QTreeWidgetItem *first = nullptr;
    QTreeWidgetItem *again = nullptr;
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    const bool several = std::count_if(boxes.cbegin(), boxes.cend(), [](const MailboxInfo &b) {
                             return b.own;
                         })
        > 1;
    for (const MailboxInfo &box : boxes) {
        const Store *store = box.own ? m_mailboxes->store(box.key()) : nullptr;
        if (!store) {
            continue;
        }
        QTreeWidgetItem *group = nullptr;
        if (several) {
            group = new QTreeWidgetItem(m_lists, {box.address});
            group->setFlags(Qt::ItemIsEnabled);
            QFont bold = group->font(0);
            bold.setBold(true);
            group->setFont(0, bold);
            group->setExpanded(true);
        }
        const QList<ParentRow> lists = store->parents(ItemKind::Task);
        for (const ParentRow &l : lists) {
            int open = 0;
            const QList<StoredItem> tasks = store->items(ItemKind::Task, l.id);
            for (const StoredItem &t : tasks) {
                open += done(QJsonDocument::fromJson(t.json.toUtf8()).object()) ? 0 : 1;
            }
            QString name = l.name;
            QString icon = QStringLiteral("view-task");
            if (l.wellKnown == QLatin1String("defaultList")) {
                name = i18nc("@item To Do's default list", "Tasks");
            } else if (l.wellKnown == QLatin1String("flaggedEmails")) {
                name = i18nc("@item To Do's list of flagged mail", "Flagged Emails");
                icon = QStringLiteral("flag-red");
            }
            auto *item = group ? new QTreeWidgetItem(group, {QString()}) : new QTreeWidgetItem(m_lists, {QString()});
            item->setText(0, open > 0 ? QStringLiteral("%1 (%2)").arg(name).arg(open) : name);
            item->setIcon(0, QIcon::fromTheme(icon));
            const QString key = box.key() + QLatin1Char('|') + QString::number(l.id);
            item->setData(0, Qt::UserRole, key);
            item->setData(0, Qt::UserRole + 1, l.wellKnown);
            item->setData(0, Qt::UserRole + 2, l.name);
            if (!first && l.wellKnown == QLatin1String("defaultList")) {
                first = item;
            }
            if (key == was) {
                again = item;
            }
        }
    }
    m_lists->blockSignals(false);
    if (QTreeWidgetItem *pick = again ? again : first) {
        m_lists->setCurrentItem(pick);
    }
    listChanged();
}

QPair<QString, qint64> TasksWindow::currentList() const
{
    QTreeWidgetItem *item = m_lists->currentItem();
    const QString key = item ? item->data(0, Qt::UserRole).toString() : QString();
    if (!key.contains(QLatin1Char('|'))) {
        return {};
    }
    return {key.section(QLatin1Char('|'), 0, 0), key.section(QLatin1Char('|'), 1).toLongLong()};
}

void TasksWindow::listChanged()
{
    const bool flagged = m_lists->currentItem() && m_lists->currentItem()->data(0, Qt::UserRole + 1).toString() == QLatin1String("flaggedEmails");
    // Flagged mail becomes a task by flagging it, not here
    m_add->setEnabled(!currentList().first.isEmpty() && !flagged);
    fillTasks();
}

void TasksWindow::fillTasks()
{
    const auto [key, list] = currentList();
    const qint64 selected = currentTask() ? currentTask()->id : 0;
    // Restores the previous value: setCurrentItem() below runs showDetails(), which guards too
    std::optional<QScopedValueRollback<bool>> filling(std::in_place, m_filling, true);
    m_tasks->clear();
    m_shown.clear();
    const Store *store = m_mailboxes->store(key);
    QList<Task> all;
    if (store) {
        const QList<StoredItem> items = store->items(ItemKind::Task, list);
        for (const StoredItem &i : items) {
            all.append({key, i.id, list, QJsonDocument::fromJson(i.json.toUtf8()).object()});
        }
    }
    // Open ones first, by due date (none last), as To Do sorts them
    std::stable_sort(all.begin(), all.end(), [](const Task &a, const Task &b) {
        if (done(a.json) != done(b.json)) {
            return !done(a.json);
        }
        const QDate da = dueDate(a.json);
        const QDate db = dueDate(b.json);
        if (da.isValid() != db.isValid()) {
            return da.isValid();
        }
        return da < db;
    });
    int completed = 0;
    for (const Task &t : std::as_const(all)) {
        if (done(t.json)) {
            ++completed;
            if (!m_showCompleted->isChecked()) {
                continue;
            }
        }
        const QDate due = dueDate(t.json);
        auto *row = new QTreeWidgetItem(m_tasks, {text(t.json, "title"), due.isValid() ? QLocale().toString(due, QStringLiteral("ddd d MMM")) : QString()});
        row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
        row->setCheckState(0, done(t.json) ? Qt::Checked : Qt::Unchecked);
        row->setData(0, Qt::UserRole, int(m_shown.size()));
        if (text(t.json, "importance") == QLatin1String("high")) {
            row->setIcon(2, QIcon::fromTheme(QStringLiteral("emblem-important")));
        }
        if (due.isValid() && due < QDate::currentDate() && !done(t.json)) {
            row->setForeground(1, QColor(0xda, 0x44, 0x53));
        }
        if (done(t.json)) {
            QFont struck = row->font(0);
            struck.setStrikeOut(true);
            row->setFont(0, struck);
            row->setForeground(0, palette().color(QPalette::PlaceholderText));
        }
        m_shown.append(t);
        if (t.id == selected) {
            m_tasks->setCurrentItem(row);
        }
    }
    filling.reset();
    statusBar()->showMessage(i18ncp("@info:status", "%1 open task", "%1 open tasks", int(all.size()) - completed)
                             + (completed > 0 && !m_showCompleted->isChecked() ? QStringLiteral(", ") + i18ncp("@info:status", "%1 completed (hidden)", "%1 completed (hidden)", completed)
                                                                                : QString()));
    if (!m_tasks->currentItem() && m_tasks->topLevelItemCount() > 0) {
        m_tasks->setCurrentItem(m_tasks->topLevelItem(0));
    } else if (!m_tasks->currentItem()) {
        showDetails();
    }
}

std::optional<TasksWindow::Task> TasksWindow::currentTask() const
{
    QTreeWidgetItem *row = m_tasks->currentItem();
    const int index = row ? row->data(0, Qt::UserRole).toInt() : -1;
    return index >= 0 && index < m_shown.size() ? std::optional<Task>(m_shown.at(index)) : std::nullopt;
}

void TasksWindow::showDetails()
{
    const std::optional<Task> t = currentTask();
    m_details->setEnabled(t.has_value());
    const QScopedValueRollback<bool> filling(m_filling, true);
    m_steps->clear();
    if (!t) {
        m_title->clear();
        m_notes->clear();
        m_created->clear();
        m_openMail->hide();
        return;
    }
    const QJsonObject &o = t->json;
    m_title->setText(text(o, "title"));
    const QDate due = dueDate(o);
    m_hasDue->setChecked(due.isValid());
    m_due->setDate(due.isValid() ? due : QDate::currentDate());
    m_due->setEnabled(due.isValid());
    const QDateTime reminder = localTime(o.value(QLatin1String("reminderDateTime")));
    m_remind->setChecked(o.value(QLatin1String("isReminderOn")).toBool() && reminder.isValid());
    m_reminder->setDateTime(reminder.isValid() ? reminder : QDateTime(QDate::currentDate().addDays(1), QTime(9, 0)));
    m_reminder->setEnabled(m_remind->isChecked());
    m_important->setChecked(text(o, "importance") == QLatin1String("high"));
    m_notes->setPlainText(o.value(QLatin1String("body")).toObject().value(QLatin1String("content")).toString().trimmed());
    m_openMail->setVisible(!o.value(QLatin1String("linkedResources")).toArray().isEmpty());
    QStringList categories;
    const QJsonArray categoryArray = o.value(QLatin1String("categories")).toArray();
    for (const QJsonValue &v : categoryArray) {
        categories.append(v.toString());
    }
    m_categoryLabels->setText(m_categoryList ? m_categoryList->labelsHtml(t->mailboxKey, categories) : categories.join(QStringLiteral(", ")));
    const QDateTime created = QDateTime::fromString(text(o, "createdDateTime").left(19), Qt::ISODate);
    m_created->setText(created.isValid() ? i18nc("@info task", "Created %1", QLocale().toString(created.toLocalTime().date(), QStringLiteral("d MMMM yyyy"))) : QString());
    loadSteps();
}

void TasksWindow::loadSteps()
{
    const std::optional<Task> t = currentTask();
    if (!t || text(t->json, "id").startsWith(QLatin1String("local:"))) {
        return;
    }
    const qint64 id = t->id;
    m_daemon->query(t->mailboxKey.section(QLatin1Char('/'), 0, 0), t->mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("taskSteps"),
                    QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("task"), id}}).toJson(QJsonDocument::Compact)), this, [this, id](const QString &json) {
                        if (!currentTask() || currentTask()->id != id || json.isEmpty()) {
                            return;
                        }
                        const QScopedValueRollback<bool> filling(m_filling, true);
                        m_steps->clear();
                        const QJsonArray steps = QJsonDocument::fromJson(json.toUtf8()).object().value(QLatin1String("value")).toArray();
                        for (const QJsonValue &v : steps) {
                            const QJsonObject s = v.toObject();
                            auto *item = new QListWidgetItem(text(s, "displayName"), m_steps);
                            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                            item->setCheckState(s.value(QLatin1String("isChecked")).toBool() ? Qt::Checked : Qt::Unchecked);
                            item->setData(Qt::UserRole, text(s, "id"));
                        }
                    });
}

void TasksWindow::addTask()
{
    const auto [key, list] = currentList();
    const QString title = m_add->text().trimmed();
    if (key.isEmpty() || title.isEmpty()) {
        return;
    }
    m_daemon->createItem(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("task"), list,
                         QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("title"), title}}).toJson(QJsonDocument::Compact)));
    m_add->clear();
}

void TasksWindow::setDone(const Task &task, bool isDone)
{
    m_daemon->updateItem(task.mailboxKey.section(QLatin1Char('/'), 0, 0), task.mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("task"), task.id,
                         QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("status"), isDone ? QStringLiteral("completed") : QStringLiteral("notStarted")}})
                                               .toJson(QJsonDocument::Compact)));
}

QJsonObject TasksWindow::detailChanges(const Task &task) const
{
    const QJsonObject &o = task.json;
    QJsonObject diff;
    if (m_title->text().trimmed() != text(o, "title")) {
        diff.insert(QStringLiteral("title"), m_title->text().trimmed());
    }
    const QDate due = dueDate(o);
    if (m_hasDue->isChecked() != due.isValid() || (m_hasDue->isChecked() && m_due->date() != due)) {
        diff.insert(QStringLiteral("dueDateTime"), m_hasDue->isChecked() ? QJsonValue(Mapper::dueDateTime(m_due->date())) : QJsonValue());
    }
    const QDateTime reminder = localTime(o.value(QLatin1String("reminderDateTime")));
    const bool remindBefore = o.value(QLatin1String("isReminderOn")).toBool() && reminder.isValid();
    if (m_remind->isChecked() != remindBefore || (m_remind->isChecked() && m_reminder->dateTime() != reminder)) {
        diff.insert(QStringLiteral("isReminderOn"), m_remind->isChecked());
        // Off needs the time cleared too: To Do ignores isReminderOn false on its own
        diff.insert(QStringLiteral("reminderDateTime"), m_remind->isChecked() ? QJsonValue(graphTime(m_reminder->dateTime())) : QJsonValue(QJsonValue::Null));
    }
    if (m_important->isChecked() != (text(o, "importance") == QLatin1String("high"))) {
        diff.insert(QStringLiteral("importance"), m_important->isChecked() ? QStringLiteral("high") : QStringLiteral("normal"));
    }
    if (m_notes->toPlainText().trimmed() != o.value(QLatin1String("body")).toObject().value(QLatin1String("content")).toString().trimmed()) {
        diff.insert(QStringLiteral("body"), QJsonObject{{QStringLiteral("contentType"), QStringLiteral("text")}, {QStringLiteral("content"), m_notes->toPlainText()}});
    }
    return diff;
}

void TasksWindow::saveDetails()
{
    const std::optional<Task> t = currentTask();
    if (!t) {
        return;
    }
    const QJsonObject diff = detailChanges(*t);
    if (!diff.isEmpty()) {
        m_daemon->updateItem(t->mailboxKey.section(QLatin1Char('/'), 0, 0), t->mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("task"), t->id,
                             QString::fromUtf8(QJsonDocument(diff).toJson(QJsonDocument::Compact)));
        statusBar()->showMessage(i18nc("@info:status", "Saved"), 3000);
    }
}

void TasksWindow::deleteTask(const Task &task)
{
    m_daemon->deleteItem(task.mailboxKey.section(QLatin1Char('/'), 0, 0), task.mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("task"), task.id);
}

void TasksWindow::newList()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, i18nc("@title:window", "New List"), i18nc("@label:textbox", "Name:"), QLineEdit::Normal, {}, &ok).trimmed();
    QString key = currentList().first;
    if (key.isEmpty()) {
        const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
        for (const MailboxInfo &box : boxes) {
            if (box.own && key.isEmpty()) {
                key = box.key();
            }
        }
    }
    if (ok && !name.isEmpty() && !key.isEmpty()) {
        m_daemon->todoList(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("create"), 0, name);
    }
}

void TasksWindow::renameList()
{
    const auto [key, list] = currentList();
    QTreeWidgetItem *item = m_lists->currentItem();
    if (key.isEmpty() || !item) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, i18nc("@title:window", "Rename List"), i18nc("@label:textbox", "Name:"), QLineEdit::Normal,
                                               item->data(0, Qt::UserRole + 2).toString(), &ok)
                             .trimmed();
    if (ok && !name.isEmpty()) {
        m_daemon->todoList(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("rename"), list, name);
    }
}

void TasksWindow::deleteList()
{
    const auto [key, list] = currentList();
    QTreeWidgetItem *item = m_lists->currentItem();
    if (key.isEmpty() || !item) {
        return;
    }
    if (KMessageBox::warningContinueCancel(this, i18nc("@info", "Delete the list \"%1\" and all its tasks?", item->data(0, Qt::UserRole + 2).toString()),
                                           i18nc("@title:window", "Delete List"), KStandardGuiItem::del())
        == KMessageBox::Continue) {
        m_daemon->todoList(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("delete"), list, {});
    }
}

void TasksWindow::showTask(const QString &mailboxKey, qint64 task)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<StoredItem> t = store ? store->item(ItemKind::Task, task) : std::nullopt;
    if (!t) {
        return;
    }
    const QString key = mailboxKey + QLatin1Char('|') + QString::number(t->parent);
    QTreeWidgetItemIterator it(m_lists);
    while (*it) {
        if ((*it)->data(0, Qt::UserRole).toString() == key) {
            m_lists->setCurrentItem(*it);
            break;
        }
        ++it;
    }
    if (done(QJsonDocument::fromJson(t->json.toUtf8()).object())) {
        m_showCompleted->setChecked(true);
    }
    for (int i = 0; i < m_tasks->topLevelItemCount(); ++i) {
        const int index = m_tasks->topLevelItem(i)->data(0, Qt::UserRole).toInt();
        if (index < m_shown.size() && m_shown.at(index).id == task) {
            m_tasks->setCurrentItem(m_tasks->topLevelItem(i));
        }
    }
}

bool TasksWindow::isTestTask(const QString &mailboxKey, qint64 task) const
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<StoredItem> t = store ? store->item(ItemKind::Task, task) : std::nullopt;
    return t && QJsonDocument::fromJson(t->json.toUtf8()).object().value(QLatin1String("title")).toString().contains(QLatin1String("[TEST Graph Mail]"));
}

bool TasksWindow::addTaskForTests(const QString &listName, const QString &title)
{
    if (!title.contains(QLatin1String("[TEST Graph Mail]"))) {
        return false;
    }
    QTreeWidgetItemIterator it(m_lists);
    while (*it) {
        if ((*it)->data(0, Qt::UserRole + 2).toString() == listName) {
            m_lists->setCurrentItem(*it);
            m_add->setText(title);
            addTask();
            return true;
        }
        ++it;
    }
    return false;
}

bool TasksWindow::completeForTests(const QString &mailboxKey, qint64 task, bool isDone)
{
    if (!isTestTask(mailboxKey, task)) {
        return false;
    }
    showTask(mailboxKey, task);
    // Through the tick box, as a click does
    if (QTreeWidgetItem *row = m_tasks->currentItem()) {
        row->setCheckState(0, isDone ? Qt::Checked : Qt::Unchecked);
        return true;
    }
    return false;
}

bool TasksWindow::editTaskForTests(const QString &mailboxKey, qint64 task, const QJsonObject &fields)
{
    if (!isTestTask(mailboxKey, task)) {
        return false;
    }
    showTask(mailboxKey, task);
    if (fields.contains(QLatin1String("title"))) {
        m_title->setText(fields.value(QLatin1String("title")).toString());
    }
    if (fields.contains(QLatin1String("due"))) {
        m_hasDue->setChecked(true);
        m_due->setDate(QDate::fromString(fields.value(QLatin1String("due")).toString(), Qt::ISODate));
    }
    if (fields.contains(QLatin1String("reminder"))) {
        // An empty reminder unticks Remind me
        const QString reminder = fields.value(QLatin1String("reminder")).toString();
        m_remind->setChecked(!reminder.isEmpty());
        if (!reminder.isEmpty()) {
            m_reminder->setDateTime(QDateTime::fromString(reminder, Qt::ISODate));
        }
    }
    if (fields.contains(QLatin1String("important"))) {
        m_important->setChecked(fields.value(QLatin1String("important")).toBool());
    }
    if (fields.contains(QLatin1String("notes"))) {
        m_notes->setPlainText(fields.value(QLatin1String("notes")).toString());
    }
    saveDetails();
    return true;
}

bool TasksWindow::addStepForTests(const QString &mailboxKey, qint64 task, const QString &step)
{
    if (!isTestTask(mailboxKey, task)) {
        return false;
    }
    showTask(mailboxKey, task);
    m_newStep->setText(step);
    Q_EMIT m_newStep->returnPressed();
    return true;
}

void TasksWindow::tickStepForTests(const QString &mailboxKey, qint64 task, const QString &step)
{
    if (!isTestTask(mailboxKey, task)) {
        qWarning() << "step tick test: not a test task";
        return;
    }
    showTask(mailboxKey, task);
    auto tries = std::make_shared<int>(0);
    auto attempt = std::make_shared<std::function<void()>>();
    *attempt = [this, step, tries, attempt] {
        const QList<QListWidgetItem *> found = m_steps->findItems(step, Qt::MatchExactly);
        if (!found.isEmpty() && !found.first()->data(Qt::UserRole).toString().isEmpty()) {
            found.first()->setCheckState(Qt::Checked);
            qWarning() << "step tick test: true";
        } else if (++*tries < 20) {
            QTimer::singleShot(250, this, *attempt);
        } else {
            qWarning() << "step tick test: step not found";
        }
    };
    (*attempt)();
}

bool TasksWindow::openMailForTests(const QString &mailboxKey, qint64 task)
{
    if (!isTestTask(mailboxKey, task)) {
        return false;
    }
    showTask(mailboxKey, task);
    auto opened = std::make_shared<bool>(false);
    const QMetaObject::Connection c = connect(this, &TasksWindow::openMail, this, [opened] {
        *opened = true;
    });
    m_openMail->click();
    disconnect(c);
    return *opened;
}

bool TasksWindow::deleteTaskForTests(const QString &mailboxKey, qint64 task)
{
    if (!isTestTask(mailboxKey, task)) {
        return false;
    }
    showTask(mailboxKey, task);
    if (const std::optional<Task> t = currentTask()) {
        deleteTask(*t);
        return true;
    }
    return false;
}

bool TasksWindow::listActionForTests(const QString &action, const QString &name, const QString &newName)
{
    if (!name.contains(QLatin1String("[TEST Graph Mail]"))) {
        return false;
    }
    QString key;
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        if (box.own && box.mailbox == QLatin1String("me") && key.isEmpty() && m_mailboxes->store(box.key()) && !m_mailboxes->store(box.key())->parents(ItemKind::Task).isEmpty()) {
            key = box.key();
        }
    }
    if (action == QLatin1String("create")) {
        m_daemon->todoList(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("create"), 0, name);
        return true;
    }
    QTreeWidgetItemIterator it(m_lists);
    while (*it) {
        if ((*it)->data(0, Qt::UserRole + 2).toString() == name) {
            const QString itemKey = (*it)->data(0, Qt::UserRole).toString();
            m_daemon->todoList(itemKey.section(QLatin1Char('|'), 0, 0).section(QLatin1Char('/'), 0, 0), itemKey.section(QLatin1Char('|'), 0, 0).section(QLatin1Char('/'), 1),
                               action, itemKey.section(QLatin1Char('|'), 1).toLongLong(), newName);
            return true;
        }
        ++it;
    }
    return false;
}

void TasksWindow::setCategoryList(CategoryList *categories)
{
    m_categoryList = categories;
    connect(categories, &CategoryList::changed, this, &TasksWindow::showDetails);
    showDetails();
}

void TasksWindow::setTaskCategory(const Task &task, const QString &name, bool on)
{
    QStringList categories;
    const QJsonArray array = task.json.value(QLatin1String("categories")).toArray();
    for (const QJsonValue &v : array) {
        categories.append(v.toString());
    }
    categories.removeAll(name);
    if (on) {
        categories.append(name);
    }
    m_daemon->updateItem(task.mailboxKey.section(QLatin1Char('/'), 0, 0), task.mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("task"), task.id,
                         QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("categories"), QJsonArray::fromStringList(categories)}}).toJson(QJsonDocument::Compact)));
}

bool TasksWindow::categorizeForTests(const QString &mailboxKey, qint64 task, const QString &name, bool on)
{
    if (!isTestTask(mailboxKey, task)) {
        return false;
    }
    showTask(mailboxKey, task);
    if (const std::optional<Task> t = currentTask()) {
        setTaskCategory(*t, name, on);
        return true;
    }
    return false;
}

}
