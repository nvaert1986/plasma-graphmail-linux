// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace GraphMail
{

class CategoryList;
class DaemonClient;

// All Categories... (docs/PHASE9.md): a mailbox's categories as Outlook keeps them (the master
// list): new, another colour, delete. Graph cannot rename a category; a new one and deleting the
// old one is the way.
class CategoriesDialog : public QDialog
{
    Q_OBJECT

public:
    CategoriesDialog(CategoryList *categories, DaemonClient *daemon, const QString &mailboxKey, QWidget *parent = nullptr);

    // A combo of Outlook's 25 colours with their swatches
    static QComboBox *colorCombo(QWidget *parent);

    // Tests: the same paths as the buttons, without asking; log the outcome
    void createForTests(const QString &name, const QString &preset);
    void recolorForTests(const QString &name, const QString &preset);
    void deleteForTests(const QString &name);

private:
    void fill();
    void call(const QString &kind, const QJsonObject &args);
    QString idOf(const QString &name) const;

    CategoryList *m_categories;
    DaemonClient *m_daemon;
    QString m_mailboxKey;
    QTreeWidget *m_list;
    QLineEdit *m_name;
    QComboBox *m_color;
    QPushButton *m_add;
    QComboBox *m_recolor;
    QPushButton *m_delete;
    QLabel *m_status;
};

}
