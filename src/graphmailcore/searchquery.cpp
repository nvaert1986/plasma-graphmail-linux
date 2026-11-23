// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "searchquery.h"

#include <QDate>
#include <QDateTime>
#include <QRegularExpression>

namespace GraphMail
{

namespace
{
// Words, "quoted phrases" and key:value / key>=value / key<=value, values may be quoted too
QStringList tokens(const QString &text)
{
    static const QRegularExpression token(QStringLiteral(R"re((\S*?(?::|>=|<=)"[^"]*"?|"[^"]*"?|\S+))re"));
    QStringList result;
    auto it = token.globalMatch(text);
    while (it.hasNext()) {
        result.append(it.next().captured(1));
    }
    return result;
}

QString unquoted(QString value)
{
    if (value.startsWith(QLatin1Char('"'))) {
        value.remove(0, 1);
    }
    if (value.endsWith(QLatin1Char('"'))) {
        value.chop(1);
    }
    return value.trimmed();
}

std::optional<bool> yesNo(const QString &value)
{
    const QString v = value.toLower();
    // "hasattachments:" alone (still typing, or as Outlook's filter writes it) means yes
    if (v.isEmpty()) {
        return true;
    }
    if (v == QLatin1String("yes") || v == QLatin1String("true") || v == QLatin1String("1")) {
        return true;
    }
    if (v == QLatin1String("no") || v == QLatin1String("false") || v == QLatin1String("0")) {
        return false;
    }
    return std::nullopt;
}

qint64 midnight(QDate date)
{
    return QDateTime(date, QTime(0, 0)).toMSecsSinceEpoch();
}

bool hasText(const QString &word)
{
    return std::any_of(word.cbegin(), word.cend(), [](QChar c) {
        return c.isLetterOrNumber();
    });
}

// A word or phrase as an FTS5 string: quoted, so operators the user typed are just text
QString ftsString(const QString &word, bool prefix)
{
    QString quoted = word;
    return QLatin1Char('"') + quoted.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"') + (prefix ? QStringLiteral("*") : QString());
}

// The fields in a fixed order: the same text always gives the same query
QStringList fieldOrder()
{
    return {QStringLiteral("from"), QStringLiteral("to"), QStringLiteral("cc"), QStringLiteral("subject"), QStringLiteral("body")};
}

QString kqlValue(const QString &value)
{
    return value.contains(QLatin1Char(' ')) ? QLatin1Char('"') + value + QLatin1Char('"') : value;
}
}

SearchQuery SearchQuery::parse(const QString &text)
{
    SearchQuery q;
    static const QStringList textFields{QStringLiteral("from"), QStringLiteral("to"), QStringLiteral("cc"), QStringLiteral("subject"), QStringLiteral("body")};
    static const QRegularExpression separator(QStringLiteral(":|>=|<="));
    const QStringList all = tokens(text);
    for (const QString &token : all) {
        const qsizetype at = token.indexOf(separator);
        const QString key = at > 0 ? token.left(at).toLower() : QString();
        const QString op = at > 0 ? (token.mid(at, 2) == QLatin1String(">=") || token.mid(at, 2) == QLatin1String("<=") ? token.mid(at, 2) : QStringLiteral(":")) : QString();
        const QString value = at > 0 ? unquoted(token.mid(at + op.size())) : QString();
        const bool quoted = at > 0 ? token.mid(at + op.size()).startsWith(QLatin1Char('"')) : token.startsWith(QLatin1Char('"'));
        if (textFields.contains(key) && op == QLatin1String(":") && hasText(value)) {
            q.fields[key].append(value);
            if (quoted) {
                q.exact.append(value);
            }
        } else if ((key == QLatin1String("hasattachments") || key == QLatin1String("hasattachment")) && yesNo(value)) {
            q.attachments = yesNo(value);
        } else if ((key == QLatin1String("category") || key == QLatin1String("categories")) && op == QLatin1String(":") && !value.isEmpty()) {
            q.categories.append(value);
        } else if ((key == QLatin1String("attachment") || key == QLatin1String("attachments")) && op == QLatin1String(":") && hasText(value)) {
            q.attachmentNames.append(value);
        } else if (key == QLatin1String("isread") && yesNo(value)) {
            q.unread = !*yesNo(value);
        } else if (key == QLatin1String("isflagged") && yesNo(value)) {
            q.flagged = yesNo(value);
        } else if (key == QLatin1String("importance") && value.compare(QLatin1String("high"), Qt::CaseInsensitive) == 0) {
            q.highImportance = true;
        } else if (key == QLatin1String("received") && QDate::fromString(value, Qt::ISODate).isValid()) {
            const QDate day = QDate::fromString(value, Qt::ISODate);
            if (op == QLatin1String(">=")) {
                q.receivedFrom = midnight(day);
            } else if (op == QLatin1String("<=")) {
                q.receivedTo = midnight(day.addDays(1));
            } else {
                q.receivedFrom = midnight(day);
                q.receivedTo = midnight(day.addDays(1));
            }
        } else if (hasText(unquoted(token))) {
            // Unknown keys ("re:", "http://...") are words like any other
            q.words.append(unquoted(token));
            if (quoted) {
                q.exact.append(unquoted(token));
            }
        }
    }
    return q;
}

bool SearchQuery::isEmpty() const
{
    return words.isEmpty() && fields.isEmpty() && !unread && !flagged && !attachments && categories.isEmpty() && attachmentNames.isEmpty() && !highImportance
        && receivedFrom == 0 && receivedTo == 0;
}

QString SearchQuery::fts() const
{
    // The FTS columns of each field (schema 4)
    static const QHash<QString, QString> columns{{QStringLiteral("from"), QStringLiteral("{sender_name sender_address}")},
                                                 {QStringLiteral("to"), QStringLiteral("{to_list}")},
                                                 {QStringLiteral("cc"), QStringLiteral("{cc_list}")},
                                                 {QStringLiteral("subject"), QStringLiteral("{subject}")},
                                                 {QStringLiteral("body"), QStringLiteral("{body_text}")}};
    QStringList terms;
    // Which term is the one being typed: the last word, or the last value of the last field
    for (qsizetype i = 0; i < words.size(); ++i) {
        terms.append(ftsString(words.at(i), !exact.contains(words.at(i))));
    }
    const QStringList order = fieldOrder();
    for (const QString &field : order) {
        const QStringList values = fields.value(field);
        for (qsizetype i = 0; i < values.size(); ++i) {
            terms.append(columns.value(field) + QStringLiteral(" : ") + ftsString(values.at(i), !exact.contains(values.at(i))));
        }
    }
    return terms.join(QLatin1Char(' '));
}

QString SearchQuery::kql() const
{
    QStringList parts;
    for (const QString &w : words) {
        parts.append(kqlValue(w));
    }
    const QStringList order = fieldOrder();
    for (const QString &field : order) {
        const QStringList values = fields.value(field);
        for (const QString &v : values) {
            parts.append(field + QLatin1Char(':') + kqlValue(v));
        }
    }
    if (attachments) {
        parts.append(*attachments ? QStringLiteral("hasattachments:true") : QStringLiteral("hasattachments:false"));
    }
    for (const QString &c : categories) {
        parts.append(QStringLiteral("category:") + kqlValue(c));
    }
    for (const QString &a : attachmentNames) {
        parts.append(QStringLiteral("attachment:") + kqlValue(a));
    }
    if (highImportance) {
        parts.append(QStringLiteral("importance:high"));
    }
    if (receivedFrom > 0) {
        parts.append(QStringLiteral("received>=") + QDateTime::fromMSecsSinceEpoch(receivedFrom).date().toString(Qt::ISODate));
    }
    if (receivedTo > 0) {
        parts.append(QStringLiteral("received<=") + QDateTime::fromMSecsSinceEpoch(receivedTo).date().addDays(-1).toString(Qt::ISODate));
    }
    return parts.join(QStringLiteral(" AND "));
}

QString bodyText(const QByteArray &content, bool html, int maxChars)
{
    QString text = QString::fromUtf8(content);
    if (html) {
        static const QRegularExpression invisible(QStringLiteral("<(head|style|script)\\b.*?</\\1\\s*>"),
                                                  QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression comment(QStringLiteral("<!--.*?-->"), QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression tag(QStringLiteral("<[^>]*>"));
        static const QRegularExpression numeric(QStringLiteral("&#(x?)([0-9a-fA-F]+);"));
        text.remove(invisible);
        text.remove(comment);
        text.replace(tag, QStringLiteral(" "));
        // Numeric entities, then the named ones mail uses
        QString decoded;
        decoded.reserve(text.size());
        qsizetype last = 0;
        auto it = numeric.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            decoded += QStringView(text).mid(last, m.capturedStart() - last);
            bool ok = false;
            const uint code = m.captured(2).toUInt(&ok, m.captured(1).isEmpty() ? 10 : 16);
            const char32_t character = ok && code > 0 && code <= 0x10FFFF ? char32_t(code) : U' ';
            decoded += QString::fromUcs4(&character, 1);
            last = m.capturedEnd();
        }
        decoded += QStringView(text).mid(last);
        text = decoded;
        // Named entities: the Latin-1 ones (160-255, in code point order) and what mail uses beyond
        static const char *const latin1[] = {"nbsp",   "iexcl", "cent",  "pound", "curren", "yen",    "brvbar", "sect",   "uml",    "copy",  "ordf",   "laquo",
                                             "not",    "shy",   "reg",   "macr",  "deg",    "plusmn", "sup2",   "sup3",   "acute",  "micro", "para",   "middot",
                                             "cedil",  "sup1",  "ordm",  "raquo", "frac14", "frac12", "frac34", "iquest", "Agrave", "Aacute", "Acirc", "Atilde",
                                             "Auml",   "Aring", "AElig", "Ccedil", "Egrave", "Eacute", "Ecirc", "Euml",   "Igrave", "Iacute", "Icirc", "Iuml",
                                             "ETH",    "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde", "Ouml",  "times",  "Oslash", "Ugrave", "Uacute", "Ucirc",
                                             "Uuml",   "Yacute", "THORN", "szlig", "agrave", "aacute", "acirc", "atilde", "auml",   "aring",  "aelig", "ccedil",
                                             "egrave", "eacute", "ecirc", "euml",  "igrave", "iacute", "icirc", "iuml",   "eth",    "ntilde", "ograve", "oacute",
                                             "ocirc",  "otilde", "ouml",  "divide", "oslash", "ugrave", "uacute", "ucirc", "uuml",   "yacute", "thorn", "yuml"};
        static const QHash<QString, char32_t> entities = [] {
            QHash<QString, char32_t> map;
            for (int i = 0; i < 96; ++i) {
                map.insert(QString::fromLatin1(latin1[i]), char32_t(160 + i));
            }
            const std::pair<const char *, char32_t> more[] = {{"lt", U'<'},          {"gt", U'>'},         {"quot", U'"'},         {"apos", U'\''},
                                                             {"amp", U'&'},          {"euro", U'\u20AC'}, {"ndash", U'\u2013'},  {"mdash", U'\u2014'},
                                                             {"lsquo", U'\u2018'}, {"rsquo", U'\u2019'}, {"ldquo", U'\u201C'}, {"rdquo", U'\u201D'},
                                                             {"bull", U'\u2022'},  {"hellip", U'\u2026'}, {"trade", U'\u2122'}, {"zwnj", U' '}};
            for (const auto &[name, character] : more) {
                map.insert(QString::fromLatin1(name), character);
            }
            return map;
        }();
        static const QRegularExpression entity(QStringLiteral("&([A-Za-z][A-Za-z0-9]*);"));
        QString named;
        named.reserve(text.size());
        last = 0;
        auto names = entity.globalMatch(text);
        while (names.hasNext()) {
            const QRegularExpressionMatch m = names.next();
            const auto found = entities.constFind(m.captured(1));
            named += QStringView(text).mid(last, m.capturedStart() - last);
            if (found == entities.cend()) {
                named += m.captured(0);
            } else {
                const char32_t character = *found;
                named += QString::fromUcs4(&character, 1);
            }
            last = m.capturedEnd();
        }
        named += QStringView(text).mid(last);
        text = named;
    }
    text = text.simplified();
    if (text.size() > maxChars) {
        text.truncate(maxChars);
    }
    return text;
}

}
