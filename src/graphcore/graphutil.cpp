// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "graphutil.h"

#include <QSet>
#include <QStringList>

namespace GraphCore
{

namespace Prefer
{
QByteArray immutableId()
{
    return QByteArrayLiteral("IdType=\"ImmutableId\"");
}

QByteArray timeZone(const QTimeZone &zone)
{
    QByteArray windowsId = QTimeZone::ianaIdToWindowsId(zone.id());
    if (windowsId.isEmpty()) {
        windowsId = QByteArrayLiteral("UTC");
    }
    return QByteArrayLiteral("outlook.timezone=\"") + windowsId + '"';
}

QByteArray textBody()
{
    return QByteArrayLiteral("outlook.body-content-type=\"text\"");
}

QByteArray htmlBody()
{
    return QByteArrayLiteral("outlook.body-content-type=\"html\"");
}

QByteArray maxPageSize(int items)
{
    return QByteArrayLiteral("odata.maxpagesize=") + QByteArray::number(items);
}

QPair<QByteArray, QByteArray> header(const QList<QByteArray> &preferences)
{
    return {QByteArrayLiteral("Prefer"), preferences.join(", ")};
}
}

QString mailboxRoot(const QString &mailbox)
{
    if (mailbox.isEmpty()) {
        return QStringLiteral("/me");
    }
    return QStringLiteral("/users/") + QString::fromLatin1(QUrl::toPercentEncoding(mailbox, "@"));
}

QString throttleKeyFor(const QString &scope, const QUrl &url, const QString &graphHost)
{
    if (url.host() != graphHost) {
        return scope;
    }
    QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!parts.isEmpty() && (parts.first() == QLatin1String("v1.0") || parts.first() == QLatin1String("beta"))) {
        parts.removeFirst();
    }

    QString mailbox;
    qsizetype resource = 0;
    if (parts.value(0) == QLatin1String("me")) {
        mailbox = QStringLiteral("me");
        resource = 1;
    } else if (parts.value(0) == QLatin1String("users") && parts.size() > 1) {
        mailbox = parts.at(1).toLower();
        resource = 2;
    } else {
        return scope;
    }

    // Resources served by Exchange Online; anything else under /me (drive, ...) is not. The People
    // API and photos count against the mailbox too ("over its MailboxConcurrency limit" on /me/people)
    static const QSet<QString> outlook{
        QStringLiteral("messages"),      QStringLiteral("mailFolders"),    QStringLiteral("sendMail"),
        QStringLiteral("events"),        QStringLiteral("calendar"),       QStringLiteral("calendars"),
        QStringLiteral("calendarView"),  QStringLiteral("calendarGroups"), QStringLiteral("findMeetingTimes"),
        QStringLiteral("reminderView"),  QStringLiteral("contacts"),       QStringLiteral("contactFolders"),
        QStringLiteral("mailboxSettings"), QStringLiteral("outlook"),      QStringLiteral("todo"),
        QStringLiteral("inferenceClassification"), QStringLiteral("getMailTips"), QStringLiteral("people"),
        QStringLiteral("photo"),         QStringLiteral("photos"),
    };
    // OData key syntax: mailFolders('inbox')
    const QString segment = parts.value(resource).section(QLatin1Char('('), 0, 0);
    return outlook.contains(segment) ? scope + QLatin1Char('|') + mailbox : scope;
}

bool isMailboxKey(const QString &key)
{
    return key.contains(QLatin1Char('|'));
}

}
