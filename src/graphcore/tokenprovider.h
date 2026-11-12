// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

namespace GraphCore
{

// Hands out access tokens for Microsoft Graph. Auth implements it for one signed-in account; the
// planned account service will hand tokens out over D-Bus through the same interface.
class TokenProvider
{
public:
    virtual ~TokenProvider() = default;

    // Blocking and thread-safe: a valid token, refreshed when it expires within five minutes.
    // forceRefresh is used after a 401 from Graph. Empty on failure, with *error set.
    virtual QString accessToken(QString *error, bool forceRefresh = false) = 0;

    // Never blocks: the current token while it is valid for more than five minutes, else empty
    virtual QString cachedAccessToken() = 0;
};

}
