// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "profilephotos.h"
#include "accesstext.h"
#include "daemonclient.h"
#include "mailboxset.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QBuffer>
#include <QFileDialog>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace GraphMail
{

ProfilePhotos::ProfilePhotos(MailboxSet *mailboxes, DaemonClient *daemon, QObject *parent)
    : QObject(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
{
}

void ProfilePhotos::load()
{
    if (!m_daemon->isRunning()) {
        return;
    }
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        const QString key = box.key();
        m_daemon->query(box.account, box.mailbox, QStringLiteral("mailboxPhoto"), QStringLiteral("{}"), this, [this, key](const QString &base64) {
            const QImage photo = QImage::fromData(QByteArray::fromBase64(base64.toLatin1()));
            if (!photo.isNull() || !m_mailboxes->photo(key).isNull()) {
                m_mailboxes->setPhoto(key, photo);
            }
        });
    }
}

void ProfilePhotos::change(QWidget *parent, const QString &mailboxKey)
{
    QStringList patterns;
    for (const char *format : {"jpg", "jpeg", "png", "webp", "bmp", "gif"}) {
        patterns.append(QStringLiteral("*.") + QLatin1String(format));
    }
    const QString file = QFileDialog::getOpenFileName(parent, i18nc("@title:window", "Change Photo"), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
                                                      i18nc("@item:inlistbox file type", "Pictures (%1)", patterns.join(QLatin1Char(' '))));
    if (file.isEmpty()) {
        return;
    }
    QImageReader reader(file);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull()) {
        KMessageBox::error(parent, i18nc("@info %1 a file", "%1 is not a picture Graph Mail can read.", file));
        return;
    }
    send(mailboxKey, image, parent);
}

void ProfilePhotos::send(const QString &mailboxKey, const QImage &image, QWidget *parent)
{
    // Square from the middle and at most 648 pixels, as Outlook uploads it; JPEG
    const int side = std::min(image.width(), image.height());
    QImage square = image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side);
    if (side > 648) {
        square = square.scaled(648, 648, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    square.convertToFormat(QImage::Format_RGB32).save(&buffer, "JPEG", 90);
    const QJsonObject args{{QStringLiteral("image"), QString::fromLatin1(jpeg.toBase64())}};
    m_daemon->query(mailboxKey.section(QLatin1Char('/'), 0, 0), mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("mailboxPhotoSet"),
                    QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this, [this, mailboxKey, square, parent](const QString &json) {
                        answered(mailboxKey, json, square, parent);
                    });
}

void ProfilePhotos::remove(QWidget *parent, const QString &mailboxKey)
{
    if (parent
        && KMessageBox::warningContinueCancel(parent, i18nc("@info", "Remove your photo? Outlook, Teams and Microsoft 365 then show your initials."),
                                              i18nc("@title:window", "Remove Photo"), KStandardGuiItem::remove())
            != KMessageBox::Continue) {
        return;
    }
    m_daemon->query(mailboxKey.section(QLatin1Char('/'), 0, 0), mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("mailboxPhotoRemove"), QStringLiteral("{}"), this,
                    [this, mailboxKey, parent](const QString &json) {
                        answered(mailboxKey, json, QImage(), parent);
                    });
}

void ProfilePhotos::answered(const QString &mailboxKey, const QString &json, const QImage &image, QWidget *parent)
{
    const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
    const bool ok = answer.value(QLatin1String("ok")).toBool();
    qWarning() << "photo test:" << ok << answer.value(QLatin1String("status")).toInt() << answer.value(QLatin1String("detail")).toString();
    QString text;
    if (ok) {
        m_mailboxes->setPhoto(mailboxKey, image);
        text = image.isNull() ? i18nc("@info:status", "Photo removed.") : i18nc("@info:status", "Photo changed. Outlook and Teams show it within a few hours.");
    } else if (answer.value(QLatin1String("status")).toInt() == 403) {
        // The app's sign-in does not include User.ReadWrite
        text = i18nc("@info", "Microsoft 365 does not let Graph Mail change this photo: its sign-in does not include changing your profile. Change it on your <a href=\"https://myaccount.microsoft.com\">Microsoft account page</a> instead.");
    } else {
        text = accessProblemText(answer.isEmpty() ? QStringLiteral("network") : answer.value(QLatin1String("problem")).toString(), mailboxKey);
    }
    if (!ok && parent) {
        KMessageBox::error(parent, text, i18nc("@title:window", "Photo"), KMessageBox::AllowLink);
    }
    Q_EMIT done(mailboxKey, ok, text);
}

void ProfilePhotos::setForTests(const QString &mailboxKey, const QString &file)
{
    if (file.isEmpty()) {
        m_daemon->query(mailboxKey.section(QLatin1Char('/'), 0, 0), mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("mailboxPhotoRemove"), QStringLiteral("{}"), this,
                        [this, mailboxKey](const QString &json) {
                            answered(mailboxKey, json, QImage(), nullptr);
                        });
        return;
    }
    QImageReader reader(file);
    reader.setAutoTransform(true);
    send(mailboxKey, reader.read(), nullptr);
}

}
