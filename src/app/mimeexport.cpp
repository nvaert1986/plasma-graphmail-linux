// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mimeexport.h"
#include "mimemapi.h"
#include "blobstore.h"
#include "daemonclient.h"
#include "mailboxset.h"
#include "store.h"

#include <KLocalizedString>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimeZone>
#include <QUuid>

namespace GraphMail
{

namespace
{
bool isAscii(const QString &text)
{
    return std::all_of(text.cbegin(), text.cend(), [](QChar c) {
        return c.unicode() < 128;
    });
}

// "Name <address>; Name <address>" (the cache's lists) with the names encoded where needed
QByteArray addressList(const QString &list)
{
    QList<QByteArray> parts;
    const QStringList entries = list.split(QStringLiteral("; "), Qt::SkipEmptyParts);
    for (const QString &entry : entries) {
        const int open = entry.lastIndexOf(QLatin1Char('<'));
        if (open > 0 && entry.endsWith(QLatin1Char('>'))) {
            parts.append(MimeExport::encodeWord(entry.left(open).trimmed()) + " " + entry.mid(open).toUtf8());
        } else {
            parts.append(entry.trimmed().toUtf8());
        }
    }
    return parts.join(", ");
}

// Base64 in lines of 76, as RFC 2045 wants
QByteArray base64Lines(const QByteArray &data)
{
    const QByteArray encoded = data.toBase64();
    QByteArray out;
    for (qsizetype i = 0; i < encoded.size(); i += 76) {
        out += encoded.mid(i, 76) + "\r\n";
    }
    return out;
}

QByteArray rfc2822Date(qint64 ms)
{
    return QLocale::c().toString(QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC), QStringLiteral("ddd, dd MMM yyyy HH:mm:ss")).toLatin1() + " +0000";
}
}

QByteArray MimeExport::encodeWord(const QString &text)
{
    if (isAscii(text)) {
        return text.toUtf8();
    }
    return "=?UTF-8?B?" + text.toUtf8().toBase64() + "?=";
}

QByteArray MimeExport::rebuild(Store *store, BlobStore *blobs, qint64 message)
{
    const std::optional<MessageRow> m = store ? store->message(message) : std::nullopt;
    if (!m) {
        return {};
    }
    QByteArray head;
    const QString from = m->senderName.isEmpty() || m->senderName == m->senderAddress ? m->senderAddress : m->senderName + QStringLiteral(" <") + m->senderAddress + QLatin1Char('>');
    head += "From: " + addressList(from) + "\r\n";
    if (!m->toList.isEmpty()) {
        head += "To: " + addressList(m->toList) + "\r\n";
    }
    if (!m->ccList.isEmpty()) {
        head += "Cc: " + addressList(m->ccList) + "\r\n";
    }
    head += "Subject: " + encodeWord(m->subject) + "\r\n";
    head += "Date: " + rfc2822Date(m->sent > 0 ? m->sent : m->received) + "\r\n";
    if (!m->internetMessageId.isEmpty()) {
        head += "Message-ID: " + m->internetMessageId.toUtf8() + "\r\n";
    }
    head += "MIME-Version: 1.0\r\n";
    head += "X-GraphMail-Rebuilt: from the local copy; the original was not available\r\n";

    // The body as cached, or the preview when the text is not on this computer
    QString error;
    const std::optional<QByteArray> body = m->bodyBlob.isEmpty() || !blobs ? std::nullopt : blobs->get(m->bodyBlob, &error);
    const bool html = body && m->bodyType == 1;
    const QByteArray text = body ? *body : (m->preview + QStringLiteral("\n\n[") + i18nc("@info in an exported message", "The message text was not on this computer.") + QLatin1Char(']')).toUtf8();
    const QByteArray bodyPart = QByteArray("Content-Type: ") + (html ? "text/html" : "text/plain") + "; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n" + base64Lines(text);

    QList<QByteArray> attachments;
    const QList<AttachmentRow> rows = store->attachments(message);
    for (const AttachmentRow &a : rows) {
        const std::optional<QByteArray> data = a.blob.isEmpty() || !blobs ? std::nullopt : blobs->get(a.blob, &error);
        if (!data) {
            continue;
        }
        QByteArray part = "Content-Type: " + (a.contentType.isEmpty() ? QByteArray("application/octet-stream") : a.contentType.toLatin1()) + "; name=\"" + encodeWord(a.name) + "\"\r\n";
        part += QByteArray("Content-Disposition: ") + (a.isInline ? "inline" : "attachment") + "; filename*=UTF-8''" + a.name.toUtf8().toPercentEncoding() + "\r\n";
        if (!a.contentId.isEmpty()) {
            QString id = a.contentId;
            id.remove(QLatin1Char('<')).remove(QLatin1Char('>'));
            part += "Content-ID: <" + id.toUtf8() + ">\r\n";
        }
        part += "Content-Transfer-Encoding: base64\r\n\r\n" + base64Lines(*data);
        attachments.append(part);
    }
    if (attachments.isEmpty()) {
        return head + bodyPart;
    }
    const QByteArray boundary = "graphmail-" + QUuid::createUuid().toString(QUuid::Id128).toLatin1();
    QByteArray out = head + "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n\r\n";
    out += "--" + boundary + "\r\n" + bodyPart;
    for (const QByteArray &part : std::as_const(attachments)) {
        out += "--" + boundary + "\r\n" + part;
    }
    out += "--" + boundary + "--\r\n";
    return out;
}

QByteArray MimeExport::mboxEntry(const QByteArray &mime, const QString &sender, const QDateTime &when)
{
    // asctime, as the "From " line of mbox has it
    const QByteArray stamp = QLocale::c().toString(when.toUTC(), QStringLiteral("ddd MMM d HH:mm:ss yyyy")).toLatin1();
    QByteArray out = "From " + (sender.isEmpty() ? QByteArray("MAILER-DAEMON") : sender.toUtf8()) + " " + stamp + "\n";
    QByteArray text = mime;
    text.replace("\r\n", "\n");
    // mboxrd: every line that reads ">*From " gets one more ">"
    static const QRegularExpression fromLine(QStringLiteral("^(>*From )"), QRegularExpression::MultilineOption);
    QString escaped = QString::fromLatin1(text);
    escaped.replace(fromLine, QStringLiteral(">\\1"));
    out += escaped.toLatin1();
    if (!out.endsWith('\n')) {
        out += '\n';
    }
    return out + '\n';
}

MimeExporter::MimeExporter(MailboxSet *mailboxes, DaemonClient *daemon, const QString &mailboxKey, const QList<qint64> &messages, const QString &file, bool mbox,
                           QWidget *parent)
    : QObject(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
    , m_mailboxKey(mailboxKey)
    , m_messages(messages)
    , m_file(file)
    , m_mbox(mbox)
    , m_parent(parent)
{
}

void MimeExporter::start()
{
    // The daemon writes each original here first
    m_tempDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/export-") + QUuid::createUuid().toString(QUuid::Id128);
    QDir().mkpath(m_tempDir);
    if (m_messages.size() > 1 && m_parent) {
        m_progress = new QProgressDialog(i18nc("@info:progress", "Exporting messages..."), i18nc("@action:button", "Cancel"), 0, int(m_messages.size()), m_parent);
        m_progress->setWindowModality(Qt::WindowModal);
        m_progress->setMinimumDuration(500);
        connect(m_progress, &QProgressDialog::canceled, this, [this] {
            m_cancelled = true;
        });
    }
    next();
}

void MimeExporter::next()
{
    if (m_progress) {
        m_progress->setValue(m_index);
    }
    if (m_cancelled || m_index >= m_messages.size()) {
        finish();
        return;
    }
    const qint64 message = m_messages.at(m_index++);
    if (m_offline || !m_daemon->isRunning()) {
        add(MimeExport::rebuild(m_mailboxes->store(m_mailboxKey), m_mailboxes->blobs(m_mailboxKey), message), message, false);
        QMetaObject::invokeMethod(this, &MimeExporter::next, Qt::QueuedConnection);
        return;
    }
    const QString temp = m_tempDir + QStringLiteral("/%1.eml").arg(message);
    const QJsonObject args{{QStringLiteral("message"), message}, {QStringLiteral("file"), temp}};
    m_daemon->query(m_mailboxKey.section(QLatin1Char('/'), 0, 0), m_mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("messageMime"),
                    QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this, [this, message, temp](const QString &json) {
                        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                        QFile file(temp);
                        if (answer.value(QLatin1String("ok")).toBool() && file.open(QIODevice::ReadOnly)) {
                            add(file.readAll(), message, true);
                            file.close();
                            file.remove();
                        } else {
                            // No answer (offline, an outage): the rest from the cache at once
                            m_offline = true;
                            add(MimeExport::rebuild(m_mailboxes->store(m_mailboxKey), m_mailboxes->blobs(m_mailboxKey), message), message, false);
                        }
                        next();
                    });
}

void MimeExporter::add(const QByteArray &mime, qint64 message, bool original)
{
    if (mime.isEmpty()) {
        return;
    }
    if (original) {
        ++m_original;
    } else {
        ++m_rebuilt;
    }
    const Store *store = m_mailboxes->store(m_mailboxKey);
    const std::optional<MessageRow> m = store ? store->message(message) : std::nullopt;
    if (!m_mbox && m_file.endsWith(QLatin1String(".msg"), Qt::CaseInsensitive)) {
        // Outlook's own format: the MIME as MAPI properties, with what the cache knows beside it
        MimeMapi::Extras extras;
        if (m) {
            extras.read = m->isRead;
            extras.flag = m->flag;
            extras.importance = m->importance;
            extras.categories = m->categories;
            extras.received = m->received;
        }
        m_out = MimeMapi::mimeToMsg(mime, extras);
        return;
    }
    if (!m_mbox) {
        m_out = mime;
        return;
    }
    m_out += MimeExport::mboxEntry(mime, m ? m->senderAddress : QString(), QDateTime::fromMSecsSinceEpoch(m ? m->received : 0, QTimeZone::UTC));
}

void MimeExporter::finish()
{
    if (m_progress) {
        m_progress->deleteLater();
    }
    QDir(m_tempDir).removeRecursively();
    bool ok = !m_cancelled && (m_original + m_rebuilt) > 0;
    if (ok) {
        QSaveFile file(m_file);
        ok = file.open(QIODevice::WriteOnly) && file.write(m_out) == m_out.size() && file.commit();
    }
    qWarning() << "export test:" << ok << m_original << "original," << m_rebuilt << "rebuilt";
    Q_EMIT finished(ok, m_original, m_rebuilt, m_file);
    deleteLater();
}

}
