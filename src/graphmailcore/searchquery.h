// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>

namespace GraphMail
{

// What the user typed in the search field, as Outlook understands it (docs/PHASE8.md):
// plain words search everything; from:, to:, cc:, subject:, body: search one field;
// category:, attachment:, hasattachments:yes, isread:no, isflagged:yes, importance:high and received>= / received<= /
// received: (a date, yyyy-mm-dd) filter. "Quoted words" stay together. The same query runs on
// the local cache (fts() plus the filters) and on Graph ($search with kql()).
struct SearchQuery {
    QStringList words;
    // "from", "to", "cc", "subject", "body": the words for that field
    QHash<QString, QStringList> fields;
    std::optional<bool> unread;
    std::optional<bool> flagged;
    std::optional<bool> attachments;
    // category:Name (the message has this category), attachment:name (an attachment's name)
    QStringList categories;
    QStringList attachmentNames;
    bool highImportance = false;
    // Local midnight in milliseconds; 0 = no limit. receivedTo is exclusive.
    qint64 receivedFrom = 0;
    qint64 receivedTo = 0;
    // The last word is still being typed: match it as a prefix locally
    bool lastIsPrefix = false;

    static SearchQuery parse(const QString &text);
    bool isEmpty() const;
    // An FTS5 MATCH expression for the words, empty when there are only filters
    QString fts() const;
    // Graph's $search (KQL). isread and isflagged are not searchable there: the caller filters
    // Graph's results on the cache's read and flag state.
    QString kql() const;
};

// The text of a body for the search index: tags, styles and scripts out, entities decoded,
// white space collapsed, at most maxChars characters
QString bodyText(const QByteArray &content, bool html, int maxChars = 32 * 1024);

}
