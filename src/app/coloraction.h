// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <KToolBarPopupAction>

#include <QColor>

class QTextEdit;

namespace GraphMail
{

// Outlook's Font Color and Highlight buttons: the icon with the last colour as a bar under it; a
// click applies that colour again, the arrow opens a palette with "More Colors...". An invalid
// colour is "Automatic" (text) or "No Color" (highlight).
class ColorAction : public KToolBarPopupAction
{
    Q_OBJECT

public:
    enum class Kind { Text, Highlight };
    ColorAction(Kind kind, QObject *parent);

    QColor color() const
    {
        return m_color;
    }
    // The palette, row by row (tests)
    static QList<QColor> palette();
    // "Automatic" / "No Color": the property goes from the selection (or what is typed next), so
    // the reader's own colours apply
    static void clearColor(QTextEdit *editor, bool background);

Q_SIGNALS:
    void colorChosen(const QColor &color);

private:
    void choose(const QColor &color);
    void updateIcon();

    Kind m_kind;
    QColor m_color;
};

}
