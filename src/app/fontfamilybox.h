// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QPointer>

class KFontAction;
class QFontComboBox;

namespace KPIMTextEdit
{
class RichTextComposer;
}

namespace GraphMail
{

// The composer's font box (0.14.4). Qt's font box reports every letter typed as a chosen font, and
// KPIMTextEdit applies each one to the selection and puts the focus back in the text: with the
// whole text selected, the second letter replaced it. A box this class has taken over applies a
// font only on Enter or a pick from its list, and only an installed one; leaving the box without
// that puts the font at the cursor back. Fonts chosen in the menu are applied as before.
class FontFamilyBox : public QObject
{
    Q_OBJECT

public:
    // Parented to the editor. The action's boxes are made when the toolbar is built (again after
    // Configure Toolbars), so each is taken over when it first gets the focus
    FontFamilyBox(KFontAction *action, KPIMTextEdit::RichTextComposer *editor);

    // The installed family for a typed name, whatever its case; empty when there is none
    static QString installedFamily(const QString &typed);

private:
    // The action's box that is or holds `widget`, if any
    QFontComboBox *boxOf(QWidget *widget) const;
    void focusChanged(QWidget *old, QWidget *now);
    void takeOver(QFontComboBox *box);
    void picked(QFontComboBox *box);
    void showFontAtCursor();

    QPointer<KFontAction> m_action;
    QPointer<KPIMTextEdit::RichTextComposer> m_editor;
};

}
