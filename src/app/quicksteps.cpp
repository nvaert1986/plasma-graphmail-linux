// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "quicksteps.h"
#include "categorylist.h"
#include "mailboxset.h"
#include "movedialog.h"
#include "store.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KMessageBox>
#include <KSharedConfig>

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
KConfigGroup group()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("QuickSteps"));
}

QJsonObject action(const char *type)
{
    return {{QStringLiteral("type"), QLatin1String(type)}};
}
}

QJsonArray QuickSteps::all()
{
    const QString saved = group().readEntry("Steps", QString());
    if (!saved.isEmpty()) {
        return QJsonDocument::fromJson(saved.toUtf8()).array();
    }
    return {QJsonObject{{QStringLiteral("name"), i18nc("@item a quick step", "Done")}, {QStringLiteral("actions"), QJsonArray{action("read"), action("complete")}}},
            QJsonObject{{QStringLiteral("name"), i18nc("@item a quick step", "Reply & Delete")}, {QStringLiteral("actions"), QJsonArray{action("reply"), action("delete")}}}};
}

void QuickSteps::save(const QJsonArray &steps)
{
    KConfigGroup g = group();
    g.writeEntry("Steps", QString::fromUtf8(QJsonDocument(steps).toJson(QJsonDocument::Compact)));
    g.sync();
}

QString QuickSteps::actionText(const QJsonObject &a)
{
    const QString type = a.value(QLatin1String("type")).toString();
    if (type == QLatin1String("read")) {
        return i18nc("@item quick step action", "Mark as read");
    }
    if (type == QLatin1String("unread")) {
        return i18nc("@item quick step action", "Mark as unread");
    }
    if (type == QLatin1String("flag")) {
        return i18nc("@item quick step action", "Flag");
    }
    if (type == QLatin1String("complete")) {
        return i18nc("@item quick step action", "Mark the flag complete");
    }
    if (type == QLatin1String("clearflag")) {
        return i18nc("@item quick step action", "Clear the flag");
    }
    if (type == QLatin1String("categorize")) {
        return i18nc("@item quick step action, %1 a category", "Categorize as %1", a.value(QLatin1String("category")).toString());
    }
    if (type == QLatin1String("move")) {
        return i18nc("@item quick step action, %1 a folder", "Move to %1", a.value(QLatin1String("folderName")).toString());
    }
    if (type == QLatin1String("delete")) {
        return i18nc("@item quick step action", "Delete");
    }
    if (type == QLatin1String("archive")) {
        return i18nc("@item quick step action", "Archive");
    }
    if (type == QLatin1String("reply")) {
        return i18nc("@item quick step action", "Reply");
    }
    if (type == QLatin1String("replyAll")) {
        return i18nc("@item quick step action", "Reply all");
    }
    if (type == QLatin1String("forward")) {
        const QString to = a.value(QLatin1String("to")).toString();
        return to.isEmpty() ? i18nc("@item quick step action", "Forward") : i18nc("@item quick step action, %1 addresses", "Forward to %1", to);
    }
    return type;
}

QString QuickSteps::describe(const QJsonObject &step)
{
    QStringList parts;
    const QJsonArray actions = step.value(QLatin1String("actions")).toArray();
    for (const QJsonValue &a : actions) {
        parts.append(actionText(a.toObject()));
    }
    return parts.join(QStringLiteral(", "));
}

QuickStepsDialog::QuickStepsDialog(MailboxSet *mailboxes, FolderModel *folders, CategoryList *categories, const QString &mailboxKey, QWidget *parent)
    : QDialog(parent)
    , m_mailboxes(mailboxes)
    , m_folders(folders)
    , m_categories(categories)
    , m_mailboxKey(mailboxKey)
    , m_steps(QuickSteps::all())
    , m_list(new QListWidget(this))
{
    setWindowTitle(i18nc("@title:window", "Manage Quick Steps"));
    auto *add = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), i18nc("@action:button", "New..."), this);
    auto *change = new QPushButton(QIcon::fromTheme(QStringLiteral("document-edit")), i18nc("@action:button", "Change..."), this);
    auto *remove = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action:button", "Delete"), this);
    auto *up = new QPushButton(QIcon::fromTheme(QStringLiteral("go-up")), i18nc("@action:button", "Move Up"), this);
    auto *down = new QPushButton(QIcon::fromTheme(QStringLiteral("go-down")), i18nc("@action:button", "Move Down"), this);
    connect(add, &QPushButton::clicked, this, [this] {
        QJsonObject step;
        if (edit(&step)) {
            m_steps.append(step);
            fill();
            m_list->setCurrentRow(int(m_steps.size()) - 1);
        }
    });
    auto changeRow = [this] {
        const int row = m_list->currentRow();
        if (row < 0) {
            return;
        }
        QJsonObject step = m_steps.at(row).toObject();
        if (edit(&step)) {
            m_steps[row] = step;
            fill();
            m_list->setCurrentRow(row);
        }
    };
    connect(change, &QPushButton::clicked, this, changeRow);
    connect(m_list, &QListWidget::itemDoubleClicked, this, changeRow);
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = m_list->currentRow();
        if (row >= 0) {
            m_steps.removeAt(row);
            fill();
        }
    });
    auto shift = [this](int step) {
        const int row = m_list->currentRow();
        const int other = row + step;
        if (row < 0 || other < 0 || other >= m_steps.size()) {
            return;
        }
        const QJsonValue a = m_steps.at(row);
        m_steps[row] = m_steps.at(other);
        m_steps[other] = a;
        fill();
        m_list->setCurrentRow(other);
    };
    connect(up, &QPushButton::clicked, this, [shift] {
        shift(-1);
    });
    connect(down, &QPushButton::clicked, this, [shift] {
        shift(1);
    });
    auto *side = new QVBoxLayout;
    for (QPushButton *b : {add, change, remove, up, down}) {
        side->addWidget(b);
    }
    side->addStretch();
    auto *middle = new QHBoxLayout;
    middle->addWidget(m_list, 1);
    middle->addLayout(side);
    auto *note = new QLabel(i18nc("@info", "Quick Steps are kept on this computer: Microsoft 365 does not share Outlook's with other apps."), this);
    note->setWordWrap(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(middle, 1);
    layout->addWidget(note);
    layout->addWidget(buttons);
    resize(620, 380);
    fill();
}

void QuickStepsDialog::fill()
{
    m_list->clear();
    for (const QJsonValue &v : std::as_const(m_steps)) {
        const QJsonObject step = v.toObject();
        auto *item = new QListWidgetItem(QIcon::fromTheme(QStringLiteral("quickopen")), step.value(QLatin1String("name")).toString(), m_list);
        item->setToolTip(QuickSteps::describe(step));
        item->setText(step.value(QLatin1String("name")).toString() + QStringLiteral(": ") + QuickSteps::describe(step));
    }
}

bool QuickStepsDialog::edit(QJsonObject *step)
{
    QDialog dialog(this);
    dialog.setWindowTitle(step->isEmpty() ? i18nc("@title:window", "New Quick Step") : i18nc("@title:window", "Change Quick Step"));
    auto *name = new QLineEdit(step->value(QLatin1String("name")).toString(), &dialog);
    auto *list = new QListWidget(&dialog);
    QJsonArray actions = step->value(QLatin1String("actions")).toArray();
    auto refresh = [list, &actions] {
        list->clear();
        for (const QJsonValue &a : std::as_const(actions)) {
            list->addItem(QuickSteps::actionText(a.toObject()));
        }
    };
    refresh();
    auto *add = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), i18nc("@action:button", "Add Action"), &dialog);
    auto *menu = new QMenu(add);
    const std::pair<const char *, KLocalizedString> simple[] = {{"read", ki18nc("@action:inmenu quick step action", "Mark as Read")},
                                                               {"unread", ki18nc("@action:inmenu quick step action", "Mark as Unread")},
                                                               {"flag", ki18nc("@action:inmenu quick step action", "Flag")},
                                                               {"complete", ki18nc("@action:inmenu quick step action", "Mark Flag Complete")},
                                                               {"clearflag", ki18nc("@action:inmenu quick step action", "Clear Flag")},
                                                               {"delete", ki18nc("@action:inmenu quick step action", "Delete")},
                                                               {"archive", ki18nc("@action:inmenu quick step action", "Archive")},
                                                               {"reply", ki18nc("@action:inmenu quick step action", "Reply")},
                                                               {"replyAll", ki18nc("@action:inmenu quick step action", "Reply All")}};
    for (const auto &[type, text] : simple) {
        const QString t = QLatin1String(type);
        menu->addAction(text.toString(), &dialog, [&actions, refresh, t] {
            actions.append(QJsonObject{{QStringLiteral("type"), t}});
            refresh();
        });
    }
    menu->addAction(i18nc("@action:inmenu quick step action", "Move to Folder..."), &dialog, [this, &dialog, &actions, refresh] {
        MoveDialog chooser(m_folders, m_mailboxKey, &dialog);
        Store *store = m_mailboxes->store(m_mailboxKey);
        if (chooser.exec() != QDialog::Accepted || chooser.folder() == 0 || !store) {
            return;
        }
        if (const std::optional<FolderRow> f = store->folder(chooser.folder())) {
            actions.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("move")},
                                       {QStringLiteral("mailbox"), m_mailboxKey},
                                       {QStringLiteral("folder"), f->graphId},
                                       {QStringLiteral("folderName"), f->name}});
            refresh();
        }
    });
    menu->addAction(i18nc("@action:inmenu quick step action", "Categorize..."), &dialog, [this, &dialog, &actions, refresh] {
        QStringList names;
        const QList<CategoryList::Category> list = m_categories->categories(m_mailboxKey);
        for (const CategoryList::Category &c : list) {
            names.append(c.name);
        }
        bool ok = false;
        const QString picked = QInputDialog::getItem(&dialog, i18nc("@title:window", "Categorize"), i18nc("@label:listbox", "Category:"), names, 0, false, &ok);
        if (ok && !picked.isEmpty()) {
            actions.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("categorize")}, {QStringLiteral("category"), picked}});
            refresh();
        }
    });
    menu->addAction(i18nc("@action:inmenu quick step action", "Forward To..."), &dialog, [&dialog, &actions, refresh] {
        bool ok = false;
        const QString to = QInputDialog::getText(&dialog, i18nc("@title:window", "Forward To"), i18nc("@label:textbox", "Addresses (empty: choose when it runs):"),
                                                 QLineEdit::Normal, {}, &ok);
        if (ok) {
            actions.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("forward")}, {QStringLiteral("to"), to.trimmed()}});
            refresh();
        }
    });
    add->setMenu(menu);
    auto *remove = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), i18nc("@action:button", "Remove Action"), &dialog);
    connect(remove, &QPushButton::clicked, &dialog, [list, &actions, refresh] {
        if (list->currentRow() >= 0) {
            actions.removeAt(list->currentRow());
            refresh();
        }
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *row = new QHBoxLayout;
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch();
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(i18nc("@label:textbox", "Name:"), &dialog));
    layout->addWidget(name);
    layout->addWidget(new QLabel(i18nc("@label", "Actions, in this order:"), &dialog));
    layout->addWidget(list, 1);
    layout->addLayout(row);
    layout->addWidget(buttons);
    dialog.resize(440, 380);
    while (dialog.exec() == QDialog::Accepted) {
        if (name->text().trimmed().isEmpty() || actions.isEmpty()) {
            KMessageBox::error(&dialog, i18nc("@info", "Give the Quick Step a name and at least one action."));
            continue;
        }
        step->insert(QStringLiteral("name"), name->text().trimmed());
        step->insert(QStringLiteral("actions"), actions);
        return true;
    }
    return false;
}

void QuickStepsDialog::accept()
{
    QuickSteps::save(m_steps);
    QDialog::accept();
}

}
