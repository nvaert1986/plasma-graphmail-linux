// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spelllanguage.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>
#include <Sonnet/GuessLanguage>
#include <Sonnet/Highlighter>
#include <Sonnet/Speller>
#include <TextCustomEditor/RichTextEditor>

#include <QComboBox>
#include <QLocale>
#include <QMap>
#include <QSet>

namespace GraphMail
{

namespace
{
// Fewer letters than this say too little about the language: the current one stays
constexpr int minLetters = 20;
// The start of the text is enough to know its language
constexpr int maxChars = 4000;

KConfigGroup composerGroup()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Composer"));
}

QString entryFor(const QString &mailboxKey)
{
    return QStringLiteral("SpellLanguage ") + mailboxKey;
}

// "nl-NL" and "nl_nl" as "nl_NL"; the language part in lower case
QString normalized(const QString &code)
{
    QString c = code;
    c.replace(QLatin1Char('-'), QLatin1Char('_'));
    const qsizetype at = c.indexOf(QLatin1Char('_'));
    return at < 0 ? c.toLower() : c.left(at).toLower() + c.mid(at);
}

QString languageOf(const QString &code)
{
    return normalized(code).section(QLatin1Char('_'), 0, 0);
}
}

QString SpellLanguage::pickDictionary(const QString &guessed, const QStringList &dictionaries, const QString &preferred, const QString &locale)
{
    const QString language = languageOf(guessed);
    if (language.isEmpty()) {
        return {};
    }
    QStringList candidates;
    for (const QString &d : dictionaries) {
        if (normalized(d).compare(normalized(guessed), Qt::CaseInsensitive) == 0) {
            return d;
        }
        if (languageOf(d) == language) {
            candidates.append(d);
        }
    }
    if (candidates.isEmpty()) {
        return {};
    }
    auto find = [&candidates](const QString &code) -> QString {
        for (const QString &c : std::as_const(candidates)) {
            if (normalized(c).compare(normalized(code), Qt::CaseInsensitive) == 0) {
                return c;
            }
        }
        return {};
    };
    const QStringList order{preferred,
                            locale,
                            QString(language + QLatin1Char('_') + language.toUpper()),
                            language == QLatin1String("en") ? QStringLiteral("en_US") : QString()};
    for (const QString &wanted : order) {
        if (!wanted.isEmpty() && languageOf(wanted) == language) {
            if (const QString found = find(wanted); !found.isEmpty()) {
                return found;
            }
        }
    }
    candidates.sort();
    return candidates.constFirst();
}

SpellLanguage::SpellLanguage(TextCustomEditor::RichTextEditor *editor, QWidget *parent)
    : QObject(parent)
    , m_editor(editor)
    , m_box(new QComboBox(parent))
{
    m_box->setToolTip(i18nc("@info:tooltip", "Spelling language. Automatic: follows the language you write in."));
    m_box->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    fill();
    connect(m_box, &QComboBox::activated, this, &SpellLanguage::chosen);
    m_guessTimer.setSingleShot(true);
    m_guessTimer.setInterval(1500);
    connect(&m_guessTimer, &QTimer::timeout, this, &SpellLanguage::guess);
    connect(editor, &QTextEdit::textChanged, this, [this] {
        if (m_automatic && !m_dictionaries.isEmpty()) {
            m_guessTimer.start();
        }
    });
}

QComboBox *SpellLanguage::comboBox() const
{
    return m_box;
}

QString SpellLanguage::currentDictionary() const
{
    return m_current;
}

void SpellLanguage::fill()
{
    const Sonnet::Speller speller;
    const QMap<QString, QString> dictionaries = speller.availableDictionaries();
    m_preferred = speller.defaultLanguage();
    m_dictionaries = dictionaries.values();
    m_box->clear();
    if (dictionaries.isEmpty()) {
        // Nothing to check with: say what to install instead of a box that does nothing
        m_box->addItem(i18nc("@item:inlistbox", "No spelling dictionaries"));
        m_box->setEnabled(false);
        m_box->setToolTip(i18nc("@info:tooltip", "No spelling dictionaries are installed. Install the hunspell dictionaries of your languages (for example hunspell-nl and hunspell-en-us) and the hunspell plugin of KDE's spell checker (Sonnet)."));
        return;
    }
    m_box->addItem(i18nc("@item:inlistbox spelling language", "Automatic"), QString());
    // By name (the map's order)
    for (auto it = dictionaries.cbegin(); it != dictionaries.cend(); ++it) {
        m_box->addItem(it.key(), it.value());
    }
}

void SpellLanguage::setMailbox(const QString &mailboxKey)
{
    m_mailbox = mailboxKey;
    if (m_dictionaries.isEmpty()) {
        return;
    }
    const QString saved = composerGroup().readEntry(entryFor(mailboxKey), QString());
    if (!saved.isEmpty() && m_dictionaries.contains(saved)) {
        apply(saved, false);
        return;
    }
    // Automatic: the KDE default until the text says otherwise
    const QString start = m_current.isEmpty() ? (m_dictionaries.contains(m_preferred) ? m_preferred : m_dictionaries.constFirst()) : m_current;
    apply(start, true);
    guess();
}

void SpellLanguage::chosen(int index)
{
    const QString code = m_box->itemData(index).toString();
    KConfigGroup group = composerGroup();
    if (!m_mailbox.isEmpty()) {
        group.writeEntry(entryFor(m_mailbox), code);
        group.sync();
    }
    if (code.isEmpty()) {
        apply(m_current, true);
        guess();
    } else {
        apply(code, false);
    }
}

void SpellLanguage::guess()
{
    if (!m_editor || !m_automatic || m_dictionaries.isEmpty()) {
        return;
    }
    const QString text = m_editor->toPlainText().left(maxChars);
    const int letters = int(std::count_if(text.cbegin(), text.cend(), [](QChar c) {
        return c.isLetter();
    }));
    if (letters < minLetters) {
        return;
    }
    QSet<QString> languages;
    for (const QString &d : std::as_const(m_dictionaries)) {
        languages.insert(languageOf(d));
    }
    Sonnet::GuessLanguage guesser;
    // Only a fairly sure guess switches the language
    guesser.setLimits(1, 0.1);
    const QString guessed = guesser.identify(text, QStringList(languages.cbegin(), languages.cend()));
    const QString dictionary = pickDictionary(guessed, m_dictionaries, m_preferred, QLocale::system().name());
    if (!dictionary.isEmpty() && dictionary != m_current) {
        apply(dictionary, true);
    }
}

void SpellLanguage::apply(const QString &dictionary, bool automatic)
{
    m_automatic = automatic;
    m_current = dictionary;
    if (m_editor) {
        m_editor->setSpellCheckingLanguage(dictionary);
        // A language picked by hand stays; with Automatic, Sonnet's own detection (when it is on
        // in KDE's settings) may still tell paragraphs in another language apart
        if (Sonnet::Highlighter *highlighter = m_editor->highlighter()) {
            highlighter->setAutoDetectLanguageDisabled(!automatic);
        }
    }
    const int own = m_box->findData(dictionary);
    if (automatic) {
        m_box->setItemText(0, own > 0 ? i18nc("@item:inlistbox %1 the language found", "Automatic (%1)", m_box->itemText(own)) : i18nc("@item:inlistbox spelling language", "Automatic"));
        m_box->setCurrentIndex(0);
    } else {
        m_box->setItemText(0, i18nc("@item:inlistbox spelling language", "Automatic"));
        m_box->setCurrentIndex(std::max(0, own));
    }
}

}
