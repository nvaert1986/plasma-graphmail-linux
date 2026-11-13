// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "rtf.h"
#include "pstalgorithms.h"
#include "pstltp.h"

#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

namespace GraphMail::Rtf
{

namespace
{
// [MS-OXRTFCP] 2.1.2.1: the dictionary's first 207 bytes
constexpr char dictionaryStart[] = "{\\rtf1\\ansi\\mac\\deff0\\deftab720{\\fonttbl;}{\\f0\\fnil \\froman \\fswiss \\fmodern \\fscript \\fdecor MS Sans "
                                   "SerifSymbolArialTimes New RomanCourier{\\colortbl\\red0\\green0\\blue0\r\n\\par \\pard\\plain\\f0\\fs20\\b\\i\\u\\tab\\tx";
constexpr quint32 typeCompressed = 0x75465A4C;
constexpr quint32 typeUncompressed = 0x414C454D;
constexpr int dictionarySize = 4096;
// Groups nested deeper than this are taken as flat (a damaged document cannot exhaust memory)
constexpr int maxDepth = 1024;

bool fail(QString *error, const QString &text)
{
    if (error) {
        *error = text;
    }
    return false;
}

// The Windows code page of an RTF \fcharset ([MSFT-RTF] font table)
int charsetCodepage(int charset, int fallback)
{
    switch (charset) {
    case 0:
        return 1252;
    case 128:
        return 932;
    case 129:
        return 949;
    case 130:
        return 1361;
    case 134:
        return 936;
    case 136:
        return 950;
    case 161:
        return 1253;
    case 162:
        return 1254;
    case 163:
        return 1258;
    case 177:
        return 1255;
    case 178:
        return 1256;
    case 186:
        return 1257;
    case 204:
        return 1251;
    case 222:
        return 874;
    case 238:
        return 1250;
    case 255:
        return 437;
    default:
        return fallback;
    }
}

// Destinations that hold no visible text
const QSet<QByteArray> &hiddenDestinations()
{
    static const QSet<QByteArray> set{"colortbl", "stylesheet", "info",     "pict",     "header",    "headerl",         "headerr",    "headerf",
                                      "footer",   "footerl",    "footerr",  "footerf",  "xmlnstbl",  "listtable",       "listoverridetable", "rsidtbl",
                                      "themedata", "colorschememapping", "datastore", "latentstyles", "generator", "objdata", "fldinst", "pn",
                                      "filetbl",  "revtbl",     "pgdsctbl", "private"};
    return set;
}

struct State {
    bool htmlrtf = false;
    bool skip = false;
    bool htmltag = false;
    bool fonttbl = false;
    int uc = 1;
    int font = -1;
};
}

std::optional<QByteArray> decompress(const QByteArray &compressed, QString *error)
{
    if (compressed.size() < 16) {
        fail(error, QStringLiteral("compressed RTF without its header"));
        return std::nullopt;
    }
    const char *h = compressed.constData();
    const quint32 compSize = qFromLittleEndian<quint32>(h);
    const quint32 rawSize = qFromLittleEndian<quint32>(h + 4);
    const quint32 type = qFromLittleEndian<quint32>(h + 8);
    const quint32 crc = qFromLittleEndian<quint32>(h + 12);
    if (type == typeUncompressed) {
        return compressed.mid(16, qsizetype(std::min<quint32>(rawSize, quint32(compressed.size() - 16))));
    }
    if (type != typeCompressed || compSize < 12) {
        fail(error, QStringLiteral("not compressed RTF"));
        return std::nullopt;
    }
    // CONTENTS: what COMPSIZE counts after the 12 header bytes that follow it, padding included
    const QByteArray contents = compressed.mid(16, qsizetype(compSize) - 12);
    if (Pst::crc(contents.constData(), contents.size()) != crc) {
        fail(error, QStringLiteral("compressed RTF with a wrong CRC"));
        return std::nullopt;
    }
    QByteArray dictionary(dictionarySize, 0);
    const qsizetype preload = qsizetype(sizeof(dictionaryStart)) - 1;
    std::copy(dictionaryStart, dictionaryStart + preload, dictionary.begin());
    int write = int(preload);
    QByteArray out;
    out.reserve(qsizetype(std::min<quint32>(rawSize, 64 * 1024 * 1024)));
    const auto *in = reinterpret_cast<const quint8 *>(contents.constData());
    const qsizetype size = contents.size();
    qsizetype pos = 0;
    while (pos < size) {
        const quint8 control = in[pos++];
        for (int bit = 0; bit < 8; ++bit) {
            if (pos >= size) {
                // Input ended before the end marker: corrupt, unless everything is there
                if (quint32(out.size()) >= rawSize) {
                    out.truncate(rawSize);
                    return out;
                }
                fail(error, QStringLiteral("compressed RTF ends early"));
                return std::nullopt;
            }
            if (!(control & (1 << bit))) {
                const char c = char(in[pos++]);
                out += c;
                dictionary[write] = c;
                write = (write + 1) % dictionarySize;
                continue;
            }
            if (pos + 1 >= size) {
                fail(error, QStringLiteral("compressed RTF ends inside a reference"));
                return std::nullopt;
            }
            // A big-endian reference: 12 bits offset, 4 bits length - 2
            const quint16 ref = quint16(in[pos] << 8 | in[pos + 1]);
            pos += 2;
            int read = ref >> 4;
            if (read == write) {
                out.truncate(std::min<qsizetype>(out.size(), rawSize));
                return out;
            }
            const int length = (ref & 0x0F) + 2;
            for (int i = 0; i < length; ++i) {
                const char c = dictionary.at(read);
                read = (read + 1) % dictionarySize;
                out += c;
                dictionary[write] = c;
                write = (write + 1) % dictionarySize;
            }
            // A damaged stream cannot grow without end
            if (quint32(out.size()) > rawSize + dictionarySize) {
                fail(error, QStringLiteral("compressed RTF larger than it says"));
                return std::nullopt;
            }
        }
    }
    if (quint32(out.size()) >= rawSize) {
        out.truncate(rawSize);
        return out;
    }
    fail(error, QStringLiteral("compressed RTF ends early"));
    return std::nullopt;
}

Body deencapsulate(const QByteArray &rtf)
{
    Body body;
    const qsizetype n = rtf.size();

    // 2.2.3.1: \fromhtml1 or \fromtext among the first 10 tokens, all groups or control words
    if (rtf.startsWith("{\\rtf1")) {
        int tokens = 0;
        for (qsizetype i = 0; i < n && tokens < 10;) {
            const char c = rtf.at(i);
            if (c == '{') {
                ++tokens;
                ++i;
            } else if (c == '\\' && i + 1 < n && QChar::fromLatin1(rtf.at(i + 1)).isLetter()) {
                qsizetype j = i + 1;
                while (j < n && QChar::fromLatin1(rtf.at(j)).isLetter()) {
                    ++j;
                }
                const QByteArray word = rtf.mid(i + 1, j - i - 1);
                qsizetype k = j;
                if (k < n && rtf.at(k) == '-') {
                    ++k;
                }
                while (k < n && QChar::fromLatin1(rtf.at(k)).isDigit()) {
                    ++k;
                }
                const QByteArray param = rtf.mid(j, k - j);
                if (word == "fromhtml" && param == "1") {
                    body.kind = Body::Kind::Html;
                    break;
                }
                if (word == "fromtext") {
                    body.kind = Body::Kind::Text;
                    break;
                }
                ++tokens;
                i = k < n && rtf.at(k) == ' ' ? k + 1 : k;
            } else if (c == '\r' || c == '\n' || c == ' ') {
                ++i;
            } else {
                break;
            }
        }
    }

    // 2.2.3.2 and 2.2.3.3: one pass, keeping text, \par and the like, the \*\htmltag contents
    QList<State> stack;
    State st;
    int defaultCodepage = 1252;
    QHash<int, int> fontCodepage;
    int fontBeingDefined = -1;
    bool starPending = false;
    int pendingSkip = 0;
    QByteArray bytes;
    QString &out = body.content;
    const auto codepage = [&] {
        return fontCodepage.value(st.font, defaultCodepage);
    };
    const auto flush = [&] {
        if (!bytes.isEmpty()) {
            out += Pst::decodeCodepage(bytes, codepage());
            bytes.clear();
        }
    };
    const auto visible = [&] {
        return !st.skip && (st.htmltag || !st.htmlrtf);
    };
    const auto addByte = [&](char b) {
        if (pendingSkip > 0) {
            --pendingSkip;
            return;
        }
        if (visible()) {
            bytes += b;
        }
    };
    const auto addText = [&](const QString &text) {
        if (pendingSkip > 0) {
            --pendingSkip;
            return;
        }
        if (visible()) {
            flush();
            out += text;
        }
    };

    for (qsizetype i = 0; i < n;) {
        const char c = rtf.at(i);
        if (c == '{') {
            flush();
            if (stack.size() < maxDepth) {
                stack.append(st);
            }
            starPending = false;
            ++i;
            continue;
        }
        if (c == '}') {
            flush();
            if (stack.isEmpty()) {
                break;
            }
            st = stack.takeLast();
            starPending = false;
            ++i;
            continue;
        }
        if (c == '\r' || c == '\n') {
            ++i;
            continue;
        }
        if (c != '\\') {
            addByte(c);
            ++i;
            continue;
        }
        // A control symbol or a control word
        if (i + 1 >= n) {
            break;
        }
        const char next = rtf.at(i + 1);
        if (!QChar::fromLatin1(next).isLetter()) {
            i += 2;
            switch (next) {
            case '\'':
                if (i + 2 <= n) {
                    bool ok = false;
                    const int value = rtf.mid(i, 2).toInt(&ok, 16);
                    i += 2;
                    if (ok) {
                        addByte(char(value));
                    }
                }
                break;
            case '{':
            case '}':
            case '\\':
                addByte(next);
                break;
            case '~':
                addText(QString(QChar(0x00A0)));
                break;
            case '_':
                addText(QString(QChar(0x00AD)));
                break;
            case '*':
                starPending = true;
                break;
            case '\r':
            case '\n':
                addText(QStringLiteral("\r\n"));
                break;
            default:
                break;
            }
            continue;
        }
        qsizetype j = i + 1;
        while (j < n && QChar::fromLatin1(rtf.at(j)).isLetter()) {
            ++j;
        }
        const QByteArray word = rtf.mid(i + 1, j - i - 1);
        qsizetype k = j;
        if (k < n && rtf.at(k) == '-') {
            ++k;
        }
        while (k < n && QChar::fromLatin1(rtf.at(k)).isDigit()) {
            ++k;
        }
        const bool hasParam = k > j && !(k == j + 1 && rtf.at(j) == '-');
        const int param = hasParam ? rtf.mid(j, k - j).toInt() : 0;
        // A space ends a control word and belongs to it
        i = k < n && rtf.at(k) == ' ' ? k + 1 : k;

        if (starPending) {
            starPending = false;
            flush();
            if (word == "htmltag") {
                st.htmltag = true;
                st.skip = false;
            } else {
                // Every other ignorable destination, \*\mhtmltag included (2.1.3.1.5)
                st.skip = true;
            }
            continue;
        }
        if (word == "htmlrtf") {
            flush();
            st.htmlrtf = !hasParam || param != 0;
        } else if (word == "ansicpg" && hasParam) {
            defaultCodepage = param;
        } else if (word == "fonttbl") {
            flush();
            st.fonttbl = true;
            st.skip = true;
        } else if (word == "f" && hasParam) {
            if (st.fonttbl) {
                fontBeingDefined = param;
            } else {
                // The font counts even where \htmlrtf hides the text (2.2.3.2)
                flush();
                st.font = param;
            }
        } else if (word == "fcharset" && st.fonttbl && hasParam) {
            fontCodepage.insert(fontBeingDefined, charsetCodepage(param, defaultCodepage));
        } else if (word == "cpg" && st.fonttbl && hasParam) {
            fontCodepage.insert(fontBeingDefined, param);
        } else if (word == "uc" && hasParam) {
            st.uc = std::max(0, param);
        } else if (word == "u" && hasParam) {
            const char16_t unit = char16_t(param < 0 ? param + 65536 : param);
            if (pendingSkip == 0 && visible()) {
                flush();
                out += QChar(unit);
            }
            pendingSkip = st.uc;
        } else if (word == "par" || word == "line") {
            addText(QStringLiteral("\r\n"));
        } else if (word == "tab") {
            addText(QStringLiteral("\t"));
        } else if (word == "lquote") {
            addText(QString(QChar(0x2018)));
        } else if (word == "rquote") {
            addText(QString(QChar(0x2019)));
        } else if (word == "ldblquote") {
            addText(QString(QChar(0x201C)));
        } else if (word == "rdblquote") {
            addText(QString(QChar(0x201D)));
        } else if (word == "bullet") {
            addText(QString(QChar(0x2022)));
        } else if (word == "endash") {
            addText(QString(QChar(0x2013)));
        } else if (word == "emdash") {
            addText(QString(QChar(0x2014)));
        } else if (hiddenDestinations().contains(word)) {
            flush();
            st.skip = true;
        }
    }
    flush();
    return body;
}

QByteArray compress(const QByteArray &rtf)
{
    QByteArray dictionary(dictionarySize, 0);
    const qsizetype preload = qsizetype(sizeof(dictionaryStart)) - 1;
    std::copy(dictionaryStart, dictionaryStart + preload, dictionary.begin());
    int write = int(preload);
    int end = int(preload);
    QByteArray contents;
    const auto add = [&](char c) {
        dictionary[write] = c;
        write = (write + 1) % dictionarySize;
        end = std::min(end + 1, dictionarySize);
    };
    const qsizetype size = rtf.size();
    qsizetype cursor = 0;
    for (;;) {
        quint8 control = 0;
        quint8 bit = 0x01;
        QByteArray tokens;
        bool done = false;
        for (;;) {
            if (cursor >= size) {
                // The end marker: a reference to the write offset, length 0 (2.3.3.2 step 8)
                const quint16 ref = quint16(write << 4);
                tokens += char(ref >> 8);
                tokens += char(ref & 0xFF);
                control |= bit;
                done = true;
                break;
            }
            // The longest match (at most 17) among offsets [start, write): a match may run on into
            // the bytes it writes itself, as the decompressor reads them back (2.3.3.2.1)
            const int maxLength = int(std::min<qsizetype>(17, size - cursor));
            int bestLength = 0;
            int bestOffset = 0;
            int offset = end < dictionarySize ? 0 : (write + 1) % dictionarySize;
            while (offset != write && bestLength < 17) {
                int length = 0;
                while (length < maxLength) {
                    const int p = (offset + length) % dictionarySize;
                    // Positions from the write offset on hold what this match writes
                    const int ahead = (p - write + dictionarySize) % dictionarySize;
                    const char d = ahead < length ? rtf.at(cursor + ahead) : dictionary.at(p);
                    if (d != rtf.at(cursor + length)) {
                        break;
                    }
                    ++length;
                }
                if (length > bestLength) {
                    bestLength = length;
                    bestOffset = offset;
                }
                offset = (offset + 1) % dictionarySize;
            }
            if (bestLength < 2) {
                tokens += rtf.at(cursor);
                add(rtf.at(cursor));
                ++cursor;
            } else {
                const quint16 ref = quint16(bestOffset << 4 | (bestLength - 2));
                tokens += char(ref >> 8);
                tokens += char(ref & 0xFF);
                control |= bit;
                for (int i = 0; i < bestLength; ++i) {
                    add(rtf.at(cursor + i));
                }
                cursor += bestLength;
            }
            if (bit == 0x80) {
                break;
            }
            bit = quint8(bit << 1);
        }
        contents += char(control);
        contents += tokens;
        if (done) {
            break;
        }
    }
    QByteArray out(16, 0);
    qToLittleEndian(quint32(contents.size() + 12), out.data());
    qToLittleEndian(quint32(size), out.data() + 4);
    qToLittleEndian(typeCompressed, out.data() + 8);
    qToLittleEndian(Pst::crc(contents.constData(), contents.size()), out.data() + 12);
    return out + contents;
}

namespace
{
// Text as RTF: the specials escaped, everything outside ASCII as \uN with a ? for old readers
QByteArray rtfText(const QString &text)
{
    QByteArray out;
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        if (u == '\\' || u == '{' || u == '}') {
            out += '\\';
            out += char(u);
        } else if (u == '\t') {
            out += "\\tab ";
        } else if (u >= 0x20 && u < 0x80) {
            out += char(u);
        } else if (u >= 0x80) {
            out += "\\u" + QByteArray::number(qint16(u)) + '?';
        }
    }
    return out;
}

// An HTML fragment kept in a \*\htmltag group: the specials escaped, line ends as \par
QByteArray htmltag(const QString &html)
{
    QByteArray out = "{\\*\\htmltag ";
    for (qsizetype i = 0; i < html.size(); ++i) {
        const char16_t u = html.at(i).unicode();
        if (u == '\r') {
            continue;
        }
        if (u == '\n') {
            out += "\\par\n";
        } else {
            out += rtfText(QString(html.at(i)));
        }
    }
    return out + '}';
}
}

QByteArray encapsulateText(const QString &text)
{
    QByteArray out = "{\\rtf1\\ansi\\ansicpg1252\\fromtext \\deff0{\\fonttbl{\\f0\\fswiss Arial;}}\\uc1\\pard\\plain\\f0\\fs20 ";
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    const QStringList lines = normalized.split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out += "\\par\n";
        }
        out += rtfText(lines.at(i));
    }
    return out + '}';
}

QByteArray encapsulateHtml(const QString &html)
{
    QByteArray out = "{\\rtf1\\ansi\\ansicpg1252\\fromhtml1 \\deff0{\\fonttbl{\\f0\\fswiss Arial;}}\\uc1\\pard\\plain\\f0\\fs20\n";
    // Tags, comments and the contents of script and style go whole into \*\htmltag; text stays
    // RTF text, its character references kept for the HTML and shown as the character in RTF
    static const QRegularExpression piece(QStringLiteral(R"((<!--.*?-->)|(<(script|style)\b.*?</\3\s*>)|(<[^>]*>)|(&#?\w+;))"),
                                          QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression breaking(QStringLiteral(R"(^<(br|/p|/div|/li|/tr|/h[1-6]|/table|/blockquote)\b)"), QRegularExpression::CaseInsensitiveOption);
    qsizetype at = 0;
    QRegularExpressionMatchIterator it = piece.globalMatch(html);
    const auto text = [&](const QString &run) {
        // Line ends in HTML text are whitespace: kept for the HTML, a space for RTF readers
        QString t = run;
        t.remove(QLatin1Char('\r'));
        qsizetype start = 0;
        for (qsizetype i = 0; i <= t.size(); ++i) {
            if (i == t.size() || t.at(i) == QLatin1Char('\n')) {
                out += rtfText(t.mid(start, i - start));
                if (i < t.size()) {
                    out += "{\\*\\htmltag \\par}\\htmlrtf  \\htmlrtf0 ";
                }
                start = i + 1;
            }
        }
    };
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        text(html.mid(at, m.capturedStart() - at));
        const QString p = m.captured(0);
        out += htmltag(p);
        if (!m.captured(5).isEmpty()) {
            // A character reference: the HTML keeps it, RTF shows the character
            const QString decoded = p == QLatin1String("&nbsp;") ? QString(QChar(0x00A0))
                : p == QLatin1String("&amp;")                   ? QStringLiteral("&")
                : p == QLatin1String("&lt;")                    ? QStringLiteral("<")
                : p == QLatin1String("&gt;")                    ? QStringLiteral(">")
                : p == QLatin1String("&quot;")                  ? QStringLiteral("\"")
                : p.startsWith(QLatin1String("&#x"))            ? QString(QChar(char16_t(p.mid(3, p.size() - 4).toUInt(nullptr, 16))))
                : p.startsWith(QLatin1String("&#"))             ? QString(QChar(char16_t(p.mid(2, p.size() - 3).toUInt())))
                                                                : QString();
            if (!decoded.isEmpty()) {
                out += "\\htmlrtf " + rtfText(decoded) + "\\htmlrtf0 ";
            }
        } else if (breaking.match(p).hasMatch()) {
            out += "\\htmlrtf \\par \\htmlrtf0\n";
        }
        at = m.capturedEnd();
    }
    text(html.mid(at));
    return out + '}';
}

}
