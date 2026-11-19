# Changelog

All notable changes to Graph Mail (plasma-graphmail-linux). The suite's apps share major.minor
(0.14 with plasma-graphcore-linux 0.14); the third number counts fixes. Fixes for a series go on its
branch (`0.14`).

## 0.14.2 (2026-11-19)

**Fixed**
- Read and unread mail are clearly apart, also in dark colour schemes, as in Outlook: read mail's
  sender and subject in a softer colour, a wider unread bar, and in a dark scheme the unread subject
  in a lighter accent colour (in a dark theme the difference was hard to see).
- Flagged mails have a light yellow row in the message list, as in Outlook (a dark yellow in dark
  colour schemes).
- Links in calendar items open: Ctrl+click (a plain click when the item cannot be edited), through
  Graph Mail's own link handling (Teams join, synced SharePoint/OneDrive copy, browser); web
  addresses in plain-text descriptions are links now.
- A signature keeps its font size in a new message (a size changed from 11 to 12 came back as 11).
  Existing signatures: open Settings > Signatures once and press OK.
- The labels in the left bar are never cut off ("Contactpersonen" in Dutch).

## 0.14.1 (2026-11-18)

**Fixed**
- Mails are drawn without the graphics card by default (Settings > Hardware Acceleration for Mail):
  the graphics driver crashed on some laptops with Intel and NVIDIA graphics.
- Narrow windows stay usable: a short date in the reading pane, a wrapping sender, minimum widths
  for the folders and the message list.
- Settings > Updates explains that the releases are not public yet.

## 0.14.0 (2026-11-17)

First release with packages (Debian 13, Fedora, Arch). Accounts under System Settings > Microsoft
365 Accounts, the system's proxy, no background downloads on metered connections, clear error
messages, people search and photos from plasma-graphcore-linux. Needs plasma-graphcore-linux 0.14.
