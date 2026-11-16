// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QDialog>
#include <QList>

#include <optional>

class KColorButton;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;

// Outlook's View Settings > Conditional Formatting (docs/PHASE9B.md 9b.27): messages whose sender
// or subject holds some text get a colour in the message list. Kept in graphmailrc
// [ConditionalFormatting] on this computer; the first rule that matches wins.
namespace GraphMail::ConditionalFormat
{

struct Rule {
    QString name;
    // "sender" (name or address) or "subject"
    QString field = QStringLiteral("sender");
    QString text;
    QColor color = QColor(0xc0, 0x39, 0x2b);
    bool enabled = true;

    bool matches(const QString &senderName, const QString &senderAddress, const QString &subject) const;
};

QList<Rule> rules();
void setRules(const QList<Rule> &rules);
std::optional<QColor> colorFor(const QList<Rule> &rules, const QString &senderName, const QString &senderAddress, const QString &subject);

class Dialog : public QDialog
{
    Q_OBJECT

public:
    explicit Dialog(QWidget *parent = nullptr);
    void accept() override;

private:
    void showRule(int row);
    void storeRule();
    void refreshList();

    QList<Rule> m_rules;
    QListWidget *m_list;
    QLineEdit *m_name;
    QComboBox *m_field;
    QLineEdit *m_text;
    KColorButton *m_color;
    QCheckBox *m_enabled;
    int m_shown = -1;
};

}
