// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QJsonObject>

class QCheckBox;
class QDateEdit;
class QLineEdit;
class QTextEdit;

namespace GraphMail
{

// A contact's details as Outlook's contact form has them. changes() gives only
// what was changed, so a save never rewrites fields this dialog does not show.
class ContactDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ContactDialog(const QJsonObject &contact, QWidget *parent = nullptr);

    QJsonObject json() const;
    QJsonObject changes() const;
    // Tests: put these fields in as if typed
    void setFields(const QJsonObject &fields);

private:
    QJsonObject m_original;
    QLineEdit *m_given;
    QLineEdit *m_surname;
    QLineEdit *m_display;
    QLineEdit *m_company;
    QLineEdit *m_jobTitle;
    QLineEdit *m_department;
    QList<QLineEdit *> m_emails;
    QLineEdit *m_business;
    QLineEdit *m_mobile;
    QLineEdit *m_home;
    QLineEdit *m_street;
    QLineEdit *m_postalCode;
    QLineEdit *m_city;
    QLineEdit *m_country;
    QCheckBox *m_hasBirthday;
    QDateEdit *m_birthday;
    QTextEdit *m_notes;
};

}
