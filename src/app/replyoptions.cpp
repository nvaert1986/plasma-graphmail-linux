// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "replyoptions.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace GraphMail::ReplyOptions
{

namespace
{
KConfigGroup group()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Composer"));
}

QString name(Mode mode)
{
    switch (mode) {
    case Mode::Prefix:
        return QStringLiteral("prefix");
    case Mode::Attach:
        return QStringLiteral("attach");
    case Mode::None:
        return QStringLiteral("none");
    case Mode::Include:
        break;
    }
    return QStringLiteral("include");
}

Mode mode(const QString &text, bool noneAllowed)
{
    if (text == QLatin1String("prefix")) {
        return Mode::Prefix;
    }
    if (text == QLatin1String("attach")) {
        return Mode::Attach;
    }
    if (text == QLatin1String("none") && noneAllowed) {
        return Mode::None;
    }
    return Mode::Include;
}
}

Mode forReplies()
{
    return mode(group().readEntry("Replies", QString()), true);
}

Mode forForwards()
{
    return mode(group().readEntry("Forwards", QString()), false);
}

void setForReplies(Mode mode)
{
    KConfigGroup g = group();
    g.writeEntry("Replies", name(mode));
    g.sync();
}

void setForForwards(Mode mode)
{
    KConfigGroup g = group();
    g.writeEntry("Forwards", name(mode == Mode::None ? Mode::Include : mode));
    g.sync();
}

QString prefixLines(const QString &text)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString &line : lines) {
        // An already quoted line gets one more ">", as mail programs nest quotes
        line.prepend(line.startsWith(QLatin1Char('>')) ? QStringLiteral(">") : QStringLiteral("> "));
    }
    return lines.join(QLatin1Char('\n'));
}

Dialog::Dialog(QWidget *parent)
    : QDialog(parent)
    , m_replies(new QComboBox(this))
    , m_forwards(new QComboBox(this))
{
    setWindowTitle(i18nc("@title:window", "Reply and Forward"));
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_replies->addItem(i18nc("@item:inlistbox", "Include the original message"), int(Mode::Include));
    m_replies->addItem(i18nc("@item:inlistbox", "Include the original and prefix each line"), int(Mode::Prefix));
    m_replies->addItem(i18nc("@item:inlistbox", "Attach the original message"), int(Mode::Attach));
    m_replies->addItem(i18nc("@item:inlistbox", "Do not include the original message"), int(Mode::None));
    m_forwards->addItem(i18nc("@item:inlistbox", "Include the original message"), int(Mode::Include));
    m_forwards->addItem(i18nc("@item:inlistbox", "Include the original and prefix each line"), int(Mode::Prefix));
    m_forwards->addItem(i18nc("@item:inlistbox", "Attach the original message"), int(Mode::Attach));
    m_replies->setCurrentIndex(m_replies->findData(int(forReplies())));
    m_forwards->setCurrentIndex(m_forwards->findData(int(forForwards())));
    form->addRow(i18nc("@label:listbox", "When replying to a message:"), m_replies);
    form->addRow(i18nc("@label:listbox", "When forwarding a message:"), m_forwards);
    layout->addLayout(form);
    auto *note = new QLabel(i18nc("@info", "Forward as Attachment and Edit as New Message work as before, whatever is chosen here."), this);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void Dialog::accept()
{
    setForReplies(Mode(m_replies->currentData().toInt()));
    setForForwards(Mode(m_forwards->currentData().toInt()));
    QDialog::accept();
}

}
