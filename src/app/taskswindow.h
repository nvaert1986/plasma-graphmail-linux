// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <KXmlGuiWindow>

#include <QJsonObject>
#include <QTimer>

class KToggleAction;
class QCheckBox;
class QDateEdit;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTextEdit;
class QToolButton;
class QTreeWidget;

namespace GraphMail
{

class CategoryList;
class DaemonClient;
class MailboxSet;

// Microsoft To Do (docs/PHASE6.md): the lists on the left, their tasks with a tick box in the
// middle, the selected task's details beside them. "Flagged Emails" holds the flagged mail.
class TasksWindow : public KXmlGuiWindow
{
    Q_OBJECT

public:
    // File > Print: the list shown (docs/PHASE9B.md); tests: into a PDF
    void printList(const QString &pdfFile = {});
    TasksWindow(MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent = nullptr);

    // Selects a task (a reminder's Open)
    void showTask(const QString &mailboxKey, qint64 task);
    // Tests: the same paths as the window's controls; only titles marked [TEST Graph Mail]
    bool addTaskForTests(const QString &listName, const QString &title);
    bool completeForTests(const QString &mailboxKey, qint64 task, bool done);
    bool editTaskForTests(const QString &mailboxKey, qint64 task, const QJsonObject &fields);
    bool addStepForTests(const QString &mailboxKey, qint64 task, const QString &step);
    // Ticks the step with this name once the steps are loaded; logs the outcome
    void tickStepForTests(const QString &mailboxKey, qint64 task, const QString &step);
    // Clicks Open the Mail; true when the flagged mail was found in the cache
    bool openMailForTests(const QString &mailboxKey, qint64 task);
    bool deleteTaskForTests(const QString &mailboxKey, qint64 task);
    bool listActionForTests(const QString &action, const QString &name, const QString &newName);
    // Categories (docs/PHASE9.md): shown in the details, Categorize
    void setCategoryList(CategoryList *categories);
    bool categorizeForTests(const QString &mailboxKey, qint64 task, const QString &name, bool on);

Q_SIGNALS:
    void navigate(int module);
    // A flagged mail's task: show the mail
    void openMail(const QString &mailboxKey, qint64 message);

private:
    struct Task {
        QString mailboxKey;
        qint64 id = 0;
        qint64 list = 0;
        QJsonObject json;
    };

    void setupActions();
    void fillLists();
    void listChanged();
    void fillTasks();
    void showDetails();
    void loadSteps();
    std::optional<Task> currentTask() const;
    QPair<QString, qint64> currentList() const;
    void addTask();
    void setDone(const Task &task, bool done);
    void saveDetails();
    QJsonObject detailChanges(const Task &task) const;
    void deleteTask(const Task &task);
    void newList();
    void renameList();
    void deleteList();
    bool isTestTask(const QString &mailboxKey, qint64 task) const;
    void setTaskCategory(const Task &task, const QString &name, bool on);

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
    QTreeWidget *m_lists;
    QLineEdit *m_add;
    QTreeWidget *m_tasks;
    QWidget *m_details;
    QLineEdit *m_title;
    QCheckBox *m_hasDue;
    QDateEdit *m_due;
    QCheckBox *m_remind;
    QDateTimeEdit *m_reminder;
    QCheckBox *m_important;
    QListWidget *m_steps;
    QLineEdit *m_newStep;
    QTextEdit *m_notes;
    QPushButton *m_openMail;
    QPushButton *m_save;
    QPushButton *m_delete;
    QLabel *m_created;
    QLabel *m_categoryLabels = nullptr;
    QToolButton *m_categorize = nullptr;
    CategoryList *m_categoryList = nullptr;
    KToggleAction *m_showCompleted = nullptr;
    QList<Task> m_shown;
    QTimer m_reloadTimer;
    bool m_filling = false;
};

}
