// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "contactdialog.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLineEdit>
#include <QTextEdit>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
QString text(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString();
}

QString firstOf(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toArray().first().toString();
}
}

ContactDialog::ContactDialog(const QJsonObject &contact, QWidget *parent)
    : QDialog(parent)
    , m_original(contact)
    , m_given(new QLineEdit(this))
    , m_surname(new QLineEdit(this))
    , m_display(new QLineEdit(this))
    , m_company(new QLineEdit(this))
    , m_jobTitle(new QLineEdit(this))
    , m_department(new QLineEdit(this))
    , m_business(new QLineEdit(this))
    , m_mobile(new QLineEdit(this))
    , m_home(new QLineEdit(this))
    , m_street(new QLineEdit(this))
    , m_postalCode(new QLineEdit(this))
    , m_city(new QLineEdit(this))
    , m_country(new QLineEdit(this))
    , m_hasBirthday(new QCheckBox(i18nc("@option:check", "Birthday:"), this))
    , m_birthday(new QDateEdit(this))
    , m_notes(new QTextEdit(this))
{
    setWindowTitle(contact.isEmpty() ? i18nc("@title:window", "New Contact") : text(contact, "displayName"));
    for (int i = 0; i < 3; ++i) {
        m_emails.append(new QLineEdit(this));
    }
    m_display->setPlaceholderText(i18nc("@info:placeholder", "From the first and last name"));
    m_birthday->setCalendarPopup(true);
    m_birthday->setDisplayFormat(QStringLiteral("dd-MM-yyyy"));
    m_birthday->setEnabled(false);
    connect(m_hasBirthday, &QCheckBox::toggled, m_birthday, &QWidget::setEnabled);

    // As in Outlook, the full name follows the first and last name until it is typed
    auto follow = [this] {
        if (!m_display->isModified()) {
            m_display->setText(QStringList{m_given->text().trimmed(), m_surname->text().trimmed()}.join(QLatin1Char(' ')).trimmed());
        }
    };
    connect(m_given, &QLineEdit::textEdited, this, follow);
    connect(m_surname, &QLineEdit::textEdited, this, follow);

    auto *name = new QGroupBox(i18nc("@title:group", "Name"), this);
    auto *nameForm = new QFormLayout(name);
    nameForm->addRow(i18nc("@label:textbox", "First name:"), m_given);
    nameForm->addRow(i18nc("@label:textbox", "Last name:"), m_surname);
    nameForm->addRow(i18nc("@label:textbox", "Full name:"), m_display);
    nameForm->addRow(i18nc("@label:textbox", "Company:"), m_company);
    nameForm->addRow(i18nc("@label:textbox", "Job title:"), m_jobTitle);
    nameForm->addRow(i18nc("@label:textbox", "Department:"), m_department);

    auto *reach = new QGroupBox(i18nc("@title:group", "E-mail and Phone"), this);
    auto *reachForm = new QFormLayout(reach);
    for (int i = 0; i < m_emails.size(); ++i) {
        reachForm->addRow(i18nc("@label:textbox, %1 1-3", "E-mail %1:", i + 1), m_emails.at(i));
    }
    reachForm->addRow(i18nc("@label:textbox", "Business phone:"), m_business);
    reachForm->addRow(i18nc("@label:textbox", "Mobile:"), m_mobile);
    reachForm->addRow(i18nc("@label:textbox", "Home phone:"), m_home);

    auto *address = new QGroupBox(i18nc("@title:group", "Business Address"), this);
    auto *addressForm = new QFormLayout(address);
    addressForm->addRow(i18nc("@label:textbox", "Street:"), m_street);
    auto *place = new QHBoxLayout;
    place->addWidget(m_postalCode, 1);
    place->addWidget(m_city, 3);
    addressForm->addRow(i18nc("@label:textbox", "Postal code, city:"), place);
    addressForm->addRow(i18nc("@label:textbox", "Country:"), m_country);
    auto *birthday = new QHBoxLayout;
    birthday->addWidget(m_hasBirthday);
    birthday->addWidget(m_birthday);
    birthday->addStretch();
    addressForm->addRow(birthday);

    auto *columns = new QHBoxLayout;
    auto *left = new QVBoxLayout;
    left->addWidget(name);
    left->addWidget(address);
    auto *right = new QVBoxLayout;
    right->addWidget(reach);
    right->addWidget(m_notes, 1);
    columns->addLayout(left, 1);
    columns->addLayout(right, 1);
    m_notes->setPlaceholderText(i18nc("@info:placeholder", "Notes"));
    m_notes->setAcceptRichText(false);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(columns, 1);
    layout->addWidget(buttons);
    resize(820, 520);

    // What the contact has
    m_given->setText(text(contact, "givenName"));
    m_surname->setText(text(contact, "surname"));
    m_display->setText(text(contact, "displayName"));
    m_display->setModified(!contact.isEmpty());
    m_company->setText(text(contact, "companyName"));
    m_jobTitle->setText(text(contact, "jobTitle"));
    m_department->setText(text(contact, "department"));
    const QJsonArray emails = contact.value(QLatin1String("emailAddresses")).toArray();
    for (int i = 0; i < m_emails.size() && i < emails.size(); ++i) {
        m_emails[i]->setText(emails.at(i).toObject().value(QLatin1String("address")).toString());
    }
    m_business->setText(firstOf(contact, "businessPhones"));
    m_mobile->setText(text(contact, "mobilePhone"));
    m_home->setText(firstOf(contact, "homePhones"));
    const QJsonObject a = contact.value(QLatin1String("businessAddress")).toObject();
    m_street->setText(text(a, "street"));
    m_postalCode->setText(text(a, "postalCode"));
    m_city->setText(text(a, "city"));
    m_country->setText(text(a, "countryOrRegion"));
    const QDate born = QDate::fromString(text(contact, "birthday").left(10), Qt::ISODate);
    m_hasBirthday->setChecked(born.isValid());
    m_birthday->setDate(born.isValid() ? born : QDate(1980, 1, 1));
    m_notes->setPlainText(text(contact, "personalNotes"));
    m_given->setFocus();
}

QJsonObject ContactDialog::json() const
{
    QJsonObject c;
    c.insert(QStringLiteral("givenName"), m_given->text().trimmed());
    c.insert(QStringLiteral("surname"), m_surname->text().trimmed());
    QString display = m_display->text().trimmed();
    if (display.isEmpty()) {
        display = QStringList{m_given->text().trimmed(), m_surname->text().trimmed()}.join(QLatin1Char(' ')).trimmed();
    }
    if (display.isEmpty()) {
        display = m_emails.first()->text().trimmed();
    }
    c.insert(QStringLiteral("displayName"), display);
    c.insert(QStringLiteral("companyName"), m_company->text().trimmed());
    c.insert(QStringLiteral("jobTitle"), m_jobTitle->text().trimmed());
    c.insert(QStringLiteral("department"), m_department->text().trimmed());
    QJsonArray emails;
    for (QLineEdit *e : m_emails) {
        if (!e->text().trimmed().isEmpty()) {
            emails.append(QJsonObject{{QStringLiteral("address"), e->text().trimmed()}, {QStringLiteral("name"), display}});
        }
    }
    c.insert(QStringLiteral("emailAddresses"), emails);
    c.insert(QStringLiteral("businessPhones"), m_business->text().trimmed().isEmpty() ? QJsonArray() : QJsonArray{m_business->text().trimmed()});
    c.insert(QStringLiteral("mobilePhone"), m_mobile->text().trimmed());
    c.insert(QStringLiteral("homePhones"), m_home->text().trimmed().isEmpty() ? QJsonArray() : QJsonArray{m_home->text().trimmed()});
    c.insert(QStringLiteral("businessAddress"),
             QJsonObject{{QStringLiteral("street"), m_street->text().trimmed()},
                         {QStringLiteral("postalCode"), m_postalCode->text().trimmed()},
                         {QStringLiteral("city"), m_city->text().trimmed()},
                         {QStringLiteral("countryOrRegion"), m_country->text().trimmed()}});
    c.insert(QStringLiteral("birthday"), m_hasBirthday->isChecked() ? QJsonValue(m_birthday->date().toString(Qt::ISODate) + QStringLiteral("T11:59:00Z")) : QJsonValue());
    c.insert(QStringLiteral("personalNotes"), m_notes->toPlainText());
    return c;
}

QJsonObject ContactDialog::changes() const
{
    const QJsonObject now = json();
    QJsonObject diff;
    for (auto it = now.constBegin(); it != now.constEnd(); ++it) {
        QJsonValue before = m_original.value(it.key());
        // Graph leaves out what is empty: empty and missing are the same
        const bool emptyNow = it.value().isNull() || (it.value().isString() && it.value().toString().isEmpty()) || (it.value().isArray() && it.value().toArray().isEmpty());
        const bool emptyBefore = before.isUndefined() || before.isNull() || (before.isString() && before.toString().isEmpty()) || (before.isArray() && before.toArray().isEmpty());
        if (emptyNow && emptyBefore) {
            continue;
        }
        if (it.key() == QLatin1String("emailAddresses")) {
            // Compared by address only (Graph adds names)
            QStringList a;
            QStringList b;
            const QJsonArray now = it.value().toArray();
            const QJsonArray was = before.toArray();
            for (const QJsonValue &v : now) {
                a.append(v.toObject().value(QLatin1String("address")).toString().toLower());
            }
            for (const QJsonValue &v : was) {
                b.append(v.toObject().value(QLatin1String("address")).toString().toLower());
            }
            if (a == b) {
                continue;
            }
        } else if (it.key() == QLatin1String("businessAddress")) {
            const QJsonObject x = it.value().toObject();
            const QJsonObject y = before.toObject();
            bool same = true;
            for (auto k = x.constBegin(); k != x.constEnd(); ++k) {
                same = same && k.value().toString() == y.value(k.key()).toString();
            }
            if (same) {
                continue;
            }
        } else if (it.key() == QLatin1String("birthday") && it.value().toString().left(10) == before.toString().left(10)) {
            continue;
        } else if (it.value() == before) {
            continue;
        }
        diff.insert(it.key(), it.value());
    }
    return diff;
}

void ContactDialog::setFields(const QJsonObject &fields)
{
    if (fields.contains(QLatin1String("givenName"))) {
        m_given->setText(fields.value(QLatin1String("givenName")).toString());
    }
    if (fields.contains(QLatin1String("surname"))) {
        m_surname->setText(fields.value(QLatin1String("surname")).toString());
    }
    if (fields.contains(QLatin1String("displayName"))) {
        m_display->setText(fields.value(QLatin1String("displayName")).toString());
        m_display->setModified(true);
    }
    if (fields.contains(QLatin1String("companyName"))) {
        m_company->setText(fields.value(QLatin1String("companyName")).toString());
    }
    if (fields.contains(QLatin1String("jobTitle"))) {
        m_jobTitle->setText(fields.value(QLatin1String("jobTitle")).toString());
    }
    if (fields.contains(QLatin1String("email"))) {
        m_emails.first()->setText(fields.value(QLatin1String("email")).toString());
    }
    if (fields.contains(QLatin1String("mobilePhone"))) {
        m_mobile->setText(fields.value(QLatin1String("mobilePhone")).toString());
    }
    if (fields.contains(QLatin1String("city"))) {
        m_city->setText(fields.value(QLatin1String("city")).toString());
    }
}

}
