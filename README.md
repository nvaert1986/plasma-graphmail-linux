# plasma-graphmail-linux

A native KDE Plasma client for Microsoft 365 mailboxes: mail, calendar, contacts and tasks from
Exchange Online, through Microsoft Graph. Written in C++ with Qt 6 and KDE Frameworks 6.

Status: early development. Nothing to use yet.

## Scope

- Exchange Online (Microsoft 365) work and school accounts only, through Microsoft Graph
- Mail, calendar, contacts and Microsoft To Do tasks
- Shared mailboxes, shared and delegated calendars and contact folders
- A local cache (all message headers, recent bodies) with full-text search

Not planned: IMAP/POP3, on-premises Exchange, Outlook add-ins, public folders.

## Building

Needs Qt 6.8 or newer, KDE Frameworks 6.13 or newer and QtKeychain.

```
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build
```

## Licence

GPL-3.0-or-later, see `LICENSES/`.
