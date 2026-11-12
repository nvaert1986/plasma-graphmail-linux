// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace GraphCore::Connectivity
{

// Whether the system reports a usable network (NetworkManager through QNetworkInformation). When
// the system cannot tell, this says online: a request then finds out (GraphResponse::isNetworkError).
// Call from the main thread first: the backend is loaded there.
bool systemReportsOnline();

// Whether the connection is metered (mobile hotspot); false when unknown
bool isMetered();

}
