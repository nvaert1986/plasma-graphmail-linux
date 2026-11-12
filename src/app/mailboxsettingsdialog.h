// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QJsonObject>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QTimeEdit;

namespace GraphMail
{

class DaemonClient;
class MailboxSet;
struct WorkingHours;

// Settings > Mailbox... (docs/PHASE9.md): the own mailbox's settings as Outlook on the web keeps
// them: working days and hours (and their time zone), the mailbox's time zone, language and date
// and time formats. Exchange uses them for meeting times, automatic replies and Outlook on the
// web; Graph Mail's calendar uses the working hours.
class MailboxSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    MailboxSettingsDialog(MailboxSet *mailboxes, DaemonClient *daemon, const QString &mailboxKey, QWidget *parent = nullptr);

    void accept() override;

    // Tests, once loaded: working days (1 = Monday, comma separated), start and end, saved as OK
    // does; logs the outcome
    void saveHoursForTests(const QString &days, const QString &start, const QString &end);

Q_SIGNALS:
    // Saved: the mailbox's settings as they are now (Graph's mailboxSettings, merged)
    void saved(const QString &mailboxKey, const QJsonObject &settings);
    void loadedForTests();

private:
    void load();
    void fill();
    // The format lists show an example in the chosen language
    void showExamples();
    WorkingHours hours() const;
    QJsonObject changes() const;
    void save(bool close);
    static void selectData(QComboBox *combo, const QString &value, const QString &text);

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
    QComboBox *m_mailbox;
    QCheckBox *m_days[7];
    QTimeEdit *m_start;
    QTimeEdit *m_end;
    QComboBox *m_workZone;
    QComboBox *m_zone;
    QComboBox *m_language;
    QComboBox *m_dateFormat;
    QComboBox *m_timeFormat;
    QLabel *m_status;
    QDialogButtonBox *m_buttons;
    QJsonObject m_settings;
    QList<QPair<QString, QString>> m_zones;
    QList<QPair<QString, QString>> m_languages;
    int m_pending = 0;
    bool m_failed = false;
};

}
