// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "coloraction.h"

#include <KLocalizedString>

#include <QApplication>
#include <QColorDialog>
#include <QGridLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QToolButton>
#include <QWidgetAction>

namespace GraphMail
{

namespace
{
constexpr int columns = 6;
}

QList<QColor> ColorAction::palette()
{
    // Close to Outlook on the web's: blue, green, yellow, orange, red, purple from light to dark,
    // then greys to black
    static const char *const names[] = {"#4fa3f7", "#6fc04b", "#f2c811", "#f7941d", "#ef5a5a", "#b266d5", //
                                        "#2e75d6", "#3e9a37", "#d6a400", "#e07b00", "#d13438", "#8e44ad", //
                                        "#1f4e99", "#2c6e2a", "#a67c00", "#b35900", "#a4262c", "#6b2c91", //
                                        "#0b2e5e", "#17401a", "#6e5200", "#7a3d00", "#6b1519", "#45175e", //
                                        "#ffffff", "#d0d0d0", "#a6a6a6", "#7f7f7f", "#404040", "#000000"};
    QList<QColor> list;
    for (const char *name : names) {
        list.append(QColor(QLatin1String(name)));
    }
    return list;
}

ColorAction::ColorAction(Kind kind, QObject *parent)
    : KToolBarPopupAction(QIcon(), kind == Kind::Text ? i18nc("@action", "Font Color") : i18nc("@action", "Highlight"), parent)
    , m_kind(kind)
    // Outlook starts with red text and yellow highlight
    , m_color(kind == Kind::Text ? QColor(QStringLiteral("#d13438")) : QColor(QStringLiteral("#f2c811")))
{
    setPopupMode(MenuButtonPopup);
    connect(this, &QAction::triggered, this, [this] {
        Q_EMIT colorChosen(m_color);
    });

    QMenu *menu = popupMenu();
    menu->addSection(kind == Kind::Text ? i18nc("@title:menu", "Font color") : i18nc("@title:menu", "Highlight color"));
    auto *grid = new QWidget(menu);
    auto *layout = new QGridLayout(grid);
    layout->setSpacing(4);
    layout->setContentsMargins(8, 4, 8, 4);
    const QList<QColor> colors = palette();
    for (qsizetype i = 0; i < colors.size(); ++i) {
        const QColor color = colors.at(i);
        auto *swatch = new QToolButton(grid);
        swatch->setAutoRaise(true);
        swatch->setFixedSize(22, 22);
        swatch->setToolTip(color.name());
        QPixmap dot(16, 16);
        dot.fill(Qt::transparent);
        QPainter painter(&dot);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QColor(0, 0, 0, 60));
        painter.setBrush(color);
        painter.drawEllipse(QRectF(0.5, 0.5, 15, 15));
        painter.end();
        swatch->setIcon(QIcon(dot));
        connect(swatch, &QToolButton::clicked, this, [this, menu, color] {
            menu->close();
            choose(color);
        });
        layout->addWidget(swatch, int(i / columns), int(i % columns));
    }
    auto *gridAction = new QWidgetAction(menu);
    gridAction->setDefaultWidget(grid);
    menu->addAction(gridAction);
    menu->addSeparator();
    menu->addAction(QIcon::fromTheme(kind == Kind::Text ? QStringLiteral("format-text-color") : QStringLiteral("edit-clear")),
                    kind == Kind::Text ? i18nc("@action:inmenu text colour", "Automatic") : i18nc("@action:inmenu highlight", "No Color"),
                    this,
                    [this] {
                        choose(QColor());
                    });
    menu->addAction(QIcon::fromTheme(QStringLiteral("color-management")), i18nc("@action:inmenu", "More Colors..."), this, [this] {
        const QColor color = QColorDialog::getColor(m_color.isValid() ? m_color : Qt::black, nullptr, text());
        if (color.isValid()) {
            choose(color);
        }
    });
    updateIcon();
}

void ColorAction::clearColor(QTextEdit *editor, bool background)
{
    // "Automatic" / "No Color": the property goes, so the reader's own colours apply
    QTextCursor cursor = editor->textCursor();
    const auto cleared = [background](QTextCharFormat format) {
        if (background) {
            format.clearBackground();
        } else {
            format.clearForeground();
        }
        return format;
    };
    if (!cursor.hasSelection()) {
        editor->setCurrentCharFormat(cleared(cursor.charFormat()));
        return;
    }
    const int start = cursor.selectionStart();
    const int end = cursor.selectionEnd();
    cursor.beginEditBlock();
    for (QTextBlock block = editor->document()->findBlock(start); block.isValid() && block.position() < end; block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const int from = std::max(start, fragment.position());
            const int to = std::min(end, fragment.position() + fragment.length());
            if (from >= to) {
                continue;
            }
            QTextCursor part(editor->document());
            part.setPosition(from);
            part.setPosition(to, QTextCursor::KeepAnchor);
            part.setCharFormat(cleared(fragment.charFormat()));
        }
    }
    cursor.endEditBlock();
}

void ColorAction::choose(const QColor &color)
{
    // "Automatic" / "No Color" applies once; the button keeps the last real colour
    if (color.isValid()) {
        m_color = color;
        updateIcon();
    }
    Q_EMIT colorChosen(color);
}

void ColorAction::updateIcon()
{
    // As Outlook draws them: an "A" (Font Color) or a highlighter, with the colour as a bar under it
    const QIcon marker = QIcon::fromTheme(QStringLiteral("draw-highlight"), QIcon::fromTheme(QStringLiteral("format-fill-color")));
    QIcon icon;
    for (const int size : {16, 22, 32}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        const int bar = std::max(3, size / 6);
        const QRect glyph(0, 0, size, size - bar);
        if (m_kind == Kind::Text) {
            QFont font = QApplication::font();
            font.setBold(true);
            font.setPixelSize(size - bar);
            painter.setFont(font);
            painter.setPen(QApplication::palette().color(QPalette::WindowText));
            painter.drawText(glyph, Qt::AlignHCenter | Qt::AlignBottom, QStringLiteral("A"));
        } else {
            marker.paint(&painter, glyph);
        }
        painter.fillRect(QRect(1, size - bar, size - 2, bar), m_color);
        painter.end();
        icon.addPixmap(pixmap);
    }
    setIcon(icon);
}

}
