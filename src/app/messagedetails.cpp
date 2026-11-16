// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "messagedetails.h"
#include "mailboxset.h"
#include "mimeexport.h"
#include "store.h"

#include <KLocalizedString>

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

namespace GraphMail
{

MessageDetailsDialog::MessageDetailsDialog(const QString &subject, QWidget *parent)
    : QDialog(parent)
    , m_text(new QPlainTextEdit(this))
{
    setWindowTitle(i18nc("@title:window %1 a subject", "Message Details: %1", subject));
    setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(i18nc("@label", "Internet headers:"), this));
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_text->setPlainText(i18nc("@info", "Loading..."));
    layout->addWidget(m_text);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton *copy = buttons->addButton(i18nc("@action:button", "Copy"), QDialogButtonBox::ActionRole);
    copy->setIcon(QIcon::fromTheme(QStringLiteral("edit-copy")));
    connect(copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_text->toPlainText());
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    resize(800, 560);
}

QString MessageDetailsDialog::headerBlock(const QByteArray &mime)
{
    qsizetype end = mime.indexOf("\r\n\r\n");
    const qsizetype lf = mime.indexOf("\n\n");
    if (end < 0 || (lf >= 0 && lf < end)) {
        end = lf;
    }
    QByteArray head = end < 0 ? mime : mime.left(end);
    head.replace("\r\n", "\n");
    // Headers are ASCII by the rules; what is not (raw UTF-8 in old mail) is shown as UTF-8
    return QString::fromUtf8(head);
}

void MessageDetailsDialog::show(const QString &headers, bool original)
{
    QString text = headers;
    if (!original) {
        text = i18nc("@info", "Microsoft 365 did not answer: these are not the message's own headers but the ones of this computer's copy.") + QStringLiteral("\n\n") + text;
    }
    m_text->setPlainText(text);
}

MessageDetailsDialog *MessageDetailsDialog::open(QWidget *parent, MailboxSet *mailboxes, DaemonClient *daemon, const QString &mailboxKey, qint64 message)
{
    const Store *store = mailboxes->store(mailboxKey);
    const std::optional<MessageRow> row = store ? store->message(message) : std::nullopt;
    auto *dialog = new MessageDetailsDialog(row ? row->subject : QString(), parent);
    dialog->QDialog::show();
    QPointer<MessageDetailsDialog> guard(dialog);
    MimeExport::fetch(mailboxes, daemon, mailboxKey, message, dialog, [guard](const QByteArray &mime, bool original) {
        if (guard) {
            guard->show(headerBlock(mime), original);
        }
    });
    return dialog;
}

}
