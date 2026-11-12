// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "searchfolderdialog.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QTimeZone>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
QString odataText(QString text)
{
    return QLatin1Char('\'') + text.replace(QLatin1Char('\''), QStringLiteral("''")) + QLatin1Char('\'');
}
}

QString SearchFolderDialog::buildFilter(const QString &from, const QString &subject, bool unread, bool flagged, bool attachments, bool important, const QDate &since)
{
    QStringList parts;
    if (!from.trimmed().isEmpty()) {
        // The address or the name: the box takes either
        parts << QStringLiteral("(contains(from/emailAddress/address,%1) or contains(from/emailAddress/name,%1))").arg(odataText(from.trimmed()));
    }
    if (!subject.trimmed().isEmpty()) {
        parts << QStringLiteral("contains(subject,%1)").arg(odataText(subject.trimmed()));
    }
    if (unread) {
        parts << QStringLiteral("isRead eq false");
    }
    if (flagged) {
        parts << QStringLiteral("flag/flagStatus eq 'flagged'");
    }
    if (attachments) {
        parts << QStringLiteral("hasAttachments eq true");
    }
    if (important) {
        parts << QStringLiteral("importance eq 'high'");
    }
    if (since.isValid()) {
        parts << QStringLiteral("receivedDateTime ge ") + QDateTime(since, QTime(0, 0)).toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ"));
    }
    return parts.join(QStringLiteral(" and "));
}

SearchFolderDialog::SearchFolderDialog(const QString &name, const QString &filter, const QString &folderName, bool editing, QWidget *parent)
    : QDialog(parent)
    , m_name(new QLineEdit(name, this))
    , m_fromOn(new QCheckBox(i18nc("@option:check search folder rule", "From contains:"), this))
    , m_from(new QLineEdit(this))
    , m_subjectOn(new QCheckBox(i18nc("@option:check search folder rule", "Subject contains:"), this))
    , m_subject(new QLineEdit(this))
    , m_unread(new QCheckBox(i18nc("@option:check search folder rule", "Unread"), this))
    , m_flagged(new QCheckBox(i18nc("@option:check search folder rule", "Flagged"), this))
    , m_attachments(new QCheckBox(i18nc("@option:check search folder rule", "Has attachments"), this))
    , m_important(new QCheckBox(i18nc("@option:check search folder rule", "High importance"), this))
    , m_sinceOn(new QCheckBox(i18nc("@option:check search folder rule", "Received since:"), this))
    , m_since(new QDateEdit(QDate::currentDate().addMonths(-1), this))
    , m_everywhere(new QRadioButton(i18nc("@option:radio search folder", "The whole mailbox"), this))
    , m_here(new QRadioButton(this))
    , m_filter(new QLineEdit(filter, this))
{
    setWindowTitle(editing ? i18nc("@title:window", "Change Search Folder") : i18nc("@title:window", "New Search Folder"));
    resize(560, 420);
    m_since->setCalendarPopup(true);
    m_from->setPlaceholderText(i18nc("@info:placeholder", "Name or address"));

    auto *rules = new QGroupBox(i18nc("@title:group", "Show mail that matches all of these"), this);
    auto *grid = new QFormLayout(rules);
    grid->addRow(m_fromOn, m_from);
    grid->addRow(m_subjectOn, m_subject);
    grid->addRow(m_sinceOn, m_since);
    auto *flags = new QHBoxLayout;
    for (QCheckBox *box : {m_unread, m_flagged, m_attachments, m_important}) {
        flags->addWidget(box);
    }
    grid->addRow(flags);

    auto *where = new QGroupBox(i18nc("@title:group", "Look in"), this);
    auto *whereLayout = new QVBoxLayout(where);
    whereLayout->addWidget(m_everywhere);
    whereLayout->addWidget(m_here);
    m_everywhere->setChecked(true);
    m_here->setText(i18nc("@option:radio search folder, %1 a folder", "%1 and its subfolders", folderName));
    m_here->setVisible(!folderName.isEmpty() && !editing);
    // Exchange keeps the folders a search folder looks in; Graph cannot change them afterwards
    where->setVisible(!editing);

    auto *advanced = new QLabel(i18nc("@label:textbox", "Filter (Exchange syntax, filled in from the rules):"), this);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:textbox", "Name:"), m_name);
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(rules);
    layout->addWidget(where);
    layout->addWidget(advanced);
    layout->addWidget(m_filter);
    layout->addStretch();
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    parse(filter);
    for (QCheckBox *box : {m_fromOn, m_subjectOn, m_unread, m_flagged, m_attachments, m_important, m_sinceOn}) {
        connect(box, &QCheckBox::toggled, this, &SearchFolderDialog::rebuild);
    }
    connect(m_from, &QLineEdit::textEdited, this, [this] {
        m_fromOn->setChecked(true);
        rebuild();
    });
    connect(m_subject, &QLineEdit::textEdited, this, [this] {
        m_subjectOn->setChecked(true);
        rebuild();
    });
    connect(m_since, &QDateEdit::dateChanged, this, [this] {
        m_sinceOn->setChecked(true);
        rebuild();
    });
    auto updateOk = [this, buttons] {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!m_name->text().trimmed().isEmpty() && !m_filter->text().trimmed().isEmpty());
    };
    connect(m_name, &QLineEdit::textChanged, this, updateOk);
    connect(m_filter, &QLineEdit::textChanged, this, updateOk);
    updateOk();
}

void SearchFolderDialog::rebuild()
{
    if (m_building) {
        return;
    }
    m_filter->setText(buildFilter(m_fromOn->isChecked() ? m_from->text() : QString(),
                                  m_subjectOn->isChecked() ? m_subject->text() : QString(),
                                  m_unread->isChecked(),
                                  m_flagged->isChecked(),
                                  m_attachments->isChecked(),
                                  m_important->isChecked(),
                                  m_sinceOn->isChecked() ? m_since->date() : QDate()));
}

void SearchFolderDialog::parse(const QString &filter)
{
    m_building = true;
    static const QRegularExpression from(QStringLiteral("contains\\(from/emailAddress/address,'((?:[^']|'')*)'\\)"));
    static const QRegularExpression subject(QStringLiteral("contains\\(subject,'((?:[^']|'')*)'\\)"));
    static const QRegularExpression since(QStringLiteral("receivedDateTime ge (\\S+)"));
    if (const QRegularExpressionMatch m = from.match(filter); m.hasMatch()) {
        m_fromOn->setChecked(true);
        m_from->setText(m.captured(1).replace(QStringLiteral("''"), QStringLiteral("'")));
    }
    if (const QRegularExpressionMatch m = subject.match(filter); m.hasMatch()) {
        m_subjectOn->setChecked(true);
        m_subject->setText(m.captured(1).replace(QStringLiteral("''"), QStringLiteral("'")));
    }
    if (const QRegularExpressionMatch m = since.match(filter); m.hasMatch()) {
        m_sinceOn->setChecked(true);
        m_since->setDate(QDateTime::fromString(m.captured(1), Qt::ISODate).toLocalTime().date());
    }
    m_unread->setChecked(filter.contains(QLatin1String("isRead eq false")));
    m_flagged->setChecked(filter.contains(QLatin1String("flag/flagStatus eq 'flagged'")));
    m_attachments->setChecked(filter.contains(QLatin1String("hasAttachments eq true")));
    m_important->setChecked(filter.contains(QLatin1String("importance eq 'high'")));
    m_building = false;
}

QString SearchFolderDialog::name() const
{
    return m_name->text().trimmed();
}

QString SearchFolderDialog::filter() const
{
    return m_filter->text().trimmed();
}

bool SearchFolderDialog::currentFolderOnly() const
{
    return m_here->isVisible() && m_here->isChecked();
}

}
