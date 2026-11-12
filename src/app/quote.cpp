// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "quote.h"
#include "blobstore.h"

#include <KLocalizedString>

#include <QDateTime>
#include <QLocale>
#include <QRegularExpression>
#include <QTextDocumentFragment>

namespace GraphMail
{

namespace
{
QString when(const MessageRow &m)
{
    const qint64 ms = m.sent > 0 ? m.sent : m.received;
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(ms).toLocalTime(), QStringLiteral("dddd d MMMM yyyy HH:mm"));
}

QString sender(const MessageRow &m)
{
    return formatRecipient({m.senderName, m.senderAddress});
}

std::optional<QString> body(const MessageRow &m, BlobStore *blobs)
{
    if (!blobs || m.bodyBlob.isEmpty()) {
        return std::nullopt;
    }
    QString error;
    const std::optional<QByteArray> data = blobs->get(m.bodyBlob, &error);
    return data ? std::optional<QString>(QString::fromUtf8(*data)) : std::nullopt;
}
}

bool Quote::mentionsAttachment(const QString &text)
{
    static const QRegularExpression words(QStringLiteral("\\b(attach(ed|ment|ments|ing)?|enclosed|bijlage[n]?|bijgevoegd|bijgesloten|"
                                                         "anhang|angeh(ä|ae)ngt|anlage|pi(è|e)ces? jointes?|ci-joint)\\b"),
                                          QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    return words.match(text).hasMatch();
}

QString Quote::subject(const QString &kind, const QString &original)
{
    static const QRegularExpression replied(QStringLiteral(R"(^\s*(re|aw|antw|sv|vs)\s*:)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression forwarded(QStringLiteral(R"(^\s*(fw|fwd|wg|doorst)\s*:)"), QRegularExpression::CaseInsensitiveOption);
    if (kind == QLatin1String("forward")) {
        return forwarded.match(original).hasMatch() ? original : QStringLiteral("FW: ") + original;
    }
    return replied.match(original).hasMatch() ? original : QStringLiteral("RE: ") + original;
}

QString Quote::headerHtml(const MessageRow &m)
{
    auto line = [](const QString &label, const QString &value) -> QString {
        return QStringLiteral("<b>") + label.toHtmlEscaped() + QStringLiteral("</b> ") + value.toHtmlEscaped() + QStringLiteral("<br>");
    };
    QString html = QStringLiteral("<hr style=\"display:inline-block;width:98%\" tabindex=\"-1\">"
                                  "<div id=\"divRplyFwdMsg\" dir=\"ltr\"><span style=\"font-family:Aptos,Calibri,Carlito,sans-serif;font-size:11pt;color:#000000\">");
    html += line(i18nc("@label quoted message header", "From:"), sender(m));
    html += line(i18nc("@label quoted message header", "Sent:"), when(m));
    html += line(i18nc("@label quoted message header", "To:"), m.toList);
    if (!m.ccList.isEmpty()) {
        html += line(i18nc("@label quoted message header", "Cc:"), m.ccList);
    }
    html += line(i18nc("@label quoted message header", "Subject:"), m.subject);
    html += QStringLiteral("</span><div>&nbsp;</div></div>");
    return html;
}

QString Quote::headerText(const MessageRow &m)
{
    QString text = QStringLiteral("________________________________\n");
    text += i18nc("@label quoted message header", "From:") + QLatin1Char(' ') + sender(m) + QLatin1Char('\n');
    text += i18nc("@label quoted message header", "Sent:") + QLatin1Char(' ') + when(m) + QLatin1Char('\n');
    text += i18nc("@label quoted message header", "To:") + QLatin1Char(' ') + m.toList + QLatin1Char('\n');
    if (!m.ccList.isEmpty()) {
        text += i18nc("@label quoted message header", "Cc:") + QLatin1Char(' ') + m.ccList + QLatin1Char('\n');
    }
    text += i18nc("@label quoted message header", "Subject:") + QLatin1Char(' ') + m.subject + QStringLiteral("\n\n");
    return text;
}

QString Quote::originalHtml(const MessageRow &m, BlobStore *blobs)
{
    const std::optional<QString> content = body(m, blobs);
    if (!content) {
        return {};
    }
    if (m.bodyType != 1) {
        return QStringLiteral("<div style=\"white-space:pre-wrap;font-family:Aptos,Calibri,Carlito,sans-serif;font-size:11pt\">") + content->toHtmlEscaped()
            + QStringLiteral("</div>");
    }
    // Its styles stay with it; the rest of <head> and the <html>/<body> tags go
    static const QRegularExpression styles(QStringLiteral("<style[^>]*>.*?</style>"), QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression bodyPart(QStringLiteral("<body[^>]*>(.*)</body>"), QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    QString kept;
    QRegularExpressionMatchIterator it = styles.globalMatch(*content);
    while (it.hasNext()) {
        kept += it.next().captured(0);
    }
    const QRegularExpressionMatch match = bodyPart.match(*content);
    return QStringLiteral("<div>") + kept + (match.hasMatch() ? match.captured(1) : *content) + QStringLiteral("</div>");
}

QString Quote::originalText(const MessageRow &m, BlobStore *blobs)
{
    const std::optional<QString> content = body(m, blobs);
    if (!content) {
        return {};
    }
    return m.bodyType == 1 ? QTextDocumentFragment::fromHtml(*content).toPlainText() : *content;
}

void Quote::recipients(const MessageRow &m, bool all, const QStringList &ownAddresses, QList<OutgoingRecipient> *to, QList<OutgoingRecipient> *cc)
{
    QStringList seen;
    for (const QString &own : ownAddresses) {
        seen.append(own.toLower());
    }
    auto add = [&seen](QList<OutgoingRecipient> *list, const OutgoingRecipient &r) {
        if (r.address.isEmpty() || seen.contains(r.address.toLower())) {
            return;
        }
        seen.append(r.address.toLower());
        list->append(r);
    };
    // Answering one's own sent message goes to its recipients, as in Outlook
    const bool own = std::any_of(ownAddresses.cbegin(), ownAddresses.cend(), [&m](const QString &a) {
        return a.compare(m.senderAddress, Qt::CaseInsensitive) == 0;
    });
    if (own) {
        const QList<OutgoingRecipient> original = parseRecipients(m.toList);
        for (const OutgoingRecipient &r : original) {
            add(to, r);
        }
        // A message to oneself: the answer goes to oneself too
        if (to->isEmpty() && !original.isEmpty()) {
            to->append(original.constFirst());
            seen.append(original.constFirst().address.toLower());
        }
    } else {
        add(to, {m.senderName, m.senderAddress});
    }
    if (!all) {
        return;
    }
    if (!own) {
        const QList<OutgoingRecipient> original = parseRecipients(m.toList);
        for (const OutgoingRecipient &r : original) {
            add(to, r);
        }
    }
    const QList<OutgoingRecipient> copies = parseRecipients(m.ccList);
    for (const OutgoingRecipient &r : copies) {
        add(cc, r);
    }
}

}
