// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mimemapi.h"
#include "pst/mapigraph.h"
#include "pst/pstwriter.h"

#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>

using namespace GraphMail;

namespace
{
// An invented message: alternative text and HTML with an inline image, a PDF, an attached message
const QByteArray sample = QByteArrayLiteral(
    "From: Alice Example <alice@example.com>\r\n"
    "To: Bob Example <bob@example.com>, carol@example.com\r\n"
    "Cc: =?UTF-8?B?RMOpbmlzZSBFeGFtcGxl?= <denise@example.com>\r\n"
    "Subject: =?UTF-8?Q?Caf=C3=A9_example?=\r\n"
    "Date: Tue, 01 Jan 2019 10:00:00 +0000\r\n"
    "Message-ID: <m1@example.com>\r\n"
    "In-Reply-To: <m0@example.com>\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/mixed; boundary=\"outer\"\r\n"
    "\r\n"
    "--outer\r\n"
    "Content-Type: multipart/related; boundary=\"rel\"\r\n"
    "\r\n"
    "--rel\r\n"
    "Content-Type: multipart/alternative; boundary=\"alt\"\r\n"
    "\r\n"
    "--alt\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "\r\n"
    "Example text body\r\n"
    "--alt\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "\r\n"
    "<p>Example <b>HTML</b> body <img src=\"cid:logo@example\"></p>\r\n"
    "--alt--\r\n"
    "--rel\r\n"
    "Content-Type: image/png; name=\"logo.png\"\r\n"
    "Content-ID: <logo@example>\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "\r\n"
    "UE5HREFUQQ==\r\n"
    "--rel--\r\n"
    "--outer\r\n"
    "Content-Type: application/pdf; name=\"report.pdf\"\r\n"
    "Content-Disposition: attachment; filename=\"report.pdf\"\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "\r\n"
    "UERGREFUQQ==\r\n"
    "--outer\r\n"
    "Content-Type: message/rfc822\r\n"
    "Content-Disposition: attachment\r\n"
    "\r\n"
    "From: Erin Example <erin@example.com>\r\n"
    "To: Alice Example <alice@example.com>\r\n"
    "Subject: Attached example\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "Inner text\r\n"
    "--outer--\r\n");
}

class MimeMapiTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void convertsMime()
    {
        MimeMapi::Extras extras;
        extras.read = false;
        extras.flag = 1;
        extras.importance = 2;
        extras.categories = {QStringLiteral("Acme")};
        extras.received = QDateTime(QDate(2019, 1, 1), QTime(10, 5), QTimeZone::UTC).toMSecsSinceEpoch();
        const Pst::Message m = MimeMapi::fromMime(sample, extras, 0x8000);
        QCOMPARE(m.string(0x0037), QStringLiteral(u"Café example"));
        QCOMPARE(m.string(0x0042), QStringLiteral("Alice Example"));
        QCOMPARE(m.string(0x5D02), QStringLiteral("alice@example.com"));
        QCOMPARE(m.string(0x0E04), QStringLiteral("Bob Example; carol@example.com"));
        QCOMPARE(m.string(0x0E03), QStringLiteral(u"Dénise Example"));
        QCOMPARE(m.recipients.size(), 3);
        QCOMPARE(m.string(0x1035), QStringLiteral("<m1@example.com>"));
        QCOMPARE(m.string(0x1042), QStringLiteral("<m0@example.com>"));
        QCOMPARE(m.properties.value(0x0039).toTime(), QDateTime(QDate(2019, 1, 1), QTime(10, 0), QTimeZone::UTC));
        QCOMPARE(m.properties.value(0x0E06).toTime(), QDateTime(QDate(2019, 1, 1), QTime(10, 5), QTimeZone::UTC));
        QCOMPARE(m.properties.value(0x0E07).toInt(), 0);
        QCOMPARE(m.properties.value(0x1090).toInt(), 2);
        QCOMPARE(m.properties.value(0x0017).toInt(), 2);
        QCOMPARE(m.properties.value(0x8000).items().size(), 1);
        QVERIFY(QString::fromUtf8(m.properties.value(0x1013).data).contains(QLatin1String("cid:logo@example")));
        QCOMPARE(m.string(0x1000).trimmed(), QStringLiteral("Example text body"));
        QVERIFY(m.string(0x007D).contains(QLatin1String("Message-ID: <m1@example.com>")));

        QCOMPARE(m.attachments.size(), 3);
        const Pst::PropertyMap &logo = m.attachments.at(0).properties;
        QCOMPARE(logo.value(0x3707).toString(), QStringLiteral("logo.png"));
        QCOMPARE(logo.value(0x3712).toString(), QStringLiteral("logo@example"));
        QCOMPARE(logo.value(0x3714).toInt(), 4);
        QCOMPARE(logo.value(0x3701).data, QByteArrayLiteral("PNGDATA"));
        const Pst::PropertyMap &pdf = m.attachments.at(1).properties;
        QCOMPARE(pdf.value(0x3707).toString(), QStringLiteral("report.pdf"));
        QCOMPARE(pdf.value(0x370E).toString(), QStringLiteral("application/pdf"));
        QVERIFY(!pdf.contains(0x3714));
        QVERIFY(m.attachments.at(2).embedded);
        QCOMPARE(m.attachments.at(2).properties.value(0x3705).toInt(), 5);
        QCOMPARE(m.attachments.at(2).embedded->string(0x0037), QStringLiteral("Attached example"));
        QCOMPARE(m.attachments.at(2).embedded->string(0x1000).trimmed(), QStringLiteral("Inner text"));
    }

    // MAPI back to MIME (opening a .msg): the message survives the trip MIME -> MAPI -> MIME -> MAPI
    void backToMime()
    {
        const Pst::Message first = MimeMapi::fromMime(sample, {}, 0);
        const QByteArray mime = MimeMapi::toMime(first);
        QVERIFY(mime.contains("multipart/mixed"));
        QVERIFY(mime.contains("multipart/related"));
        QVERIFY(mime.contains("message/rfc822"));
        const Pst::Message again = MimeMapi::fromMime(mime, {}, 0);
        QCOMPARE(again.string(0x0037), QStringLiteral(u"Café example"));
        QCOMPARE(again.string(0x5D02), QStringLiteral("alice@example.com"));
        QCOMPARE(again.recipients.size(), 3);
        QCOMPARE(again.string(0x0E03), QStringLiteral(u"Dénise Example"));
        QCOMPARE(again.string(0x1035), QStringLiteral("<m1@example.com>"));
        QCOMPARE(again.properties.value(0x0039).toTime(), first.properties.value(0x0039).toTime());
        QVERIFY(QString::fromUtf8(again.properties.value(0x1013).data).contains(QLatin1String("cid:logo@example")));
        QCOMPARE(again.string(0x1000).trimmed(), QStringLiteral("Example text body"));
        QCOMPARE(again.attachments.size(), 3);
        QCOMPARE(again.attachments.at(0).properties.value(0x3712).toString(), QStringLiteral("logo@example"));
        QCOMPARE(again.attachments.at(0).properties.value(0x3714).toInt(), 4);
        QCOMPARE(again.attachments.at(1).properties.value(0x3701).data, QByteArrayLiteral("PDFDATA"));
        QVERIFY(again.attachments.at(2).embedded);
        QCOMPARE(again.attachments.at(2).embedded->string(0x0037), QStringLiteral("Attached example"));

        // A text-only message is no multipart
        Pst::Message plain;
        plain.properties.insert(0x0037, {Pst::PtypString, QByteArray(reinterpret_cast<const char *>(u"Plain"), 10)});
        plain.properties.insert(0x1000, {Pst::PtypString, QByteArray(reinterpret_cast<const char *>(u"Just text"), 18)});
        const QByteArray plainMime = MimeMapi::toMime(plain);
        QVERIFY(!plainMime.contains("multipart"));
        QCOMPARE(MimeMapi::fromMime(plainMime, {}, 0).string(0x1000).trimmed(), QStringLiteral("Just text"));
    }

    // MIME to a PST and back to what Graph would get: the export and the import agree
    void roundTrip()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("export.pst"));
        QString error;
        Pst::PstWriter w;
        QVERIFY(w.create(path, QStringLiteral("Example export"), &error));
        const quint16 keywords = w.namedProperty(Pst::psPublicStrings, QStringLiteral("Keywords"));
        const quint32 folder = w.addFolder(Pst::PstWriter::nidIpmSubtree, QStringLiteral("Inbox"));
        MimeMapi::Extras extras;
        extras.categories = {QStringLiteral("Acme"), QStringLiteral("Red")};
        const quint32 nid = w.addMessage(folder, MimeMapi::fromMime(sample, extras, keywords));
        QVERIFY(nid);
        QVERIFY2(w.finish(&error), qPrintable(error));

        Pst::PstFile r;
        QVERIFY2(r.open(path, &error), qPrintable(error));
        const std::optional<Pst::Message> m = r.message(nid, &error);
        QVERIFY2(m, qPrintable(error));
        const MapiGraph::Import g = MapiGraph::toGraph(*m, r.namedProperties());
        QCOMPARE(g.message.value(QLatin1String("subject")).toString(), QStringLiteral(u"Café example"));
        QCOMPARE(g.message.value(QLatin1String("toRecipients")).toArray().size(), 2);
        QCOMPARE(g.message.value(QLatin1String("ccRecipients")).toArray().size(), 1);
        QCOMPARE(g.message.value(QLatin1String("categories")).toArray(), (QJsonArray{QStringLiteral("Acme"), QStringLiteral("Red")}));
        QVERIFY(g.message.value(QLatin1String("isRead")).toBool());
        QVERIFY(g.message.value(QLatin1String("body")).toObject().value(QLatin1String("content")).toString().contains(QLatin1String("<b>HTML</b>")));
        QCOMPARE(g.files.size(), 2);
        QVERIFY(g.files.at(0).isInline);
        QCOMPARE(g.files.at(1).data, QByteArrayLiteral("PDFDATA"));
        QCOMPARE(g.items.size(), 1);
        QVERIFY(g.skipped.isEmpty());
    }
};

QTEST_GUILESS_MAIN(MimeMapiTest)
#include "mimemapitest.moc"
