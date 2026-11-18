# SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.25)

# The version follows the last finished phase (0.1.0 = phase 1), patch numbers for fixes
project(plasma-graphmail-linux VERSION 0.14.1 LANGUAGES CXX)

# Same floor as plasma-onedrive-linux: the shared Graph library is built by both
set(QT_MIN_VERSION "6.8.0")
set(KF_MIN_VERSION "6.13.0")
set(KDE_COMPILERSETTINGS_LEVEL "6.0")

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMQtDeclareLoggingCategory)
include(ECMAddTests)
include(FeatureSummary)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Gui Widgets Network Concurrent Sql DBus OpenGLWidgets WebEngineWidgets PrintSupport Test)
find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS Config ConfigWidgets CoreAddons DBusAddons I18n IconThemes KIO Notifications NotifyConfig Parts WidgetsAddons XmlGui)
# The suite's shared Microsoft Graph core and its account service (plasma-graphcore-linux, phase 10):
# accounts, the Graph client, sign-in through graphaccountsd
find_package(GraphCore 0.14 REQUIRED)
# The tray widget (plasmoid/): installed as a Plasma applet
find_package(Plasma REQUIRED)
# KDE PIM: attached mails (.eml) in KDE's mail viewer, Outlook's winmail.dat
# MimeTreeParserCore links libkleo without finding it itself
find_package(KPim6Libkleo REQUIRED)
find_package(KPim6MimeTreeParserWidgets REQUIRED)
# kmime moved to KDE Frameworks (CMake package KF6Mime, target KF6::Mime) in 2026; older releases still
# ship KPim6Mime (KPim6::Mime). The headers are <KMime/...> in both. graphmail_kmime stands for whichever exists
find_package(KF6Mime QUIET)
add_library(graphmail_kmime INTERFACE)
if(KF6Mime_FOUND)
    set(GRAPHMAIL_KMIME_TARGET KF6::Mime)
else()
    find_package(KPim6Mime REQUIRED)
    set(GRAPHMAIL_KMIME_TARGET KPim6::Mime)
endif()
target_link_libraries(graphmail_kmime INTERFACE ${GRAPHMAIL_KMIME_TARGET})
# kmime 25.x changed its API (CreatePolicy, owning pointers, NewlineType); Debian 13 still has 24.12
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES ${GRAPHMAIL_KMIME_TARGET})
check_cxx_source_compiles("
#include <KMime/Message>
int main() { KMime::Message m; return m.subject(KMime::CreatePolicy::DontCreate) ? 1 : 0; }
" GRAPHMAIL_KMIME_CREATE_POLICY)
unset(CMAKE_REQUIRED_LIBRARIES)
if(GRAPHMAIL_KMIME_CREATE_POLICY)
    add_compile_definitions(GRAPHMAIL_KMIME_CREATE_POLICY)
endif()
find_package(KPim6Tnef REQUIRED)
find_package(KPim6TextEdit REQUIRED)
# The composer's emoji picker (KTextAddons, already there for KPim6TextEdit)
find_package(KF6TextEmoticonsWidgets REQUIRED)
# Holidays in the calendar (KDE's holiday regions)
find_package(KF6Holidays REQUIRED)
find_package(KF6CalendarCore REQUIRED)
# Bodies and attachments in the blob store are zstd-compressed
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

# The daemon starts the app by absolute path (notification actions): a systemd user service does not
# necessarily have the install prefix (for example ~/.local/bin) in PATH
# The daily check for a new release on GitHub (packages from GitHub come from no repository).
# Distributions that ship updates themselves turn it off
option(GRAPHMAIL_UPDATE_CHECK "Check GitHub once a day for a new release and tell the user" ON)
add_feature_info(UpdateCheck GRAPHMAIL_UPDATE_CHECK "daily check for a new Graph Mail release on GitHub")

add_compile_definitions(GRAPHMAIL_VERSION="${PROJECT_VERSION}" GRAPHMAIL_BINDIR="${KDE_INSTALL_FULL_BINDIR}")

add_subdirectory(src)
add_subdirectory(data)
add_subdirectory(icons)
ki18n_install(po)

# Graph Mail in the system tray: unread mail and the reminders that are due
plasma_install_package(plasmoid/package io.github.nvaert1986.graphmail.tray)

if(BUILD_TESTING)
    add_subdirectory(autotests)
endif()

feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
