// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <KXmlGuiWindow>

#include <QHash>
#include <QJsonObject>
#include <QPixmap>
#include <QTimer>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace GraphMail
{

class CategoryList;
class DaemonClient;
class MailboxSet;

// People (docs/PHASE6.md): the contact folders of every mailbox and the organisation's directory
// on the left, the people with a filter in the middle, the selected one's card on the right.
class ContactsWindow : public KXmlGuiWindow
{
    Q_OBJECT

public:
    // File > Print: the list shown (docs/PHASE9B.md); tests: into a PDF
    void printList(const QString &pdfFile = {});
    ContactsWindow(MailboxSet *mailboxes, DaemonClient *daemon, QWidget *parent = nullptr);

    // Tests: a new contact in the default folder of this mailbox, saved; the dialog
    QWidget *newContactForTests(const QString &mailboxKey, const QJsonObject &fields);
    // Tests: change or delete a contact (only names marked [TEST Graph Mail])
    QWidget *editContactForTests(const QString &mailboxKey, qint64 contact, const QJsonObject &changes);
    bool deleteContactForTests(const QString &mailboxKey, qint64 contact);
    // Tests: search the directory and select the first person
    void searchDirectoryForTests(const QString &text);
    // Categories (docs/PHASE9.md): on the card, Categorize for own contacts
    void setCategoryList(CategoryList *categories);
    bool categorizeForTests(const QString &mailboxKey, qint64 contact, const QString &name, bool on);
    // Tests: search the directory for this address and Add to Contacts, the name marked
    // [TEST Graph Mail]; logs the outcome
    void addFromDirectoryForTests(const QString &address);

Q_SIGNALS:
    void navigate(int module);

private:
    struct Person {
        // A contact of a mailbox (row id) or a directory entry (id 0)
        QString mailboxKey;
        qint64 id = 0;
        qint64 folder = 0;
        QJsonObject json;
    };

    void setupActions();
    void fillSources();
    void sourceChanged();
    void fillList();
    void showPerson(const Person &person);
    void searchDirectory();
    void loadPhoto(const Person &person);
    std::optional<Person> current() const;
    // The folder new contacts go to: the selected one, else the user's default
    QPair<QString, qint64> targetFolder() const;
    void newContact();
    void editContact(const Person &person);
    void deleteContact(const Person &person);
    void newMessage(const Person &person);
    void addToContacts(const Person &person);
    void setContactCategory(const Person &person, const QString &name, bool on);

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
    QTreeWidget *m_sources;
    QLineEdit *m_filter;
    QTreeWidget *m_list;
    QLabel *m_photo;
    QLabel *m_card;
    QPushButton *m_message;
    QPushButton *m_edit;
    QPushButton *m_delete;
    QPushButton *m_add;
    QPushButton *m_categorize = nullptr;
    CategoryList *m_categoryList = nullptr;
    QList<Person> m_people;
    QHash<QString, QPixmap> m_photos;
    QTimer m_searchTimer;
    QTimer m_reloadTimer;
    bool m_directory = false;
};

}
