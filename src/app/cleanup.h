// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QList>

#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace GraphMail
{
class Store;
struct FolderRow;

class MailboxSet;

// Cleanup of old mail, like Outlook's AutoArchive (docs/PHASE9B.md 9b.18): off by default. One
// default (what to do with mail older than so many days) for every mail folder of the own
// mailboxes, and a folder may have its own setting or be left alone. Runs by hand (Clean Up Now)
// and, when chosen, every so many days while Graph Mail is open. Kept in graphmailrc [Cleanup].
namespace Cleanup
{

enum class Action { Archive, ExportPst, Delete, Report };

struct Settings {
    bool enabled = false;
    Action action = Action::Archive;
    int days = 365;
    // 0: only by hand
    int everyDays = 0;
    // Where Export to PST writes its files
    QString pstFolder;
    qint64 lastRun = 0;
};

struct FolderRule {
    enum class Mode { Default, Off, Own };
    Mode mode = Mode::Default;
    Action action = Action::Archive;
    int days = 365;
};

// What one folder would get: its messages older than the age, and what happens to them
struct Candidate {
    QString mailboxKey;
    qint64 folder = 0;
    QString folderName;
    Action action = Action::Report;
    int days = 0;
    QList<qint64> messages;
};

Settings settings();
void setSettings(const Settings &settings);
void setLastRun(qint64 ms);
FolderRule folderRule(const QString &mailboxKey, const QString &graphId);
void setFolderRule(const QString &mailboxKey, const QString &graphId, const FolderRule &rule);
// The folders a cleanup never touches: Deleted Items, Drafts, Outbox, Junk, the Archive itself
bool excluded(const QString &wellKnown);
// The same for a folder and everything under it (Archive/2024, a folder in Deleted Items, ...)
bool excluded(const Store *store, const FolderRow &folder);
// The effective rule of a folder: its own, the default when cleanup is on, or none
std::optional<FolderRule> effective(const Settings &settings, const FolderRule &rule);
// Every folder of the own mailboxes with mail older than its age
QList<Candidate> plan(MailboxSet *mailboxes, qint64 nowMs);
QString actionText(Action action);

// Settings > Clean Up Old Mail...
class Dialog : public QDialog
{
    Q_OBJECT

public:
    explicit Dialog(QWidget *parent = nullptr);
    void accept() override;

Q_SIGNALS:
    // Clean Up Now (the settings are saved first)
    void runRequested();

private:
    void save();
    void updateEnabled();

    QCheckBox *m_enabled;
    QComboBox *m_action;
    QSpinBox *m_days;
    QCheckBox *m_scheduled;
    QSpinBox *m_every;
    QLineEdit *m_pstFolder;
    QPushButton *m_browse;
    QLabel *m_lastRun;
};

// A folder's own cleanup setting (the folder's menu)
class FolderDialog : public QDialog
{
    Q_OBJECT

public:
    FolderDialog(const QString &mailboxKey, const QString &graphId, const QString &folderName, QWidget *parent = nullptr);
    void accept() override;

private:
    QString m_mailboxKey;
    QString m_graphId;
    QComboBox *m_mode;
    QComboBox *m_action;
    QSpinBox *m_days;
};

// What Clean Up Now would do, per folder; Run carries it out (unless every rule only reports)
class ReportDialog : public QDialog
{
    Q_OBJECT

public:
    ReportDialog(const QList<Candidate> &candidates, QWidget *parent = nullptr);
};

}

}
