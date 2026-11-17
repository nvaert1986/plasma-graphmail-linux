// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <graphcore/updatecheck.h>

#include <QString>

// New versions of Graph Mail: packages from GitHub come from no repository, so no package manager
// tells their users about a new release. The check is the suite's (GraphCore::UpdateChecker, the
// same as Graph Sync's); this says which repository and version, where the answer is kept, and
// whether the user wants it. graphmaild announces a new version, Settings > Updates shows it.
namespace GraphMail::Updates
{
// Built with the check (CMake option GRAPHMAIL_UPDATE_CHECK); distributions that ship updates
// themselves leave it out
bool compiledIn();
// graphmailrc [General] CheckForUpdates (on unless the user turned it off)
bool enabled();
void setEnabled(bool on);
// The version running here
QString currentVersion();
// Graph Mail's repository, version, state file (state folder/graphmail/updatecheck), User-Agent
GraphCore::UpdateCheckSettings settings();
}
