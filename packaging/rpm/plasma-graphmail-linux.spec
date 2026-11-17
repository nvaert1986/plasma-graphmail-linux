# SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Build: rpmbuild -ba --define "_sourcedir $PWD" plasma-graphmail-linux.spec
# (with plasma-graphmail-linux-<version>.tar.gz next to it)

Name:           plasma-graphmail-linux
Version:        0.14.0
Release:        1%{?dist}
Summary:        Mail, calendar, contacts and tasks of Microsoft 365 for KDE Plasma
License:        GPL-3.0-or-later
URL:            https://github.com/nvaert1986/plasma-graphmail-linux
Source0:        %{url}/archive/refs/tags/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  extra-cmake-modules
BuildRequires:  gettext
BuildRequires:  plasma-graphcore-linux-devel >= 0.14.0
BuildRequires:  pkgconfig(libzstd)
BuildRequires:  cmake(Qt6Core)
BuildRequires:  cmake(Qt6Concurrent)
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6OpenGLWidgets)
BuildRequires:  cmake(Qt6PrintSupport)
BuildRequires:  cmake(Qt6Sql)
BuildRequires:  cmake(Qt6Test)
BuildRequires:  cmake(Qt6WebEngineWidgets)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(KF6CalendarCore)
BuildRequires:  cmake(KF6Config)
BuildRequires:  cmake(KF6ConfigWidgets)
BuildRequires:  cmake(KF6CoreAddons)
BuildRequires:  cmake(KF6DBusAddons)
BuildRequires:  cmake(KF6Holidays)
BuildRequires:  cmake(KF6I18n)
BuildRequires:  cmake(KF6IconThemes)
BuildRequires:  cmake(KF6KIO)
BuildRequires:  cmake(KF6Notifications)
BuildRequires:  cmake(KF6NotifyConfig)
BuildRequires:  cmake(KF6Parts)
BuildRequires:  cmake(KF6TextEmoticonsWidgets)
BuildRequires:  cmake(KF6WidgetsAddons)
BuildRequires:  cmake(KF6XmlGui)
BuildRequires:  cmake(KPim6Libkleo)
BuildRequires:  cmake(KF6Mime)
BuildRequires:  cmake(KPim6MimeTreeParserWidgets)
BuildRequires:  cmake(KPim6TextEdit)
BuildRequires:  cmake(KPim6Tnef)
BuildRequires:  cmake(Plasma)
# Needed by the CMake config of mimetreeparser, which mimetreeparser-devel does not pull in itself
BuildRequires:  cmake(KPim6Mbox)
BuildRequires:  cmake(KF6SyntaxHighlighting)

# The library comes with rpm's automatic requires. The account service the sign-in goes through:
Requires:       plasma-graphcore-linux >= 0.14.0
# Loaded at run time, so rpm cannot find them by itself: the QML modules of the tray applet
# (plasma-workspace has org.kde.plasma.workspace.dbus and pulls in libplasma, which has org.kde.plasma.plasmoid)
Requires:       kf6-kcmutils
Requires:       kf6-kirigami
Requires:       plasma-workspace
Requires:       qt6-qtdeclarative
# The SQLite driver of the local cache is part of qt6-qtbase
Requires:       qt6-qtbase
# Previews of PDF and Office attachments: Okular's part, and LibreOffice for the conversion
Recommends:     okular
Recommends:     libreoffice-writer

%description
A native client for Exchange Online mailboxes through Microsoft Graph: mail,
calendar, contacts and Microsoft To Do tasks, shared mailboxes and calendars,
a local cache with full-text search, a system tray applet with the unread
count and the reminders that are due, and a KRunner search of your mail. The
sign-in comes from the account service of plasma-graphcore-linux. Not
affiliated with Microsoft.

%prep
%autosetup -p1

%build
%cmake -DBUILD_TESTING=OFF
%cmake_build

%install
%cmake_install
%find_lang %{name}
%find_lang plasma_applet_io.github.nvaert1986.graphmail.tray

%files -f %{name}.lang -f plasma_applet_io.github.nvaert1986.graphmail.tray.lang
%license LICENSES/GPL-3.0-or-later.txt
%doc README.md
%{_bindir}/graphmailctl
%{_bindir}/graphmaild
%{_bindir}/plasma-graphmail-linux
%{_datadir}/applications/io.github.nvaert1986.graphmail.desktop
%{_datadir}/dbus-1/services/io.github.nvaert1986.GraphMail.service
%{_datadir}/icons/hicolor/scalable/apps/io.github.nvaert1986.graphmail.svg
%{_datadir}/knotifications6/plasma-graphmail-linux.notifyrc
%{_datadir}/krunner/dbusplugins/io.github.nvaert1986.graphmail-runner.desktop
%{_datadir}/mime/packages/io.github.nvaert1986.graphmail-mime.xml
%{_datadir}/plasma/plasmoids/io.github.nvaert1986.graphmail.tray/
%{_userunitdir}/plasma-graphmail-linux.service

%changelog
* Tue Nov 17 2026 nvaert1986 <25772781+nvaert1986@users.noreply.github.com> - 0.14.0-1
- First packaged release: mail, calendar, contacts and tasks of Exchange Online, the tray
  applet, KRunner search, the sign-in through the account service of plasma-graphcore-linux
