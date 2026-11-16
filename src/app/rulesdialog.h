// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace GraphMail
{

class CategoryList;
class DaemonClient;
class FolderModel;
class MailboxSet;

// One inbox rule (Graph's messageRule) in Outlook's words: the conditions and actions people use.
// What the rule has beyond those (made in Outlook: exceptions, other conditions) is kept as it is.
class RuleEditor : public QDialog
{
    Q_OBJECT

public:
    RuleEditor(MailboxSet *mailboxes, FolderModel *folders, CategoryList *categories, const QString &mailboxKey, const QJsonObject &rule, QWidget *parent = nullptr);

    // The rule as Graph takes it for a POST or PATCH
    QJsonObject rule() const;
    void accept() override;

    // Tests: "subject" fills Subject contains, then Mark as read and Importance high
    void fillForTests(const QString &name, const QString &subject);

private:
    struct FolderChoice {
        QCheckBox *check;
        QPushButton *button;
        QString graphId;
    };
    void chooseFolder(FolderChoice *choice);
    void showFolder(FolderChoice *choice);

    MailboxSet *m_mailboxes;
    FolderModel *m_folders;
    QString m_mailboxKey;
    QJsonObject m_rule;
    QLineEdit *m_name;
    QCheckBox *m_enabled;
    QCheckBox *m_from;
    QLineEdit *m_fromText;
    QCheckBox *m_sentToMe;
    QCheckBox *m_sentOnlyToMe;
    QCheckBox *m_subject;
    QLineEdit *m_subjectText;
    QCheckBox *m_bodyOrSubject;
    QLineEdit *m_bodyOrSubjectText;
    QCheckBox *m_senderContains;
    QLineEdit *m_senderContainsText;
    QCheckBox *m_hasAttachments;
    QCheckBox *m_importance;
    QComboBox *m_importanceValue;
    FolderChoice m_move;
    FolderChoice m_copy;
    QCheckBox *m_delete;
    QCheckBox *m_markRead;
    QCheckBox *m_markImportance;
    QComboBox *m_markImportanceValue;
    QCheckBox *m_categorize;
    QComboBox *m_category;
    QCheckBox *m_forward;
    QLineEdit *m_forwardText;
    QCheckBox *m_stop;
    QLabel *m_status;
};

// Settings > Rules... (docs/PHASE9.md): the own mailbox's inbox rules in their order, on/off,
// new, change, delete, move up and down. Exchange runs them, so they also work while Graph Mail
// is closed, and Outlook shows the same rules.
class RulesDialog : public QDialog
{
    Q_OBJECT

public:
    RulesDialog(MailboxSet *mailboxes, DaemonClient *daemon, FolderModel *folders, CategoryList *categories, const QString &mailboxKey, QWidget *parent = nullptr);

    // A rule in a sentence: "From ...; subject contains ... : move to ..., mark as read"
    static QString summary(const QJsonObject &rule, MailboxSet *mailboxes, const QString &mailboxKey);

    // Tests, once loaded: create (name|subject), toggle (name), up (name), delete (name); logs
    void runForTests(const QString &spec);
    // Tests: the editor for a new rule, filled as for "create", shown and not saved
    RuleEditor *editorForTests(const QString &name, const QString &subject);
    // Create Rule from a message: the editor with this draft once the rules are in, then saved last
    void createFrom(const QJsonObject &draft);

Q_SIGNALS:
    void loadedForTests();

private:
    void load();
    void fill();
    void call(const QString &kind, const QJsonObject &args);
    QJsonObject selected() const;
    QJsonObject byName(const QString &name) const;
    void edit(const QJsonObject &rule, bool forTests = false, const QString &name = {}, const QString &subject = {});
    void move(const QJsonObject &rule, int step);
    void remove(const QJsonObject &rule, bool ask);
    void updateButtons();

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
    FolderModel *m_folders;
    CategoryList *m_categories;
    QComboBox *m_mailbox;
    QTreeWidget *m_list;
    QPushButton *m_new;
    QPushButton *m_change;
    QPushButton *m_delete;
    QPushButton *m_up;
    QPushButton *m_down;
    QLabel *m_status;
    QJsonArray m_rules;
    bool m_filling = false;
};

}
