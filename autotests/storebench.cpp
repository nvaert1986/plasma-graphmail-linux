// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Phase 8 stress test (docs/PHASE8.md): a synthetic cache of many messages through the Store, and
// how long the things the windows do take on it. Not part of ctest; run it by hand:
//   build/bin/storebench [messages] [directory]

#include "searchquery.h"
#include "store.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QTextStream>

using namespace GraphMail;

namespace
{
QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

QString words(QRandomGenerator &random, const QStringList &vocabulary, int count)
{
    QStringList picked;
    for (int i = 0; i < count; ++i) {
        picked.append(vocabulary.at(int(random.bounded(quint32(vocabulary.size())))));
    }
    return picked.join(QLatin1Char(' '));
}

template<typename F>
double ms(F &&work, int repeat = 1)
{
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < repeat; ++i) {
        work();
    }
    return double(timer.nsecsElapsed()) / 1e6 / repeat;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const int total = argc > 1 ? QString::fromLatin1(argv[1]).toInt() : 500000;
    const QString dir = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QDir::tempPath() + QStringLiteral("/graphmail-storebench");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/cache.db");

    Store store;
    QString error;
    if (!store.open(path, Store::Mode::ReadWrite, &error)) {
        out() << "cannot open: " << error << Qt::endl;
        return 1;
    }
    // A vocabulary like mail: common words, rarer words, names
    QStringList vocabulary;
    for (const char *w : {"meeting", "report", "invoice", "project", "update", "server", "backup", "firewall", "customer", "order", "planning", "review",
                          "question", "offer", "contract", "migration", "storage", "license", "network", "monday", "friday", "agenda", "ticket", "status"}) {
        vocabulary.append(QString::fromLatin1(w));
    }
    for (int i = 0; i < 2000; ++i) {
        vocabulary.append(QStringLiteral("word%1").arg(i));
    }
    QRandomGenerator random(42);
    const int folderCount = 50;
    QList<qint64> folders;
    for (int f = 0; f < folderCount; ++f) {
        folders.append(store.upsertFolder({0, QStringLiteral("F%1").arg(f), {}, QStringLiteral("Folder %1").arg(f), f == 0 ? QStringLiteral("inbox") : QString(), 0, 0}));
    }

    // The inbox gets a fifth of everything (the biggest folder decides paging)
    const qint64 now = 1800000000000;
    const double insert = ms([&] {
        const int batch = 5000;
        for (int start = 0; start < total; start += batch) {
            store.inTransaction([&] {
                for (int i = start; i < std::min(total, start + batch); ++i) {
                    MessageRow m;
                    m.graphId = QStringLiteral("M%1").arg(i);
                    m.folder = random.bounded(5) == 0 ? folders.first() : folders.at(1 + int(random.bounded(quint32(folderCount - 1))));
                    m.received = now - qint64(i) * 60000;
                    m.sent = m.received - 1000;
                    m.subject = words(random, vocabulary, 6);
                    m.senderName = QStringLiteral("Person %1").arg(random.bounded(3000));
                    m.senderAddress = QStringLiteral("person%1@example.com").arg(random.bounded(3000));
                    m.toList = QStringLiteral("Someone %1 <someone%1@example.com>").arg(random.bounded(3000));
                    m.preview = words(random, vocabulary, 30);
                    m.isRead = random.bounded(10) != 0;
                    m.hasAttachments = random.bounded(8) == 0;
                    m.changeKey = QStringLiteral("ck");
                    m.conversation = QStringLiteral("c%1").arg(i / 3);
                    store.upsertMessage(m);
                    // Bodies offline for the newest tenth (the 90-day window of a busy mailbox)
                    if (i < total / 10) {
                        store.setBody(store.messageByGraphId(m.graphId)->id, QStringLiteral("b%1").arg(i), 1, 4000, words(random, vocabulary, 300));
                    }
                }
                return true;
            });
        }
    });
    out() << "messages: " << total << " in " << folderCount << " folders, bodies with text: " << total / 10 << Qt::endl;
    out() << QStringLiteral("insert: %1 s (%2 per second)").arg(insert / 1000, 0, 'f', 1).arg(int(total / (insert / 1000))) << Qt::endl;
    out() << QStringLiteral("cache file: %1 MB").arg(double(QFileInfo(path).size()) / 1e6, 0, 'f', 1) << Qt::endl;

    // What the windows do
    const qint64 inbox = folders.first();
    QList<ListRow> page;
    out() << QStringLiteral("first screen of the inbox (200 rows): %1 ms").arg(ms([&] {
                 page = store.listPage(inbox, std::nullopt, 200);
             }, 10), 0, 'f', 2)
          << Qt::endl;
    std::optional<PageCursor> cursor;
    const double deep = ms([&] {
        for (int i = 0; i < 400; ++i) {
            const QList<ListRow> p = store.listPage(inbox, cursor, 200);
            if (p.isEmpty()) {
                break;
            }
            cursor = PageCursor{p.last().received, p.last().id};
        }
    });
    out() << QStringLiteral("scrolling through the whole inbox (screens of 200): %1 ms").arg(deep, 0, 'f', 1) << Qt::endl;
    out() << QStringLiteral("unread counts of all folders: %1 ms").arg(ms([&] {
                 store.unreadCounts();
             }, 10), 0, 'f', 2)
          << Qt::endl;
    for (const QString &text : {QStringLiteral("firewall "), QStringLiteral("word1234 "), QStringLiteral("migr"), QStringLiteral("from:person12 "),
                                QStringLiteral("body:contract "), QStringLiteral("isread:no hasattachments:yes"), QStringLiteral("backup isread:no received>=2027-01-01")}) {
        int hits = 0;
        const double t = ms([&] {
            hits = int(store.search(SearchQuery::parse(text), {}, 500).size());
        }, 5);
        out() << QStringLiteral("search %1: %2 ms (%3 rows, at most 500)").arg(QLatin1Char('"') + text.trimmed() + QLatin1Char('"'), -40).arg(t, 0, 'f', 2).arg(hits) << Qt::endl;
    }
    return 0;
}
