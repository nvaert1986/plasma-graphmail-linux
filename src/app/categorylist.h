// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>

#include <functional>

class QMenu;

namespace GraphMail
{

class DaemonClient;

// The master categories of each mailbox (docs/PHASE9.md): names, ids and colours, read through
// the daemon and kept here for the lists, the reading pane and the calendar.
class CategoryList : public QObject
{
    Q_OBJECT

public:
    struct Category {
        QString id;
        QString name;
        // Outlook's colour: "preset0" .. "preset24", or "none"
        QString preset;
    };

    explicit CategoryList(DaemonClient *daemon, QObject *parent = nullptr);

    // Reads a mailbox's categories again (on start, after a change)
    void load(const QString &mailboxKey);
    QList<Category> categories(const QString &mailboxKey) const;
    // A category's colour; one the mailbox does not know is grey, as in Outlook
    QColor color(const QString &mailboxKey, const QString &name) const;
    bool contains(const QString &mailboxKey, const QString &name) const;

    // A Categorize menu: the mailbox's categories to tick (ticked: in current); toggled gets the
    // name and the new state
    void fillMenu(QMenu *menu, const QString &mailboxKey, const QStringList &current, const std::function<void(const QString &, bool)> &toggled) const;
    // Category labels in their colours as rich text (cards, details)
    QString labelsHtml(const QString &mailboxKey, const QStringList &names) const;

    // Outlook's 25 colours and their names
    static QStringList presets();
    static QColor presetColor(const QString &preset);
    static QString presetName(const QString &preset);

Q_SIGNALS:
    void changed(const QString &mailboxKey);

private:
    DaemonClient *m_daemon;
    QHash<QString, QList<Category>> m_categories;
};

}
