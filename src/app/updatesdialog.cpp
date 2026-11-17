// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "updatesdialog.h"
#include "updates.h"

#include <KLocalizedString>

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

#include <graphcore/updatecheck.h>

namespace GraphMail
{

UpdatesDialog::UpdatesDialog(QWidget *parent)
    : QDialog(parent)
    , m_checker(new GraphCore::UpdateChecker(Updates::settings(), this))
{
    setWindowTitle(i18nc("@title:window", "Updates"));
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(i18nc("@info", "Graph Mail packages from GitHub come from no repository, so no package manager tells you "
                                            "about a new version. Graph Mail can ask GitHub once a day for its newest release; nothing "
                                            "but that request is sent."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_enabled = new QCheckBox(i18nc("@option:check", "Check for new versions once a day"), this);
    m_enabled->setChecked(Updates::enabled());
    layout->addWidget(m_enabled);
    connect(m_enabled, &QCheckBox::toggled, this, [this](bool on) {
        Updates::setEnabled(on);
        refresh();
    });

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);

    auto *buttons = new QHBoxLayout;
    m_checkNow = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), i18nc("@action:button", "Check Now"), this);
    m_notes = new QPushButton(QIcon::fromTheme(QStringLiteral("internet-services")), i18nc("@action:button", "Release Notes..."), this);
    m_skip = new QPushButton(i18nc("@action:button", "Skip This Version"), this);
    buttons->addWidget(m_checkNow);
    buttons->addWidget(m_notes);
    buttons->addWidget(m_skip);
    buttons->addStretch();
    layout->addLayout(buttons);
    connect(m_checkNow, &QPushButton::clicked, this, [this] {
        m_checker->check(true);
        refresh();
    });
    connect(m_notes, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(m_checker->page());
    });
    connect(m_skip, &QPushButton::clicked, this, [this] {
        m_checker->skip(m_checker->available());
        refresh();
    });
    connect(m_checker, &GraphCore::UpdateChecker::checked, this, [this](bool ok, const QString &error) {
        refresh(ok ? QString() : error);
    });

    layout->addStretch();
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(close);
    setMinimumWidth(480);
    refresh();
}

void UpdatesDialog::checkNowForTests()
{
    m_checkNow->click();
}

QString UpdatesDialog::statusForTests() const
{
    return m_status->text() + (m_skip->isVisible() ? QStringLiteral(" [notes, skip]") : QString());
}

void UpdatesDialog::refresh(const QString &error)
{
    const bool on = m_enabled->isChecked();
    QStringList lines{i18nc("@info", "This is version %1.", Updates::currentVersion())};
    const QString available = m_checker->available();
    if (m_checker->busy()) {
        lines.append(i18nc("@info", "Asking GitHub..."));
    } else if (!error.isEmpty()) {
        lines.append(i18nc("@info %1 the reason", "Could not ask GitHub: %1", error));
    } else if (!available.isEmpty()) {
        lines.append(i18nc("@info %1 a version", "Version %1 is available.", available));
    } else if (const QString latest = m_checker->latest(); !latest.isEmpty()) {
        lines.append(i18nc("@info %1 a version", "This is the newest version (newest release: %1).", latest));
    }
    if (const QDateTime last = m_checker->lastCheck(); last.isValid()) {
        lines.append(i18nc("@info %1 a date and time", "Last asked: %1", QLocale().toString(last.toLocalTime(), QLocale::ShortFormat)));
    }
    m_status->setText(lines.join(QLatin1Char('\n')));
    m_checkNow->setEnabled(on && !m_checker->busy());
    m_notes->setVisible(!available.isEmpty());
    m_skip->setVisible(!available.isEmpty());
}

}
