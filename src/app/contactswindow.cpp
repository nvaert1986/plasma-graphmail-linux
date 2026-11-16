// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "contactswindow.h"
#include "printing.h"
#include "categorylist.h"
#include "avatar.h"
#include "composer.h"
#include "contactdialog.h"
#include "daemonclient.h"
#include "mailboxset.h"
#include "mimeexport.h"
#include "store.h"
#include "vcard.h"

#include <KActionCollection>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KMessageBox>
#include <KSelectAction>
#include <KSharedConfig>
#include <KStandardAction>

#include <QHBoxLayout>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QStatusBar>
#include <QRegularExpression>
#include <QPainter>
#include <QLocale>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSplitter>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <memory>

namespace GraphMail
{

namespace
{
QString text(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString();
}

// The initials circle of the message list, as a picture
QPixmap avatarPixmap(const QString &name, const QString &address, int size)
{
    QPixmap pixmap(size * 2, size * 2);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    Avatar::paint(&painter, QRect(0, 0, size, size), name, address);
    return pixmap;
}

// The first address of a contact (emailAddresses) or a directory entry (mail, else the UPN)
QString mailOf(const QJsonObject &o)
{
    const QString address = o.value(QLatin1String("emailAddresses")).toArray().first().toObject().value(QLatin1String("address")).toString();
    if (!address.isEmpty()) {
        return address;
    }
    return text(o, "mail").isEmpty() ? text(o, "userPrincipalName") : text(o, "mail");
}

QString phoneOf(const QJsonObject &o)
{
    const QString business = o.value(QLatin1String("businessPhones")).toArray().first().toString();
    return business.isEmpty() ? text(o, "mobilePhone") : business;
}

QString companyOf(const QJsonObject &o)
{
    return text(o, "companyName").isEmpty() ? text(o, "department") : text(o, "companyName");
}

KConfigGroup contactsGroup()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Contacts"));
}
}

QString ContactsWindow::listName(const QJsonObject &o, NameOrder order)
{
    // Contacts have a middle name, people of the directory do not
    const QString first = QStringList{text(o, "givenName"), text(o, "middleName")}.join(QLatin1Char(' ')).simplified();
    const QString last = text(o, "surname").simplified();
    if (order == NameOrder::DisplayName || (first.isEmpty() && last.isEmpty())) {
        return text(o, "displayName");
    }
    if (first.isEmpty() || last.isEmpty()) {
        return first + last;
    }
    if (order == NameOrder::LastFirst) {
        return last + QStringLiteral(", ") + first;
    }
    return first + QLatin1Char(' ') + last;
}

ContactsWindow::ContactsWindow(MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent)
    : KXmlGuiWindow(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
    , m_sources(new QTreeWidget(this))
    , m_filter(new QLineEdit(this))
    , m_list(new QTreeWidget(this))
    , m_photo(new QLabel(this))
    , m_card(new QLabel(this))
    , m_message(new QPushButton(QIcon::fromTheme(QStringLiteral("mail-message-new")), i18nc("@action:button", "New Message"), this))
    , m_edit(new QPushButton(QIcon::fromTheme(QStringLiteral("document-edit")), i18nc("@action:button", "Edit"), this))
    , m_delete(new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action:button", "Delete"), this))
    , m_add(new QPushButton(QIcon::fromTheme(QStringLiteral("list-add-user"), QIcon::fromTheme(QStringLiteral("contact-new"))), i18nc("@action:button", "Add to Contacts"), this))
    , m_forward(new QPushButton(QIcon::fromTheme(QStringLiteral("mail-forward")), i18nc("@action:button Outlook's Forward Contact, As a vCard", "Forward as vCard"), this))
{
    setWindowTitle(i18nc("@title:window", "Contacts"));
    m_sources->setHeaderHidden(true);
    m_sources->setFrameShape(QFrame::NoFrame);
    m_filter->setClearButtonEnabled(true);
    m_list->setColumnCount(4);
    m_list->setHeaderLabels({i18nc("@title:column", "Name"), i18nc("@title:column", "E-mail"), i18nc("@title:column", "Company"), i18nc("@title:column", "Phone")});
    m_list->setRootIsDecorated(false);
    m_list->setAlternatingRowColors(true);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(0, Qt::AscendingOrder);
    m_photo->setFixedSize(96, 96);
    m_photo->setAlignment(Qt::AlignCenter);
    m_card->setTextFormat(Qt::RichText);
    m_card->setWordWrap(true);
    m_card->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_card->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_card->setOpenExternalLinks(false);
    connect(m_card, &QLabel::linkActivated, this, [this](const QString &link) {
        if (const std::optional<Person> p = current(); p && link.startsWith(QLatin1String("mailto:"))) {
            newMessage(*p);
        } else if (link.startsWith(QLatin1String("person:"))) {
            // A manager or report: their card, from the directory
            searchDirectoryForTests(link.mid(7));
        }
    });

    auto *left = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(m_sources);
    auto *middle = new QWidget(this);
    auto *middleLayout = new QVBoxLayout(middle);
    middleLayout->setContentsMargins(0, 0, 0, 0);
    middleLayout->addWidget(m_filter);
    middleLayout->addWidget(m_list, 1);
    auto *right = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(right);
    auto *header = new QHBoxLayout;
    header->addWidget(m_photo, 0, Qt::AlignTop);
    header->addWidget(m_card, 1);
    rightLayout->addLayout(header, 1);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_message);
    buttons->addWidget(m_forward);
    buttons->addWidget(m_edit);
    buttons->addWidget(m_add);
    m_categorize = new QPushButton(QIcon::fromTheme(QStringLiteral("tag")), i18nc("@action:button", "Categorize"), this);
    auto *categorizeMenu = new QMenu(m_categorize);
    m_categorize->setMenu(categorizeMenu);
    connect(categorizeMenu, &QMenu::aboutToShow, this, [this, categorizeMenu] {
        const std::optional<Person> p = current();
        if (!p || p->id == 0 || !m_categoryList) {
            categorizeMenu->clear();
            return;
        }
        QStringList current;
        const QJsonArray array = p->json.value(QLatin1String("categories")).toArray();
        for (const QJsonValue &v : array) {
            current.append(v.toString());
        }
        const Person person = *p;
        m_categoryList->fillMenu(categorizeMenu, person.mailboxKey, current, [this, person](const QString &name, bool on) {
            setContactCategory(person, name, on);
        });
    });
    buttons->addWidget(m_categorize);
    buttons->addStretch();
    buttons->addWidget(m_delete);
    rightLayout->addLayout(buttons);
    auto *splitter = new QSplitter(this);
    splitter->addWidget(left);
    splitter->addWidget(middle);
    splitter->addWidget(right);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 1);
    splitter->setSizes({220, 560, 480});
    setCentralWidget(splitter);

    connect(m_sources, &QTreeWidget::currentItemChanged, this, &ContactsWindow::sourceChanged);
    connect(m_list, &QTreeWidget::currentItemChanged, this, [this] {
        const std::optional<Person> p = current();
        if (p) {
            showPerson(*p);
        } else {
            m_card->clear();
            m_photo->clear();
        }
        const bool own = p && p->id != 0;
        m_message->setEnabled(p && !mailOf(p->json).isEmpty());
        m_forward->setEnabled(p.has_value());
        m_edit->setVisible(own);
        m_delete->setVisible(own);
        m_categorize->setVisible(own);
        m_add->setVisible(p && p->id == 0);
    });
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] {
        if (const std::optional<Person> p = current()) {
            p->id != 0 ? editContact(*p) : newMessage(*p);
        }
    });
    connect(m_message, &QPushButton::clicked, this, [this] {
        if (const std::optional<Person> p = current()) {
            newMessage(*p);
        }
    });
    connect(m_edit, &QPushButton::clicked, this, [this] {
        if (const std::optional<Person> p = current()) {
            editContact(*p);
        }
    });
    connect(m_delete, &QPushButton::clicked, this, [this] {
        if (const std::optional<Person> p = current()) {
            deleteContact(*p);
        }
    });
    connect(m_add, &QPushButton::clicked, this, [this] {
        if (const std::optional<Person> p = current()) {
            addToContacts(*p);
        }
    });
    connect(m_forward, &QPushButton::clicked, this, &ContactsWindow::forwardAsVCard);
    // The filter filters a folder; in the directory it searches (after a pause)
    m_searchTimer.setSingleShot(true);
    m_searchTimer.setInterval(400);
    connect(&m_searchTimer, &QTimer::timeout, this, &ContactsWindow::searchDirectory);
    connect(m_filter, &QLineEdit::textChanged, this, [this] {
        if (m_directory) {
            m_searchTimer.start();
        } else {
            fillList();
        }
    });
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(300);
    connect(&m_reloadTimer, &QTimer::timeout, this, [this] {
        if (!m_directory) {
            sourceChanged();
        }
    });
    connect(m_mailboxes, &MailboxSet::changed, this, &ContactsWindow::fillSources);
    connect(m_daemon, &DaemonClient::collectionChanged, this, [this](const QString &, const QString &, const QString &collection, int) {
        if (collection == QLatin1String("contacts")) {
            m_reloadTimer.start();
        }
    });

    setupActions();
    setupGUI(QSize(1280, 760), ToolBar | Keys | Save | Create | StatusBar, QStringLiteral("graphmailcontactsui.rc"));
    setAutoSaveSettings(QStringLiteral("ContactsWindow"));
    fillSources();
}

ContactsWindow::~ContactsWindow() = default;

void ContactsWindow::setupActions()
{
    KActionCollection *actions = actionCollection();
    QAction *add = actions->addAction(QStringLiteral("new_contact"));
    add->setText(i18nc("@action", "New Contact"));
    add->setIconText(i18nc("@action short toolbar text", "New"));
    add->setIcon(QIcon::fromTheme(QStringLiteral("list-add-user"), QIcon::fromTheme(QStringLiteral("contact-new"))));
    actions->setDefaultShortcut(add, QKeySequence(Qt::CTRL | Qt::Key_N));
    connect(add, &QAction::triggered, this, &ContactsWindow::newContact);
    QAction *search = actions->addAction(QStringLiteral("find_contact"));
    search->setText(i18nc("@action", "Find a Person"));
    search->setIcon(QIcon::fromTheme(QStringLiteral("search")));
    actions->setDefaultShortcut(search, QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(search, &QAction::triggered, this, [this] {
        m_filter->setFocus();
        m_filter->selectAll();
    });
    // Outlook's People option "Display names as": the list's names and their order
    auto *order = new KSelectAction(i18nc("@action", "Name Order"), this);
    actions->addAction(QStringLiteral("name_order"), order);
    order->setItems({i18nc("@item:inmenu name order", "Display Name"), i18nc("@item:inmenu name order", "First Last"), i18nc("@item:inmenu name order", "Last, First")});
    order->setCurrentItem(std::clamp(contactsGroup().readEntry("NameOrder", 0), 0, 2));
    connect(order, &KSelectAction::indexTriggered, this, [this](int index) {
        KConfigGroup group = contactsGroup();
        group.writeEntry("NameOrder", index);
        group.sync();
        fillList();
    });
    const struct {
        const char *name;
        int module;
        const char *icon;
        KLocalizedString text;
        Qt::Key key;
    } modules[] = {{"go_mail", 1, "mail-message", ki18nc("@action", "Mail"), Qt::Key_1},
                   {"go_calendar", 2, "view-calendar", ki18nc("@action", "Calendar"), Qt::Key_2},
                   {"go_tasks", 4, "view-task", ki18nc("@action", "Tasks"), Qt::Key_4}};
    for (const auto &m : modules) {
        QAction *a = actions->addAction(QLatin1String(m.name));
        a->setText(m.text.toString());
        a->setIcon(QIcon::fromTheme(QLatin1String(m.icon)));
        actions->setDefaultShortcut(a, QKeySequence(Qt::CTRL | m.key));
        const int module = m.module;
        connect(a, &QAction::triggered, this, [this, module] {
            Q_EMIT navigate(module);
        });
    }
    // Close: the window it is in (the one application window when it is embedded)
    KStandardAction::print(
        this,
        [this] {
            printList();
        },
        actions);
    KStandardAction::close(
        this,
        [this] {
            window()->close();
        },
        actions);
}

void ContactsWindow::fillSources()
{
    const QString was = m_sources->currentItem() ? m_sources->currentItem()->data(0, Qt::UserRole).toString() : QString();
    m_sources->blockSignals(true);
    m_sources->clear();
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    QTreeWidgetItem *first = nullptr;
    QTreeWidgetItem *again = nullptr;
    for (const MailboxInfo &box : boxes) {
        const Store *store = m_mailboxes->store(box.key());
        if (!store || !box.contacts) {
            continue;
        }
        auto *group = new QTreeWidgetItem(m_sources, {box.label()});
        group->setFlags(Qt::ItemIsEnabled);
        QFont bold = group->font(0);
        bold.setBold(true);
        group->setFont(0, bold);
        group->setExpanded(true);
        const QList<ParentRow> folders = store->parents(ItemKind::Contact);
        for (const ParentRow &f : folders) {
            const QString name = f.graphId == QLatin1String("contacts") ? i18nc("@item the default contacts folder", "Contacts") : f.name;
            auto *item = new QTreeWidgetItem(group, {QStringLiteral("%1 (%2)").arg(name).arg(store->itemCount(ItemKind::Contact, f.id))});
            item->setIcon(0, QIcon::fromTheme(QStringLiteral("view-pim-contacts")));
            const QString key = box.key() + QLatin1Char('|') + QString::number(f.id);
            item->setData(0, Qt::UserRole, key);
            if (!first && box.own) {
                first = item;
            }
            if (key == was) {
                again = item;
            }
        }
    }
    auto *directory = new QTreeWidgetItem(m_sources, {i18nc("@item the organisation's people", "Directory")});
    directory->setIcon(0, QIcon::fromTheme(QStringLiteral("system-users")));
    directory->setData(0, Qt::UserRole, QStringLiteral("directory"));
    directory->setToolTip(0, i18nc("@info:tooltip", "Everyone in your organisation (read only)"));
    m_sources->blockSignals(false);
    m_sources->setCurrentItem(again ? again : (first ? first : directory));
}

void ContactsWindow::sourceChanged()
{
    QTreeWidgetItem *item = m_sources->currentItem();
    if (!item || item->data(0, Qt::UserRole).toString().isEmpty()) {
        return;
    }
    m_directory = item->data(0, Qt::UserRole).toString() == QLatin1String("directory");
    m_filter->setPlaceholderText(m_directory ? i18nc("@info:placeholder", "Search people in your organisation") : i18nc("@info:placeholder", "Filter contacts"));
    m_people.clear();
    if (!m_directory) {
        const QString key = item->data(0, Qt::UserRole).toString();
        const QString mailboxKey = key.section(QLatin1Char('|'), 0, 0);
        const qint64 folder = key.section(QLatin1Char('|'), 1).toLongLong();
        if (const Store *store = m_mailboxes->store(mailboxKey)) {
            const QList<StoredItem> items = store->items(ItemKind::Contact, folder);
            for (const StoredItem &i : items) {
                m_people.append({mailboxKey, i.id, folder, QJsonDocument::fromJson(i.json.toUtf8()).object()});
            }
        }
        fillList();
    } else {
        fillList();
        searchDirectory();
    }
}

void ContactsWindow::fillList()
{
    const qint64 selected = current() ? current()->id : -1;
    const QString selectedMail = current() ? mailOf(current()->json) : QString();
    m_list->clear();
    const QString filter = m_directory ? QString() : m_filter->text().trimmed();
    const auto order = NameOrder(contactsGroup().readEntry("NameOrder", 0));
    for (int i = 0; i < m_people.size(); ++i) {
        const QJsonObject &o = m_people.at(i).json;
        const QString name = text(o, "displayName");
        const QString shown = listName(o, order);
        if (!filter.isEmpty()
            && !(name.contains(filter, Qt::CaseInsensitive) || shown.contains(filter, Qt::CaseInsensitive) || mailOf(o).contains(filter, Qt::CaseInsensitive)
                 || companyOf(o).contains(filter, Qt::CaseInsensitive))) {
            continue;
        }
        auto *row = new QTreeWidgetItem(m_list, {shown, mailOf(o), companyOf(o), phoneOf(o)});
        row->setData(0, Qt::UserRole, i);
        row->setIcon(0, QIcon(avatarPixmap(name, mailOf(o), 22)));
        if ((selected > 0 && m_people.at(i).id == selected) || (selected == 0 && mailOf(o) == selectedMail)) {
            m_list->setCurrentItem(row);
        }
    }
    m_list->resizeColumnToContents(1);
    if (!m_list->currentItem() && m_list->topLevelItemCount() > 0) {
        m_list->setCurrentItem(m_list->topLevelItem(0));
    }
    statusBar()->showMessage(i18ncp("@info:status", "%1 person", "%1 people", m_list->topLevelItemCount()));
}

void ContactsWindow::searchDirectory()
{
    const QString text = m_filter->text().trimmed();
    if (text.size() < 2) {
        m_people.clear();
        fillList();
        statusBar()->showMessage(i18nc("@info:status", "Type at least two letters to search your organisation"));
        return;
    }
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    // The directory of the account of the first own mailbox
    QString account;
    for (const MailboxInfo &box : boxes) {
        if (box.own) {
            account = box.account;
            break;
        }
    }
    statusBar()->showMessage(i18nc("@info:status", "Searching..."));
    m_daemon->query(account, QStringLiteral("me"), QStringLiteral("directory"), QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("text"), text}}).toJson(QJsonDocument::Compact)), this,
                    [this, text](const QString &json) {
                        if (!m_directory || m_filter->text().trimmed() != text) {
                            return;
                        }
                        m_people.clear();
                        const QJsonArray value = QJsonDocument::fromJson(json.toUtf8()).object().value(QLatin1String("value")).toArray();
                        for (const QJsonValue &v : value) {
                            m_people.append({QString(), 0, 0, v.toObject()});
                        }
                        fillList();
                        if (json.isEmpty()) {
                            statusBar()->showMessage(i18nc("@info:status", "The directory cannot be searched (offline?)"));
                        }
                    });
}

std::optional<ContactsWindow::Person> ContactsWindow::current() const
{
    QTreeWidgetItem *row = m_list->currentItem();
    const int index = row ? row->data(0, Qt::UserRole).toInt() : -1;
    return index >= 0 && index < m_people.size() ? std::optional<Person>(m_people.at(index)) : std::nullopt;
}

void ContactsWindow::showPerson(const Person &p)
{
    const QJsonObject &o = p.json;
    QString html = QStringLiteral("<p style='font-size:large'><b>%1</b></p>").arg(text(o, "displayName").toHtmlEscaped());
    const QStringList role{text(o, "jobTitle"), text(o, "department"), text(o, "companyName"), text(o, "officeLocation")};
    QStringList parts;
    for (const QString &r : role) {
        if (!r.isEmpty()) {
            parts.append(r.toHtmlEscaped());
        }
    }
    if (!parts.isEmpty()) {
        html += QStringLiteral("<p>") + parts.join(QStringLiteral(", ")) + QStringLiteral("</p>");
    }
    auto line = [&html](const QString &label, const QString &value) {
        if (!value.isEmpty()) {
            html += QStringLiteral("<tr><td style='padding-right:12px;color:gray'>%1</td><td>%2</td></tr>").arg(label.toHtmlEscaped(), value);
        }
    };
    html += QStringLiteral("<table>");
    QStringList mails;
    const QJsonArray emails = o.value(QLatin1String("emailAddresses")).toArray();
    for (const QJsonValue &e : emails) {
        mails.append(e.toObject().value(QLatin1String("address")).toString());
    }
    if (mails.isEmpty() && !mailOf(o).isEmpty()) {
        mails.append(mailOf(o));
    }
    for (const QString &m : std::as_const(mails)) {
        line(i18nc("@label contact card", "E-mail"), QStringLiteral("<a href='mailto:%1'>%1</a>").arg(m.toHtmlEscaped()));
    }
    const QJsonArray business = o.value(QLatin1String("businessPhones")).toArray();
    for (const QJsonValue &b : business) {
        line(i18nc("@label contact card", "Business"), b.toString().toHtmlEscaped());
    }
    line(i18nc("@label contact card", "Mobile"), text(o, "mobilePhone").toHtmlEscaped());
    const QJsonArray home = o.value(QLatin1String("homePhones")).toArray();
    for (const QJsonValue &h : home) {
        line(i18nc("@label contact card", "Home"), h.toString().toHtmlEscaped());
    }
    const QJsonObject a = o.value(QLatin1String("businessAddress")).toObject();
    static const QRegularExpression nonBlank(QStringLiteral("\\S"));
    const QString address = QStringList{text(a, "street"), QStringList{text(a, "postalCode"), text(a, "city")}.join(QLatin1Char(' ')).trimmed(), text(a, "countryOrRegion")}
                                .filter(nonBlank)
                                .join(QStringLiteral("<br>"));
    line(i18nc("@label contact card", "Address"), address);
    const QDate born = QDate::fromString(text(o, "birthday").left(10), Qt::ISODate);
    if (born.isValid()) {
        line(i18nc("@label contact card", "Birthday"), QLocale().toString(born, QStringLiteral("d MMMM yyyy")));
    }
    html += QStringLiteral("</table>");
    if (!text(o, "personalNotes").isEmpty()) {
        html += QStringLiteral("<p>") + text(o, "personalNotes").toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")) + QStringLiteral("</p>");
    }
    if (p.id == 0) {
        html += QStringLiteral("<p style='color:gray'>") + i18nc("@info contact card", "From your organisation's directory").toHtmlEscaped() + QStringLiteral("</p>");
    }
    QStringList categories;
    const QJsonArray categoryArray = o.value(QLatin1String("categories")).toArray();
    for (const QJsonValue &v : categoryArray) {
        categories.append(v.toString());
    }
    if (!categories.isEmpty()) {
        html += QStringLiteral("<p>") + (m_categoryList ? m_categoryList->labelsHtml(p.mailboxKey, categories) : categories.join(QStringLiteral(", ")).toHtmlEscaped())
            + QStringLiteral("</p>");
    }
    m_card->setText(html);
    m_photo->setPixmap(avatarPixmap(text(o, "displayName"), mailOf(o), 96));
    loadPhoto(p);
    loadOrgChart(p, html);
}

void ContactsWindow::loadOrgChart(const Person &p, const QString &html)
{
    // Outlook's Organization: the manager and direct reports from the directory (needs User.Read.All,
    // which the daemon asks for separately; without it the card stays as it is)
    const QString user = p.id == 0 && !text(p.json, "id").isEmpty() ? text(p.json, "id") : mailOf(p.json);
    QString account;
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        if (box.own) {
            account = box.account;
            break;
        }
    }
    if (user.isEmpty() || account.isEmpty()) {
        return;
    }
    const QString shownMail = mailOf(p.json);
    m_daemon->orgChart(account, user, this, [this, html, shownMail](const QString &json) {
        const std::optional<Person> now = current();
        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
        if (!now || mailOf(now->json) != shownMail || !answer.value(QLatin1String("ok")).toBool()) {
            return;
        }
        const auto link = [](const QJsonObject &person) {
            const QString address = text(person, "mail").isEmpty() ? text(person, "userPrincipalName") : text(person, "mail");
            QString shown = QStringLiteral("<a href=\"person:%1\">%2</a>").arg(address.toHtmlEscaped(), text(person, "displayName").toHtmlEscaped());
            if (!text(person, "jobTitle").isEmpty()) {
                shown += QStringLiteral(" <span style='color:gray'>(%1)</span>").arg(text(person, "jobTitle").toHtmlEscaped());
            }
            return shown;
        };
        QString extra;
        if (const QJsonObject manager = answer.value(QLatin1String("manager")).toObject(); !manager.isEmpty()) {
            extra += QStringLiteral("<p><span style='color:gray'>%1</span> %2</p>").arg(i18nc("@info contact card", "Manager:").toHtmlEscaped(), link(manager));
        }
        QStringList reports;
        const QJsonArray list = answer.value(QLatin1String("reports")).toArray();
        for (const QJsonValue &v : list) {
            reports.append(link(v.toObject()));
        }
        if (!reports.isEmpty()) {
            extra += QStringLiteral("<p><span style='color:gray'>%1</span><br>%2</p>").arg(i18nc("@info contact card", "Direct reports:").toHtmlEscaped(), reports.join(QStringLiteral("<br>")));
        }
        if (!extra.isEmpty()) {
            m_card->setText(html + extra);
        }
    });
}

void ContactsWindow::loadPhoto(const Person &p)
{
    const QString id = text(p.json, "id");
    if (id.isEmpty() || id.startsWith(QLatin1String("local:"))) {
        return;
    }
    if (m_photos.contains(id)) {
        if (!m_photos.value(id).isNull()) {
            m_photo->setPixmap(m_photos.value(id).scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        return;
    }
    const QString key = p.mailboxKey.isEmpty() ? QString() : p.mailboxKey;
    QString account = key.section(QLatin1Char('/'), 0, 0);
    QString mailbox = key.section(QLatin1Char('/'), 1);
    if (account.isEmpty()) {
        const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
        for (const MailboxInfo &box : boxes) {
            if (box.own) {
                account = box.account;
                mailbox = box.mailbox;
                break;
            }
        }
    }
    const QJsonObject args = p.id != 0 ? QJsonObject{{QStringLiteral("contact"), id}} : QJsonObject{{QStringLiteral("user"), id}};
    m_daemon->query(account, mailbox, QStringLiteral("photo"), QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this, [this, id](const QString &base64) {
        QPixmap photo;
        if (!base64.isEmpty()) {
            photo.loadFromData(QByteArray::fromBase64(base64.toLatin1()));
        }
        // Remembered also when there is none: no question again for this person
        m_photos.insert(id, photo);
        if (!photo.isNull() && current() && text(current()->json, "id") == id) {
            m_photo->setPixmap(photo.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    });
}

QPair<QString, qint64> ContactsWindow::targetFolder() const
{
    if (QTreeWidgetItem *item = m_sources->currentItem(); item && item->data(0, Qt::UserRole).toString().contains(QLatin1Char('|'))) {
        const QString key = item->data(0, Qt::UserRole).toString();
        return {key.section(QLatin1Char('|'), 0, 0), key.section(QLatin1Char('|'), 1).toLongLong()};
    }
    // The default folder of the first own mailbox
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        const Store *store = box.own ? m_mailboxes->store(box.key()) : nullptr;
        const QList<ParentRow> folders = store ? store->parents(ItemKind::Contact) : QList<ParentRow>();
        for (const ParentRow &f : folders) {
            if (f.graphId == QLatin1String("contacts")) {
                return {box.key(), f.id};
            }
        }
    }
    return {};
}

void ContactsWindow::newContact()
{
    const auto [mailboxKey, folder] = targetFolder();
    if (mailboxKey.isEmpty()) {
        return;
    }
    ContactDialog dialog({}, this);
    if (dialog.exec() == QDialog::Accepted) {
        m_daemon->createItem(mailboxKey.section(QLatin1Char('/'), 0, 0), mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("contact"), folder,
                             QString::fromUtf8(QJsonDocument(dialog.json()).toJson(QJsonDocument::Compact)));
    }
}

void ContactsWindow::editContact(const Person &p)
{
    ContactDialog dialog(p.json, this);
    if (dialog.exec() == QDialog::Accepted) {
        const QJsonObject diff = dialog.changes();
        if (!diff.isEmpty()) {
            m_daemon->updateItem(p.mailboxKey.section(QLatin1Char('/'), 0, 0), p.mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("contact"), p.id,
                                 QString::fromUtf8(QJsonDocument(diff).toJson(QJsonDocument::Compact)));
        }
    }
}

void ContactsWindow::deleteContact(const Person &p)
{
    if (KMessageBox::warningContinueCancel(this, i18nc("@info", "Delete the contact \"%1\"?", text(p.json, "displayName")), i18nc("@title:window", "Delete Contact"),
                                           KStandardGuiItem::del())
        == KMessageBox::Continue) {
        m_daemon->deleteItem(p.mailboxKey.section(QLatin1Char('/'), 0, 0), p.mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("contact"), p.id);
    }
}

void ContactsWindow::newMessage(const Person &p)
{
    ComposeRequest request;
    // A shared mailbox's contact writes from that mailbox, as in Outlook
    request.mailboxKey = p.mailboxKey;
    if (request.mailboxKey.isEmpty()) {
        request.mailboxKey = targetFolder().first;
    }
    request.to = {{text(p.json, "displayName"), mailOf(p.json)}};
    (new ComposerWindow(m_mailboxes, m_daemon, request))->show();
}

QWidget *ContactsWindow::forwardAsVCard()
{
    const std::optional<Person> p = current();
    if (!p) {
        return nullptr;
    }
    // In the app's cache, not /tmp: the daemon reads the attachment when the draft is saved
    if (!m_vcards) {
        const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir().mkpath(cache);
        m_vcards = std::make_unique<QTemporaryDir>(cache + QStringLiteral("/vcard-XXXXXX"));
    }
    // A folder per forward: the file keeps the person's name, as the receiver sees it
    const QString name = text(p->json, "displayName").isEmpty() ? mailOf(p->json) : text(p->json, "displayName");
    const QString dir = m_vcards->path() + QLatin1Char('/') + QUuid::createUuid().toString(QUuid::Id128);
    QFile file(dir + QLatin1Char('/') + MimeExport::safeFileName(name) + QStringLiteral(".vcf"));
    const QByteArray card = VCard::fromContact(p->json);
    if (!m_vcards->isValid() || !QDir().mkpath(dir) || !file.open(QIODevice::WriteOnly) || file.write(card) != card.size()) {
        KMessageBox::error(this, i18nc("@info", "Could not write the contact card to a file."));
        return nullptr;
    }
    file.close();
    ComposeRequest request;
    request.mailboxKey = p->mailboxKey.isEmpty() ? targetFolder().first : p->mailboxKey;
    auto *composer = new ComposerWindow(m_mailboxes, m_daemon, request);
    composer->prefill({}, {}, {}, name, QString(), {file.fileName()});
    composer->show();
    return composer;
}

void ContactsWindow::addToContacts(const Person &p)
{
    const auto [mailboxKey, folder] = targetFolder();
    const QString targetKey = mailboxKey.isEmpty() || m_directory ? QString() : mailboxKey;
    // From the directory: into the user's own default folder
    QPair<QString, qint64> target = {targetKey, folder};
    if (targetKey.isEmpty()) {
        const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
        for (const MailboxInfo &box : boxes) {
            const Store *store = box.own ? m_mailboxes->store(box.key()) : nullptr;
            const QList<ParentRow> folders = store ? store->parents(ItemKind::Contact) : QList<ParentRow>();
            for (const ParentRow &f : folders) {
                if (f.graphId == QLatin1String("contacts") && target.first.isEmpty()) {
                    target = {box.key(), f.id};
                }
            }
        }
    }
    if (target.first.isEmpty()) {
        return;
    }
    const QJsonObject &o = p.json;
    QJsonObject contact{{QStringLiteral("displayName"), text(o, "displayName")},
                        {QStringLiteral("givenName"), text(o, "givenName")},
                        {QStringLiteral("surname"), text(o, "surname")},
                        {QStringLiteral("jobTitle"), text(o, "jobTitle")},
                        {QStringLiteral("department"), text(o, "department")},
                        {QStringLiteral("companyName"), text(o, "companyName")},
                        {QStringLiteral("officeLocation"), text(o, "officeLocation")},
                        {QStringLiteral("businessPhones"), o.value(QLatin1String("businessPhones"))},
                        {QStringLiteral("mobilePhone"), text(o, "mobilePhone")},
                        {QStringLiteral("emailAddresses"), QJsonArray{QJsonObject{{QStringLiteral("address"), mailOf(o)}, {QStringLiteral("name"), text(o, "displayName")}}}}};
    m_daemon->createItem(target.first.section(QLatin1Char('/'), 0, 0), target.first.section(QLatin1Char('/'), 1), QStringLiteral("contact"), target.second,
                         QString::fromUtf8(QJsonDocument(contact).toJson(QJsonDocument::Compact)));
    statusBar()->showMessage(i18nc("@info:status", "Added %1 to your contacts", text(o, "displayName")), 5000);
}

QWidget *ContactsWindow::newContactForTests(const QString &mailboxKey, const QJsonObject &fields)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const QList<ParentRow> folders = store ? store->parents(ItemKind::Contact) : QList<ParentRow>();
    qint64 folder = 0;
    for (const ParentRow &f : folders) {
        if (f.graphId == QLatin1String("contacts")) {
            folder = f.id;
        }
    }
    auto *dialog = new ContactDialog({}, this);
    dialog->setFields(fields);
    dialog->show();
    connect(dialog, &QDialog::accepted, this, [this, dialog, mailboxKey, folder] {
        if (dialog->json().value(QLatin1String("displayName")).toString().contains(QLatin1String("[TEST Graph Mail]"))) {
            m_daemon->createItem(mailboxKey.section(QLatin1Char('/'), 0, 0), mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("contact"), folder,
                                 QString::fromUtf8(QJsonDocument(dialog->json()).toJson(QJsonDocument::Compact)));
        }
    });
    return dialog;
}

QWidget *ContactsWindow::editContactForTests(const QString &mailboxKey, qint64 contact, const QJsonObject &changes)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<StoredItem> item = store ? store->item(ItemKind::Contact, contact) : std::nullopt;
    const QJsonObject json = item ? QJsonDocument::fromJson(item->json.toUtf8()).object() : QJsonObject();
    if (!text(json, "displayName").contains(QLatin1String("[TEST Graph Mail]"))) {
        return nullptr;
    }
    auto *dialog = new ContactDialog(json, this);
    dialog->setFields(changes);
    dialog->show();
    connect(dialog, &QDialog::accepted, this, [this, dialog, mailboxKey, contact] {
        const QJsonObject diff = dialog->changes();
        qWarning().noquote() << "contact edit test, changed:" << diff.keys().join(QLatin1Char(','));
        if (!diff.isEmpty()) {
            m_daemon->updateItem(mailboxKey.section(QLatin1Char('/'), 0, 0), mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("contact"), contact,
                                 QString::fromUtf8(QJsonDocument(diff).toJson(QJsonDocument::Compact)));
        }
    });
    return dialog;
}

bool ContactsWindow::deleteContactForTests(const QString &mailboxKey, qint64 contact)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<StoredItem> item = store ? store->item(ItemKind::Contact, contact) : std::nullopt;
    if (!item || !item->json.contains(QLatin1String("[TEST Graph Mail]"))) {
        return false;
    }
    m_daemon->deleteItem(mailboxKey.section(QLatin1Char('/'), 0, 0), mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("contact"), contact);
    return true;
}

void ContactsWindow::searchDirectoryForTests(const QString &text)
{
    for (int i = 0; i < m_sources->topLevelItemCount(); ++i) {
        if (m_sources->topLevelItem(i)->data(0, Qt::UserRole).toString() == QLatin1String("directory")) {
            m_sources->setCurrentItem(m_sources->topLevelItem(i));
        }
    }
    m_filter->setText(text);
}

void ContactsWindow::addFromDirectoryForTests(const QString &address)
{
    searchDirectoryForTests(address.section(QLatin1Char('@'), 0, 0));
    auto tries = std::make_shared<int>(0);
    auto attempt = std::make_shared<std::function<void()>>();
    *attempt = [this, address, tries, attempt] {
        for (const Person &p : std::as_const(m_people)) {
            if (p.id == 0 && mailOf(p.json).compare(address, Qt::CaseInsensitive) == 0) {
                Person marked = p;
                marked.json.insert(QStringLiteral("displayName"), QString(text(p.json, "displayName") + QStringLiteral(" [TEST Graph Mail]")));
                addToContacts(marked);
                qWarning() << "add from directory test: true";
                return;
            }
        }
        if (++*tries < 25) {
            QTimer::singleShot(200, this, *attempt);
        } else {
            qWarning() << "add from directory test: not found";
        }
    };
    QTimer::singleShot(200, this, *attempt);
}

void ContactsWindow::setCategoryList(CategoryList *categories)
{
    m_categoryList = categories;
    connect(categories, &CategoryList::changed, this, [this] {
        if (const std::optional<Person> p = current()) {
            showPerson(*p);
        }
    });
}

void ContactsWindow::setContactCategory(const Person &person, const QString &name, bool on)
{
    QStringList categories;
    const QJsonArray array = person.json.value(QLatin1String("categories")).toArray();
    for (const QJsonValue &v : array) {
        categories.append(v.toString());
    }
    categories.removeAll(name);
    if (on) {
        categories.append(name);
    }
    m_daemon->updateItem(person.mailboxKey.section(QLatin1Char('/'), 0, 0), person.mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("contact"), person.id,
                         QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("categories"), QJsonArray::fromStringList(categories)}}).toJson(QJsonDocument::Compact)));
}

bool ContactsWindow::categorizeForTests(const QString &mailboxKey, qint64 contact, const QString &name, bool on)
{
    const Store *store = m_mailboxes->store(mailboxKey);
    const std::optional<StoredItem> item = store ? store->item(ItemKind::Contact, contact) : std::nullopt;
    if (!item || !item->json.contains(QLatin1String("[TEST Graph Mail]"))) {
        return false;
    }
    setContactCategory({mailboxKey, item->id, item->parent, QJsonDocument::fromJson(item->json.toUtf8()).object()}, name, on);
    return true;
}

void ContactsWindow::printList(const QString &pdfFile)
{
    const QString title = m_sources->currentItem() ? m_sources->currentItem()->text(0) : windowTitle();
    Printing::printHtml(this, title, Printing::treeHtml(m_list, title), pdfFile);
}

}
