// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <graphcore/connectivity.h>
#include <graphcore/proxy.h>
#include "accounts.h"
#include "daemon.h"

#include <KLocalizedString>
#include <KSignalHandler>

#include <QCoreApplication>

#include <csignal>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("graphmaild"));
    // Accounts are shared by the suite; the app registration per account is this app's own
    QCoreApplication::setApplicationVersion(QStringLiteral(GRAPHMAIL_VERSION));
    KLocalizedString::setApplicationDomain("plasma-graphmail-linux");
    // Every Graph request through the system's proxy (System Settings > Network > Proxy)
    GraphCore::Proxy::useSystemSettings();
    // The network backend (online, metered) loads on the main thread; the mailbox threads only read it
    GraphCore::Connectivity::systemReportsOnline();

    // systemd stops the service with SIGTERM: close every cache cleanly
    KSignalHandler::self()->watchSignal(SIGTERM);
    KSignalHandler::self()->watchSignal(SIGINT);
    QObject::connect(KSignalHandler::self(), &KSignalHandler::signalReceived, &app, &QCoreApplication::quit);

    GraphMail::Daemon daemon;
    QString error;
    if (!daemon.start(&error)) {
        qCritical().noquote() << error;
        return 1;
    }
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &daemon, &GraphMail::Daemon::shutdown);
    return app.exec();
}
