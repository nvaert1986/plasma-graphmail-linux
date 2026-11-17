// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QCheckBox;
class QLabel;
class QPushButton;

namespace GraphCore
{
class UpdateChecker;
}

namespace GraphMail
{

// Settings > Updates: the daily check for a new Graph Mail release on GitHub (on or off), what it
// found, Check Now, the release notes and Skip This Version. graphmaild does the daily check and
// the notification; this reads and writes the same state.
class UpdatesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit UpdatesDialog(QWidget *parent = nullptr);

    // Tests: press Check Now
    void checkNowForTests();
    QString statusForTests() const;

private:
    void refresh(const QString &error = {});

    GraphCore::UpdateChecker *m_checker;
    QCheckBox *m_enabled;
    QLabel *m_status;
    QPushButton *m_checkNow;
    QPushButton *m_notes;
    QPushButton *m_skip;
};

}
