// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fontfamilybox.h"

#include <KFontAction>
#include <KPIMTextEdit/RichTextComposer>
#include <KPIMTextEdit/RichTextComposerControler>

#include <QApplication>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QToolBar>

namespace GraphMail
{

namespace
{
// Set on a box once it is taken over
constexpr char takenOver[] = "graphmail_fontbox";
}

FontFamilyBox::FontFamilyBox(KFontAction *action, KPIMTextEdit::RichTextComposer *editor)
    : QObject(editor)
    , m_action(action)
    , m_editor(editor)
{
    connect(qApp, &QApplication::focusChanged, this, &FontFamilyBox::focusChanged);
}

QString FontFamilyBox::installedFamily(const QString &typed)
{
    const QString name = typed.trimmed();
    if (name.isEmpty()) {
        return {};
    }
    const QStringList families = QFontDatabase::families();
    for (const QString &family : families) {
        if (family.compare(name, Qt::CaseInsensitive) == 0) {
            return family;
        }
    }
    return {};
}

QFontComboBox *FontFamilyBox::boxOf(QWidget *widget) const
{
    if (!m_action || !widget) {
        return nullptr;
    }
    // The focus is on the box or its line edit
    auto *box = qobject_cast<QFontComboBox *>(widget);
    if (!box) {
        box = qobject_cast<QFontComboBox *>(widget->parentWidget());
    }
    // This action's box: made in a toolbar, for that toolbar
    auto *bar = box ? qobject_cast<QToolBar *>(box->parentWidget()) : nullptr;
    return bar && bar->widgetForAction(m_action) == box ? box : nullptr;
}

void FontFamilyBox::focusChanged(QWidget *old, QWidget *now)
{
    // The box's list opening is no leaving
    if (now && now->window()->windowType() == Qt::Popup) {
        return;
    }
    QFontComboBox *left = boxOf(old);
    QFontComboBox *entered = boxOf(now);
    if (left && left != entered && left->property(takenOver).toBool()) {
        // Left without Enter or a pick (or after one): the box shows the font at the cursor again
        showFontAtCursor();
    }
    if (entered && !entered->property(takenOver).toBool()) {
        takeOver(entered);
    }
}

void FontFamilyBox::takeOver(QFontComboBox *box)
{
    box->setProperty(takenOver, true);
    // KFontAction's own reaction to every change of the box's text
    disconnect(box, &QFontComboBox::currentFontChanged, m_action, nullptr);
    // A name that is no font does not become an entry of the list
    box->setInsertPolicy(QComboBox::NoInsert);
    // Enter, or a font picked from the list
    connect(box, &QComboBox::activated, this, [this, box] {
        picked(box);
    });
}

void FontFamilyBox::picked(QFontComboBox *box)
{
    if (!m_action || !m_editor) {
        return;
    }
    const QString family = installedFamily(box->currentText());
    if (family.isEmpty()) {
        showFontAtCursor();
        return;
    }
    m_action->setFont(family);
    // Applies it to the selection (or the word at the cursor) and puts the focus back in the text
    m_editor->composerControler()->setFontFamily(family);
}

void FontFamilyBox::showFontAtCursor()
{
    if (m_action && m_editor) {
        m_action->setFont(m_editor->currentCharFormat().font().family());
    }
}

}
