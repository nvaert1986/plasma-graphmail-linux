// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <KPIMTextEdit/RichTextComposer>

namespace GraphMail
{

// The composer's and the signature editor's text: KPIMTextEdit's editor, with pasted HTML from a
// browser (Outlook on the web) rewritten first into what Qt's HTML import keeps, so a signature
// copied from there keeps its layout
class MailEditor : public KPIMTextEdit::RichTextComposer
{
    Q_OBJECT

public:
    using KPIMTextEdit::RichTextComposer::RichTextComposer;

    // Qt's import drops CSS widths of table cells and reads a unitless line-height (1.333, the
    // font size times that in a browser) as a percentage of its own line spacing, which comes out
    // a third larger. Widths become width attributes, line heights pixels from the font size.
    static QString htmlForQt(const QString &html);

protected:
    void insertFromMimeData(const QMimeData *source) override;
};

}
