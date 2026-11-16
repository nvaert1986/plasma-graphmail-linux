// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "maileditor.h"

#include <QMimeData>
#include <QRegularExpression>

#include <cmath>

namespace GraphMail
{

namespace
{
// A font size of a style in pixels (pt at 96 dpi as browsers count); 0 when there is none
double fontPixels(const QString &style)
{
    static const QRegularExpression size(QStringLiteral(R"(font-size\s*:\s*([0-9.]+)\s*(pt|px))"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = size.match(style);
    if (!m.hasMatch()) {
        return 0;
    }
    const double value = m.captured(1).toDouble();
    return m.captured(2).compare(QLatin1String("pt"), Qt::CaseInsensitive) == 0 ? value * 96.0 / 72.0 : value;
}

QString fixStyle(const QString &style)
{
    static const QRegularExpression lineHeight(QStringLiteral(R"((^|;)\s*line-height\s*:\s*([^;]*))"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = lineHeight.match(style);
    if (!m.hasMatch()) {
        return style;
    }
    const QString value = m.captured(2).trimmed();
    bool number = false;
    double factor = value.toDouble(&number);
    if (!number && value.endsWith(QLatin1Char('%'))) {
        factor = value.chopped(1).toDouble(&number) / 100.0;
    }
    // A browser's factor of the font size, in pixels; without a font size (or "initial",
    // "normal") Qt's own spacing
    QString replacement;
    if (number && factor > 0) {
        if (const double font = fontPixels(style); font > 0) {
            replacement = QStringLiteral("line-height:%1px").arg(qRound(font * factor));
        }
    } else if (value.endsWith(QLatin1String("px"), Qt::CaseInsensitive)) {
        replacement = QStringLiteral("line-height:") + value;
    }
    QString out = style;
    out.replace(m.capturedStart(0), m.capturedLength(0), m.captured(1) + replacement);
    return out;
}
}

QString MailEditor::htmlForQt(const QString &html)
{
    // Every tag with a style attribute
    static const QRegularExpression tag(QStringLiteral(R"(<([a-zA-Z][a-zA-Z0-9]*)(\s[^>]*?)?(/?)>)"));
    static const QRegularExpression styleAttribute(QStringLiteral(R"re(\sstyle\s*=\s*"([^"]*)")re"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression cssWidth(QStringLiteral(R"((^|;)\s*width\s*:\s*([0-9.]+)px)"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression widthAttribute(QStringLiteral(R"(\swidth\s*=)"), QRegularExpression::CaseInsensitiveOption);
    QString out;
    out.reserve(html.size());
    qsizetype last = 0;
    QRegularExpressionMatchIterator it = tag.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += QStringView(html).mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        const QString name = m.captured(1).toLower();
        QString attributes = m.captured(2);
        const QRegularExpressionMatch s = styleAttribute.match(attributes);
        if (s.hasMatch()) {
            const QString fixed = fixStyle(s.captured(1));
            attributes.replace(s.capturedStart(1), s.capturedLength(1), fixed);
            // Table cells: the CSS width (of the content, as browsers count it) plus the cell's
            // side padding as the attribute Qt reads. Not for tables: a browser widens a table
            // that is too narrow for its cells (Outlook's 38 px row of social icons), Qt does not
            if ((name == QLatin1String("td") || name == QLatin1String("th")) && !widthAttribute.match(attributes).hasMatch()) {
                const QRegularExpressionMatch w = cssWidth.match(fixed);
                if (w.hasMatch()) {
                    static const QRegularExpression sidePadding(QStringLiteral(R"((^|;)\s*padding-(left|right)\s*:\s*([0-9.]+)px)"), QRegularExpression::CaseInsensitiveOption);
                    double width = w.captured(2).toDouble();
                    QRegularExpressionMatchIterator paddings = sidePadding.globalMatch(fixed);
                    while (paddings.hasNext()) {
                        width += paddings.next().captured(3).toDouble();
                    }
                    attributes += QStringLiteral(" width=\"%1\"").arg(int(std::ceil(width)));
                }
            }
        }
        out += QLatin1Char('<') + m.captured(1) + attributes + m.captured(3) + QLatin1Char('>');
    }
    out += QStringView(html).mid(last);
    // A paragraph holding only a line break is one empty line in a browser, two in Qt
    static const QRegularExpression onlyBreak(QStringLiteral(R"re((<p\b[^>]*>)((?:\s*<span\b[^>]*>)*)\s*<br\s*/?>\s*((?:</span>\s*)*)</p>)re"),
                                              QRegularExpression::CaseInsensitiveOption);
    out.replace(onlyBreak, QStringLiteral("\\1\\2&nbsp;\\3</p>"));
    return out;
}

void MailEditor::insertFromMimeData(const QMimeData *source)
{
    if (!source || !source->hasHtml() || textMode() != RichTextComposer::Rich) {
        RichTextComposer::insertFromMimeData(source);
        return;
    }
    QMimeData copy;
    for (const QString &format : source->formats()) {
        copy.setData(format, source->data(format));
    }
    copy.setHtml(htmlForQt(source->html()));
    RichTextComposer::insertFromMimeData(&copy);
}

}
