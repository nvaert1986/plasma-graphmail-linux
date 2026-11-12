# SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

configure_file(plasma-graphmail-linux.service.in ${CMAKE_CURRENT_BINARY_DIR}/plasma-graphmail-linux.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/plasma-graphmail-linux.service DESTINATION ${KDE_INSTALL_SYSTEMDUSERUNITDIR})

install(FILES plasma-graphmail-linux.notifyrc DESTINATION ${KDE_INSTALL_KNOTIFYRCDIR})

# D-Bus activation: the app (or graphmailctl) starts the daemon through systemd when it is not running
configure_file(io.github.nvaert1986.GraphMail.service.in ${CMAKE_CURRENT_BINARY_DIR}/io.github.nvaert1986.GraphMail.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.nvaert1986.GraphMail.service DESTINATION ${KDE_INSTALL_DBUSSERVICEDIR})

configure_file(io.github.nvaert1986.graphmail.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/io.github.nvaert1986.graphmail.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.nvaert1986.graphmail.desktop DESTINATION ${KDE_INSTALL_APPDIR})
