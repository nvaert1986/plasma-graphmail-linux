// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mailboxsettingsdialog.h"
#include "accesstext.h"
#include "daemonclient.h"
#include "mailboxset.h"
#include "mailboxsettings.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimeEdit>
#include <QVBoxLayout>

#include <algorithm>

namespace GraphMail
{

namespace
{
// Formats Outlook on the web offers (.NET notation, as Exchange stores them)
const char *const dateFormats[] = {"d-M-yyyy", "dd-MM-yyyy", "dd-MMM-yy", "d MMM yyyy", "dd/MM/yyyy", "d/M/yyyy", "M/d/yyyy", "MM/dd/yyyy", "yyyy-MM-dd"};
const char *const timeFormats[] = {"HH:mm", "H:mm", "h:mm tt", "hh:mm tt"};

// A .NET format shown with today's date or the time 14:05 in the mailbox's language
QString example(const QString &format, const QString &locale, bool time)
{
    QString qt = format;
    qt.replace(QLatin1String("tt"), QLatin1String("AP"));
    const QLocale language(QString(locale).replace(QLatin1Char('-'), QLatin1Char('_')));
    return time ? language.toString(QTime(14, 5), qt) : language.toString(QDate::currentDate(), qt);
}
}

MailboxSettingsDialog::MailboxSettingsDialog(MailboxSet *mailboxes, DaemonClient *daemon, const QString &mailboxKey, QWidget *parent)
    : QDialog(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
    , m_mailbox(new QComboBox(this))
    , m_start(new QTimeEdit(this))
    , m_end(new QTimeEdit(this))
    , m_workZone(new QComboBox(this))
    , m_zone(new QComboBox(this))
    , m_language(new QComboBox(this))
    , m_dateFormat(new QComboBox(this))
    , m_timeFormat(new QComboBox(this))
    , m_status(new QLabel(this))
    , m_buttons(new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this))
{
    setWindowTitle(i18nc("@title:window", "Mailbox Settings"));
    // Only the own mailboxes: Graph does not give another mailbox's settings
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        if (box.own) {
            m_mailbox->addItem(box.address.isEmpty() ? box.label() : box.address, box.key());
        }
    }
    m_mailbox->setCurrentIndex(std::max(0, m_mailbox->findData(mailboxKey)));
    connect(m_mailbox, &QComboBox::currentIndexChanged, this, &MailboxSettingsDialog::load);

    // The days in the order of the user's week
    auto *days = new QHBoxLayout;
    const int first = int(QLocale().firstDayOfWeek());
    for (int i = 0; i < 7; ++i) {
        const int day = (first - 1 + i) % 7 + 1;
        m_days[day - 1] = new QCheckBox(QLocale().dayName(day, QLocale::ShortFormat), this);
        days->addWidget(m_days[day - 1]);
    }
    days->addStretch();
    auto *hours = new QHBoxLayout;
    hours->addWidget(m_start);
    hours->addWidget(new QLabel(i18nc("@label between two times", "to"), this));
    hours->addWidget(m_end);
    hours->addStretch();
    auto *work = new QGroupBox(i18nc("@title:group", "Working Hours"), this);
    auto *workForm = new QFormLayout(work);
    workForm->addRow(i18nc("@label", "Days:"), days);
    workForm->addRow(i18nc("@label", "Hours:"), hours);
    workForm->addRow(i18nc("@label:listbox", "Time zone:"), m_workZone);

    for (QComboBox *combo : {m_dateFormat, m_timeFormat}) {
        combo->setEditable(true);
        combo->setInsertPolicy(QComboBox::NoInsert);
    }
    auto *region = new QGroupBox(i18nc("@title:group", "Language and Time"), this);
    auto *regionForm = new QFormLayout(region);
    regionForm->addRow(i18nc("@label:listbox", "Language:"), m_language);
    regionForm->addRow(i18nc("@label:listbox", "Time zone:"), m_zone);
    regionForm->addRow(i18nc("@label:listbox", "Date format:"), m_dateFormat);
    regionForm->addRow(i18nc("@label:listbox", "Time format:"), m_timeFormat);
    auto *note = new QLabel(i18nc("@info", "Exchange uses these for meeting times, automatic replies and Outlook on the web. Graph Mail shows dates and times as set in Plasma's "
                                           "region settings, as Outlook follows those of Windows."),
                            this);
    note->setWordWrap(true);
    regionForm->addRow(note);
    // The examples follow the chosen language
    connect(m_language, &QComboBox::currentIndexChanged, this, &MailboxSettingsDialog::showExamples);

    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    m_status->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    if (m_mailbox->count() > 1) {
        auto *form = new QFormLayout;
        form->addRow(i18nc("@label:listbox", "Mailbox:"), m_mailbox);
        layout->addLayout(form);
    } else {
        m_mailbox->hide();
    }
    layout->addWidget(work);
    layout->addWidget(region);
    layout->addStretch();
    layout->addWidget(m_status);
    layout->addWidget(m_buttons);
    resize(620, sizeHint().height());
    load();
}

void MailboxSettingsDialog::load()
{
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    m_status->setText(i18nc("@info:status", "Reading the mailbox settings..."));
    const QString key = m_mailbox->currentData().toString();
    const QString account = key.section(QLatin1Char('/'), 0, 0);
    const QString mailbox = key.section(QLatin1Char('/'), 1);
    m_pending = 3;
    m_failed = false;
    auto done = [this, key] {
        if (--m_pending == 0 && !m_failed && key == m_mailbox->currentData().toString()) {
            fill();
        }
    };
    m_daemon->query(account, mailbox, QStringLiteral("mailboxSettings"), QStringLiteral("{}"), this, [this, key, done](const QString &json) {
        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
        if (!answer.value(QLatin1String("ok")).toBool()) {
            m_failed = true;
            m_status->setText(accessProblemText(answer.isEmpty() ? QStringLiteral("network") : answer.value(QLatin1String("problem")).toString(), key));
        }
        m_settings = answer.value(QLatin1String("body")).toObject();
        m_settings.remove(QStringLiteral("@odata.context"));
        QJsonObject logged = m_settings;
        logged.remove(QStringLiteral("automaticRepliesSetting"));
        qWarning().noquote() << "mailbox settings test: loaded" << QString::fromUtf8(QJsonDocument(logged).toJson(QJsonDocument::Compact));
        done();
    });
    // The lists Exchange accepts; empty when they cannot be read (the current values stay)
    auto list = [](const QString &json, const char *key, const char *text) {
        QList<QPair<QString, QString>> out;
        const QJsonArray value = QJsonDocument::fromJson(json.toUtf8()).object().value(QLatin1String("value")).toArray();
        for (const QJsonValue &v : value) {
            out.append({v.toObject().value(QLatin1String(key)).toString(), v.toObject().value(QLatin1String(text)).toString()});
        }
        return out;
    };
    m_daemon->query(account, mailbox, QStringLiteral("supportedTimeZones"), QStringLiteral("{}"), this, [this, list, done](const QString &json) {
        m_zones = list(json, "alias", "displayName");
        done();
    });
    m_daemon->query(account, mailbox, QStringLiteral("supportedLanguages"), QStringLiteral("{}"), this, [this, list, done](const QString &json) {
        m_languages = list(json, "locale", "displayName");
        std::sort(m_languages.begin(), m_languages.end(), [](const QPair<QString, QString> &a, const QPair<QString, QString> &b) {
            return QString::localeAwareCompare(a.second, b.second) < 0;
        });
        done();
    });
}

void MailboxSettingsDialog::selectData(QComboBox *combo, const QString &value, const QString &text)
{
    int i = combo->findData(value);
    if (i < 0 && !value.isEmpty()) {
        combo->addItem(text.isEmpty() ? value : text, value);
        i = combo->count() - 1;
    }
    combo->setCurrentIndex(i);
}

void MailboxSettingsDialog::fill()
{
    const WorkingHours work = WorkingHours::fromJson(m_settings.value(QLatin1String("workingHours")).toObject());
    for (int d = 1; d <= 7; ++d) {
        m_days[d - 1]->setChecked(work.days.contains(d));
    }
    m_start->setTime(work.start);
    m_end->setTime(work.end);
    for (QComboBox *combo : {m_workZone, m_zone}) {
        combo->clear();
        for (const auto &z : std::as_const(m_zones)) {
            combo->addItem(z.second, z.first);
        }
    }
    // A mailbox whose working hours were never set has none: its own time zone, as Outlook does
    const QString zone = m_settings.value(QLatin1String("timeZone")).toString();
    selectData(m_workZone, work.timeZone.isEmpty() ? zone : work.timeZone, {});
    selectData(m_zone, zone, {});

    const QJsonObject language = m_settings.value(QLatin1String("language")).toObject();
    {
        const QSignalBlocker blocker(m_language);
        m_language->clear();
        for (const auto &l : std::as_const(m_languages)) {
            m_language->addItem(l.second, l.first);
        }
        selectData(m_language, language.value(QLatin1String("locale")).toString(), language.value(QLatin1String("displayName")).toString());
    }
    const std::pair<QComboBox *, QString> formats[] = {{m_dateFormat, m_settings.value(QLatin1String("dateFormat")).toString()},
                                                       {m_timeFormat, m_settings.value(QLatin1String("timeFormat")).toString()}};
    for (const auto &[combo, current] : formats) {
        combo->clear();
        if (combo == m_dateFormat) {
            for (const char *f : dateFormats) {
                combo->addItem(QLatin1String(f), QLatin1String(f));
            }
        } else {
            for (const char *f : timeFormats) {
                combo->addItem(QLatin1String(f), QLatin1String(f));
            }
        }
        selectData(combo, current, {});
    }
    showExamples();
    m_status->clear();
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
    Q_EMIT loadedForTests();
}

void MailboxSettingsDialog::showExamples()
{
    const QString locale = m_language->currentData().toString();
    for (QComboBox *combo : {m_dateFormat, m_timeFormat}) {
        for (int i = 0; i < combo->count(); ++i) {
            combo->setItemText(i, combo->itemData(i).toString() + QStringLiteral("  (") + example(combo->itemData(i).toString(), locale, combo == m_timeFormat) + QLatin1Char(')'));
        }
    }
}

WorkingHours MailboxSettingsDialog::hours() const
{
    WorkingHours w;
    w.days.clear();
    for (int d = 1; d <= 7; ++d) {
        if (m_days[d - 1]->isChecked()) {
            w.days.append(d);
        }
    }
    w.start = m_start->time();
    w.end = m_end->time();
    w.timeZone = m_workZone->currentData().toString();
    if (w.timeZone.isEmpty()) {
        w.timeZone = m_zone->currentData().toString();
    }
    return w;
}

QJsonObject MailboxSettingsDialog::changes() const
{
    // Only what changed: Exchange checks every field it gets (a format against the language)
    QJsonObject out;
    const QJsonObject work = hours().toJson();
    if (WorkingHours::fromJson(work).toJson() != WorkingHours::fromJson(m_settings.value(QLatin1String("workingHours")).toObject()).toJson()) {
        out.insert(QStringLiteral("workingHours"), work);
    }
    if (m_zone->currentData().toString() != m_settings.value(QLatin1String("timeZone")).toString()) {
        out.insert(QStringLiteral("timeZone"), m_zone->currentData().toString());
    }
    const QString locale = m_language->currentData().toString();
    if (locale != m_settings.value(QLatin1String("language")).toObject().value(QLatin1String("locale")).toString()) {
        out.insert(QStringLiteral("language"), QJsonObject{{QStringLiteral("locale"), locale}});
    }
    // Typed formats have no item data: the text is the format
    auto format = [](QComboBox *combo) {
        const int i = combo->findText(combo->currentText());
        return i >= 0 && !combo->itemData(i).toString().isEmpty() ? combo->itemData(i).toString() : combo->currentText().section(QStringLiteral("  ("), 0, 0).trimmed();
    };
    if (format(m_dateFormat) != m_settings.value(QLatin1String("dateFormat")).toString()) {
        out.insert(QStringLiteral("dateFormat"), format(m_dateFormat));
    }
    if (format(m_timeFormat) != m_settings.value(QLatin1String("timeFormat")).toString()) {
        out.insert(QStringLiteral("timeFormat"), format(m_timeFormat));
    }
    return out;
}

void MailboxSettingsDialog::accept()
{
    const WorkingHours w = hours();
    if (w.days.isEmpty()) {
        m_status->setText(i18nc("@info", "Choose at least one working day."));
        return;
    }
    if (w.end <= w.start) {
        m_status->setText(i18nc("@info", "The working day must end after it starts."));
        return;
    }
    save(true);
}

void MailboxSettingsDialog::save(bool close)
{
    const QJsonObject change = changes();
    if (change.isEmpty()) {
        qWarning() << "mailbox settings test: nothing changed";
        if (close) {
            QDialog::accept();
        }
        return;
    }
    const QString key = m_mailbox->currentData().toString();
    m_status->setText(i18nc("@info:status", "Saving..."));
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("mailboxSettingsUpdate"),
                    QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("settings"), change}}).toJson(QJsonDocument::Compact)), this,
                    [this, key, change, close](const QString &json) {
                        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                        const bool ok = answer.value(QLatin1String("ok")).toBool();
                        qWarning() << "mailbox settings test:" << ok << answer.value(QLatin1String("detail")).toString();
                        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
                        if (!ok) {
                            const QString problem = answer.value(QLatin1String("problem")).toString();
                            const QString detail = answer.value(QLatin1String("detail")).toString();
                            // Exchange's reason (a format the language does not have) says more
                            m_status->setText(problem == QLatin1String("error") && !detail.isEmpty()
                                                  ? i18nc("@info %1 Exchange's reason", "Microsoft 365 did not accept the settings: %1", detail)
                                                  : accessProblemText(answer.isEmpty() ? QStringLiteral("network") : problem, key));
                            return;
                        }
                        for (auto it = change.begin(); it != change.end(); ++it) {
                            m_settings.insert(it.key(), it.value());
                        }
                        m_status->clear();
                        Q_EMIT saved(key, m_settings);
                        if (close) {
                            QDialog::accept();
                        }
                    });
}

void MailboxSettingsDialog::saveHoursForTests(const QString &days, const QString &start, const QString &end)
{
    const QStringList list = days.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (int d = 1; d <= 7; ++d) {
        m_days[d - 1]->setChecked(list.contains(QString::number(d)));
    }
    m_start->setTime(QTime::fromString(start, QStringLiteral("HH:mm")));
    m_end->setTime(QTime::fromString(end, QStringLiteral("HH:mm")));
    save(false);
}

}
