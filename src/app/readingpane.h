// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "eventdialog.h"

#include <QHash>
#include <QWidget>

#include <memory>

class QTemporaryDir;

class KMessageWidget;
class QLabel;
class QStackedWidget;

namespace GraphMail
{

class AttachmentStrip;
class DaemonClient;
class MailboxSet;
class BlobStore;
class MailView;
class Store;
struct AttachmentRow;
struct MessageRow;

// The message on the right: subject, sender with initials circle, To/Cc, date, then the body in a
// locked-down MailView. Internet content (tracking images) stays blocked until the user loads it
// for this message, or always for this sender (graphmailrc [Reading] TrustedSenders).
class ReadingPane : public QWidget
{
    Q_OBJECT

public:
    explicit ReadingPane(QWidget *parent = nullptr);
    ~ReadingPane() override;

    void showMessage(Store *store, BlobStore *blobs, qint64 message);
    // The same message again (its body or inline images arrived)
    void reload();
    // Tests: the rendered body as PDF; the application quits when it is written
    bool printBodyToPdf(const QString &file);
    // "Load Images" for the message on screen
    void loadImages();
    // The daemon stored (or failed to fetch) an attachment's content
    void attachmentReady(qint64 message, qint64 attachment, bool ok);
    // Tests: Save All into this folder without a dialog; how many are still waiting for content
    void saveAllTo(const QString &folder);
    int pendingCount() const;
    void clear();
    // The message's categories as labels in their colours (docs/PHASE9.md), under the subject
    void setCategoryLabels(const QList<QPair<QString, QColor>> &labels);
    // For meeting requests (Accept / Tentative / Decline) and .ics files (Add to Calendar)
    void setContext(DaemonClient *daemon, MailboxSet *mailboxes);
    // Tests: the meeting bar's answer, as its button does (false while the bar is not there yet)
    bool respondToMeetingForTests(const QString &action, const QString &comment);
    qint64 message() const;

Q_SIGNALS:
    // The body is not in the cache yet: the window asks the daemon for it
    void bodyNeeded(qint64 message);
    // An attachment's content is not in the cache yet
    void attachmentNeeded(qint64 attachment);

private:
    class AvatarLabel;

    QStackedWidget *m_stack;
    QLabel *m_empty;
    QLabel *m_subject;
    QLabel *m_categoryLabels = nullptr;
    AvatarLabel *m_avatar;
    QLabel *m_from;
    QLabel *m_recipients;
    QLabel *m_date;
    void showBody();
    void refreshAttachments();
    // Open or save an attachment once its content is cached (asks the daemon when it is not)
    void openAttachment(qint64 attachment);
    void saveAttachment(qint64 attachment, const QString &path);
    // Asks for a folder when none is given
    void saveAllAttachments(const QString &folder = {});
    void runPending(qint64 attachment);
    std::optional<QByteArray> contentOf(qint64 attachment, AttachmentRow *row) const;
    static QString fileNameFor(const AttachmentRow &row, const QByteArray &data);
    void showNotice(const QString &text);

    struct Pending {
        bool open = false;
        QString savePath;
        // Save All: the name is chosen once the content is known (an attached mail gets .eml)
        QString saveFolder;
    };

    KMessageWidget *m_remoteBar;
    KMessageWidget *m_notice;
    // A meeting request or cancellation: the event's time, the answer buttons
    KMessageWidget *m_meeting;
    void showMeeting(const MessageRow &message);
    // sendMode: 0 send now, 1 with a comment, 2 no response, -1 ask with a menu
    void proposeNewTime(const QString &eventGraphId, const EventDialog::NewTime &time);
    // The invitation bar after an answer; the request leaves the Inbox when so set
    void answered(const QString &text);
    void respondToMeeting(const QString &eventGraphId, const QString &action, int sendMode = -1, const QString &comment = {});
    QString m_meetingEventId;
    QString mailboxKey() const;
    DaemonClient *m_daemon = nullptr;
    MailboxSet *m_mailboxes = nullptr;
    AttachmentStrip *m_attachments;
    QHash<qint64, Pending> m_pending;
    // Private copies of opened attachments; removed when the app quits
    std::unique_ptr<QTemporaryDir> m_tempDir;
    MailView *m_body;
    Store *m_store = nullptr;
    BlobStore *m_blobs = nullptr;
    qint64 m_message = 0;
    QString m_sender;
    // "Load images" for the message on screen
    bool m_allowRemote = false;
    // What the body view shows: body blob, inline images present, remote allowed; reload() skips
    // when nothing changed (the daemon answers every request, also when all was cached already)
    QString m_shown;
    QString shownState() const;
};

}
