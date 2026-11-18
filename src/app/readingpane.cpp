// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "readingpane.h"
#include "graphsynclink.h"
#include "attachmentpreview.h"
#include "attachmentstrip.h"
#include "avatar.h"
#include "blobstore.h"
#include "daemonclient.h"
#include "eventdialog.h"
#include "icsdialog.h"
#include "mailboxset.h"
#include "mailview.h"
#include "readreceipts.h"
#include "store.h"
#include "tnefdialog.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KMessageBox>
#include <KMessageWidget>
#include <KSharedConfig>

#include <QAction>
#include <QDesktopServices>
#include <QDir>
#include <QTimeZone>
#include <QMenu>
#include <QJsonObject>
#include <QJsonDocument>
#include <QInputDialog>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <KIO/JobUiDelegateFactory>
#include <KIO/OpenUrlJob>
#include <MimeTreeParserWidgets/MessageViewerDialog>
#include "mimemapi.h"
#include <QCoreApplication>
#include <QWebEnginePage>

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
KConfigGroup readingGroup()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Reading"));
}

bool isTrusted(const QString &sender)
{
    return !sender.isEmpty() && readingGroup().readEntry("TrustedSenders", QStringList()).contains(sender.toLower());
}
}

class ReadingPane::AvatarLabel : public QWidget
{
public:
    explicit AvatarLabel(QWidget *parent)
        : QWidget(parent)
    {
        setFixedSize(40, 40);
    }
    void set(const QString &name, const QString &address)
    {
        m_name = name;
        m_address = address;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        Avatar::paint(&painter, rect(), m_name, m_address);
    }

private:
    QString m_name;
    QString m_address;
};

ReadingPane::ReadingPane(QWidget *parent)
    : QWidget(parent)
    , m_stack(new QStackedWidget(this))
    , m_empty(new QLabel(i18nc("@info reading pane without a message", "Select a message to read it"), this))
    , m_subject(new QLabel(this))
    , m_avatar(new AvatarLabel(this))
    , m_from(new QLabel(this))
    , m_recipients(new QLabel(this))
    , m_date(new QLabel(this))
    , m_remoteBar(new KMessageWidget(this))
    , m_notice(new KMessageWidget(this))
    , m_meeting(new KMessageWidget(this))
    , m_unsubscribeBar(new KMessageWidget(this))
    , m_receiptBar(new KMessageWidget(this))
    , m_attachments(new AttachmentStrip(this))
    , m_body(new MailView(this))
{
    // The body's right-click menu: New Message To... and Print...
    connect(m_body, &MailView::mailtoRequested, this, &ReadingPane::mailtoRequested);
    connect(m_body, &MailView::printRequested, this, &ReadingPane::printRequested);
    m_notice->setMessageType(KMessageWidget::Error);
    m_notice->setWordWrap(true);
    m_notice->hide();
    m_meeting->setWordWrap(true);
    m_meeting->setCloseButtonVisible(false);
    m_meeting->hide();
    connect(m_attachments, &AttachmentStrip::openRequested, this, &ReadingPane::openAttachment);
    connect(m_attachments, &AttachmentStrip::previewRequested, this, &ReadingPane::previewAttachment);
    connect(m_attachments, &AttachmentStrip::saveRequested, this, [this](qint64 attachment) {
        AttachmentRow row;
        contentOf(attachment, &row);
        const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).filePath(row.name);
        const QString path = QFileDialog::getSaveFileName(this, i18nc("@title:window", "Save Attachment"), suggested);
        if (!path.isEmpty()) {
            saveAttachment(attachment, path);
        }
    });
    connect(m_attachments, &AttachmentStrip::saveToCloudRequested, this, [this](qint64 attachment) {
        AttachmentRow row;
        contentOf(attachment, &row);
        // The folder used last when it is still synced, else the own OneDrive; Graph Sync's
        // locations are in the dialog's Places
        KConfigGroup group = KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Reading"));
        QString folder = group.readEntry("CloudSaveFolder");
        if (folder.isEmpty() || !QDir(folder).exists() || GraphSyncLink::locationOf(folder).id.isEmpty()) {
            const QList<GraphSyncLink::Location> all = GraphSyncLink::locations();
            folder = all.isEmpty() ? QString() : all.constFirst().localPath;
            for (const GraphSyncLink::Location &l : all) {
                if (l.kind == QLatin1String("personal")) {
                    folder = l.localPath;
                    break;
                }
            }
        }
        const QString path = QFileDialog::getSaveFileName(this, i18nc("@title:window", "Save to OneDrive or SharePoint"), QDir(folder).filePath(row.name));
        if (path.isEmpty()) {
            return;
        }
        group.writeEntry("CloudSaveFolder", QFileInfo(path).absolutePath());
        saveAttachment(attachment, path);
    });
    connect(m_attachments, &AttachmentStrip::saveAllRequested, this, [this] {
        saveAllAttachments();
    });
    // Blend into the header instead of a white box
    m_attachments->viewport()->setAutoFillBackground(false);
    m_attachments->setAutoFillBackground(false);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setEnabled(false);

    QFont subjectFont = m_subject->font();
    subjectFont.setPointSizeF(subjectFont.pointSizeF() * 1.35);
    subjectFont.setBold(true);
    m_subject->setFont(subjectFont);
    m_subject->setWordWrap(true);
    m_subject->setTextInteractionFlags(Qt::TextSelectableByMouse);
    for (QLabel *label : {m_from, m_recipients, m_date}) {
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    m_recipients->setWordWrap(true);
    // Sender and date wrap instead of setting a minimum width for the whole reading pane: a long
    // sender with the long date squeezed the folders and the list in a narrow window (0.14.1)
    m_from->setWordWrap(true);
    m_recipients->setForegroundRole(QPalette::PlaceholderText);
    m_date->setForegroundRole(QPalette::PlaceholderText);
    // Outlook's bar: "Click here to download pictures..."
    m_remoteBar->setText(i18nc("@info", "Pictures and other content from the internet were blocked to protect your privacy."));
    m_remoteBar->setMessageType(KMessageWidget::Information);
    m_remoteBar->setCloseButtonVisible(false);
    m_remoteBar->setWordWrap(true);
    auto *loadOnce = new QAction(QIcon::fromTheme(QStringLiteral("download")), i18nc("@action", "Load Images"), m_remoteBar);
    connect(loadOnce, &QAction::triggered, this, [this] {
        m_allowRemote = true;
        showBody();
    });
    auto *trust = new QAction(QIcon::fromTheme(QStringLiteral("security-high")), i18nc("@action", "Always for This Sender"), m_remoteBar);
    connect(trust, &QAction::triggered, this, [this] {
        KConfigGroup group = readingGroup();
        QStringList trusted = group.readEntry("TrustedSenders", QStringList());
        if (!trusted.contains(m_sender.toLower())) {
            trusted.append(m_sender.toLower());
            group.writeEntry("TrustedSenders", trusted);
            group.sync();
        }
        m_allowRemote = true;
        showBody();
    });
    m_remoteBar->addAction(loadOnce);
    m_remoteBar->addAction(trust);
    m_remoteBar->hide();
    // Outlook's "Unsubscribe" above a newsletter
    m_unsubscribeBar->setCloseButtonVisible(false);
    m_unsubscribeBar->setWordWrap(true);
    auto *unsubscribeAction = new QAction(QIcon::fromTheme(QStringLiteral("list-remove-user")), i18nc("@action", "Unsubscribe"), m_unsubscribeBar);
    connect(unsubscribeAction, &QAction::triggered, this, &ReadingPane::unsubscribe);
    m_unsubscribeBar->addAction(unsubscribeAction);
    m_unsubscribeBar->hide();
    // Outlook's "The sender requested a read receipt. Send a receipt?"
    m_receiptBar->setMessageType(KMessageWidget::Information);
    m_receiptBar->setCloseButtonVisible(false);
    m_receiptBar->setWordWrap(true);
    m_receiptBar->setText(i18nc("@info", "The sender asked for a read receipt."));
    auto *sendReceipt = new QAction(QIcon::fromTheme(QStringLiteral("mail-receipt")), i18nc("@action", "Send Receipt"), m_receiptBar);
    connect(sendReceipt, &QAction::triggered, this, [this] {
        answerReceipt(true);
    });
    auto *noReceipt = new QAction(QIcon::fromTheme(QStringLiteral("dialog-cancel")), i18nc("@action", "Don't Send"), m_receiptBar);
    connect(noReceipt, &QAction::triggered, this, [this] {
        answerReceipt(false);
    });
    m_receiptBar->addAction(sendReceipt);
    m_receiptBar->addAction(noReceipt);
    m_receiptBar->hide();

    auto *message = new QWidget(this);
    auto *layout = new QVBoxLayout(message);
    layout->setContentsMargins(16, 12, 16, 0);
    layout->addWidget(m_subject);
    m_categoryLabels = new QLabel(message);
    m_categoryLabels->setTextFormat(Qt::RichText);
    m_categoryLabels->hide();
    layout->addWidget(m_categoryLabels);
    auto *sender = new QHBoxLayout;
    sender->addWidget(m_avatar, 0, Qt::AlignTop);
    auto *senderText = new QVBoxLayout;
    senderText->setSpacing(2);
    auto *fromLine = new QHBoxLayout;
    fromLine->addWidget(m_from, 1);
    fromLine->addWidget(m_date, 0, Qt::AlignRight | Qt::AlignTop);
    senderText->addLayout(fromLine);
    senderText->addWidget(m_recipients);
    sender->addLayout(senderText, 1);
    layout->addLayout(sender);
    auto *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    layout->addWidget(m_attachments);
    layout->addWidget(line);
    layout->addWidget(m_notice);
    layout->addWidget(m_meeting);
    layout->addWidget(m_receiptBar);
    layout->addWidget(m_unsubscribeBar);
    layout->addWidget(m_remoteBar);
    layout->addWidget(m_body, 1);

    m_stack->addWidget(m_empty);
    m_stack->addWidget(message);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(m_stack);
}

ReadingPane::~ReadingPane() = default;

qint64 ReadingPane::message() const
{
    return m_message;
}

void ReadingPane::setContext(DaemonClient *daemon, MailboxSet *mailboxes)
{
    m_daemon = daemon;
    m_mailboxes = mailboxes;
}

QString ReadingPane::mailboxKey() const
{
    if (!m_mailboxes) {
        return {};
    }
    const QList<MailboxInfo> boxes = m_mailboxes->mailboxes();
    for (const MailboxInfo &box : boxes) {
        if (m_mailboxes->store(box.key()) == m_store) {
            return box.key();
        }
    }
    return {};
}

void ReadingPane::showMeeting(const MessageRow &message)
{
    const QString key = mailboxKey();
    if (!m_daemon || key.isEmpty()) {
        return;
    }
    const qint64 id = message.id;
    const bool request = message.itemType == 1;
    m_daemon->query(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), QStringLiteral("meetingEvent"),
                    QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("message"), id}}).toJson(QJsonDocument::Compact)), this,
                    [this, id, request, key](const QString &json) {
                        if (id != m_message || json.isEmpty()) {
                            return;
                        }
                        const QJsonObject event = QJsonDocument::fromJson(json.toUtf8()).object().value(QLatin1String("event")).toObject();
                        const QString eventId = event.value(QLatin1String("id")).toString();
                        if (eventId.isEmpty()) {
                            return;
                        }
                        m_meetingEventId = request && !event.value(QLatin1String("isCancelled")).toBool() ? eventId : QString();
                        // The time in local time (asked in UTC); all-day events by date
                        auto when = [&event](const char *key) {
                            QDateTime t = QDateTime::fromString(event.value(QLatin1String(key)).toObject().value(QLatin1String("dateTime")).toString().left(19), Qt::ISODate);
                            t.setTimeZone(QTimeZone::UTC);
                            return t;
                        };
                        const bool allDay = event.value(QLatin1String("isAllDay")).toBool();
                        const QDateTime start = allDay ? when("start") : when("start").toLocalTime();
                        const QDateTime end = allDay ? when("end") : when("end").toLocalTime();
                        const QString format = QLocale().name() == QLatin1String("C") ? QStringLiteral("ddd dd-MM-yyyy HH:mm") : QLocale().dateTimeFormat(QLocale::LongFormat);
                        QString time = allDay ? QLocale().toString(start.date(), QStringLiteral("dddd d MMMM yyyy"))
                                              : i18nc("@info meeting start - end", "%1 - %2", QLocale().toString(start, format), QLocale().toString(end.time(), QStringLiteral("HH:mm")));
                        const QString location = event.value(QLatin1String("location")).toObject().value(QLatin1String("displayName")).toString();
                        if (!location.isEmpty()) {
                            time += QStringLiteral(", ") + location;
                        }
                        const QString response = event.value(QLatin1String("responseStatus")).toObject().value(QLatin1String("response")).toString();
                        const QList<QAction *> old = m_meeting->actions();
                        for (QAction *a : old) {
                            m_meeting->removeAction(a);
                            a->deleteLater();
                        }
                        if (event.value(QLatin1String("isCancelled")).toBool() || !request) {
                            m_meeting->setMessageType(KMessageWidget::Warning);
                            const bool cancelled = event.value(QLatin1String("isCancelled")).toBool();
                            m_meeting->setText(cancelled ? i18nc("@info %1 the meeting's time", "This meeting was cancelled: %1", time) : i18nc("@info %1 the meeting's time", "Meeting: %1", time));
                            const Store *store = m_mailboxes->store(key);
                            const std::optional<EventRow> cached = store ? store->eventByGraphId(eventId) : std::nullopt;
                            if (cancelled && cached) {
                                const qint64 row = cached->id;
                                auto *remove = new QAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18nc("@action", "Remove from Calendar"), m_meeting);
                                connect(remove, &QAction::triggered, this, [this, key, row] {
                                    m_daemon->deleteEvent(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), row, false, {});
                                    m_meeting->setText(i18nc("@info", "Removed from the calendar."));
                                    const QList<QAction *> actions = m_meeting->actions();
                                    for (QAction *a : actions) {
                                        m_meeting->removeAction(a);
                                    }
                                });
                                m_meeting->addAction(remove);
                            }
                        } else {
                            m_meeting->setMessageType(KMessageWidget::Information);
                            const QString answered = response == QLatin1String("accepted") ? i18nc("@info", "You accepted.")
                                : response == QLatin1String("tentativelyAccepted")        ? i18nc("@info", "You accepted tentatively.")
                                : response == QLatin1String("declined")                   ? i18nc("@info", "You declined.")
                                                                                           : i18nc("@info", "Please respond.");
                            m_meeting->setText(i18nc("@info meeting time, then the answer so far", "Meeting: %1. %2", time, answered));
                            const struct {
                                const char *action;
                                const char *icon;
                                KLocalizedString text;
                            } answers[] = {{"accept", "dialog-ok-apply", ki18nc("@action", "Accept")},
                                           {"tentativelyAccept", "dialog-question", ki18nc("@action", "Tentative")},
                                           {"decline", "dialog-cancel", ki18nc("@action", "Decline")}};
                            for (const auto &answer : answers) {
                                auto *a = new QAction(QIcon::fromTheme(QLatin1String(answer.icon)), answer.text.toString(), m_meeting);
                                const QString action = QLatin1String(answer.action);
                                connect(a, &QAction::triggered, this, [this, eventId, action] {
                                    respondToMeeting(eventId, action);
                                });
                                m_meeting->addAction(a);
                            }
                            if (event.value(QLatin1String("allowNewTimeProposals")).toBool(true)) {
                                auto *propose = new QAction(QIcon::fromTheme(QStringLiteral("appointment-new")), i18nc("@action", "Propose New Time..."), m_meeting);
                                connect(propose, &QAction::triggered, this, [this, eventId, start, end] {
                                    if (const std::optional<EventDialog::NewTime> t = EventDialog::askNewTime(this, start, end)) {
                                        proposeNewTime(eventId, *t);
                                    }
                                });
                                m_meeting->addAction(propose);
                            }
                        }
                        m_meeting->animatedShow();
                    });
}

bool ReadingPane::respondToMeetingForTests(const QString &action, const QString &comment)
{
    if (m_meetingEventId.isEmpty()) {
        return false;
    }
    respondToMeeting(m_meetingEventId, action, comment.isEmpty() ? 0 : 1, comment);
    return true;
}

void ReadingPane::respondToMeeting(const QString &eventGraphId, const QString &action, int sendMode, const QString &givenComment)
{
    const QString key = mailboxKey();
    if (key.isEmpty()) {
        return;
    }
    if (sendMode < 0) {
        QMenu menu(this);
        QAction *now = menu.addAction(i18nc("@action:inmenu", "Send the Response Now"));
        QAction *edit = menu.addAction(i18nc("@action:inmenu", "Edit the Response Before Sending..."));
        menu.addAction(i18nc("@action:inmenu", "Do Not Send a Response"));
        QAction *chosen = menu.exec(QCursor::pos());
        if (!chosen) {
            return;
        }
        sendMode = chosen == now ? 0 : chosen == edit ? 1 : 2;
    }
    QString comment = givenComment;
    if (sendMode == 1 && comment.isEmpty()) {
        bool ok = false;
        comment = QInputDialog::getMultiLineText(this, i18nc("@title:window", "Response"), i18nc("@label:textbox", "Message to the organizer:"), {}, &ok);
        if (!ok) {
            return;
        }
    }
    m_daemon->respondMeeting(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), eventGraphId, action, comment, sendMode != 2);
    answered(action == QLatin1String("accept") ? i18nc("@info", "Accepted.") : action == QLatin1String("decline") ? i18nc("@info", "Declined.") : i18nc("@info", "Accepted tentatively."));
}

void ReadingPane::proposeNewTime(const QString &eventGraphId, const EventDialog::NewTime &time)
{
    const QString key = mailboxKey();
    if (key.isEmpty()) {
        return;
    }
    m_daemon->proposeNewTime(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), eventGraphId, time.action, time.comment, time.start.toMSecsSinceEpoch(),
                             time.end.toMSecsSinceEpoch());
    answered(i18nc("@info", "New time proposed."));
}

void ReadingPane::answered(const QString &text)
{
    m_meeting->setText(text);
    const QList<QAction *> actions = m_meeting->actions();
    for (QAction *a : actions) {
        m_meeting->removeAction(a);
    }
    // As Outlook: the answered request leaves the Inbox (Calendar > Settings)
    const QString key = mailboxKey();
    if (EventDialog::deleteRequestAfterResponding() && m_message != 0 && !key.isEmpty()) {
        qWarning() << "meeting test: request deleted after responding" << m_message;
        m_daemon->remove(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), m_message);
    }
}

void ReadingPane::setCategoryLabels(const QList<QPair<QString, QColor>> &labels)
{
    QStringList parts;
    for (const auto &[name, color] : labels) {
        const QString text = color.lightnessF() > 0.6 ? QStringLiteral("#000000") : QStringLiteral("#ffffff");
        parts.append(QStringLiteral("<span style=\"background-color:%1; color:%2\">&nbsp;%3&nbsp;</span>").arg(color.name(), text, name.toHtmlEscaped()));
    }
    m_categoryLabels->setText(parts.join(QStringLiteral("&nbsp; ")));
    m_categoryLabels->setVisible(!parts.isEmpty());
}

void ReadingPane::bodyHtmlForTests(const std::function<void(const QString &)> &done)
{
    m_body->page()->toHtml(done);
}

void ReadingPane::zoom(int step)
{
    const double factor = step == 0 ? 1.0 : MailView::zoom() * (step > 0 ? 1.1 : 1.0 / 1.1);
    MailView::setZoom(factor);
    m_body->setZoomFactor(MailView::zoom());
}

void ReadingPane::clear()
{
    m_message = 0;
    m_store = nullptr;
    m_blobs = nullptr;
    m_pending.clear();
    m_remoteBar->hide();
    m_notice->hide();
    m_meeting->hide();
    m_unsubscribeBar->hide();
    m_receiptBar->hide();
    m_attachments->setAttachments({}, false);
    m_stack->setCurrentIndex(0);
}

void ReadingPane::clickLinkForTests(const QString &text)
{
    m_body->clickLinkForTests(text);
}

void ReadingPane::loadImages()
{
    m_allowRemote = true;
    showBody();
}

bool ReadingPane::printBodyToPdf(const QString &file)
{
    if (m_message == 0) {
        return false;
    }
    connect(m_body->page(), &QWebEnginePage::pdfPrintingFinished, qApp, &QCoreApplication::quit);
    m_body->page()->printToPdf(file);
    return true;
}

QString ReadingPane::shownState() const
{
    const std::optional<MessageRow> m = m_store ? m_store->message(m_message) : std::nullopt;
    if (!m) {
        return {};
    }
    int images = 0;
    const QList<AttachmentRow> attachments = m_store->attachments(m_message);
    for (const AttachmentRow &a : attachments) {
        images += (a.isInline && !a.blob.isEmpty()) ? 1 : 0;
    }
    return QStringLiteral("%1/%2/%3/%4").arg(m_message).arg(m->bodyBlob).arg(images).arg(m_allowRemote || isTrusted(m_sender));
}

void ReadingPane::reload()
{
    if (m_message == 0 || !m_store) {
        return;
    }
    // The list may have arrived with the body (or later)
    refreshAttachments();
    if (shownState() != m_shown) {
        showBody();
    }
}

void ReadingPane::refreshAttachments()
{
    const std::optional<MessageRow> m = m_store ? m_store->message(m_message) : std::nullopt;
    if (!m) {
        m_attachments->setAttachments({}, false);
        return;
    }
    const QList<AttachmentRow> rows = m_store->attachments(m_message);
    m_attachments->setAttachments(rows, m->hasAttachments && rows.isEmpty());
}

std::optional<QByteArray> ReadingPane::contentOf(qint64 attachment, AttachmentRow *row) const
{
    const QList<AttachmentRow> rows = m_store ? m_store->attachments(m_message) : QList<AttachmentRow>();
    for (const AttachmentRow &a : rows) {
        if (a.id == attachment) {
            if (row) {
                *row = a;
            }
            QString error;
            return (a.blob.isEmpty() || !m_blobs) ? std::nullopt : m_blobs->get(a.blob, &error);
        }
    }
    return std::nullopt;
}

QString ReadingPane::fileNameFor(const AttachmentRow &row, const QByteArray &data)
{
    // No path separators or control characters from a sender's file name
    QString name = row.name.isEmpty() ? i18nc("@item file name of an attachment without a name", "attachment") : row.name;
    name.replace(QLatin1Char('/'), QLatin1Char('_')).replace(QLatin1Char('\\'), QLatin1Char('_'));
    name.removeIf([](QChar c) {
        return c.category() == QChar::Other_Control;
    });
    if (name.startsWith(QLatin1Char('.'))) {
        name.prepend(QLatin1Char('_'));
    }
    // An attached mail or Outlook item comes as MIME: give it the extension mail programs know.
    // Graph's MIME may start with "To:", which the MIME database does not recognise, so also look
    // for a header block with MIME-Version (rows cached before the content type was set).
    const QByteArray head = data.left(qsizetype(data.indexOf("\r\n\r\n") >= 0 ? data.indexOf("\r\n\r\n") : std::min<qsizetype>(data.size(), 8192)));
    const bool isMail = row.contentType == QLatin1String("message/rfc822") || QMimeDatabase().mimeTypeForData(data).inherits(QStringLiteral("message/rfc822"))
        || (head.contains("\nMIME-Version:") && (head.startsWith("To:") || head.startsWith("From:") || head.contains("\nSubject:")));
    if (isMail && !name.endsWith(QLatin1String(".eml"), Qt::CaseInsensitive)) {
        name += QStringLiteral(".eml");
    }
    return name;
}

void ReadingPane::showNotice(const QString &text)
{
    m_notice->setText(text);
    m_notice->animatedShow();
}

void ReadingPane::openAttachment(qint64 attachment)
{
    m_pending[attachment].open = true;
    runPending(attachment);
}

void ReadingPane::previewAttachment(qint64 attachment)
{
    m_pending[attachment].preview = true;
    runPending(attachment);
}

QWidget *ReadingPane::lastPreview() const
{
    return m_lastPreview;
}

void ReadingPane::saveAttachment(qint64 attachment, const QString &path)
{
    m_pending[attachment].savePath = path;
    runPending(attachment);
}

void ReadingPane::saveAllTo(const QString &folder)
{
    saveAllAttachments(folder);
}

int ReadingPane::pendingCount() const
{
    return int(m_pending.size());
}

void ReadingPane::saveAllAttachments(const QString &folder)
{
    const QString dir = !folder.isEmpty() ? folder
                                          : QFileDialog::getExistingDirectory(this, i18nc("@title:window", "Save All Attachments"),
                                                                              QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    if (dir.isEmpty() || !m_store) {
        return;
    }
    const QList<AttachmentRow> rows = m_store->attachments(m_message);
    for (const AttachmentRow &a : rows) {
        if (!a.isInline) {
            m_pending[a.id].saveFolder = dir;
            runPending(a.id);
        }
    }
}

void ReadingPane::attachmentReady(qint64 message, qint64 attachment, bool ok)
{
    if (message != m_message || !m_pending.contains(attachment)) {
        return;
    }
    if (!ok) {
        m_pending.remove(attachment);
        showNotice(i18nc("@info", "The attachment could not be downloaded. Check the connection and try again."));
        return;
    }
    runPending(attachment);
}

void ReadingPane::runPending(qint64 attachment)
{
    AttachmentRow row;
    const std::optional<QByteArray> data = contentOf(attachment, &row);
    if (!data) {
        // Not cached yet: the daemon fetches it and attachmentReady() comes back here
        Q_EMIT attachmentNeeded(attachment);
        return;
    }
    Pending pending = m_pending.take(attachment);
    if (!pending.saveFolder.isEmpty()) {
        // Never overwrite: "report.pdf", then "report (1).pdf", ...
        const QFileInfo base(QDir(pending.saveFolder).filePath(fileNameFor(row, *data)));
        QString path = base.filePath();
        for (int n = 1; QFileInfo::exists(path); ++n) {
            path = base.dir().filePath(QStringLiteral("%1 (%2)%3").arg(base.completeBaseName()).arg(n).arg(base.suffix().isEmpty() ? QString() : QLatin1Char('.') + base.suffix()));
        }
        pending.savePath = path;
    }
    if (!pending.savePath.isEmpty()) {
        QFile file(pending.savePath);
        if (!file.open(QIODevice::WriteOnly) || file.write(*data) != data->size()) {
            showNotice(i18nc("@info", "Could not save %1: %2", pending.savePath, file.errorString()));
        }
    }
    if (pending.preview) {
        auto *preview = new AttachmentPreview(fileNameFor(row, *data), *data, this);
        connect(preview, &AttachmentPreview::openRequested, this, [this, attachment] {
            openAttachment(attachment);
        });
        preview->show();
        m_lastPreview = preview;
    }
    if (pending.open) {
        if (!m_tempDir) {
            m_tempDir = std::make_unique<QTemporaryDir>(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("graphmail-XXXXXX")));
        }
        // A private, read-only copy (as Outlook does): edits would be lost with the copy
        const QString path = QDir(m_tempDir->path()).filePath(QStringLiteral("%1/%2").arg(attachment).arg(fileNameFor(row, *data)));
        QDir().mkpath(QFileInfo(path).path());
        QFile file(path);
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        if (!file.open(QIODevice::WriteOnly) || file.write(*data) != data->size()) {
            showNotice(i18nc("@info", "Could not open the attachment: %1", file.errorString()));
            return;
        }
        file.close();
        file.setPermissions(QFileDevice::ReadOwner);
        const QString name = QFileInfo(path).fileName();
        // An Outlook message: as MIME into the same viewer
        if (name.endsWith(QLatin1String(".msg"), Qt::CaseInsensitive)) {
            QString error;
            if (MimeMapi::msgToEml(path, path + QStringLiteral(".eml"), &error)) {
                auto *viewer = new MimeTreeParser::Widgets::MessageViewerDialog(path + QStringLiteral(".eml"), this);
                viewer->setAttribute(Qt::WA_DeleteOnClose);
                viewer->show();
                return;
            }
        }
        // An attached mail: KDE's mail viewer, inside Graph Mail (also signed or encrypted mail)
        if (name.endsWith(QLatin1String(".eml"), Qt::CaseInsensitive)) {
            auto *viewer = new MimeTreeParser::Widgets::MessageViewerDialog(path, this);
            viewer->setAttribute(Qt::WA_DeleteOnClose);
            viewer->show();
            return;
        }
        // Outlook's TNEF container: the files inside it
        if (name.compare(QLatin1String("winmail.dat"), Qt::CaseInsensitive) == 0 || row.contentType == QLatin1String("application/ms-tnef")) {
            if (TnefDialog *dialog = TnefDialog::open(path, path + QStringLiteral(".files"), this)) {
                dialog->show();
                return;
            }
        }
        // An invitation or a calendar file: its events, with Add to Calendar
        if (name.endsWith(QLatin1String(".ics"), Qt::CaseInsensitive) || row.contentType.startsWith(QLatin1String("text/calendar"))) {
            if (IcsDialog *dialog = IcsDialog::open(path, m_mailboxes, m_daemon, this)) {
                dialog->show();
                return;
            }
        }
        auto *job = new KIO::OpenUrlJob(QUrl::fromLocalFile(path));
        job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, this));
        job->start();
    }
}

void ReadingPane::showMessage(Store *store, BlobStore *blobs, qint64 message)
{
    const std::optional<MessageRow> m = store ? store->message(message) : std::nullopt;
    if (!m) {
        clear();
        return;
    }
    const bool sameMessage = message == m_message && store == m_store;
    m_store = store;
    m_blobs = blobs;
    m_message = message;
    m_sender = m->senderAddress;
    if (!sameMessage) {
        m_allowRemote = false;
        m_markReadWhenKnown = false;
        m_pending.clear();
        m_notice->hide();
        m_meeting->hide();
        m_unsubscribeBar->hide();
        m_unsubscribe = {};
        m_meetingEventId.clear();
        if (m->itemType == 1 || m->itemType == 3) {
            showMeeting(*m);
        }
    }
    refreshAttachments();
    m_stack->setCurrentIndex(1);
    m_subject->setText(m->subject.isEmpty() ? i18nc("@info a message without a subject", "(no subject)") : m->subject);
    m_avatar->set(m->senderName, m->senderAddress);
    const QString name = m->senderName.isEmpty() ? m->senderAddress : m->senderName;
    m_from->setText(QStringLiteral("<b>%1</b> &lt;%2&gt;").arg(name.toHtmlEscaped(), m->senderAddress.toHtmlEscaped()));
    QString recipients = i18nc("@info message recipients", "To: %1", m->toList);
    if (!m->ccList.isEmpty()) {
        recipients += QLatin1Char('\n') + i18nc("@info message copy recipients", "Cc: %1", m->ccList);
    }
    m_recipients->setText(recipients);
    // Short, as Outlook ("wo 18-11-2026 15:23"); the full date and time zone in the tooltip
    const QDateTime received = QDateTime::fromMSecsSinceEpoch(m->received);
    m_date->setText(QLocale().toString(received, QStringLiteral("ddd ") + QLocale().dateFormat(QLocale::ShortFormat) + QLatin1Char(' ') + QLocale().timeFormat(QLocale::ShortFormat)));
    m_date->setToolTip(QLocale().toString(received, QLocale::LongFormat));

    showBody();
    // Always ask: the daemon answers at once when everything is cached, and fetches inline images
    // that bodies cached before 2026-11-11 lack
    Q_EMIT bodyNeeded(message);
}

void ReadingPane::showBody()
{
    const std::optional<MessageRow> m = m_store ? m_store->message(m_message) : std::nullopt;
    if (!m) {
        return;
    }
    QString error;
    const std::optional<QByteArray> body = (!m->bodyBlob.isEmpty() && m_blobs) ? m_blobs->get(m->bodyBlob, &error) : std::nullopt;
    if (!body) {
        m_remoteBar->hide();
        m_body->showText(m->preview + QStringLiteral("\n\n") + i18nc("@info body still downloading", "(Loading the message...)"));
        return;
    }
    m_shown = shownState();
    const bool allow = m_allowRemote || isTrusted(m_sender);
    m_remoteBar->setVisible(m->bodyType == 1 && !allow && MailView::hasRemoteContent(*body));
    m_body->showBody(m_store, m_blobs, m_message, allow);
    showUnsubscribe();
    showReceiptQuestion();
}

bool ReadingPane::remoteAllowed() const
{
    return m_allowRemote || isTrusted(m_sender);
}

void ReadingPane::markReadWhenKnown()
{
    m_markReadWhenKnown = true;
    showReceiptQuestion();
}

void ReadingPane::showReceiptQuestion()
{
    const std::optional<MessageRow> m = m_store ? m_store->message(m_message) : std::nullopt;
    const ReadReceipts::Decision decision = m ? ReadReceipts::decide(ReadReceipts::mode(), m_store->readReceiptRequested(m_message), false) : ReadReceipts::Decision::Wait;
    const bool ask = m && !m->isRead && m_daemon && decision == ReadReceipts::Decision::Ask;
    m_receiptBar->setVisible(ask);
    // Waiting to mark read: now that the request is known, no receipt was asked for
    if (m_markReadWhenKnown && m && !m->isRead && m_daemon && (decision == ReadReceipts::Decision::DontSend || decision == ReadReceipts::Decision::Send)) {
        m_markReadWhenKnown = false;
        const QString key = mailboxKey();
        m_daemon->markRead(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), m_message, true, decision == ReadReceipts::Decision::Send);
    }
}

void ReadingPane::answerReceipt(bool send)
{
    const QString key = mailboxKey();
    m_receiptBar->animatedHide();
    if (m_daemon && !key.isEmpty()) {
        m_daemon->markRead(key.section(QLatin1Char('/'), 0, 0), key.section(QLatin1Char('/'), 1), m_message, true, send);
    }
}

void ReadingPane::showUnsubscribe()
{
    // The header comes with the body; a bar that already answered stays as it is
    if (!m_store || m_unsubscribe.kind != Unsubscribe::Kind::None) {
        return;
    }
    bool oneClick = false;
    m_unsubscribe = Unsubscribe::choose(m_store->unsubscribe(m_message, &oneClick), oneClick);
    if (m_unsubscribe.kind == Unsubscribe::Kind::None) {
        m_unsubscribeBar->hide();
        return;
    }
    m_unsubscribeBar->setMessageType(KMessageWidget::Information);
    m_unsubscribeBar->setText(i18nc("@info", "This message comes from a mailing list."));
    for (QAction *action : m_unsubscribeBar->actions()) {
        action->setVisible(true);
    }
    m_unsubscribeBar->show();
}

void ReadingPane::unsubscribe()
{
    const Unsubscribe::Way way = m_unsubscribe;
    const QString host = way.url.host();
    QString question;
    switch (way.kind) {
    case Unsubscribe::Kind::OneClick:
        question = i18nc("@info %1 a web site", "Unsubscribe from this mailing list? Graph Mail sends the list's own unsubscribe request to %1.", host);
        break;
    case Unsubscribe::Kind::Mail:
        question = i18nc("@info %1 an e-mail address", "Unsubscribe from this mailing list? A message to %1 opens; send it to unsubscribe.", way.url.path());
        break;
    case Unsubscribe::Kind::Browser:
        question = i18nc("@info %1 a web site", "Unsubscribe from this mailing list? The list's page on %1 opens in your browser.", host);
        break;
    case Unsubscribe::Kind::None:
        return;
    }
    if (KMessageBox::questionTwoActions(this,
                                        question,
                                        i18nc("@title:window", "Unsubscribe"),
                                        KGuiItem(i18nc("@action:button", "Unsubscribe"), QStringLiteral("list-remove-user")),
                                        KStandardGuiItem::cancel())
        != KMessageBox::PrimaryAction) {
        return;
    }
    if (way.kind == Unsubscribe::Kind::Mail) {
        Q_EMIT mailtoRequested(way.url.toString());
        return;
    }
    if (way.kind == Unsubscribe::Kind::Browser) {
        QDesktopServices::openUrl(way.url);
        return;
    }
    for (QAction *action : m_unsubscribeBar->actions()) {
        action->setVisible(false);
    }
    m_unsubscribeBar->setText(i18nc("@info", "Unsubscribing..."));
    const qint64 message = m_message;
    Unsubscribe::post(way.url, this, [this, message, host](bool ok, const QString &detail) {
        if (m_message != message) {
            return;
        }
        if (ok) {
            m_unsubscribeBar->setMessageType(KMessageWidget::Positive);
            m_unsubscribeBar->setText(i18nc("@info %1 a web site", "%1 took the unsubscribe request.", host));
            return;
        }
        m_unsubscribeBar->setMessageType(KMessageWidget::Warning);
        m_unsubscribeBar->setText(i18nc("@info %1 a web site, %2 the reason", "%1 did not take the unsubscribe request (%2).", host, detail));
        for (QAction *action : m_unsubscribeBar->actions()) {
            action->setVisible(true);
        }
    });
}

}
