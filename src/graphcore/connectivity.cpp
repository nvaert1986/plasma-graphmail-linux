// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "connectivity.h"

#include <QNetworkInformation>

namespace GraphCore::Connectivity
{

namespace
{
QNetworkInformation *information()
{
    if (!QNetworkInformation::instance()) {
        QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability);
    }
    return QNetworkInformation::instance();
}
}

bool systemReportsOnline()
{
    QNetworkInformation *info = information();
    if (!info) {
        return true;
    }
    switch (info->reachability()) {
    case QNetworkInformation::Reachability::Disconnected:
    case QNetworkInformation::Reachability::Local:
        return false;
    default:
        return true;
    }
}

bool isMetered()
{
    QNetworkInformation *info = information();
    return info && info->supports(QNetworkInformation::Feature::Metered) && info->isMetered();
}

}
