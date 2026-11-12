// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "recipientedit.h"
#include "mailboxset.h"
#include "mailboxworker.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QSet>
#include <QStringListModel>

namespace GraphMail
{

AddressBook::AddressBook(MailboxSet *mailboxes, QObject *parent)
    : QObject(parent)
    , m_model(new QStringListModel(this))
{
    QStringList entries;
    QSet<QString> seen;
    auto add = [&](const OutgoingRecipient &r) {
        const QString key = r.address.trimmed().toLower();
        if (key.isEmpty() || !key.contains(QLatin1Char('@')) || seen.contains(key)) {
            return;
        }
        seen.insert(key);
        entries.append(formatRecipient({r.name.trimmed(), r.address.trimmed()}));
    };
    const QList<MailboxInfo> boxes = mailboxes->mailboxes();
    QSet<QString> accounts;
    for (const MailboxInfo &box : boxes) {
        accounts.insert(box.account);
    }
    // The ranking of /me/people first: those are the people the user mails most
    for (const QString &account : std::as_const(accounts)) {
        QFile file(peopleFile(account));
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonArray people = QJsonDocument::fromJson(file.readAll()).array();
            for (const QJsonValue &p : people) {
                add({p.toObject().value(QLatin1String("name")).toString(), p.toObject().value(QLatin1String("address")).toString()});
            }
        }
    }
    for (const MailboxInfo &box : boxes) {
        if (const Store *store = mailboxes->store(box.key())) {
            const QList<OutgoingRecipient> known = store->knownAddresses(1000);
            for (const OutgoingRecipient &r : known) {
                add(r);
            }
        }
    }
    m_model->setStringList(entries);
}

QStringListModel *AddressBook::model() const
{
    return m_model;
}

RecipientEdit::RecipientEdit(AddressBook *book, QWidget *parent)
    : QLineEdit(parent)
    , m_completer(new QCompleter(book->model(), this))
{
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setFilterMode(Qt::MatchContains);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setMaxVisibleItems(10);
    m_completer->setWidget(this);
    connect(m_completer, qOverload<const QString &>(&QCompleter::activated), this, &RecipientEdit::insertCompletion);
    connect(this, &QLineEdit::textEdited, this, &RecipientEdit::complete);
}

int RecipientEdit::tokenStart() const
{
    const QString before = text().left(cursorPosition());
    const qsizetype separator = std::max(before.lastIndexOf(QLatin1Char(';')), before.lastIndexOf(QLatin1Char(',')));
    return int(separator + 1);
}

void RecipientEdit::complete()
{
    const QString token = text().mid(tokenStart(), cursorPosition() - tokenStart()).trimmed();
    if (token.size() < 2) {
        m_completer->popup()->hide();
        return;
    }
    m_completer->setCompletionPrefix(token);
    if (m_completer->completionCount() == 0) {
        m_completer->popup()->hide();
        return;
    }
    m_completer->complete();
}

void RecipientEdit::insertCompletion(const QString &completion)
{
    const int start = tokenStart();
    const QString after = text().mid(cursorPosition());
    QString head = text().left(start);
    if (!head.isEmpty() && !head.endsWith(QLatin1Char(' '))) {
        head += QLatin1Char(' ');
    }
    const QString inserted = head + completion + QStringLiteral("; ");
    setText(inserted + after.trimmed());
    setCursorPosition(int(inserted.size()));
}

void RecipientEdit::keyPressEvent(QKeyEvent *event)
{
    // The popup takes Enter, Tab and Escape while it is open
    if (m_completer->popup()->isVisible()) {
        switch (event->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Escape:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            event->ignore();
            return;
        default:
            break;
        }
    }
    QLineEdit::keyPressEvent(event);
}

QList<OutgoingRecipient> RecipientEdit::recipients(QStringList *invalid) const
{
    return parseRecipients(text(), invalid);
}

void RecipientEdit::setRecipients(const QList<OutgoingRecipient> &recipients)
{
    QStringList parts;
    for (const OutgoingRecipient &r : recipients) {
        parts.append(formatRecipient(r));
    }
    setText(parts.isEmpty() ? QString() : parts.join(QStringLiteral("; ")) + QStringLiteral("; "));
}

}
