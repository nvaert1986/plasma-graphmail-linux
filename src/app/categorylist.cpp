// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "categorylist.h"
#include "daemonclient.h"

#include <KLocalizedString>

#include <QAction>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QPixmap>

namespace GraphMail
{

namespace
{
struct Preset {
    const char *rgb;
    KLocalizedString name;
};

const QList<Preset> &presetTable()
{
    // Outlook's category colours, preset0 .. preset24
    static const QList<Preset> table{
        {"#e74856", ki18nc("@item category colour", "Red")},          {"#f7630c", ki18nc("@item category colour", "Orange")},
        {"#8e562e", ki18nc("@item category colour", "Brown")},        {"#ffb900", ki18nc("@item category colour", "Yellow")},
        {"#10893e", ki18nc("@item category colour", "Green")},        {"#00b7c3", ki18nc("@item category colour", "Teal")},
        {"#837c42", ki18nc("@item category colour", "Olive")},        {"#0078d4", ki18nc("@item category colour", "Blue")},
        {"#8764b8", ki18nc("@item category colour", "Purple")},       {"#c50f1f", ki18nc("@item category colour", "Cranberry")},
        {"#69797e", ki18nc("@item category colour", "Steel")},        {"#4a5459", ki18nc("@item category colour", "Dark steel")},
        {"#a0a0a0", ki18nc("@item category colour", "Grey")},         {"#505050", ki18nc("@item category colour", "Dark grey")},
        {"#000000", ki18nc("@item category colour", "Black")},        {"#a4262c", ki18nc("@item category colour", "Dark red")},
        {"#ca5010", ki18nc("@item category colour", "Dark orange")},  {"#603d30", ki18nc("@item category colour", "Dark brown")},
        {"#c19c00", ki18nc("@item category colour", "Dark yellow")},  {"#0b6a0b", ki18nc("@item category colour", "Dark green")},
        {"#005b70", ki18nc("@item category colour", "Dark teal")},    {"#5c5e15", ki18nc("@item category colour", "Dark olive")},
        {"#004e8c", ki18nc("@item category colour", "Dark blue")},    {"#4b3a76", ki18nc("@item category colour", "Dark purple")},
        {"#750b1c", ki18nc("@item category colour", "Dark cranberry")},
    };
    return table;
}

int presetIndex(const QString &preset)
{
    bool ok = false;
    const int index = preset.startsWith(QLatin1String("preset")) ? preset.mid(6).toInt(&ok) : -1;
    return ok && index >= 0 && index < presetTable().size() ? index : -1;
}
}

CategoryList::CategoryList(DaemonClient *daemon, QObject *parent)
    : QObject(parent)
    , m_daemon(daemon)
{
}

void CategoryList::load(const QString &mailboxKey)
{
    m_daemon->query(mailboxKey.section(QLatin1Char('/'), 0, 0), mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("masterCategories"), QStringLiteral("{}"), this,
                    [this, mailboxKey](const QString &json) {
                        if (json.isEmpty()) {
                            return; // offline: what was read before stays
                        }
                        QList<Category> list;
                        const QJsonArray value = QJsonDocument::fromJson(json.toUtf8()).object().value(QLatin1String("value")).toArray();
                        for (const QJsonValue &v : value) {
                            const QJsonObject c = v.toObject();
                            list.append({c.value(QLatin1String("id")).toString(), c.value(QLatin1String("displayName")).toString(), c.value(QLatin1String("color")).toString()});
                        }
                        m_categories.insert(mailboxKey, list);
                        Q_EMIT changed(mailboxKey);
                    });
}

QList<CategoryList::Category> CategoryList::categories(const QString &mailboxKey) const
{
    return m_categories.value(mailboxKey);
}

QColor CategoryList::color(const QString &mailboxKey, const QString &name) const
{
    const QList<Category> list = m_categories.value(mailboxKey);
    for (const Category &c : list) {
        if (c.name.compare(name, Qt::CaseInsensitive) == 0) {
            return presetColor(c.preset);
        }
    }
    return presetColor(QStringLiteral("preset12"));
}

bool CategoryList::contains(const QString &mailboxKey, const QString &name) const
{
    const QList<Category> list = m_categories.value(mailboxKey);
    return std::any_of(list.cbegin(), list.cend(), [&name](const Category &c) {
        return c.name.compare(name, Qt::CaseInsensitive) == 0;
    });
}

void CategoryList::fillMenu(QMenu *menu, const QString &mailboxKey, const QStringList &current, const std::function<void(const QString &, bool)> &toggled) const
{
    menu->clear();
    const QList<Category> list = m_categories.value(mailboxKey);
    for (const Category &c : list) {
        QPixmap swatch(14, 14);
        swatch.fill(presetColor(c.preset));
        QAction *a = menu->addAction(QIcon(swatch), c.name);
        a->setCheckable(true);
        a->setChecked(current.contains(c.name));
        const QString name = c.name;
        QObject::connect(a, &QAction::toggled, menu, [toggled, name](bool on) {
            toggled(name, on);
        });
    }
    if (list.isEmpty()) {
        menu->addAction(i18nc("@item:inmenu", "(no categories)"))->setEnabled(false);
    }
}

QString CategoryList::labelsHtml(const QString &mailboxKey, const QStringList &names) const
{
    QStringList parts;
    for (const QString &name : names) {
        const QColor c = color(mailboxKey, name);
        parts.append(QStringLiteral("<span style=\"background-color:%1; color:%2\">&nbsp;%3&nbsp;</span>")
                         .arg(c.name(), c.lightnessF() > 0.6 ? QStringLiteral("#000000") : QStringLiteral("#ffffff"), name.toHtmlEscaped()));
    }
    return parts.join(QStringLiteral("&nbsp; "));
}

QStringList CategoryList::presets()
{
    QStringList names;
    for (int i = 0; i < presetTable().size(); ++i) {
        names.append(QStringLiteral("preset%1").arg(i));
    }
    return names;
}

QColor CategoryList::presetColor(const QString &preset)
{
    const int index = presetIndex(preset);
    return index < 0 ? QColor(QStringLiteral("#a0a0a0")) : QColor(QLatin1String(presetTable().at(index).rgb));
}

QString CategoryList::presetName(const QString &preset)
{
    const int index = presetIndex(preset);
    return index < 0 ? i18nc("@item category colour", "No colour") : presetTable().at(index).name.toString();
}

}
