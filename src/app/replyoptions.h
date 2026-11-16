// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QComboBox;

// Settings > Reply and Forward (Outlook's Mail options "Replies and forwards"): how the original
// comes along in a reply and in a forward. Kept in graphmailrc [Composer].
namespace GraphMail::ReplyOptions
{

enum class Mode {
    // The original below the new text (the default, as before)
    Include,
    // The original below, every line prefixed with "> " (plain text) or indented (HTML)
    Prefix,
    // The original attached as a mail file, no text of it in the message
    Attach,
    // Nothing of the original (replies only)
    None,
};

Mode forReplies();
Mode forForwards();
void setForReplies(Mode mode);
void setForForwards(Mode mode);
// "> " before every line of a quoted text, as mail programs mark an original
QString prefixLines(const QString &text);

class Dialog : public QDialog
{
    Q_OBJECT

public:
    explicit Dialog(QWidget *parent = nullptr);
    void accept() override;

private:
    QComboBox *m_replies;
    QComboBox *m_forwards;
};

}
