// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pst/mapigraph.h"
#include "pst/rtf.h"

#include <QRandomGenerator>
#include <QTest>

using namespace GraphMail;

namespace
{
// [MS-OXRTFEX] 3.1: HTML encapsulated in RTF (the spec's own example)
const QByteArray encapsulated = QByteArrayLiteral(
    "{\\rtf1\\ANSI\\ansicpg1251\\fromhtml1 \\deff0\n"
    "{\\fonttbl {\\f0\\fmodern Courier New;}{\\f1\\fswiss Arial;}{\\f2\\fswiss\\fcharset0 Arial;}}\n"
    "{\\colortbl\\red0\\green0\\blue0;\\red0\\green0\\blue255;}\n"
    "{\\*\\htmltag64}\n"
    "\\uc1\\pard\\plain\\deftab360 \\f0\\fs24\n"
    "{\\*\\htmltag <HTML><head>\\par\n"
    "<style>\\par\n"
    "<!--\\par\n"
    " /* Style Definitions */\\par\n"
    " p.MsoNormal, li.MsoNormal \\{font-family:Arial;\\}\\par\n"
    "-->\\par\n"
    "</style>\\par\n"
    "\\tab <!-- This is a HTML comment.\\par\n"
    "There is a horizontal tab (%x09) character before the comment, \\par\n"
    "and some new lines inside the comment. -->\\par\n"
    "</head>\\par\n"
    "<body>\\par\n"
    "<p\\par\n"
    "class=\"MsoNormal\">}\n"
    "{\\htmlrtf \\f1 \\htmlrtf0 Note the line break inside a P tag. {\\*\\htmltag <b>}{\\htmlrtf \\b\n"
    "\\htmlrtf0 This is a bold text{\\*\\htmltag </b>}} \\htmlrtf\\par\\htmlrtf0}\n"
    "\\htmlrtf \\par \\htmlrtf0\n"
    "{\\*\\htmltag </p>\\par\n"
    "<p class=\"MsoNormal\">\\par}\n"
    "{\\htmlrtf \\f1 \\htmlrtf0 This is a normal text with a character references:\n"
    "{\\*\\htmltag &nbsp;}\\htmlrtf \\'a0\\htmlrtf0 {\\*\\htmltag &lt;}\\htmlrtf <\\htmlrtf0 {\\*\\htmltag\n"
    "&uml;}\\htmlrtf {\\f2\\'a8}\\htmlrtf0{\\*\\htmltag <br>\\par}\\htmlrtf\\line\\htmlrtf0\n"
    "characters which have special meaning in RTF: \\{\\}\\\\{\\*\\htmltag\n"
    "<br>\\par}\\htmlrtf\\line\\htmlrtf0\\htmlrtf\\par\\htmlrtf0}\n"
    "{\\*\\htmltag </p>\\par\n"
    "<ol>\\par\n"
    "    <li class=\"MsoNormal\">}{\\htmlrtf\n"
    "{{\\*\\pn\\pnlvlbody\\pndec\\pnstart1\\pnindent360{\\pntxta.}}\\li360\\fi-360{\\pntext 1.\\tab} \\f1\n"
    "\\htmlrtf0 This is a list item}\\htmlrtf\\par\\htmlrtf0}\n"
    "{\\*\\htmltag \\par\n"
    "</ol>\\par\n"
    "</body>\\par\n"
    "</HTML>\\par }}");
}

class RtfTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // [MS-OXRTFCP] 3.1.1 and 3.1.2: the spec's compressed examples
    void decompressSamples()
    {
        QString error;
        const std::optional<QByteArray> one = Rtf::decompress(QByteArray::fromHex("2d0000002b0000004c5a4675f1c5c7a703000a0072637067313235423"
                                                                                  "20af32068656c090020627705b06c647d0a800fa0"),
                                                              &error);
        QVERIFY2(one, qPrintable(error));
        QCOMPARE(*one, QByteArrayLiteral("{\\rtf1\\ansi\\ansicpg1252\\pard hello world}\r\n"));

        // A reference that reads across the write position
        const std::optional<QByteArray> two = Rtf::decompress(QByteArray::fromHex("1a0000001c0000004c5a4675e2d44b51410004205758595a0d6e7d010eb0"), &error);
        QVERIFY2(two, qPrintable(error));
        QCOMPARE(two->size(), 0x1C);
        QVERIFY(two->startsWith("{\\rtf1 WXYZWXYZWXYZWXYZWXYZ"));

        // Uncompressed contents are copied; damage is refused
        QCOMPARE(*Rtf::decompress(QByteArray::fromHex("0f000000030000004d454c4100000000") + "abc", &error), QByteArrayLiteral("abc"));
        QByteArray damaged = QByteArray::fromHex("2d0000002b0000004c5a4675f1c5c7a703000a0072637067313235423"
                                                 "20af32068656c090020627705b06c647d0a800fa0");
        damaged[20] = 'X';
        QVERIFY(!Rtf::decompress(damaged, &error));
        QVERIFY(error.contains(QLatin1String("CRC")));
        QVERIFY(!Rtf::decompress(QByteArrayLiteral("short"), &error));
        QVERIFY(!Rtf::decompress(QByteArray::fromHex("2d0000002b000000585858580000000000"), &error));
    }

    // The dictionary's preloaded text is 207 bytes (2.1.2.1); a wrong preload would break sample 1
    void htmlFromRtf()
    {
        const Rtf::Body body = Rtf::deencapsulate(encapsulated);
        QCOMPARE(body.kind, Rtf::Body::Kind::Html);
        const QString &h = body.content;
        QVERIFY2(h.startsWith(QLatin1String("<HTML><head>\r\n<style>\r\n<!--\r\n /* Style Definitions */\r\n p.MsoNormal, li.MsoNormal {font-family:Arial;}\r\n")), qPrintable(h));
        QVERIFY(h.contains(QLatin1String("\t<!-- This is a HTML comment.\r\n")));
        QVERIFY(h.contains(QLatin1String("<p\r\nclass=\"MsoNormal\">Note the line break inside a P tag. <b>This is a bold text</b> </p>\r\n")));
        qWarning().noquote() << h.mid(h.indexOf(QLatin1String("normal text")), 120);
        // The spaces the spec's HTML has between these are, in its RTF, the delimiters of the control
        // words before them: strict RTF reading leaves none (the bytes win, as in PST-NOTES)
        QVERIFY(h.contains(QLatin1String("character references:&nbsp;&lt;&uml;<br>\r\n")));
        QVERIFY(h.contains(QLatin1String("characters which have special meaning in RTF: {}\\<br>\r\n")));
        QVERIFY(h.contains(QLatin1String("<li class=\"MsoNormal\">This is a list item\r\n</ol>")));
        QVERIFY(h.trimmed().endsWith(QLatin1String("</HTML>")));
        // What only RTF had is gone: no list number, no non-breaking space byte, no font names
        QVERIFY(!h.contains(QLatin1String("1.\t")));
        QVERIFY(!h.contains(QChar(0x00A0)));
        QVERIFY(!h.contains(QLatin1String("Courier")));
    }

    void textFromRtf()
    {
        // \fromtext: the original text; code page from the font table, \u with its fallback skipped
        const Rtf::Body text = Rtf::deencapsulate(QByteArrayLiteral("{\\rtf1\\ansi\\ansicpg1252\\fromtext \\deff0{\\fonttbl{\\f0\\fswiss\\fcharset204 Arial;}}"
                                                                    "\\uc1\\pard\\f0 \\'cf\\'f0\\'e8\\'e2\\'e5\\'f2 \\u8364?5\\par Second line\\tab end}"));
        QCOMPARE(text.kind, Rtf::Body::Kind::Text);
        QCOMPARE(text.content, QStringLiteral(u"Привет €5\r\nSecond line\tend"));

        // Plain RTF: its text, hidden destinations left out
        const Rtf::Body plain = Rtf::deencapsulate(QByteArrayLiteral("{\\rtf1\\ansi{\\fonttbl{\\f0 Times;}}{\\colortbl;\\red255\\green0\\blue0;}"
                                                                     "{\\info{\\title Secret title}}\\pard\\b Bold\\b0  and {\\*\\generator Example;}normal\\par}"));
        QCOMPARE(plain.kind, Rtf::Body::Kind::PlainFromRtf);
        QCOMPARE(plain.content, QStringLiteral("Bold and normal\r\n"));

        // No crash on nonsense
        QVERIFY(Rtf::deencapsulate(QByteArrayLiteral("{{{{\\\\\\'zz\\u-")).content.size() < 10);
        QVERIFY(Rtf::deencapsulate(QByteArrayLiteral("plain words")).content == QLatin1String("plain words"));
    }

    // [MS-OXRTFCP] 3.2: compressing gives the spec's bytes exactly (the same as its decompression
    // samples), and anything compressed comes back as it was
    void compressSamples()
    {
        QCOMPARE(Rtf::compress(QByteArrayLiteral("{\\rtf1\\ansi\\ansicpg1252\\pard hello world}\r\n")),
                 QByteArray::fromHex("2d0000002b0000004c5a4675f1c5c7a703000a0072637067313235423"
                                     "20af32068656c090020627705b06c647d0a800fa0"));
        QCOMPARE(Rtf::compress(QByteArrayLiteral("{\\rtf1 WXYZWXYZWXYZWXYZWXYZ}")), QByteArray::fromHex("1a0000001c0000004c5a4675e2d44b51410004205758595a0d6e7d010eb0"));
        // Nothing at all: the end marker alone
        QString error;
        QCOMPARE(*Rtf::decompress(Rtf::compress(QByteArray()), &error), QByteArray());

        QRandomGenerator rng(4242);
        for (int round = 0; round < 60; ++round) {
            // Text with repeats (long matches, wrapping the 4 KB dictionary) and random bytes
            QByteArray input;
            const int pieces = int(rng.bounded(200));
            for (int i = 0; i < pieces; ++i) {
                if (rng.bounded(2)) {
                    input += QByteArrayLiteral("{\\rtf1\\ansi Example text ").repeated(int(rng.bounded(5)));
                } else {
                    for (int k = 0, n = int(rng.bounded(80)); k < n; ++k) {
                        input += char(rng.bounded(256));
                    }
                }
            }
            const std::optional<QByteArray> back = Rtf::decompress(Rtf::compress(input), &error);
            QVERIFY2(back, qPrintable(error));
            QCOMPARE(*back, input);
        }
    }

    // [MS-OXRTFEX] 2.1: what we encapsulate comes back out as it went in
    void encapsulation()
    {
        const QString html = QStringLiteral(u"<html><head>\n<style>p {color:red}</style>\n<!-- a comment {} -->\n</head>"
                                            u"<body>\n<p class=\"x\">Caf\u00e9 &amp; Acme &nbsp;&lt;tag&gt; {braces} \\ end</p>\n"
                                            u"<p>Second line<br>third \u20ac 5</p>\n</body></html>");
        Rtf::Body back = Rtf::deencapsulate(Rtf::encapsulateHtml(html));
        QCOMPARE(back.kind, Rtf::Body::Kind::Html);
        QString expected = html;
        expected.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
        QCOMPARE(back.content, expected);

        // And through compression, as PidTagRtfCompressed carries it
        QString error;
        const std::optional<QByteArray> raw = Rtf::decompress(Rtf::compress(Rtf::encapsulateHtml(html)), &error);
        QVERIFY2(raw, qPrintable(error));
        QCOMPARE(Rtf::deencapsulate(*raw).content, expected);

        const QString text = QStringLiteral(u"First line\nSecond line with {braces} and \\ and \u00e9\n\tTabbed \u20ac");
        back = Rtf::deencapsulate(Rtf::encapsulateText(text));
        QCOMPARE(back.kind, Rtf::Body::Kind::Text);
        expected = text;
        expected.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
        QCOMPARE(back.content, expected);
    }

    // Characters past U+FFFF (two \\u, each a signed 16-bit number), other scripts, right to left,
    // combining marks, joiners and invisible spaces survive writing and reading
    void unicode()
    {
        const QString t = QStringLiteral(u"Ren\u00e9 e\u0301 \u20ac \u00bd \U0001F600 \U0001F44D\U0001F3FD \U0001F468\u200D\U0001F469\u200D\U0001F467 "
                                         u"\U0001D518 \u4E2D\u6587 \u65E5\u672C \u0395\u03BB \u041A\u0438 \u0645\u0631\u062D\u0628\u0627 \u05E9\u05DC\u05D5\u05DD "
                                         u"\u00a0\u200b \uFFFD");
        const std::optional<QByteArray> text = Rtf::decompress(Rtf::compress(Rtf::encapsulateText(t)), nullptr);
        QVERIFY(text);
        QCOMPARE(Rtf::deencapsulate(*text).content, t);
        const QString html = QStringLiteral("<p>") + t + QStringLiteral("</p>");
        const std::optional<QByteArray> h = Rtf::decompress(Rtf::compress(Rtf::encapsulateHtml(html)), nullptr);
        QVERIFY(h);
        QCOMPARE(Rtf::deencapsulate(*h).content, html);
    }

    // The body Graph gets: HTML, else HTML from RTF, else text, else text from RTF
    void bestBody()
    {
        const QByteArray compressed = QByteArray::fromHex("2d0000002b0000004c5a4675f1c5c7a703000a0072637067313235423"
                                                          "20af32068656c090020627705b06c647d0a800fa0");
        Pst::Message m;
        m.properties.insert(0x1009, {Pst::PtypBinary, compressed});
        MapiGraph::Body b = MapiGraph::bestBody(m);
        QVERIFY(!b.html);
        // The CRLF after the document's closing brace is no part of it
        QCOMPARE(b.content, QStringLiteral("hello world"));
        QVERIFY(!b.note.isEmpty());

        // An empty paragraph (Outlook's body of a contact without notes) is no body, nothing lost
        Pst::Message empty;
        empty.properties.insert(0x1009, {Pst::PtypBinary, Rtf::compress(QByteArrayLiteral("{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl{\\f0 Calibri;}}\\pard\\f0\\fs24 \r\n\\par }"))});
        const MapiGraph::Body none = MapiGraph::bestBody(empty);
        QVERIFY(none.content.isEmpty());
        QVERIFY(none.note.isEmpty());

        m.properties.insert(0x1000, {Pst::PtypString, QByteArray(reinterpret_cast<const char *>(u"Text body"), 18)});
        b = MapiGraph::bestBody(m);
        QCOMPARE(b.content, QStringLiteral("Text body"));
        QVERIFY(b.note.isEmpty());

        m.properties.insert(0x1013, {Pst::PtypBinary, QByteArrayLiteral("<p>HTML</p>")});
        b = MapiGraph::bestBody(m);
        QVERIFY(b.html);
        QCOMPARE(b.content, QStringLiteral("<p>HTML</p>"));
    }
};

QTEST_GUILESS_MAIN(RtfTest)
#include "rtftest.moc"
