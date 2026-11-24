// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// The composer's OneDrive and SharePoint files (phase 10 step 7, with Graph Sync installed): synced
// files attached as links, large attachments put into OneDrive, and on Send the recipients given
// access, as Outlook does (people you specify can edit, no separate invitation mail).

#include "composer.h"
#include "graphmail_debug.h"
#include "graphsynclink.h"
#include "maileditor.h"
#include "recipientedit.h"

#include <KActionCollection>
#include <KLocalizedString>
#include <KMessageBox>
#include <KPIMTextEdit/RichTextComposer>

#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QStatusBar>
#include <QTextEdit>

namespace GraphMail
{

namespace
{
constexpr int pollMs = 3000;
}

QString ComposerWindow::accountId() const
{
    return m_mailboxKey.section(QLatin1Char('/'), 0, 0);
}

QStringList ComposerWindow::recipientAddresses() const
{
    QStringList invalid;
    QStringList addresses;
    const QList<OutgoingRecipient> all = m_to->recipients(&invalid) + m_cc->recipients(&invalid) + m_bcc->recipients(&invalid);
    for (const OutgoingRecipient &r : all) {
        if (!r.address.isEmpty() && !addresses.contains(r.address, Qt::CaseInsensitive)) {
            addresses.append(r.address);
        }
    }
    return addresses;
}

void ComposerWindow::attachFromCloud()
{
    const QList<GraphSyncLink::Location> locations = GraphSyncLink::locations(accountId());
    if (locations.isEmpty()) {
        return;
    }
    QString start = locations.constFirst().localPath;
    for (const GraphSyncLink::Location &l : locations) {
        if (l.kind == QLatin1String("personal")) {
            start = l.localPath;
            break;
        }
    }
    // Graph Sync's locations are in the dialog's Places
    attachCloudFiles(QFileDialog::getOpenFileNames(this, i18nc("@title:window", "Attach from OneDrive or SharePoint"), start));
}

void ComposerWindow::attachCloudFiles(const QStringList &paths)
{
    QStringList outside;
    for (const QString &path : paths) {
        if (!QFileInfo(path).isFile()) {
            continue;
        }
        if (GraphSyncLink::locationOf(path, accountId()).id.isEmpty()) {
            outside.append(QFileInfo(path).fileName());
            continue;
        }
        addCloudFile(QFileInfo(path).absoluteFilePath(), false);
    }
    if (!outside.isEmpty() && !m_testMode) {
        KMessageBox::information(this,
                           i18nc("@info %1 file names", "These files are not in a OneDrive or SharePoint folder that Graph Sync syncs for this account: %1", outside.join(QStringLiteral(", "))),
                           i18nc("@title:window", "Attach from OneDrive or SharePoint"));
    }
}

void ComposerWindow::addCloudFile(const QString &path, bool uploading)
{
    CloudFile file;
    file.path = path;
    file.name = QFileInfo(path).fileName();
    file.uploading = uploading;
    file.cursor = m_editor->textCursor();
    m_cloud.append(file);
    refreshAttachmentList();
    setDirty();
    requestCloudLink(int(m_cloud.size()) - 1);
}

void ComposerWindow::requestCloudLink(int index)
{
    const QString path = m_cloud.at(index).path;
    GraphSyncLink::fileLink(path, this, [this, path](const QJsonObject &answer) {
        // The list may have changed meanwhile (removed): find it again by path
        const auto it = std::find_if(m_cloud.begin(), m_cloud.end(), [&path](const CloudFile &f) {
            return f.path == path;
        });
        if (it == m_cloud.end()) {
            return;
        }
        const QString url = answer.value(QLatin1String("url")).toString();
        if (!url.isEmpty() && answer.value(QLatin1String("synced")).toBool()) {
            const bool first = it->url.isEmpty();
            it->url = url;
            it->uploading = false;
            it->error.clear();
            if (first) {
                // The link where the file was attached, as Outlook puts it in the text
                QTextCursor cursor = it->cursor.isNull() ? m_editor->textCursor() : it->cursor;
                if (m_editor->textMode() == KPIMTextEdit::RichTextComposer::Rich) {
                    cursor.insertHtml(QStringLiteral("<a href=\"%1\">%2</a>&nbsp;").arg(url.toHtmlEscaped(), it->name.toHtmlEscaped()));
                } else {
                    cursor.insertText(it->name + QStringLiteral(": ") + url + QLatin1Char(' '));
                }
                setDirty();
            }
            if (m_testMode) {
                qCWarning(GRAPHMAIL).noquote() << "cloud test:" << it->name << "link";
            }
        } else if (answer.value(QLatin1String("notSynced")).toBool() || (!url.isEmpty() && !answer.value(QLatin1String("synced")).toBool())) {
            // Not online yet (just put into a synced folder): asked again shortly
            it->uploading = true;
            if (!m_cloudPoll) {
                m_cloudPoll = new QTimer(this);
                m_cloudPoll->setInterval(pollMs);
                connect(m_cloudPoll, &QTimer::timeout, this, [this] {
                    bool waiting = false;
                    for (int i = 0; i < m_cloud.size(); ++i) {
                        if (m_cloud.at(i).uploading) {
                            waiting = true;
                            requestCloudLink(i);
                        }
                    }
                    if (!waiting) {
                        m_cloudPoll->stop();
                    }
                });
            }
            m_cloudPoll->start();
        } else {
            it->uploading = false;
            it->error = answer.value(QLatin1String("error")).toString();
            qCWarning(GRAPHMAIL).noquote() << "No OneDrive link for" << it->name << ":" << it->error;
            if (m_testMode) {
                qCWarning(GRAPHMAIL).noquote() << "cloud test:" << it->name << "error";
            }
        }
        refreshAttachmentList();
        // Send was pressed while files were uploading: now that all are online, send
        if (m_sendWhenReady && std::none_of(m_cloud.cbegin(), m_cloud.cend(), [](const CloudFile &f) {
                return f.uploading || (f.url.isEmpty() && f.error.isEmpty());
            })) {
            m_sendWhenReady = false;
            statusBar()->clearMessage();
            if (QAction *send = actionCollection()->action(QStringLiteral("send"))) {
                send->trigger();
            }
        }
    });
}

bool ComposerWindow::putInOneDrive(const QString &path)
{
    const GraphSyncLink::Location onedrive = GraphSyncLink::personalLocation(accountId());
    if (onedrive.id.isEmpty()) {
        return false;
    }
    // Outlook's place for them: OneDrive/Attachments
    const QDir folder(QDir(onedrive.localPath).filePath(QStringLiteral("Attachments")));
    if (!QDir().mkpath(folder.path())) {
        return false;
    }
    const QFileInfo source(path);
    QString target = folder.filePath(source.fileName());
    for (int n = 2; QFileInfo::exists(target); ++n) {
        target = folder.filePath(QStringLiteral("%1 (%2)%3").arg(source.completeBaseName()).arg(n).arg(source.suffix().isEmpty() ? QString() : QLatin1Char('.') + source.suffix()));
    }
    if (!QFile::copy(path, target)) {
        return false;
    }
    addCloudFile(target, true);
    return true;
}

bool ComposerWindow::cloudReadyToSend()
{
    if (m_cloud.isEmpty()) {
        return true;
    }
    if (m_sharing) {
        return false;
    }
    // Still uploading, or Graph Sync has not answered yet (no link, no error): wait for it
    if (std::any_of(m_cloud.cbegin(), m_cloud.cend(), [](const CloudFile &f) {
            return f.uploading || (f.url.isEmpty() && f.error.isEmpty());
        })) {
        // Sent by itself once the files are online (requestCloudLink)
        m_sendWhenReady = true;
        statusBar()->showMessage(i18nc("@info:status", "Waiting for the files to be uploaded to OneDrive; the message is sent then."));
        return false;
    }
    QStringList recipients = recipientAddresses();
    recipients.sort(Qt::CaseInsensitive);
    if (recipients.isEmpty() || recipients == m_sharedWith) {
        return true;
    }
    // Give every recipient access to every file, one after another
    QList<int> todo;
    for (int i = 0; i < m_cloud.size(); ++i) {
        if (!m_cloud.at(i).url.isEmpty()) {
            todo.append(i);
        }
    }
    if (todo.isEmpty()) {
        return true;
    }
    m_sharing = true;
    statusBar()->showMessage(i18nc("@info:status", "Giving the recipients access to the OneDrive and SharePoint files..."));
    auto errors = std::make_shared<QStringList>();
    auto remaining = std::make_shared<int>(int(todo.size()));
    for (int i : std::as_const(todo)) {
        const CloudFile file = m_cloud.at(i);
        GraphSyncLink::shareFile(file.path, recipients, this, [this, file, errors, remaining, recipients](const QJsonObject &answer) {
            if (answer.contains(QLatin1String("error"))) {
                errors->append(i18nc("@info %1 a file name, %2 the reason", "%1: %2", file.name, answer.value(QLatin1String("error")).toString()));
            }
            if (--*remaining > 0) {
                return;
            }
            m_sharing = false;
            statusBar()->clearMessage();
            if (!errors->isEmpty()) {
                if (m_testMode) {
                    qCWarning(GRAPHMAIL).noquote() << "cloud test: sharing failed" << errors->size();
                    m_sendChecked = false;
                    return;
                }
                const auto answer = KMessageBox::warningTwoActions(this,
                                                                   i18nc("@info", "The recipients could not be given access to every file:\n%1\n\nSend the message anyway?", errors->join(QLatin1Char('\n'))),
                                                                   i18nc("@title:window", "Send"),
                                                                   KGuiItem(i18nc("@action:button", "Send Anyway"), QStringLiteral("mail-send")),
                                                                   KStandardGuiItem::cancel());
                if (answer != KMessageBox::PrimaryAction) {
                    // Not sent: the next Send asks its questions again
                    m_sendChecked = false;
                    return;
                }
            }
            m_sharedWith = recipients;
            if (QAction *send = actionCollection()->action(QStringLiteral("send"))) {
                send->trigger();
            }
        });
    }
    return false;
}

QJsonArray ComposerWindow::cloudJson() const
{
    QJsonArray files;
    for (const CloudFile &f : m_cloud) {
        files.append(QJsonObject{{QStringLiteral("path"), f.path},
                                 {QStringLiteral("name"), f.name},
                                 {QStringLiteral("url"), f.url},
                                 {QStringLiteral("uploading"), f.uploading}});
    }
    return files;
}

void ComposerWindow::restoreCloud(const QJsonArray &files)
{
    for (const QJsonValue &value : files) {
        const QJsonObject o = value.toObject();
        CloudFile f;
        f.path = o.value(QLatin1String("path")).toString();
        f.name = o.value(QLatin1String("name")).toString();
        f.url = o.value(QLatin1String("url")).toString();
        f.uploading = o.value(QLatin1String("uploading")).toBool();
        // A link already in the saved text stays where it is; one still uploading goes at the end
        QTextCursor end = m_editor->textCursor();
        end.movePosition(QTextCursor::End);
        f.cursor = end;
        m_cloud.append(f);
    }
    for (int i = 0; i < m_cloud.size(); ++i) {
        if (m_cloud.at(i).uploading) {
            requestCloudLink(i);
        }
    }
}

QStringList ComposerWindow::cloudStateForTests() const
{
    QStringList states;
    for (const CloudFile &f : m_cloud) {
        states.append(f.name + QLatin1Char(' ') + (!f.error.isEmpty() ? QStringLiteral("error") : (f.uploading ? QStringLiteral("uploading") : QStringLiteral("link"))));
    }
    return states;
}

}
