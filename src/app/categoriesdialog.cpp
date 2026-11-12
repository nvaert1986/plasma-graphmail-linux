// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "categoriesdialog.h"
#include "accesstext.h"
#include "categorylist.h"
#include "daemonclient.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
QIcon swatch(const QColor &color)
{
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(QRect(0, 0, 14, 14), 3, 3);
    return QIcon(pixmap);
}
}

QComboBox *CategoriesDialog::colorCombo(QWidget *parent)
{
    auto *combo = new QComboBox(parent);
    const QStringList presets = CategoryList::presets();
    for (const QString &preset : presets) {
        combo->addItem(swatch(CategoryList::presetColor(preset)), CategoryList::presetName(preset), preset);
    }
    return combo;
}

CategoriesDialog::CategoriesDialog(CategoryList *categories, DaemonClient *daemon, const QString &mailboxKey, QWidget *parent)
    : QDialog(parent)
    , m_categories(categories)
    , m_daemon(daemon)
    , m_mailboxKey(mailboxKey)
    , m_list(new QTreeWidget(this))
    , m_name(new QLineEdit(this))
    , m_color(colorCombo(this))
    , m_add(new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), i18nc("@action:button", "New"), this))
    , m_recolor(colorCombo(this))
    , m_delete(new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action:button", "Delete"), this))
    , m_status(new QLabel(this))
{
    setWindowTitle(i18nc("@title:window", "Categories"));
    resize(480, 460);
    m_list->setHeaderLabels({i18nc("@title:column", "Category"), i18nc("@title:column", "Colour")});
    m_list->setRootIsDecorated(false);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_name->setPlaceholderText(i18nc("@info:placeholder", "Name of a new category"));
    m_status->setWordWrap(true);
    m_add->setEnabled(false);
    m_recolor->setEnabled(false);
    m_delete->setEnabled(false);

    auto *newRow = new QHBoxLayout;
    newRow->addWidget(m_name, 1);
    newRow->addWidget(m_color);
    newRow->addWidget(m_add);
    auto *selectedRow = new QHBoxLayout;
    selectedRow->addWidget(new QLabel(i18nc("@label:listbox", "Colour of the selected one:"), this));
    selectedRow->addWidget(m_recolor);
    selectedRow->addStretch();
    selectedRow->addWidget(m_delete);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_list, 1);
    layout->addLayout(selectedRow);
    layout->addLayout(newRow);
    layout->addWidget(m_status);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_name, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_add->setEnabled(!text.trimmed().isEmpty());
    });
    connect(m_add, &QPushButton::clicked, this, [this] {
        call(QStringLiteral("categoryCreate"), {{QStringLiteral("name"), m_name->text().trimmed()}, {QStringLiteral("color"), m_color->currentData().toString()}});
        m_name->clear();
    });
    connect(m_list, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        m_recolor->setEnabled(item);
        m_delete->setEnabled(item);
        if (item) {
            const QSignalBlocker blocker(m_recolor);
            m_recolor->setCurrentIndex(std::max(0, m_recolor->findData(item->data(1, Qt::UserRole).toString())));
        }
    });
    connect(m_recolor, &QComboBox::activated, this, [this] {
        if (const QTreeWidgetItem *item = m_list->currentItem()) {
            call(QStringLiteral("categoryUpdate"), {{QStringLiteral("id"), item->data(0, Qt::UserRole).toString()}, {QStringLiteral("color"), m_recolor->currentData().toString()}});
        }
    });
    connect(m_delete, &QPushButton::clicked, this, [this] {
        const QTreeWidgetItem *item = m_list->currentItem();
        if (item
            && KMessageBox::warningContinueCancel(this,
                                                  i18nc("@info %1 a category", "Delete the category %1? Items keep its name, without its colour.", item->text(0)),
                                                  i18nc("@title:window", "Delete Category"), KStandardGuiItem::del())
                == KMessageBox::Continue) {
            call(QStringLiteral("categoryDelete"), {{QStringLiteral("id"), item->data(0, Qt::UserRole).toString()}});
        }
    });
    connect(m_categories, &CategoryList::changed, this, [this](const QString &key) {
        if (key == m_mailboxKey) {
            fill();
        }
    });
    fill();
    m_categories->load(m_mailboxKey);
}

void CategoriesDialog::fill()
{
    m_list->clear();
    const QList<CategoryList::Category> list = m_categories->categories(m_mailboxKey);
    for (const CategoryList::Category &c : list) {
        auto *item = new QTreeWidgetItem(m_list, {c.name, CategoryList::presetName(c.preset)});
        item->setIcon(0, swatch(CategoryList::presetColor(c.preset)));
        item->setData(0, Qt::UserRole, c.id);
        item->setData(1, Qt::UserRole, c.preset);
    }
}

void CategoriesDialog::call(const QString &kind, const QJsonObject &args)
{
    m_status->setText(i18nc("@info:status", "Saving..."));
    m_daemon->query(m_mailboxKey.section(QLatin1Char('/'), 0, 0), m_mailboxKey.section(QLatin1Char('/'), 1), kind,
                    QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this, [this](const QString &json) {
                        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                        qWarning() << "categories test:" << answer.value(QLatin1String("ok")).toBool() << answer.value(QLatin1String("detail")).toString();
                        m_status->setText(answer.value(QLatin1String("ok")).toBool()
                                              ? QString()
                                              : accessProblemText(answer.isEmpty() ? QStringLiteral("error") : answer.value(QLatin1String("problem")).toString(), m_mailboxKey));
                        m_categories->load(m_mailboxKey);
                    });
}

QString CategoriesDialog::idOf(const QString &name) const
{
    const QList<CategoryList::Category> list = m_categories->categories(m_mailboxKey);
    for (const CategoryList::Category &c : list) {
        if (c.name == name) {
            return c.id;
        }
    }
    return {};
}

void CategoriesDialog::createForTests(const QString &name, const QString &preset)
{
    call(QStringLiteral("categoryCreate"), {{QStringLiteral("name"), name}, {QStringLiteral("color"), preset}});
}

void CategoriesDialog::recolorForTests(const QString &name, const QString &preset)
{
    call(QStringLiteral("categoryUpdate"), {{QStringLiteral("id"), idOf(name)}, {QStringLiteral("color"), preset}});
}

void CategoriesDialog::deleteForTests(const QString &name)
{
    call(QStringLiteral("categoryDelete"), {{QStringLiteral("id"), idOf(name)}});
}

}
