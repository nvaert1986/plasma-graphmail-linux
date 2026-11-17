// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "updates.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QStandardPaths>

namespace GraphMail::Updates
{

namespace
{
KConfigGroup general()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals);
    // The app and the daemon both write this file: always the current state
    config->reparseConfiguration();
    return config->group(QStringLiteral("General"));
}
}

bool compiledIn()
{
#ifdef GRAPHMAIL_UPDATE_CHECK
    return true;
#else
    return false;
#endif
}

bool enabled()
{
    return compiledIn() && general().readEntry("CheckForUpdates", true);
}

void setEnabled(bool on)
{
    KConfigGroup group = general();
    group.writeEntry("CheckForUpdates", on);
    group.sync();
}

QString currentVersion()
{
#ifdef GRAPHMAIL_TESTING
    // Tests only (Debug builds of this project, never in a package): pretend to be another version
    if (const QString test = qEnvironmentVariable("GRAPHMAIL_TEST_CURRENT_VERSION"); !test.isEmpty()) {
        return test;
    }
#endif
    return QStringLiteral(GRAPHMAIL_VERSION);
}

GraphCore::UpdateCheckSettings settings()
{
    GraphCore::UpdateCheckSettings settings;
    settings.repository = QStringLiteral("nvaert1986/plasma-graphmail-linux");
    settings.currentVersion = currentVersion();
    settings.stateFile = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/graphmail/updatecheck");
    settings.userAgent = QStringLiteral("plasma-graphmail-linux/%1").arg(QStringLiteral(GRAPHMAIL_VERSION));
#ifdef GRAPHMAIL_TESTING
    // Tests only: ask a local server instead of GitHub
    if (const QString test = qEnvironmentVariable("GRAPHMAIL_TEST_UPDATE_URL"); !test.isEmpty()) {
        settings.latestUrl = QUrl(test);
    }
#endif
    return settings;
}

}
