// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

class QComboBox;

namespace TextCustomEditor
{
class RichTextEditor;
}

namespace GraphMail
{

// The spelling language of the composer (0.14.4): a box with "Automatic" first and then every
// installed dictionary. Automatic guesses the language of the text typed so far (Sonnet's guesser)
// and switches the spell checker to the matching dictionary, whatever the global KDE setting for
// language detection is; a language picked by hand stays for that mail. The choice is remembered
// per sending mailbox (graphmailrc [Composer] "SpellLanguage <mailbox>", empty = Automatic).
class SpellLanguage : public QObject
{
    Q_OBJECT

public:
    SpellLanguage(TextCustomEditor::RichTextEditor *editor, QWidget *parent);

    // For the toolbar
    QComboBox *comboBox() const;
    // The sending mailbox changed (or was set first): its remembered choice applies
    void setMailbox(const QString &mailboxKey);
    // The dictionary in use now ("" when none is installed)
    QString currentDictionary() const;

    // The installed dictionary for a guessed language ("nl", "en"): the same code; else of that
    // language, in this order: the preferred one (the KDE default), the one of `locale` (the
    // system's), the language's own country (nl_NL, de_DE), en_US for English, the first. Empty
    // when none is of that language. Public for tests.
    static QString pickDictionary(const QString &guessed, const QStringList &dictionaries, const QString &preferred, const QString &locale);

private:
    void fill();
    void chosen(int index);
    void guess();
    void apply(const QString &dictionary, bool automatic);

    QPointer<TextCustomEditor::RichTextEditor> m_editor;
    QComboBox *m_box;
    QTimer m_guessTimer;
    QString m_mailbox;
    QString m_current;
    QString m_preferred;
    QStringList m_dictionaries;
    bool m_automatic = true;
};

}
