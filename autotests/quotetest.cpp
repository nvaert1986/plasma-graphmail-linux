// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "quote.h"

#include <QTest>

using namespace GraphMail;

class QuoteTest : public QObject
{
    Q_OBJECT

private:
    static QStringList addresses(const QList<OutgoingRecipient> &list)
    {
        QStringList result;
        for (const OutgoingRecipient &r : list) {
            result.append(r.address);
        }
        return result;
    }

    static MessageRow message(const QString &from, const QString &to, const QString &cc)
    {
        MessageRow m;
        m.senderName = QStringLiteral("Sender");
        m.senderAddress = from;
        m.toList = to;
        m.ccList = cc;
        m.subject = QStringLiteral("Plans");
        return m;
    }

private Q_SLOTS:
    void parsesTypedAndCachedRecipients()
    {
        QStringList invalid;
        const QList<OutgoingRecipient> list = parseRecipients(
            QStringLiteral("Doe, Jane <jane@example.com>; bob@example.com, \"Smith; Ann\" <ann@example.org>; not an address; "), &invalid);
        QCOMPARE(addresses(list), (QStringList{QStringLiteral("jane@example.com"), QStringLiteral("bob@example.com"), QStringLiteral("ann@example.org")}));
        QCOMPARE(list.at(0).name, QStringLiteral("Doe, Jane"));
        QCOMPARE(list.at(2).name, QStringLiteral("Smith; Ann"));
        QCOMPARE(invalid, QStringList{QStringLiteral("not an address")});
        QCOMPARE(formatRecipient(list.at(0)), QStringLiteral("Doe, Jane <jane@example.com>"));
        QCOMPARE(formatRecipient(list.at(1)), QStringLiteral("bob@example.com"));
        QVERIFY(parseRecipients(QStringLiteral("  ;  ")).isEmpty());
    }

    // The attachment reminder's words, and the ones that only look like them
    void attachmentReminder()
    {
        for (const char *text : {"See the attached file", "I attach the offer", "De offerte zit in de bijlage.", "Bijgevoegd vind je het rapport",
                                 "Im Anhang die Rechnung", "Veuillez trouver ci-joint", "la pièce jointe", "Two ATTACHMENTS follow"}) {
            QVERIFY2(Quote::mentionsAttachment(QString::fromUtf8(text)), text);
        }
        for (const char *text : {"Attaché at the embassy", "Let us meet tomorrow", "bijlagenummer staat erop", "attachable"}) {
            QVERIFY2(!Quote::mentionsAttachment(QString::fromUtf8(text)), text);
        }
    }

    void subjectsDoNotStack()
    {
        QCOMPARE(Quote::subject(QStringLiteral("reply"), QStringLiteral("Plans")), QStringLiteral("RE: Plans"));
        QCOMPARE(Quote::subject(QStringLiteral("replyAll"), QStringLiteral("RE: Plans")), QStringLiteral("RE: Plans"));
        QCOMPARE(Quote::subject(QStringLiteral("reply"), QStringLiteral("AW: Plans")), QStringLiteral("AW: Plans"));
        QCOMPARE(Quote::subject(QStringLiteral("forward"), QStringLiteral("Fwd: Plans")), QStringLiteral("Fwd: Plans"));
        QCOMPARE(Quote::subject(QStringLiteral("forward"), QStringLiteral("RE: Plans")), QStringLiteral("FW: RE: Plans"));
    }

    void replyRecipients()
    {
        const QStringList own{QStringLiteral("me@example.com")};
        const MessageRow m = message(QStringLiteral("boss@example.com"),
                                     QStringLiteral("Me <me@example.com>; Colleague <colleague@example.com>"),
                                     QStringLiteral("cc@example.com; BOSS@example.com"));
        QList<OutgoingRecipient> to;
        QList<OutgoingRecipient> cc;
        Quote::recipients(m, false, own, &to, &cc);
        QCOMPARE(addresses(to), QStringList{QStringLiteral("boss@example.com")});
        QVERIFY(cc.isEmpty());

        to.clear();
        Quote::recipients(m, true, own, &to, &cc);
        // The user is left out, the sender is not doubled in Cc
        QCOMPARE(addresses(to), (QStringList{QStringLiteral("boss@example.com"), QStringLiteral("colleague@example.com")}));
        QCOMPARE(addresses(cc), QStringList{QStringLiteral("cc@example.com")});

        // Answering one's own sent message goes to its recipients
        to.clear();
        cc.clear();
        Quote::recipients(message(QStringLiteral("me@example.com"), QStringLiteral("client@example.com"), {}), false, own, &to, &cc);
        QCOMPARE(addresses(to), QStringList{QStringLiteral("client@example.com")});

        // A message to oneself is answered to oneself
        to.clear();
        Quote::recipients(message(QStringLiteral("me@example.com"), QStringLiteral("Me <me@example.com>"), {}), true, own, &to, &cc);
        QCOMPARE(addresses(to), QStringList{QStringLiteral("me@example.com")});
    }

    void headerBlock()
    {
        const MessageRow m = message(QStringLiteral("boss@example.com"), QStringLiteral("me@example.com"), QStringLiteral("cc@example.com"));
        const QString text = Quote::headerText(m);
        QVERIFY(text.contains(QLatin1String("From: Sender <boss@example.com>")));
        QVERIFY(text.contains(QLatin1String("Cc: cc@example.com")));
        QVERIFY(text.contains(QLatin1String("Subject: Plans")));
        const QString html = Quote::headerHtml(m);
        QVERIFY(html.contains(QLatin1String("Sender &lt;boss@example.com&gt;")));
        QVERIFY(html.contains(QLatin1String("divRplyFwdMsg")));
        // Without a cached body there is nothing to quote (the composer asks the daemon)
        QVERIFY(Quote::originalHtml(m, nullptr).isEmpty());
    }
};

QTEST_GUILESS_MAIN(QuoteTest)

#include "quotetest.moc"
