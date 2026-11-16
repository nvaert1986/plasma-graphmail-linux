// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "recurrence.h"
#include "calendaroptions.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLocale>
#include <QRadioButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimeZone>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
const QStringList &graphDays()
{
    static const QStringList days{QStringLiteral("monday"), QStringLiteral("tuesday"), QStringLiteral("wednesday"), QStringLiteral("thursday"),
                                  QStringLiteral("friday"), QStringLiteral("saturday"), QStringLiteral("sunday")};
    return days;
}

const QStringList &indexes()
{
    static const QStringList names{QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("third"), QStringLiteral("fourth"), QStringLiteral("last")};
    return names;
}

QString indexText(const QString &index)
{
    if (index == QLatin1String("first")) {
        return i18nc("@item the first weekday of the month", "first");
    }
    if (index == QLatin1String("second")) {
        return i18nc("@item the second weekday of the month", "second");
    }
    if (index == QLatin1String("third")) {
        return i18nc("@item the third weekday of the month", "third");
    }
    if (index == QLatin1String("fourth")) {
        return i18nc("@item the fourth weekday of the month", "fourth");
    }
    return i18nc("@item the last weekday of the month", "last");
}

QString dayList(const QJsonArray &days)
{
    QStringList names;
    for (const QJsonValue &d : days) {
        names.append(QLocale().standaloneDayName(dayOfWeekFromGraph(d.toString()), QLocale::LongFormat));
    }
    return QLocale().createSeparatedList(names);
}

// Windows' name of the system time zone (Graph's recurrenceTimeZone)
QString windowsZone()
{
    const QByteArray windows = QTimeZone::ianaIdToWindowsId(QTimeZone::systemTimeZoneId());
    return windows.isEmpty() ? QStringLiteral("UTC") : QString::fromLatin1(windows);
}
}

QString graphDayName(int dayOfWeek)
{
    return graphDays().value(dayOfWeek - 1);
}

int dayOfWeekFromGraph(const QString &name)
{
    return int(graphDays().indexOf(name.toLower())) + 1;
}

QString describeRecurrence(const QJsonObject &recurrence)
{
    const QJsonObject pattern = recurrence.value(QLatin1String("pattern")).toObject();
    const QJsonObject range = recurrence.value(QLatin1String("range")).toObject();
    const QString type = pattern.value(QLatin1String("type")).toString();
    const int interval = std::max(1, pattern.value(QLatin1String("interval")).toInt(1));
    const QJsonArray days = pattern.value(QLatin1String("daysOfWeek")).toArray();
    QString text;
    if (type == QLatin1String("daily")) {
        text = i18ncp("@info recurrence", "Every day", "Every %1 days", interval);
    } else if (type == QLatin1String("weekly")) {
        text = i18ncp("@info recurrence, %2 days", "Every week on %2", "Every %1 weeks on %2", interval, dayList(days));
    } else if (type == QLatin1String("absoluteMonthly")) {
        text = i18ncp("@info recurrence, %2 day of the month", "Every month on day %2", "Every %1 months on day %2", interval,
                      pattern.value(QLatin1String("dayOfMonth")).toInt());
    } else if (type == QLatin1String("relativeMonthly")) {
        text = i18ncp("@info recurrence: every month on the second Tuesday", "Every month on the %2 %3", "Every %1 months on the %2 %3", interval,
                      indexText(pattern.value(QLatin1String("index")).toString()), dayList(days));
    } else if (type == QLatin1String("absoluteYearly")) {
        text = i18nc("@info recurrence: every year on 5 March", "Every year on %1",
                     QLocale().toString(QDate(2000, std::max(1, pattern.value(QLatin1String("month")).toInt()), std::max(1, pattern.value(QLatin1String("dayOfMonth")).toInt())),
                                        QStringLiteral("d MMMM")));
    } else if (type == QLatin1String("relativeYearly")) {
        text = i18nc("@info recurrence: every year on the last Friday of March", "Every year on the %1 %2 of %3",
                     indexText(pattern.value(QLatin1String("index")).toString()),
                     dayList(days),
                     QLocale().standaloneMonthName(std::max(1, pattern.value(QLatin1String("month")).toInt())));
    } else {
        return i18nc("@info recurrence of an unknown kind", "Repeats");
    }
    const QString rangeType = range.value(QLatin1String("type")).toString();
    if (rangeType == QLatin1String("endDate")) {
        text = i18nc("@info recurrence, end", "%1, until %2", text,
                     QLocale().toString(QDate::fromString(range.value(QLatin1String("endDate")).toString(), Qt::ISODate), QStringLiteral("d MMMM yyyy")));
    } else if (rangeType == QLatin1String("numbered")) {
        text = i18ncp("@info recurrence, %2 the rule", "%2, once", "%2, %1 times", range.value(QLatin1String("numberOfOccurrences")).toInt(), text);
    }
    return text;
}

QJsonObject presetRecurrence(RepeatPreset preset, QDate start)
{
    QJsonObject pattern{{QStringLiteral("interval"), 1}, {QStringLiteral("firstDayOfWeek"), graphDayName(CalendarOptions::firstDayOfWeek())}};
    switch (preset) {
    case RepeatPreset::Daily:
        pattern.insert(QStringLiteral("type"), QStringLiteral("daily"));
        break;
    case RepeatPreset::Weekdays:
        pattern.insert(QStringLiteral("type"), QStringLiteral("weekly"));
        pattern.insert(QStringLiteral("daysOfWeek"),
                       QJsonArray{QStringLiteral("monday"), QStringLiteral("tuesday"), QStringLiteral("wednesday"), QStringLiteral("thursday"), QStringLiteral("friday")});
        break;
    case RepeatPreset::Weekly:
        pattern.insert(QStringLiteral("type"), QStringLiteral("weekly"));
        pattern.insert(QStringLiteral("daysOfWeek"), QJsonArray{graphDayName(start.dayOfWeek())});
        break;
    case RepeatPreset::Monthly:
        pattern.insert(QStringLiteral("type"), QStringLiteral("absoluteMonthly"));
        pattern.insert(QStringLiteral("dayOfMonth"), start.day());
        break;
    case RepeatPreset::Yearly:
        pattern.insert(QStringLiteral("type"), QStringLiteral("absoluteYearly"));
        pattern.insert(QStringLiteral("dayOfMonth"), start.day());
        pattern.insert(QStringLiteral("month"), start.month());
        break;
    }
    return {{QStringLiteral("pattern"), pattern},
            {QStringLiteral("range"),
             QJsonObject{{QStringLiteral("type"), QStringLiteral("noEnd")},
                         {QStringLiteral("startDate"), start.toString(Qt::ISODate)},
                         {QStringLiteral("recurrenceTimeZone"), windowsZone()}}}};
}

RecurrenceDialog::RecurrenceDialog(const QJsonObject &recurrence, QDate start, QWidget *parent)
    : QDialog(parent)
    , m_start(start)
    , m_type(new QComboBox(this))
    , m_interval(new QSpinBox(this))
    , m_unit(new QLabel(this))
    , m_details(new QStackedWidget(this))
    , m_monthlyOnDay(new QRadioButton(this))
    , m_dayOfMonth(new QSpinBox(this))
    , m_monthlyRelative(new QRadioButton(this))
    , m_index(new QComboBox(this))
    , m_weekday(new QComboBox(this))
    , m_month(new QComboBox(this))
    , m_noEnd(new QRadioButton(i18nc("@option:radio recurrence end", "No end date"), this))
    , m_endDate(new QRadioButton(i18nc("@option:radio recurrence end", "End by:"), this))
    , m_until(new QDateEdit(this))
    , m_numbered(new QRadioButton(i18nc("@option:radio recurrence end", "End after:"), this))
    , m_count(new QSpinBox(this))
    , m_summary(new QLabel(this))
{
    setWindowTitle(i18nc("@title:window", "Repeat"));
    m_type->addItems({i18nc("@item:inlistbox recurrence", "Daily"), i18nc("@item:inlistbox recurrence", "Weekly"), i18nc("@item:inlistbox recurrence", "Monthly"),
                      i18nc("@item:inlistbox recurrence", "Yearly")});
    m_interval->setRange(1, 99);

    // Weekly: the days
    auto *weekly = new QWidget(this);
    auto *weekLayout = new QHBoxLayout(weekly);
    weekLayout->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < 7; ++i) {
        const int day = (CalendarOptions::firstDayOfWeek() - 1 + i) % 7 + 1;
        auto *box = new QCheckBox(QLocale().standaloneDayName(day, QLocale::ShortFormat), weekly);
        box->setProperty("day", day);
        weekLayout->addWidget(box);
        m_days.append(box);
        connect(box, &QCheckBox::toggled, this, &RecurrenceDialog::updateSummary);
    }
    weekLayout->addStretch();

    // Monthly and yearly: a day number, or "the second Tuesday"
    for (const QString &i : indexes()) {
        m_index->addItem(indexText(i), i);
    }
    for (int d = 1; d <= 7; ++d) {
        m_weekday->addItem(QLocale().standaloneDayName(d, QLocale::LongFormat), graphDayName(d));
    }
    for (int m = 1; m <= 12; ++m) {
        m_month->addItem(QLocale().standaloneMonthName(m), m);
    }
    m_dayOfMonth->setRange(1, 31);
    auto *monthly = new QWidget(this);
    auto *monthLayout = new QFormLayout(monthly);
    monthLayout->setContentsMargins(0, 0, 0, 0);
    auto *onDay = new QHBoxLayout;
    m_monthlyOnDay->setText(i18nc("@option:radio monthly on day N", "On day"));
    onDay->addWidget(m_monthlyOnDay);
    onDay->addWidget(m_dayOfMonth);
    onDay->addStretch();
    auto *relative = new QHBoxLayout;
    m_monthlyRelative->setText(i18nc("@option:radio monthly on the second Tuesday", "On the"));
    relative->addWidget(m_monthlyRelative);
    relative->addWidget(m_index);
    relative->addWidget(m_weekday);
    relative->addStretch();
    auto *inMonth = new QHBoxLayout;
    inMonth->addWidget(new QLabel(i18nc("@label yearly: in March", "In:"), monthly));
    inMonth->addWidget(m_month);
    inMonth->addStretch();
    monthLayout->addRow(onDay);
    monthLayout->addRow(relative);
    monthLayout->addRow(inMonth);

    m_details->addWidget(new QWidget(this)); // daily: nothing more
    m_details->addWidget(weekly);
    m_details->addWidget(monthly);

    m_until->setCalendarPopup(true);
    m_count->setRange(1, 999);
    m_summary->setWordWrap(true);
    QFont italic = m_summary->font();
    italic.setItalic(true);
    m_summary->setFont(italic);

    auto *pattern = new QGroupBox(i18nc("@title:group", "Pattern"), this);
    auto *patternLayout = new QVBoxLayout(pattern);
    auto *every = new QHBoxLayout;
    every->addWidget(m_type);
    every->addWidget(new QLabel(i18nc("@label recurrence: every N days", "every"), pattern));
    every->addWidget(m_interval);
    every->addWidget(m_unit);
    every->addStretch();
    patternLayout->addLayout(every);
    patternLayout->addWidget(m_details);

    auto *range = new QGroupBox(i18nc("@title:group", "Range"), this);
    auto *rangeLayout = new QFormLayout(range);
    rangeLayout->addRow(m_noEnd);
    rangeLayout->addRow(m_endDate, m_until);
    auto *times = new QHBoxLayout;
    times->addWidget(m_count);
    times->addWidget(new QLabel(i18nc("@label recurrence: end after N occurrences", "occurrences"), range));
    times->addStretch();
    rangeLayout->addRow(m_numbered, times);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(pattern);
    layout->addWidget(range);
    layout->addWidget(m_summary);
    layout->addWidget(buttons);

    // What there is (or a weekly rule on the start day)
    const QJsonObject initial = recurrence.isEmpty() ? presetRecurrence(RepeatPreset::Weekly, start) : recurrence;
    const QJsonObject p = initial.value(QLatin1String("pattern")).toObject();
    const QJsonObject r = initial.value(QLatin1String("range")).toObject();
    const QString type = p.value(QLatin1String("type")).toString();
    m_type->setCurrentIndex(type == QLatin1String("daily") ? 0 : type == QLatin1String("weekly") ? 1 : type.endsWith(QLatin1String("Monthly")) ? 2 : 3);
    m_interval->setValue(std::max(1, p.value(QLatin1String("interval")).toInt(1)));
    const QJsonArray days = p.value(QLatin1String("daysOfWeek")).toArray();
    for (QCheckBox *box : std::as_const(m_days)) {
        box->setChecked(days.contains(graphDayName(box->property("day").toInt())));
    }
    const bool relativeRule = type.startsWith(QLatin1String("relative"));
    m_monthlyRelative->setChecked(relativeRule);
    m_monthlyOnDay->setChecked(!relativeRule);
    m_dayOfMonth->setValue(p.value(QLatin1String("dayOfMonth")).toInt(start.day()) > 0 ? p.value(QLatin1String("dayOfMonth")).toInt() : start.day());
    m_index->setCurrentIndex(std::max(0, m_index->findData(p.value(QLatin1String("index")).toString(QStringLiteral("first")))));
    m_weekday->setCurrentIndex(std::max(0, m_weekday->findData(days.isEmpty() ? graphDayName(start.dayOfWeek()) : days.first().toString())));
    m_month->setCurrentIndex(std::max(0, (p.value(QLatin1String("month")).toInt() > 0 ? p.value(QLatin1String("month")).toInt() : start.month()) - 1));
    const QString rangeType = r.value(QLatin1String("type")).toString();
    m_noEnd->setChecked(rangeType != QLatin1String("endDate") && rangeType != QLatin1String("numbered"));
    m_endDate->setChecked(rangeType == QLatin1String("endDate"));
    m_numbered->setChecked(rangeType == QLatin1String("numbered"));
    const QDate until = QDate::fromString(r.value(QLatin1String("endDate")).toString(), Qt::ISODate);
    m_until->setDate(until.isValid() && until.year() > 1 ? until : start.addMonths(3));
    m_count->setValue(std::max(1, r.value(QLatin1String("numberOfOccurrences")).toInt(10)));

    auto update = [this] {
        const int type = m_type->currentIndex();
        m_details->setCurrentIndex(type == 0 ? 0 : type == 1 ? 1 : 2);
        const bool yearly = type == 3;
        m_month->setEnabled(yearly);
        m_unit->setText(type == 0 ? i18nc("@label recurrence unit", "day(s)") : type == 1 ? i18nc("@label recurrence unit", "week(s)")
                            : type == 2                                                  ? i18nc("@label recurrence unit", "month(s)")
                                                                                         : i18nc("@label recurrence unit", "year(s)"));
        m_interval->setEnabled(!yearly);
        updateSummary();
    };
    connect(m_type, &QComboBox::currentIndexChanged, this, update);
    for (QObject *o : std::initializer_list<QObject *>{m_interval, m_dayOfMonth, m_count}) {
        connect(static_cast<QSpinBox *>(o), &QSpinBox::valueChanged, this, &RecurrenceDialog::updateSummary);
    }
    for (QRadioButton *b : {m_monthlyOnDay, m_monthlyRelative, m_noEnd, m_endDate, m_numbered}) {
        connect(b, &QRadioButton::toggled, this, &RecurrenceDialog::updateSummary);
    }
    for (QComboBox *c : {m_index, m_weekday, m_month}) {
        connect(c, &QComboBox::currentIndexChanged, this, &RecurrenceDialog::updateSummary);
    }
    connect(m_until, &QDateEdit::dateChanged, this, &RecurrenceDialog::updateSummary);
    update();
}

QJsonObject RecurrenceDialog::recurrence() const
{
    QJsonObject pattern{{QStringLiteral("interval"), m_interval->isEnabled() ? m_interval->value() : 1},
                        {QStringLiteral("firstDayOfWeek"), graphDayName(CalendarOptions::firstDayOfWeek())}};
    switch (m_type->currentIndex()) {
    case 0:
        pattern.insert(QStringLiteral("type"), QStringLiteral("daily"));
        break;
    case 1: {
        QJsonArray days;
        for (QCheckBox *box : m_days) {
            if (box->isChecked()) {
                days.append(graphDayName(box->property("day").toInt()));
            }
        }
        if (days.isEmpty()) {
            days.append(graphDayName(m_start.dayOfWeek()));
        }
        pattern.insert(QStringLiteral("type"), QStringLiteral("weekly"));
        pattern.insert(QStringLiteral("daysOfWeek"), days);
        break;
    }
    default: {
        const bool yearly = m_type->currentIndex() == 3;
        if (m_monthlyRelative->isChecked()) {
            pattern.insert(QStringLiteral("type"), yearly ? QStringLiteral("relativeYearly") : QStringLiteral("relativeMonthly"));
            pattern.insert(QStringLiteral("index"), m_index->currentData().toString());
            pattern.insert(QStringLiteral("daysOfWeek"), QJsonArray{m_weekday->currentData().toString()});
        } else {
            pattern.insert(QStringLiteral("type"), yearly ? QStringLiteral("absoluteYearly") : QStringLiteral("absoluteMonthly"));
            pattern.insert(QStringLiteral("dayOfMonth"), m_dayOfMonth->value());
        }
        if (yearly) {
            pattern.insert(QStringLiteral("month"), m_month->currentData().toInt());
        }
        break;
    }
    }
    QJsonObject range{{QStringLiteral("startDate"), m_start.toString(Qt::ISODate)}, {QStringLiteral("recurrenceTimeZone"), windowsZone()}};
    if (m_endDate->isChecked()) {
        range.insert(QStringLiteral("type"), QStringLiteral("endDate"));
        range.insert(QStringLiteral("endDate"), m_until->date().toString(Qt::ISODate));
    } else if (m_numbered->isChecked()) {
        range.insert(QStringLiteral("type"), QStringLiteral("numbered"));
        range.insert(QStringLiteral("numberOfOccurrences"), m_count->value());
    } else {
        range.insert(QStringLiteral("type"), QStringLiteral("noEnd"));
    }
    return {{QStringLiteral("pattern"), pattern}, {QStringLiteral("range"), range}};
}

void RecurrenceDialog::updateSummary()
{
    m_until->setEnabled(m_endDate->isChecked());
    m_count->setEnabled(m_numbered->isChecked());
    m_dayOfMonth->setEnabled(m_monthlyOnDay->isChecked());
    m_index->setEnabled(m_monthlyRelative->isChecked());
    m_weekday->setEnabled(m_monthlyRelative->isChecked());
    m_summary->setText(describeRecurrence(recurrence()));
}

}
