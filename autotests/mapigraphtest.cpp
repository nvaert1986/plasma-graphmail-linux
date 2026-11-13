// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pst/mapigraph.h"

#include <QJsonArray>
#include <QTest>
#include <QtEndian>

using namespace GraphMail;

namespace
{
QByteArray le32(quint32 v)
{
    QByteArray b(4, 0);
    qToLittleEndian(v, b.data());
    return b;
}
Pst::PropValue str(const QString &s)
{
    return {Pst::PtypString, QByteArray(reinterpret_cast<const char *>(s.utf16()), s.size() * 2)};
}
Pst::PropValue int32(quint32 v)
{
    return {Pst::PtypInteger32, le32(v)};
}
QHash<quint32, Pst::PropValue> recipient(int type, const QString &name, const QString &address, const QString &addressType, const QString &smtp = {})
{
    QHash<quint32, Pst::PropValue> r;
    r.insert(0x0C150003, int32(quint32(type)));
    r.insert(0x3001001F, str(name));
    r.insert(0x3003001F, str(address));
    r.insert(0x3002001F, str(addressType));
    if (!smtp.isEmpty()) {
        r.insert(0x39FE001F, str(smtp));
    }
    return r;
}
}

class MapiGraphTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // A received message with everything Graph can hold
    void fullMessage()
    {
        Pst::Message m;
        m.properties.insert(0x0037, str(QStringLiteral(u"\u0001\u0005RE: Example subject")));
        m.properties.insert(0x0042, str(QStringLiteral("Alice Example")));
        m.properties.insert(0x0064, str(QStringLiteral("SMTP")));
        m.properties.insert(0x0065, str(QStringLiteral("alice@example.com")));
        m.properties.insert(0x0E07, int32(0x11));
        m.properties.insert(0x0017, int32(2));
        m.properties.insert(0x1090, int32(2));
        m.properties.insert(0x0036, int32(3));
        m.properties.insert(0x0E06, {Pst::PtypTime, QByteArray::fromHex("00403ef064a1d401")});
        m.properties.insert(0x0039, {Pst::PtypTime, QByteArray::fromHex("00403ef064a1d401")});
        // HTML in Windows-1252: 0x80 is the euro sign
        m.properties.insert(0x1013, {Pst::PtypBinary, QByteArray("<p>Price \x80 5</p>")});
        m.properties.insert(0x3FDE, int32(1252));
        m.properties.insert(0x8003, {Pst::PtypString | Pst::PtypMultiple, le32(2) + le32(12) + le32(20) + str(QStringLiteral("Acme")).data + str(QStringLiteral("Red")).data});
        m.recipients = {recipient(1, QStringLiteral("Bob Example"), QStringLiteral("bob@example.com"), QStringLiteral("SMTP")),
                        recipient(2, QStringLiteral("Carol Example"), QStringLiteral("/o=Example/cn=Carol"), QStringLiteral("EX"), QStringLiteral("carol@example.com")),
                        recipient(3, QStringLiteral("Dave Example"), QStringLiteral("dave@example.com"), QStringLiteral("SMTP")),
                        recipient(1, QStringLiteral("Erin Example"), QStringLiteral("/o=Example/cn=Erin"), QStringLiteral("EX"))};
        Pst::Attachment logo;
        logo.properties.insert(0x3705, int32(1));
        logo.properties.insert(0x3707, str(QStringLiteral("logo.png")));
        logo.properties.insert(0x370E, str(QStringLiteral("image/png")));
        logo.properties.insert(0x3712, str(QStringLiteral("logo@example")));
        logo.properties.insert(0x3714, int32(4));
        logo.properties.insert(0x3701, {Pst::PtypBinary, QByteArrayLiteral("PNGDATA")});
        Pst::Attachment doc;
        doc.properties.insert(0x3705, int32(1));
        doc.properties.insert(0x3704, str(QStringLiteral("REPORT~1.PDF")));
        doc.properties.insert(0x3701, {Pst::PtypBinary, QByteArrayLiteral("PDFDATA")});
        Pst::Attachment linked;
        linked.properties.insert(0x3705, int32(2));
        linked.properties.insert(0x3707, str(QStringLiteral("share.lnk")));
        Pst::Message inner;
        inner.properties.insert(0x0037, str(QStringLiteral("Attached example")));
        inner.properties.insert(0x1000, str(QStringLiteral("Inner text")));
        Pst::Attachment attached;
        attached.properties.insert(0x3705, int32(5));
        attached.properties.insert(0x3001, str(QStringLiteral("Attached example")));
        attached.embedded = std::make_shared<Pst::Message>(inner);
        m.attachments = {logo, doc, linked, attached};

        QHash<quint16, Pst::NamedProperty> names;
        Pst::NamedProperty keywords;
        keywords.guid = Pst::psPublicStrings;
        keywords.isString = true;
        keywords.name = QStringLiteral("Keywords");
        names.insert(0x8003, keywords);

        const MapiGraph::Import out = MapiGraph::toGraph(m, names);
        const QJsonObject j = out.message;
        QCOMPARE(j.value(QLatin1String("subject")).toString(), QStringLiteral("RE: Example subject"));
        QCOMPARE(j.value(QLatin1String("body")).toObject().value(QLatin1String("contentType")).toString(), QStringLiteral("html"));
        QCOMPARE(j.value(QLatin1String("body")).toObject().value(QLatin1String("content")).toString(), QStringLiteral(u"<p>Price € 5</p>"));
        QCOMPARE(j.value(QLatin1String("from")).toObject().value(QLatin1String("emailAddress")).toObject().value(QLatin1String("address")).toString(),
                 QStringLiteral("alice@example.com"));
        QCOMPARE(j.value(QLatin1String("toRecipients")).toArray().size(), 1);
        QCOMPARE(j.value(QLatin1String("ccRecipients")).toArray().first().toObject().value(QLatin1String("emailAddress")).toObject().value(QLatin1String("address")).toString(),
                 QStringLiteral("carol@example.com"));
        QCOMPARE(j.value(QLatin1String("bccRecipients")).toArray().size(), 1);
        QCOMPARE(j.value(QLatin1String("importance")).toString(), QStringLiteral("high"));
        QVERIFY(j.value(QLatin1String("isRead")).toBool());
        QCOMPARE(j.value(QLatin1String("flag")).toObject().value(QLatin1String("flagStatus")).toString(), QStringLiteral("flagged"));
        QCOMPARE(j.value(QLatin1String("categories")).toArray(), (QJsonArray{QStringLiteral("Acme"), QStringLiteral("Red")}));
        const QJsonArray props = j.value(QLatin1String("singleValueExtendedProperties")).toArray();
        QCOMPARE(props.size(), 4);
        // Read, not unsent: a received message
        QCOMPARE(props.at(0).toObject().value(QLatin1String("value")).toString(), QStringLiteral("1"));
        QCOMPARE(props.at(1).toObject().value(QLatin1String("value")).toString(), QStringLiteral("2019-01-01T00:00:00Z"));

        QCOMPARE(out.files.size(), 2);
        QCOMPARE(out.files.at(0).name, QStringLiteral("logo.png"));
        QVERIFY(out.files.at(0).isInline);
        QCOMPARE(out.files.at(0).contentId, QStringLiteral("logo@example"));
        QCOMPARE(out.files.at(1).name, QStringLiteral("REPORT~1.PDF"));
        QCOMPARE(out.files.at(1).contentType, QStringLiteral("application/octet-stream"));
        QVERIFY(!out.files.at(1).isInline);
        QCOMPARE(out.items.size(), 1);
        QCOMPARE(out.items.first().value(QLatin1String("item")).toObject().value(QLatin1String("subject")).toString(), QStringLiteral("Attached example"));
        // An Exchange-only recipient and the linked attachment are reported, not lost silently
        QCOMPARE(out.skipped.size(), 2);
        QVERIFY(out.skipped.at(0).contains(QLatin1String("Erin Example")));
        QVERIFY(out.skipped.at(1).contains(QLatin1String("share.lnk")));
    }

    // Plain text, unread, no dates; RTF without text is reported
    void plainAndRtf()
    {
        Pst::Message text;
        text.properties.insert(0x1000, str(QStringLiteral("Plain example")));
        const MapiGraph::Import a = MapiGraph::toGraph(text, {});
        QCOMPARE(a.message.value(QLatin1String("body")).toObject().value(QLatin1String("contentType")).toString(), QStringLiteral("text"));
        QVERIFY(!a.message.value(QLatin1String("isRead")).toBool());
        QCOMPARE(a.message.value(QLatin1String("importance")).toString(), QStringLiteral("normal"));
        QCOMPARE(a.message.value(QLatin1String("singleValueExtendedProperties")).toArray().size(), 1);
        QVERIFY(!a.message.contains(QLatin1String("from")));
        QVERIFY(a.skipped.isEmpty());
        QVERIFY(!a.message.contains(QLatin1String("internetMessageId")));
        text.properties.insert(0x1035, str(QStringLiteral("<one@example.com>")));
        QCOMPARE(MapiGraph::toGraph(text, {}).message.value(QLatin1String("internetMessageId")).toString(), QStringLiteral("<one@example.com>"));

        Pst::Message rtf;
        rtf.properties.insert(0x1009, {Pst::PtypBinary, QByteArrayLiteral("rtf")});
        // RTF that does not decompress: the log says so
        const QStringList skipped = MapiGraph::toGraph(rtf, {}).skipped;
        QCOMPARE(skipped.size(), 1);
        QVERIFY(skipped.first().startsWith(QLatin1String("body (damaged RTF")));
    }

    void mailClasses()
    {
        QVERIFY(MapiGraph::isMail(QStringLiteral("IPM.Note")));
        QVERIFY(MapiGraph::isMail(QStringLiteral("IPM.Note.SMIME")));
        QVERIFY(MapiGraph::isMail(QStringLiteral("ipm.schedule.meeting.request")));
        QVERIFY(MapiGraph::isMail(QStringLiteral("REPORT.IPM.Note.NDR")));
        QVERIFY(MapiGraph::isMail(QString()));
        QVERIFY(!MapiGraph::isMail(QStringLiteral("IPM.Contact")));
        QVERIFY(!MapiGraph::isMail(QStringLiteral("IPM.Appointment")));
        QVERIFY(!MapiGraph::isMail(QStringLiteral("IPM.Notes")));
    }

    void addresses()
    {
        QCOMPARE(MapiGraph::smtpAddress(QString(), QStringLiteral("a@example.com"), QStringLiteral("SMTP")), QStringLiteral("a@example.com"));
        QCOMPARE(MapiGraph::smtpAddress(QStringLiteral("b@example.com"), QStringLiteral("/o=Example"), QStringLiteral("EX")), QStringLiteral("b@example.com"));
        QCOMPARE(MapiGraph::smtpAddress(QString(), QStringLiteral("/o=Example"), QStringLiteral("EX")), QString());
        QCOMPARE(MapiGraph::smtpAddress(QString(), QStringLiteral("c@example.com"), QString()), QStringLiteral("c@example.com"));
    }

    void codepages()
    {
        QCOMPARE(Pst::decodeCodepage(QByteArray("Caf\xe9"), 28591), QStringLiteral(u"Café"));
        QCOMPARE(Pst::decodeCodepage(QByteArray("\x80"), 0), QStringLiteral(u"€"));
        QCOMPARE(Pst::decodeCodepage(QByteArray("Caf\xc3\xa9"), 65001), QStringLiteral(u"Café"));
        QCOMPARE(Pst::decodeCodepage(QByteArray("\x82\xa0"), 932), QStringLiteral(u"あ"));
    }
};

QTEST_GUILESS_MAIN(MapiGraphTest)
#include "mapigraphtest.moc"
