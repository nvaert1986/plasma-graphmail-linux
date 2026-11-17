# plasma-graphmail-linux

A native KDE Plasma client for Microsoft 365 mailboxes: mail, calendar, contacts and tasks from
Exchange Online, through Microsoft Graph. Written in C++ with Qt 6 and KDE Frameworks 6.

Status: version 0.12.0, in development; not packaged yet.

## Scope

- Exchange Online (Microsoft 365) work and school accounts only, through Microsoft Graph
- Mail, calendar, contacts and Microsoft To Do tasks
- Shared mailboxes, shared and delegated calendars and contact folders
- A local cache (all message headers, recent bodies) with full-text search
- A system tray icon with the unread count and the reminders that are due (Snooze, Dismiss)
- A daily check for a new release on GitHub (Settings > Updates; off with `-DGRAPHMAIL_UPDATE_CHECK=OFF`)

Not planned: IMAP/POP3, on-premises Exchange, Outlook add-ins, public folders.

## Building

Needs Qt 6.8 or newer, KDE Frameworks 6.13 or newer and these libraries (CMake package, with the
Gentoo package that provides it):

| Needed for | CMake packages | Gentoo |
|---|---|---|
| Build | ECM, PkgConfig | kde-frameworks/extra-cmake-modules, virtual/pkgconfig |
| Qt | Qt6 Core, Gui, Widgets, Network, Concurrent, Sql (SQLite), DBus, OpenGLWidgets, WebEngineWidgets, PrintSupport, Test | dev-qt/qtbase, dev-qt/qtwebengine |
| KDE Frameworks | KF6 Config, ConfigWidgets, CoreAddons, DBusAddons, I18n, IconThemes, KIO, Notifications, NotifyConfig, Parts, WidgetsAddons, XmlGui | the kde-frameworks/ package of each |
| Calendar | KF6CalendarCore, KF6Holidays | kde-frameworks/kcalendarcore, kde-frameworks/kholidays |
| Tray icon | Plasma (the widget is a Plasma applet) | kde-plasma/libplasma |
| Composer | KPim6TextEdit, KF6TextEmoticonsWidgets | kde-apps/kpimtextedit, dev-libs/ktextaddons |
| Attached mails, winmail.dat | KPim6MimeTreeParserWidgets, KPim6Mime, KPim6Tnef, KPim6Libkleo | kde-apps/mimetreeparser, kde-apps/kmime, kde-apps/libktnef, kde-apps/libkleo |
| Accounts and sign-in | GraphCore: the suite's shared Microsoft Graph library and its account service `graphaccountsd`, which owns the sign-in for every app of the suite | [plasma-graphcore-linux](https://github.com/nvaert1986/plasma-graphcore-linux) 0.12 (not packaged yet: build and install it first) |
| Cache | libzstd | app-arch/zstd |

```
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build
```

## Optional programs

Graph Mail works without these. They only add the features named here:

- **Okular**: previews of PDF attachments (and of Office documents, see below) inside Graph Mail.
- **LibreOffice**: previews of Word, Excel and PowerPoint attachments. Off by default, because not
  every office uses LibreOffice; turn it on under Settings > Preview Office Documents with
  LibreOffice. Needs Okular too. With OnlyOffice or Microsoft 365 on the web, use Open instead.

Without them, Open shows any attachment in the program the desktop uses for that kind of file.

## Licence

GPL-3.0-or-later, see `LICENSES/`.
