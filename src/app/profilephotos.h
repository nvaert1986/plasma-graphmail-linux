// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>

class QWidget;

namespace GraphMail
{

class DaemonClient;
class MailboxSet;

// The mailboxes' photos (docs/PHASE9.md): read once per mailbox into MailboxSet, and the user's
// own changed or removed (Change Photo..., Remove Photo). Microsoft 365 shows the same photo in
// Outlook and Teams.
class ProfilePhotos : public QObject
{
    Q_OBJECT

public:
    ProfilePhotos(MailboxSet *mailboxes, DaemonClient *daemon, QObject *parent = nullptr);

    void load();
    // Asks for a picture, cuts it square and sends it
    void change(QWidget *parent, const QString &mailboxKey);
    void remove(QWidget *parent, const QString &mailboxKey);
    // Tests: this picture file, or remove when it is empty; logs the outcome
    void setForTests(const QString &mailboxKey, const QString &file);

Q_SIGNALS:
    // A change or removal went through, or not (the reason in words)
    void done(const QString &mailboxKey, bool ok, const QString &text);

private:
    void send(const QString &mailboxKey, const QImage &image, QWidget *parent);
    void answered(const QString &mailboxKey, const QString &json, const QImage &image, QWidget *parent);

    MailboxSet *m_mailboxes;
    DaemonClient *m_daemon;
};

}
