// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "calendarwindow.h"
#include "printing.h"
#include "calendarsharingdialog.h"
#include "categorylist.h"
#include "accesstext.h"
#include "calendarviews.h"
#include "daemonclient.h"
#include "eventdialog.h"
#include "mailboxset.h"
#include "recipientedit.h"

#include <KActionCollection>
#include <KConfigGroup>
#include <KDatePicker>
#include <KLocalizedString>
#include <KMessageBox>
#include <KSharedConfig>
#include <KStandardAction>

#include <QActionGroup>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QApplication>
#include <QPainter>
#include <QPrinter>
#include <QTimeZone>
#include <QLineEdit>
#include <QFormLayout>
#include <QComboBox>
#include <QCheckBox>
#include <KToggleAction>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QRadioButton>
#include <QScopedValueRollback>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
KConfigGroup windowGroup()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("CalendarWindow"));
}

QDate weekStart(QDate date)
{
    const int first = QLocale().firstDayOfWeek();
    return date.addDays(-((date.dayOfWeek() - first + 7) % 7));
}
}

CalendarWindow::CalendarWindow(MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent)
    : KXmlGuiWindow(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
    , m_data(new CalendarData(mailboxes, this))
    , m_date(QDate::currentDate())
    , m_picker(new KDatePicker(this))
    , m_calendarList(new QTreeWidget(this))
    , m_title(new QLabel(this))
    , m_stack(new QStackedWidget(this))
    , m_grid(new TimeGridView(this))
    , m_month(new MonthView(this))
    , m_agenda(new AgendaView(this))
{
    setWindowTitle(i18nc("@title:window", "Calendar"));

    m_calendarList->setHeaderHidden(true);
    m_calendarList->setRootIsDecorated(true);
    m_calendarList->setFrameShape(QFrame::NoFrame);
    connect(m_calendarList, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item) {
        if (m_filling || !item->parent()) {
            return;
        }
        m_data->setShown(item->data(0, Qt::UserRole).toString(), item->checkState(0) == Qt::Checked);
        refresh();
    });
    m_calendarList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_calendarList, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QTreeWidgetItem *item = m_calendarList->itemAt(pos);
        const QString key = item ? item->data(0, Qt::UserRole).toString() : QString();
        // As Outlook's "Add Calendar" on the list: on any place of it
        QMenu menu(this);
        menu.addAction(actionCollection()->action(QStringLiteral("open_colleague_calendar")));
        const QList<CalendarInfo> calendars = m_data->calendars();
        for (const CalendarInfo &c : calendars) {
            const std::optional<MailboxInfo> box = m_mailboxes->mailbox(c.mailboxKey);
            // Free/busy people, calendar-only mailboxes and colleagues' calendars shared with the
            // user can go again; the rest belongs to Settings > Mailboxes
            const bool removable = CalendarData::isFreeBusy(c.mailboxKey) || (box && !box->own && !box->mail) || isSharedWithMe(c);
            if (c.key() == key && box && box->own && c.canEdit) {
                menu.addSeparator();
                menu.addAction(QIcon::fromTheme(QStringLiteral("document-share")), i18nc("@action:inmenu", "Sharing..."), this, [this, c] {
                    openSharing(c.key());
                });
                break;
            }
            if (c.key() == key && removable) {
                menu.addSeparator();
                menu.addAction(QIcon::fromTheme(QStringLiteral("list-remove")),
                               isSharedWithMe(c) ? i18nc("@action:inmenu", "Remove from My Calendars") : i18nc("@action:inmenu", "Remove from Graph Mail"),
                               this,
                               [this, c] {
                    removeCalendarEntry(c, true);
                });
                break;
            }
        }
        menu.exec(m_calendarList->viewport()->mapToGlobal(pos));
    });
    connect(m_picker, &KDatePicker::dateChanged, this, [this](QDate date) {
        if (date.isValid() && date != m_date) {
            goTo(date);
        }
    });

    auto *left = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(m_picker);
    leftLayout->addWidget(m_calendarList, 1);

    m_stack->addWidget(m_grid);
    m_stack->addWidget(m_month);
    m_stack->addWidget(m_agenda);
    QFont titleFont = m_title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.3);
    m_title->setFont(titleFont);
    m_title->setContentsMargins(8, 4, 8, 4);
    auto *right = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);
    rightLayout->addWidget(m_title);
    rightLayout->addWidget(m_stack, 1);

    auto *splitter = new QSplitter(this);
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, 1040});
    setCentralWidget(splitter);

    for (CalendarView *view : std::initializer_list<CalendarView *>{m_grid, m_month, m_agenda}) {
        connect(view, &CalendarView::eventActivated, this, &CalendarWindow::activate);
        connect(view, &CalendarView::eventMenuRequested, this, &CalendarWindow::showMenu);
        connect(view, &CalendarView::newEventRequested, this, &CalendarWindow::newEvent);
        connect(view, &CalendarView::dayActivated, this, [this](QDate day) {
            m_date = day;
            setMode(Mode::Day);
        });
    }

    // A calendar sync redraws (a little later: one change often comes with more)
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(300);
    connect(&m_reloadTimer, &QTimer::timeout, this, [this] {
        m_data->reload();
        fillCalendarList();
        refresh();
    });
    // A mailbox added or removed (Settings > Mailboxes)
    connect(m_mailboxes, &MailboxSet::changed, &m_reloadTimer, qOverload<>(&QTimer::start));
    connect(m_daemon, &DaemonClient::collectionChanged, this, [this](const QString &, const QString &, const QString &collection, int) {
        if (collection == QLatin1String("calendar")) {
            m_reloadTimer.start();
        }
    });
    connect(m_daemon, &DaemonClient::operationFailed, this, [this](const QString &, const QString &, const QString &, const QString &error) {
        if (isVisible()) {
            statusBar()->showMessage(i18nc("@info:status", "Exchange did not accept a change: %1", error), 10000);
        }
        m_reloadTimer.start();
    });

    setupActions();
    setupGUI(QSize(1300, 820), ToolBar | Keys | Save | Create | StatusBar, QStringLiteral("graphmailcalendarui.rc"));
    setAutoSaveSettings(QStringLiteral("CalendarWindow"));

    fillCalendarList();
    setMode(Mode(std::clamp(windowGroup().readEntry("Mode", int(Mode::WorkWeek)), 0, 4)));
    m_grid->scrollToHour(7);
    m_month->setWeekNumbers(calendarGroup().readEntry("WeekNumbers", false));
    applySecondTimeZone();
    loadWorkingHours();
}

void CalendarWindow::loadWorkingHours()
{
    const QString account = m_mailboxes->primaryAccount();
    if (account.isEmpty()) {
        return;
    }
    m_daemon->query(account, QStringLiteral("me"), QStringLiteral("mailboxSettings"), QStringLiteral("{}"), this, [this](const QString &json) {
        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
        if (answer.value(QLatin1String("ok")).toBool()) {
            setWorkingHours(WorkingHours::fromJson(answer.value(QLatin1String("body")).toObject().value(QLatin1String("workingHours")).toObject()));
        }
    });
}

void CalendarWindow::setWorkingHours(const WorkingHours &hours)
{
    m_workingHours = hours;
    m_grid->setWorkingHours(hours);
    // Opened at the start of the working day, as Outlook does (once: later the user's scrolling stays)
    if (!m_scrolledToWork) {
        m_scrolledToWork = true;
        const double start = hours.localHours(m_date.addDays(1 - m_date.dayOfWeek()).addDays(hours.days.value(0, 1) - 1)).first;
        m_grid->scrollToHour(std::max(0, int(start) - 1));
    }
    if (m_mode == Mode::WorkWeek) {
        setMode(m_mode);
    }
}

CalendarWindow::~CalendarWindow() = default;

void CalendarWindow::setupActions()
{
    KActionCollection *actions = actionCollection();

    QAction *newEventAction = actions->addAction(QStringLiteral("new_event"));
    newEventAction->setText(i18nc("@action", "New Event"));
    newEventAction->setIconText(i18nc("@action short toolbar text", "New"));
    newEventAction->setIcon(QIcon::fromTheme(QStringLiteral("appointment-new")));
    actions->setDefaultShortcut(newEventAction, QKeySequence(Qt::CTRL | Qt::Key_N));
    connect(newEventAction, &QAction::triggered, this, [this] {
        // The next full hour of the day shown
        QDateTime start(m_date, QTime(QTime::currentTime().hour() + 1 < 24 ? QTime::currentTime().hour() + 1 : 9, 0));
        newEvent(start, start.addSecs(1800), false);
    });

    QAction *today = actions->addAction(QStringLiteral("go_today"));
    today->setText(i18nc("@action", "Today"));
    today->setIcon(QIcon::fromTheme(QStringLiteral("go-jump-today")));
    actions->setDefaultShortcut(today, QKeySequence(Qt::CTRL | Qt::Key_T));
    connect(today, &QAction::triggered, this, [this] {
        goTo(QDate::currentDate());
    });
    QAction *previous = actions->addAction(QStringLiteral("go_previous"));
    previous->setText(i18nc("@action", "Previous"));
    previous->setIcon(QIcon::fromTheme(QStringLiteral("go-previous")));
    actions->setDefaultShortcut(previous, QKeySequence(Qt::ALT | Qt::Key_Left));
    connect(previous, &QAction::triggered, this, [this] {
        step(-1);
    });
    QAction *next = actions->addAction(QStringLiteral("go_next"));
    next->setText(i18nc("@action", "Next"));
    next->setIcon(QIcon::fromTheme(QStringLiteral("go-next")));
    actions->setDefaultShortcut(next, QKeySequence(Qt::ALT | Qt::Key_Right));
    connect(next, &QAction::triggered, this, [this] {
        step(1);
    });

    // The views, with Outlook's Ctrl+Alt+1..4
    const struct {
        const char *name;
        Mode mode;
        const char *icon;
        KLocalizedString text;
        QKeySequence shortcut;
    } modes[] = {{"view_day", Mode::Day, "view-calendar-day", ki18nc("@action", "Day"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_1)},
                 {"view_workweek", Mode::WorkWeek, "view-calendar-workweek", ki18nc("@action", "Work Week"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_2)},
                 {"view_week", Mode::Week, "view-calendar-week", ki18nc("@action", "Week"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_3)},
                 {"view_month", Mode::Month, "view-calendar-month", ki18nc("@action", "Month"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_4)},
                 {"view_agenda", Mode::Agenda, "view-calendar-agenda", ki18nc("@action", "Agenda"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_5)}};
    auto *group = new QActionGroup(this);
    for (const auto &m : modes) {
        QAction *action = actions->addAction(QLatin1String(m.name));
        action->setText(m.text.toString());
        action->setIcon(QIcon::fromTheme(QLatin1String(m.icon)));
        action->setCheckable(true);
        action->setData(int(m.mode));
        actions->setDefaultShortcut(action, m.shortcut);
        group->addAction(action);
        const Mode mode = m.mode;
        connect(action, &QAction::triggered, this, [this, mode] {
            setMode(mode);
        });
    }

    QAction *mail = actions->addAction(QStringLiteral("go_mail"));
    mail->setText(i18nc("@action", "Mail"));
    mail->setIcon(QIcon::fromTheme(QStringLiteral("mail-message")));
    actions->setDefaultShortcut(mail, QKeySequence(Qt::CTRL | Qt::Key_1));
    connect(mail, &QAction::triggered, this, &CalendarWindow::mailRequested);

    const struct {
        const char *name;
        int module;
        const char *icon;
        KLocalizedString text;
        Qt::Key key;
    } others[] = {{"go_contacts", 3, "view-pim-contacts", ki18nc("@action", "Contacts"), Qt::Key_3}, {"go_tasks", 4, "view-task", ki18nc("@action", "Tasks"), Qt::Key_4}};
    for (const auto &m : others) {
        QAction *a = actions->addAction(QLatin1String(m.name));
        a->setText(m.text.toString());
        a->setIcon(QIcon::fromTheme(QLatin1String(m.icon)));
        actions->setDefaultShortcut(a, QKeySequence(Qt::CTRL | m.key));
        const int module = m.module;
        connect(a, &QAction::triggered, this, [this, module] {
            Q_EMIT navigate(module);
        });
    }

    QAction *sync = actions->addAction(QStringLiteral("sync"));
    sync->setText(i18nc("@action", "Sync"));
    sync->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    actions->setDefaultShortcut(sync, Qt::Key_F9);
    connect(sync, &QAction::triggered, this, [this] {
        m_daemon->syncNow();
    });

    QAction *remove = actions->addAction(QStringLiteral("delete_event"));
    remove->setText(i18nc("@action", "Delete Event"));
    remove->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete")));
    actions->setDefaultShortcut(remove, Qt::Key_Delete);
    connect(remove, &QAction::triggered, this, [this] {
        if (const std::optional<DisplayEvent> e = currentView()->selected(); e && !CalendarData::isFreeBusy(e->mailboxKey)) {
            deleteEvent(*e);
        }
    });

    // Week numbers (month view and the title), as Outlook's calendar option
    auto *weeks = new KToggleAction(i18nc("@action", "Show Week Numbers"), this);
    actions->addAction(QStringLiteral("week_numbers"), weeks);
    weeks->setChecked(calendarGroup().readEntry("WeekNumbers", false));
    connect(weeks, &KToggleAction::toggled, this, [this](bool on) {
        KConfigGroup group = calendarGroup();
        group.writeEntry("WeekNumbers", on);
        group.sync();
        m_month->setWeekNumbers(on);
        refresh();
    });
    auto *deleteRequests = new KToggleAction(i18nc("@action", "Delete Meeting Requests After Responding"), this);
    actions->addAction(QStringLiteral("delete_request_after_responding"), deleteRequests);
    deleteRequests->setChecked(EventDialog::deleteRequestAfterResponding());
    connect(deleteRequests, &KToggleAction::toggled, this, [](bool on) {
        KConfigGroup group = calendarGroup();
        group.writeEntry("DeleteRequestAfterResponding", on);
        group.sync();
    });
    QAction *secondZone = actions->addAction(QStringLiteral("second_time_zone"));
    secondZone->setText(i18nc("@action", "Second Time Zone..."));
    secondZone->setIcon(QIcon::fromTheme(QStringLiteral("preferences-system-time")));
    connect(secondZone, &QAction::triggered, this, &CalendarWindow::askSecondTimeZone);

    QAction *colleague = actions->addAction(QStringLiteral("open_colleague_calendar"));
    colleague->setText(i18nc("@action", "Add a Colleague's Calendar..."));
    colleague->setIcon(QIcon::fromTheme(QStringLiteral("resource-calendar-insert")));
    connect(colleague, &QAction::triggered, this, &CalendarWindow::openColleagueCalendar);

    KStandardAction::print(
        this,
        [this] {
            printView();
        },
        actions);

    // Close: the window it is in (the one application window when it is embedded)
    KStandardAction::close(
        this,
        [this] {
            window()->close();
        },
        actions);
}

void CalendarWindow::closeEvent(QCloseEvent *event)
{
    KConfigGroup group = windowGroup();
    group.writeEntry("Mode", int(m_mode));
    group.sync();
    KXmlGuiWindow::closeEvent(event);
}

void CalendarWindow::fillCalendarList()
{
    const QScopedValueRollback<bool> filling(m_filling, true);
    m_calendarList->clear();
    QHash<QString, QTreeWidgetItem *> groups;
    const QList<CalendarInfo> calendars = m_data->calendars();
    for (const CalendarInfo &c : calendars) {
        QTreeWidgetItem *group = groups.value(c.group);
        if (!group) {
            group = new QTreeWidgetItem(m_calendarList, {c.group});
            QFont bold = group->font(0);
            bold.setBold(true);
            group->setFont(0, bold);
            group->setFlags(Qt::ItemIsEnabled);
            groups.insert(c.group, group);
            group->setExpanded(true);
        }
        auto *item = new QTreeWidgetItem(group, {c.name});
        QPixmap swatch(14, 14);
        swatch.fill(c.color);
        item->setIcon(0, QIcon(swatch));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(0, c.shown ? Qt::Checked : Qt::Unchecked);
        item->setData(0, Qt::UserRole, c.key());
        item->setToolTip(0, c.canEdit ? c.owner : i18nc("@info:tooltip %1 the owner", "%1 (read only)", c.owner));
    }
}

QPair<QDate, QDate> CalendarWindow::range() const
{
    switch (m_mode) {
    case Mode::Day:
        return {m_date, m_date};
    case Mode::WorkWeek: {
        // The working days of the week (Monday to Friday unless the mailbox says otherwise)
        const QDate monday = m_date.addDays(1 - m_date.dayOfWeek());
        if (m_workingHours.days.isEmpty()) {
            return {monday, monday.addDays(4)};
        }
        return {monday.addDays(m_workingHours.days.constFirst() - 1), monday.addDays(m_workingHours.days.constLast() - 1)};
    }
    case Mode::Week: {
        const QDate first = weekStart(m_date);
        return {first, first.addDays(6)};
    }
    case Mode::Month: {
        const QDate first = weekStart(QDate(m_date.year(), m_date.month(), 1));
        return {first, first.addDays(41)};
    }
    case Mode::Agenda:
        return {m_date, m_date.addDays(29)};
    }
    return {m_date, m_date};
}

CalendarView *CalendarWindow::currentView() const
{
    return static_cast<CalendarView *>(m_stack->currentWidget());
}

void CalendarWindow::setMode(Mode mode)
{
    m_mode = mode;
    const QStringList names{QStringLiteral("view_day"), QStringLiteral("view_workweek"), QStringLiteral("view_week"), QStringLiteral("view_month"), QStringLiteral("view_agenda")};
    if (QAction *action = actionCollection()->action(names.at(int(mode)))) {
        action->setChecked(true);
    }
    m_stack->setCurrentWidget(mode == Mode::Month ? static_cast<QWidget *>(m_month) : mode == Mode::Agenda ? static_cast<QWidget *>(m_agenda) : m_grid);
    refresh();
}

void CalendarWindow::goTo(QDate date)
{
    m_date = date;
    refresh();
}

void CalendarWindow::step(int direction)
{
    switch (m_mode) {
    case Mode::Day:
        m_date = m_date.addDays(direction);
        break;
    case Mode::WorkWeek:
    case Mode::Week:
        m_date = m_date.addDays(7 * direction);
        break;
    case Mode::Month:
        m_date = m_date.addMonths(direction);
        break;
    case Mode::Agenda:
        m_date = m_date.addDays(30 * direction);
        break;
    }
    refresh();
}

void CalendarWindow::refresh()
{
    const auto [first, last] = range();
    const QList<DisplayEvent> events = m_data->events(first, last);
    currentView()->setEvents(first, last, events);
    fetchFreeBusy(first, last);
    m_picker->blockSignals(true);
    m_picker->setDate(m_date);
    m_picker->blockSignals(false);
    QString title;
    if (m_mode == Mode::Month) {
        title = QLocale().toString(m_date, QStringLiteral("MMMM yyyy"));
    } else if (first == last) {
        title = QLocale().toString(first, QStringLiteral("dddd d MMMM yyyy"));
    } else if (first.month() == last.month()) {
        title = i18nc("@title days of one month", "%1 - %2", QString::number(first.day()), QLocale().toString(last, QStringLiteral("d MMMM yyyy")));
    } else {
        title = i18nc("@title a range of days", "%1 - %2", QLocale().toString(first, QStringLiteral("d MMMM")), QLocale().toString(last, QStringLiteral("d MMMM yyyy")));
    }
    // The week, as Outlook shows it with week numbers on
    if (m_mode != Mode::Month && m_mode != Mode::Agenda && calendarGroup().readEntry("WeekNumbers", false)) {
        title = i18nc("@title %1 days, %2 a week number", "%1 (week %2)", title, first.weekNumber());
    }
    m_title->setText(title);
}

KConfigGroup CalendarWindow::calendarGroup()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Calendar"));
}

void CalendarWindow::printView(const QString &pdfFile)
{
    const QString title = m_title->text();
    Printing::print(
        this,
        title,
        [this, title](QPainter *painter, QPrinter *printer) {
            const QRectF page(QPointF(0, 0), printer->pageLayout().paintRectPixels(printer->resolution()).size());
            QFont font = QApplication::font();
            font.setBold(true);
            font.setPixelSize(int(page.height() / 32));
            painter->setFont(font);
            painter->setPen(Qt::black);
            const QRectF titleRect(page.left(), page.top(), page.width(), page.height() / 18);
            painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, title);
            const QRectF body(page.left(), titleRect.bottom() + page.height() / 80, page.width(), page.height() - titleRect.height() - page.height() / 80);
            if (currentView() == m_grid) {
                const auto [from, to] = m_grid->printHours();
                m_grid->printTo(painter, body, from, to);
            } else {
                Printing::renderLight(currentView(), painter, body);
            }
        },
        pdfFile);
}

void CalendarWindow::applySecondTimeZone()
{
    const KConfigGroup group = calendarGroup();
    // No name, no second column (an empty name gives the local zone, a valid one)
    const QString name = group.readEntry("SecondTimeZone", QString());
    const QTimeZone zone = name.isEmpty() ? QTimeZone() : QTimeZone(name.toUtf8());
    QString label = group.readEntry("SecondTimeZoneLabel", QString());
    if (zone.isValid() && label.isEmpty()) {
        label = zone.displayName(QTimeZone::GenericTime, QTimeZone::ShortName);
    }
    QString own = group.readEntry("OwnTimeZoneLabel", QString());
    if (own.isEmpty()) {
        own = QTimeZone::systemTimeZone().displayName(QTimeZone::GenericTime, QTimeZone::ShortName);
    }
    m_grid->setSecondTimeZone(zone, label, own);
}

void CalendarWindow::askSecondTimeZone()
{
    QDialog dialog(this);
    dialog.setWindowTitle(i18nc("@title:window", "Second Time Zone"));
    auto *on = new QCheckBox(i18nc("@option:check", "Show a second time zone"), &dialog);
    auto *zone = new QComboBox(&dialog);
    // The zones by their offset and city, as Outlook lists them
    QList<QPair<int, QByteArray>> zones;
    const QList<QByteArray> ids = QTimeZone::availableTimeZoneIds();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const QByteArray &id : ids) {
        if (id.contains('/') && !id.startsWith("Etc/")) {
            zones.append({QTimeZone(id).offsetFromUtc(now), id});
        }
    }
    std::sort(zones.begin(), zones.end());
    for (const auto &[offset, id] : std::as_const(zones)) {
        const QString sign = offset < 0 ? QStringLiteral("-") : QStringLiteral("+");
        const int minutes = std::abs(offset) / 60;
        zone->addItem(QStringLiteral("(UTC%1%2:%3) %4")
                          .arg(sign)
                          .arg(minutes / 60, 2, 10, QLatin1Char('0'))
                          .arg(minutes % 60, 2, 10, QLatin1Char('0'))
                          .arg(QString::fromUtf8(id).replace(QLatin1Char('_'), QLatin1Char(' '))),
                      QString::fromUtf8(id));
    }
    auto *label = new QLineEdit(&dialog);
    auto *ownLabel = new QLineEdit(&dialog);
    label->setPlaceholderText(i18nc("@info:placeholder", "Shown above its hours, e.g. New York"));
    ownLabel->setPlaceholderText(QTimeZone::systemTimeZone().displayName(QTimeZone::GenericTime, QTimeZone::ShortName));
    const KConfigGroup group = calendarGroup();
    const QString current = group.readEntry("SecondTimeZone", QString());
    on->setChecked(!current.isEmpty());
    zone->setCurrentIndex(std::max(0, zone->findData(current.isEmpty() ? QStringLiteral("America/New_York") : current)));
    label->setText(group.readEntry("SecondTimeZoneLabel", QString()));
    ownLabel->setText(group.readEntry("OwnTimeZoneLabel", QString()));
    for (QWidget *w : std::initializer_list<QWidget *>{zone, label, ownLabel}) {
        w->setEnabled(on->isChecked());
        connect(on, &QCheckBox::toggled, w, &QWidget::setEnabled);
    }
    auto *form = new QFormLayout;
    form->addRow(i18nc("@label:listbox", "Time zone:"), zone);
    form->addRow(i18nc("@label:textbox", "Label:"), label);
    form->addRow(i18nc("@label:textbox", "Label of your own time zone:"), ownLabel);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(on);
    layout->addLayout(form);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    setSecondTimeZoneForTests(on->isChecked() ? zone->currentData().toString() : QString(), label->text().trimmed(), ownLabel->text().trimmed());
}

void CalendarWindow::setSecondTimeZoneForTests(const QString &zone, const QString &label, const QString &ownLabel)
{
    KConfigGroup group = calendarGroup();
    group.writeEntry("SecondTimeZone", zone);
    group.writeEntry("SecondTimeZoneLabel", label);
    group.writeEntry("OwnTimeZoneLabel", ownLabel);
    group.sync();
    applySecondTimeZone();
}

void CalendarWindow::newEvent(const QDateTime &start, const QDateTime &end, bool allDay)
{
    const QList<CalendarInfo> writable = m_data->writable();
    if (writable.isEmpty()) {
        return;
    }
    EventEditRequest request;
    request.mailboxKey = writable.first().mailboxKey;
    request.calendar = writable.first().id;
    request.start = start;
    request.end = end;
    request.allDay = allDay;
    auto *dialog = new EventDialog(m_mailboxes, m_daemon, m_data, request, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

QWidget *CalendarWindow::newEventForTests(const QDateTime &start, const QDateTime &end)
{
    const QList<CalendarInfo> writable = m_data->writable();
    if (writable.isEmpty()) {
        return nullptr;
    }
    EventEditRequest request;
    request.mailboxKey = writable.first().mailboxKey;
    request.calendar = writable.first().id;
    request.start = start;
    request.end = end;
    auto *dialog = new EventDialog(m_mailboxes, m_daemon, m_data, request, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    return dialog;
}

QWidget *CalendarWindow::lastDialog() const
{
    return m_lastDialog;
}

std::optional<DisplayEvent> CalendarWindow::chooseOccurrenceOrSeries(const DisplayEvent &event, bool forDelete, bool *series)
{
    *series = false;
    if (!event.recurring() || event.seriesMaster.isEmpty()) {
        return event;
    }
    const auto master = [this, &event, series]() -> std::optional<DisplayEvent> {
        const Store *store = m_mailboxes->store(event.mailboxKey);
        const std::optional<EventRow> row = store ? store->eventByGraphId(event.seriesMaster) : std::nullopt;
        if (!row) {
            return std::nullopt;
        }
        *series = true;
        return m_data->eventById(event.mailboxKey, row->id);
    };
    if (m_testMode) {
        return m_testSeries ? master() : std::optional<DisplayEvent>(event);
    }
    QDialog dialog(this);
    dialog.setWindowTitle(forDelete ? i18nc("@title:window", "Delete Recurring Event") : i18nc("@title:window", "Open Recurring Event"));
    auto *label = new QLabel(i18nc("@info", "\"%1\" is a recurring event.", event.subject), &dialog);
    auto *one = new QRadioButton(forDelete ? i18nc("@option:radio", "Delete this occurrence") : i18nc("@option:radio", "Open this occurrence"), &dialog);
    auto *all = new QRadioButton(forDelete ? i18nc("@option:radio", "Delete the series") : i18nc("@option:radio", "Open the series"), &dialog);
    one->setChecked(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(label);
    layout->addWidget(one);
    layout->addWidget(all);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    if (one->isChecked()) {
        return event;
    }
    // The series master: in the cache with the window's events
    const std::optional<DisplayEvent> whole = master();
    if (!whole) {
        KMessageBox::error(this, i18nc("@info", "The series of this event is not in the calendar yet. Try again after the next sync."));
    }
    return whole;
}

void CalendarWindow::activate(const DisplayEvent &event)
{
    // A colleague's free/busy block: nothing to open
    if (CalendarData::isFreeBusy(event.mailboxKey)) {
        return;
    }
    bool series = false;
    if (const std::optional<DisplayEvent> target = chooseOccurrenceOrSeries(event, false, &series)) {
        editEvent(*target, series);
    }
}

void CalendarWindow::openEvent(const QString &mailboxKey, qint64 event)
{
    if (const std::optional<DisplayEvent> e = m_data->eventById(mailboxKey, event)) {
        goTo(e->start.date());
        editEvent(*e, false);
    }
}

QWidget *CalendarWindow::editEvent(const DisplayEvent &event, bool series)
{
    EventEditRequest request;
    request.event = event;
    request.series = series;
    request.mailboxKey = event.mailboxKey;
    auto *dialog = new EventDialog(m_mailboxes, m_daemon, m_data, request, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_lastDialog = dialog;
    connect(dialog, &EventDialog::deleteRequested, this, [this, event, series] {
        // The occurrence-or-series question was asked when it was opened
        if (series) {
            const QString account = event.mailboxKey.section(QLatin1Char('/'), 0, 0);
            const QString mailbox = event.mailboxKey.section(QLatin1Char('/'), 1);
            if (KMessageBox::warningContinueCancel(this, i18nc("@info", "Delete every occurrence of \"%1\"?", event.subject), i18nc("@title:window", "Delete Series"),
                                                   KStandardGuiItem::del())
                == KMessageBox::Continue) {
                m_daemon->deleteEvent(account, mailbox, event.id, false, {});
            }
            return;
        }
        deleteEvent(event);
    });
    dialog->show();
    return dialog;
}

void CalendarWindow::setTestAnswers(bool series, const QString &comment)
{
    m_testMode = true;
    m_testSeries = series;
    m_testComment = comment;
}

std::optional<DisplayEvent> CalendarWindow::testEvent(const QString &mailboxKey, qint64 id) const
{
    const std::optional<DisplayEvent> e = m_data->eventById(mailboxKey, id);
    if (!e || !e->subject.contains(QLatin1String("[TEST Graph Mail]"))) {
        qWarning() << "Not a test event:" << mailboxKey << id;
        return std::nullopt;
    }
    return e;
}

QWidget *CalendarWindow::editEventForTests(const QString &mailboxKey, qint64 id)
{
    const std::optional<DisplayEvent> e = testEvent(mailboxKey, id);
    if (!e) {
        return nullptr;
    }
    bool series = false;
    const std::optional<DisplayEvent> target = chooseOccurrenceOrSeries(*e, false, &series);
    return target ? editEvent(*target, series) : nullptr;
}

bool CalendarWindow::deleteEventForTests(const QString &mailboxKey, qint64 id)
{
    const std::optional<DisplayEvent> e = testEvent(mailboxKey, id);
    if (e) {
        deleteEvent(*e);
    }
    return e.has_value();
}

bool CalendarWindow::respondForTests(const QString &mailboxKey, qint64 id, const QString &action)
{
    const std::optional<DisplayEvent> e = testEvent(mailboxKey, id);
    if (e) {
        respond(*e, action, m_testComment.isEmpty() ? 0 : 1);
    }
    return e.has_value();
}

bool CalendarWindow::proposeForTests(const QString &mailboxKey, qint64 id, const QString &spec)
{
    const std::optional<DisplayEvent> e = testEvent(mailboxKey, id);
    const QDateTime start = QDateTime::fromString(spec.section(QLatin1Char('|'), 1, 1), Qt::ISODate);
    const QDateTime end = QDateTime::fromString(spec.section(QLatin1Char('|'), 2, 2), Qt::ISODate);
    if (!e || !start.isValid() || end <= start) {
        return false;
    }
    m_daemon->proposeNewTime(mailboxKey.section(QLatin1Char('/'), 0, 0), mailboxKey.section(QLatin1Char('/'), 1), e->graphId, spec.section(QLatin1Char('|'), 3, 3), m_testComment,
                             start.toMSecsSinceEpoch(), end.toMSecsSinceEpoch());
    return true;
}

void CalendarWindow::deleteEvent(const DisplayEvent &original)
{
    if (!original.canEdit) {
        return;
    }
    bool series = false;
    const std::optional<DisplayEvent> chosen = chooseOccurrenceOrSeries(original, true, &series);
    if (!chosen) {
        return;
    }
    const DisplayEvent &event = *chosen;
    const QString account = event.mailboxKey.section(QLatin1Char('/'), 0, 0);
    const QString mailbox = event.mailboxKey.section(QLatin1Char('/'), 1);
    const QString organizer = event.json.value(QLatin1String("organizer")).toObject().value(QLatin1String("emailAddress")).toObject().value(QLatin1String("address")).toString().toLower();
    const bool othersMeeting = !event.isOrganizer && !organizer.isEmpty() && !m_data->ownAddresses().contains(organizer);
    if (othersMeeting && !event.cancelled) {
        // Someone else's meeting: deleting is declining, with or without telling them
        respond(event, QStringLiteral("decline"), m_testMode ? (m_testComment.isEmpty() ? 0 : 1) : -1);
        return;
    }
    const bool attendees = !event.json.value(QLatin1String("attendees")).toArray().isEmpty();
    if (event.isOrganizer && attendees && !event.cancelled) {
        bool ok = m_testMode;
        const QString comment = m_testMode ? m_testComment : QInputDialog::getMultiLineText(this,
                                                               i18nc("@title:window", "Cancel Meeting"),
                                                               i18nc("@label:textbox", "The attendees get a cancellation. Message (optional):"),
                                                               {},
                                                               &ok);
        if (ok) {
            m_daemon->deleteEvent(account, mailbox, event.id, true, comment);
        }
        return;
    }
    if (m_testMode
        || KMessageBox::warningContinueCancel(this, i18nc("@info", "Delete \"%1\"?", event.subject), i18nc("@title:window", "Delete Event"), KStandardGuiItem::del())
            == KMessageBox::Continue) {
        m_daemon->deleteEvent(account, mailbox, event.id, false, {});
    }
}

void CalendarWindow::respond(const DisplayEvent &event, const QString &action, int sendMode)
{
    // sendMode: 0 send now, 1 edit first, 2 do not send, -1 ask
    if (sendMode < 0) {
        QMenu menu(this);
        QAction *now = menu.addAction(i18nc("@action:inmenu", "Send the Response Now"));
        QAction *edit = menu.addAction(i18nc("@action:inmenu", "Edit the Response Before Sending..."));
        menu.addAction(i18nc("@action:inmenu", "Do Not Send a Response"));
        QAction *chosen = menu.exec(QCursor::pos());
        if (!chosen) {
            return;
        }
        sendMode = chosen == now ? 0 : chosen == edit ? 1 : 2;
    }
    QString comment = m_testComment;
    if (sendMode == 1 && !m_testMode) {
        bool ok = false;
        comment = QInputDialog::getMultiLineText(this, i18nc("@title:window", "Response"), i18nc("@label:textbox", "Message to the organizer:"), {}, &ok);
        if (!ok) {
            return;
        }
    }
    m_daemon->respondEvent(event.mailboxKey.section(QLatin1Char('/'), 0, 0), event.mailboxKey.section(QLatin1Char('/'), 1), event.id, action, comment, sendMode != 2);
}

void CalendarWindow::showMenu(const DisplayEvent &event, const QPoint &globalPos)
{
    if (CalendarData::isFreeBusy(event.mailboxKey)) {
        return;
    }
    QMenu menu(this);
    menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), i18nc("@action:inmenu", "Open"), this, [this, event] {
        activate(event);
    });
    const QString joinUrl = event.json.value(QLatin1String("onlineMeeting")).toObject().value(QLatin1String("joinUrl")).toString();
    if (!joinUrl.isEmpty()) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("call-start")), i18nc("@action:inmenu", "Join Teams Meeting"), this, [joinUrl] {
            QDesktopServices::openUrl(QUrl(joinUrl));
        });
    }
    // Categorize: the mailbox's categories to tick (a series as a whole)
    if (event.canEdit && m_categoryList) {
        QMenu *categorize = menu.addMenu(QIcon::fromTheme(QStringLiteral("tag")), i18nc("@title:menu", "Categorize"));
        const QStringList current = m_data->categoriesOf(event);
        const QList<CategoryList::Category> categories = m_categoryList->categories(event.mailboxKey);
        for (const CategoryList::Category &c : categories) {
            QPixmap swatch(14, 14);
            swatch.fill(CategoryList::presetColor(c.preset));
            QAction *a = categorize->addAction(QIcon(swatch), c.name);
            a->setCheckable(true);
            a->setChecked(current.contains(c.name));
            const QString name = c.name;
            connect(a, &QAction::toggled, this, [this, event, name](bool on) {
                setEventCategory(event, name, on);
            });
        }
        if (categories.isEmpty()) {
            categorize->addAction(i18nc("@item:inmenu", "(no categories)"))->setEnabled(false);
        }
    }
    const QString organizer = event.json.value(QLatin1String("organizer")).toObject().value(QLatin1String("emailAddress")).toObject().value(QLatin1String("address")).toString().toLower();
    const bool othersMeeting = !event.isOrganizer && !organizer.isEmpty() && !m_data->ownAddresses().contains(organizer);
    if (othersMeeting && event.canEdit && !event.cancelled) {
        menu.addSeparator();
        const struct {
            const char *action;
            const char *icon;
            KLocalizedString text;
        } answers[] = {{"accept", "dialog-ok-apply", ki18nc("@action:inmenu", "Accept")},
                       {"tentativelyAccept", "dialog-question", ki18nc("@action:inmenu", "Tentative")},
                       {"decline", "dialog-cancel", ki18nc("@action:inmenu", "Decline")}};
        for (const auto &answer : answers) {
            QMenu *sub = menu.addMenu(QIcon::fromTheme(QLatin1String(answer.icon)), answer.text.toString());
            const QString action = QLatin1String(answer.action);
            sub->addAction(i18nc("@action:inmenu", "Send the Response Now"), this, [this, event, action] {
                respond(event, action, 0);
            });
            sub->addAction(i18nc("@action:inmenu", "Edit the Response Before Sending..."), this, [this, event, action] {
                respond(event, action, 1);
            });
            sub->addAction(i18nc("@action:inmenu", "Do Not Send a Response"), this, [this, event, action] {
                respond(event, action, 2);
            });
        }
        if (event.json.value(QLatin1String("allowNewTimeProposals")).toBool(true)) {
            menu.addAction(QIcon::fromTheme(QStringLiteral("appointment-new")), i18nc("@action:inmenu", "Propose New Time..."), this, [this, event] {
                if (const std::optional<EventDialog::NewTime> t = EventDialog::askNewTime(this, event.start, event.end)) {
                    m_daemon->proposeNewTime(event.mailboxKey.section(QLatin1Char('/'), 0, 0), event.mailboxKey.section(QLatin1Char('/'), 1), event.graphId, t->action, t->comment,
                                             t->start.toMSecsSinceEpoch(), t->end.toMSecsSinceEpoch());
                }
            });
        }
    }
    if (event.canEdit) {
        menu.addSeparator();
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action:inmenu", "Delete"), this, [this, event] {
            deleteEvent(event);
        });
    }
    menu.exec(globalPos);
}

void CalendarWindow::fetchFreeBusy(QDate first, QDate last)
{
    const QHash<QString, QStringList> missing = m_data->freeBusyMissing(first, last);
    for (auto it = missing.cbegin(); it != missing.cend(); ++it) {
        const QString account = it.key();
        if (m_freeBusyPending.contains(account)) {
            continue;
        }
        m_freeBusyPending.insert(account);
        const QStringList addresses = it.value();
        // getSchedule takes at most 62 days; the widest view (month) shows 42
        const QJsonObject args{{QStringLiteral("addresses"), QJsonArray::fromStringList(addresses)},
                               {QStringLiteral("start"), QDateTime(first, QTime(0, 0)).toMSecsSinceEpoch()},
                               {QStringLiteral("end"), QDateTime(last.addDays(1), QTime(0, 0)).toMSecsSinceEpoch()},
                               {QStringLiteral("interval"), 30}};
        m_daemon->query(account, QStringLiteral("me"), QStringLiteral("schedule"), QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this,
                        [this, account, addresses, first, last](const QString &json) {
                            m_freeBusyPending.remove(account);
                            const QJsonArray value = QJsonDocument::fromJson(json.toUtf8()).object().value(QLatin1String("value")).toArray();
                            QStringList answered;
                            for (const QJsonValue &v : value) {
                                m_data->setFreeBusy(v.toObject(), first, last);
                                answered.append(v.toObject().value(QLatin1String("scheduleId")).toString().toLower());
                            }
                            // No answer (offline): remembered as empty, asked again after ten minutes
                            for (const QString &address : addresses) {
                                if (!answered.contains(address.toLower())) {
                                    m_data->setFreeBusy(QJsonObject{{QStringLiteral("scheduleId"), address},
                                                                    {QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), QStringLiteral("no answer")}}}},
                                                        first,
                                                        last);
                                }
                            }
                            qWarning() << "free/busy test:" << answered.size() << "of" << addresses.size() << "answered";
                            const auto [f, l] = range();
                            currentView()->setEvents(f, l, m_data->events(f, l));
                        });
    }
}

void CalendarWindow::addColleague(const QString &address, QObject *context, const std::function<void(const QString &, bool)> &done)
{
    const QString account = m_mailboxes->primaryAccount();
    m_daemon->checkMailbox(account, address, false, context, [this, account, address, context, done](const QString &reply) {
        const QJsonObject access = QJsonDocument::fromJson(reply.toUtf8()).object();
        const QString problem = access.value(QLatin1String("problem")).toString();
        const QString name = access.value(QLatin1String("name")).toString();
        const QString found = access.value(QLatin1String("address")).toString();
        qWarning() << "colleague calendar test:" << found << "calendar" << access.value(QLatin1String("calendar")).toBool() << "problem" << problem;
        if (access.isEmpty() || !problem.isEmpty()) {
            done(accessProblemText(access.isEmpty() ? QStringLiteral("error") : problem, address), false);
            return;
        }
        if (!access.value(QLatin1String("calendar")).toBool()) {
            m_data->addFreeBusy(account, found, name);
            fillCalendarList();
            refresh();
            done(i18nc("@info %1 a person", "%1 has not shared their calendar with you: their free/busy times are shown.", name), true);
            return;
        }
        // Shared with the user and already among their calendars: show it
        const QList<CalendarInfo> calendars = m_data->calendars();
        for (const CalendarInfo &c : calendars) {
            if (c.owner.compare(found, Qt::CaseInsensitive) == 0 && !CalendarData::isFreeBusy(c.mailboxKey)) {
                m_data->setShown(c.key(), true);
                fillCalendarList();
                refresh();
                done(i18nc("@info %1 a person", "%1's calendar is shown.", name), true);
                return;
            }
        }
        // Shared but not yet here: a calendar-only (or full) mailbox
        m_daemon->checkMailbox(account, found, true, context, [this, name, done](const QString &added) {
            const QJsonObject result = QJsonDocument::fromJson(added.toUtf8()).object();
            const QString problem = result.value(QLatin1String("problem")).toString();
            if (result.isEmpty() || !problem.isEmpty()) {
                done(accessProblemText(result.isEmpty() ? QStringLiteral("error") : problem, name), false);
                return;
            }
            m_mailboxes->reload();
            QTimer::singleShot(3000, this, [this] {
                m_mailboxes->reload();
            });
            done(i18nc("@info %1 a person", "%1's calendar was added; Graph Mail is copying it now.", name), true);
        });
    });
}

void CalendarWindow::openColleagueCalendar()
{
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(i18nc("@title:window", "Add a Colleague's Calendar"));
    auto *book = new AddressBook(m_mailboxes, dialog);
    auto *address = new RecipientEdit(book, dialog);
    address->setPlaceholderText(i18nc("@info:placeholder", "Name or address of a colleague"));
    auto *status = new QLabel(i18nc("@info", "Graph Mail shows their calendar if it is shared with you, else when they are free or busy."), dialog);
    status->setWordWrap(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Close, dialog);
    auto *layout = new QVBoxLayout(dialog);
    layout->addWidget(address);
    layout->addWidget(status);
    layout->addWidget(buttons);
    dialog->resize(480, 160);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, address, status, buttons] {
        const QList<OutgoingRecipient> people = address->recipients();
        const QString text = people.isEmpty() ? address->text().trimmed() : people.first().address;
        if (text.isEmpty()) {
            return;
        }
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        status->setText(i18nc("@info:status", "Looking up %1...", text));
        addColleague(text, dialog, [dialog, status, buttons](const QString &message, bool ok) {
            status->setText(message);
            buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
            if (ok) {
                QTimer::singleShot(2500, dialog, &QDialog::accept);
            }
        });
    });
    dialog->show();
}

void CalendarWindow::removeCalendarEntry(const CalendarInfo &calendar, bool ask)
{
    if (ask
        && KMessageBox::warningContinueCancel(this,
                                              isSharedWithMe(calendar)
                                                  ? i18nc("@info %1 a calendar",
                                                          "Remove %1 from your calendars? It also leaves your calendar list in Outlook; the calendar itself stays with its owner.",
                                                          calendar.name)
                                                  : i18nc("@info %1 a calendar", "Remove %1 from Graph Mail? Nothing changes in Microsoft 365.", calendar.name),
                                              i18nc("@title:window", "Remove Calendar"),
                                              KStandardGuiItem::remove())
            != KMessageBox::Continue) {
        return;
    }
    if (isSharedWithMe(calendar)) {
        m_daemon->query(calendar.mailboxKey.section(QLatin1Char('/'), 0, 0), calendar.mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("calendarRemove"),
                        QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("calendar"), calendar.id}}).toJson(QJsonDocument::Compact)), this,
                        [this, calendar](const QString &json) {
                            const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                            qWarning() << "colleague remove test:" << answer.value(QLatin1String("ok")).toBool() << answer.value(QLatin1String("detail")).toString();
                            if (!answer.value(QLatin1String("ok")).toBool()) {
                                statusBar()->showMessage(accessProblemText(answer.isEmpty() ? QStringLiteral("error") : answer.value(QLatin1String("problem")).toString(), calendar.name), 8000);
                            }
                        });
        return;
    }
    if (CalendarData::isFreeBusy(calendar.mailboxKey)) {
        m_data->removeFreeBusy(calendar.graphId);
        fillCalendarList();
        refresh();
        qWarning() << "colleague remove test: free/busy removed";
        return;
    }
    m_daemon->removeMailbox(calendar.mailboxKey.section(QLatin1Char('/'), 0, 0), calendar.mailboxKey.section(QLatin1Char('/'), 1), this, [this](const QString &reply) {
        qWarning() << "colleague remove test:" << reply;
        m_mailboxes->reload();
    });
}

void CalendarWindow::addColleagueForTests(const QString &address)
{
    addColleague(address, this, [](const QString &message, bool ok) {
        qWarning() << "colleague calendar test: done" << ok << message;
    });
}

void CalendarWindow::removeColleagueForTests(const QString &address)
{
    const QList<CalendarInfo> calendars = m_data->calendars();
    for (const CalendarInfo &c : calendars) {
        const std::optional<MailboxInfo> box = m_mailboxes->mailbox(c.mailboxKey);
        const bool removable = CalendarData::isFreeBusy(c.mailboxKey) || (box && !box->own && !box->mail) || isSharedWithMe(c);
        if (removable && c.owner.compare(address, Qt::CaseInsensitive) == 0) {
            removeCalendarEntry(c, false);
            return;
        }
    }
    qWarning() << "colleague remove test: not found";
}

CalendarSharingDialog *CalendarWindow::openSharing(const QString &calendarKey)
{
    const QList<CalendarInfo> calendars = m_data->calendars();
    std::optional<CalendarInfo> target;
    for (const CalendarInfo &c : calendars) {
        const bool primaryDefault = calendarKey.isEmpty() && c.isDefault && c.mailboxKey == m_mailboxes->primaryAccount() + QStringLiteral("/me");
        if (c.key() == calendarKey || primaryDefault) {
            target = c;
        }
    }
    if (!target) {
        return nullptr;
    }
    auto *dialog = new CalendarSharingDialog(m_mailboxes, m_daemon, *target, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    return dialog;
}

bool CalendarWindow::isSharedWithMe(const CalendarInfo &calendar) const
{
    // In the user's own mailbox, someone else's, read only (a shared calendar the user accepted)
    const std::optional<MailboxInfo> box = m_mailboxes->mailbox(calendar.mailboxKey);
    return box && box->own && !calendar.isDefault && !calendar.canEdit && !calendar.owner.isEmpty()
        && !m_data->ownAddresses().contains(calendar.owner.toLower());
}

void CalendarWindow::setCategoryList(CategoryList *categories)
{
    m_categoryList = categories;
    m_data->setCategoryList(categories);
    connect(categories, &CategoryList::changed, &m_reloadTimer, qOverload<>(&QTimer::start));
    m_reloadTimer.start();
}

void CalendarWindow::setEventCategory(const DisplayEvent &event, const QString &name, bool on)
{
    // A recurring event's categories live on its series master
    qint64 target = event.id;
    if (event.recurring() && !event.seriesMaster.isEmpty()) {
        const Store *store = m_mailboxes->store(event.mailboxKey);
        if (const std::optional<EventRow> master = store ? store->eventByGraphId(event.seriesMaster) : std::nullopt) {
            target = master->id;
        }
    }
    QStringList categories = m_data->categoriesOf(event);
    categories.removeAll(name);
    if (on) {
        categories.append(name);
    }
    m_daemon->updateEvent(event.mailboxKey.section(QLatin1Char('/'), 0, 0), event.mailboxKey.section(QLatin1Char('/'), 1), target,
                          QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("categories"), QJsonArray::fromStringList(categories)}}).toJson(QJsonDocument::Compact)));
}

void CalendarWindow::categorizeEventForTests(const QString &mailboxKey, qint64 event, const QString &name, bool on)
{
    const std::optional<DisplayEvent> e = m_data->eventById(mailboxKey, event);
    if (!e || !e->subject.contains(QLatin1String("[TEST Graph Mail]"))) {
        qWarning() << "categories test: not a test event";
        return;
    }
    setEventCategory(*e, name, on);
    qWarning() << "categories test: event" << (on ? "set" : "unset");
}

}
