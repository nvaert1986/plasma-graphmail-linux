# Changelog

All notable changes to Graph Mail (plasma-graphmail-linux). The suite's apps share major.minor
(0.14 with plasma-graphcore-linux 0.14); the third number counts fixes. Fixes for a series go on its
branch (`0.14`).

## 0.14.4 (2026-11-23)

**Added**
- A spelling language box on the composer's formatting row. **Automatic** (the default) follows the
  language you write in, also when KDE's own language detection is off; a language picked by hand
  stays for that mail. The choice is remembered per sending mailbox.
- Search: **Best Match First** in the search's Filters menu: mail with the words in the subject or
  sender comes before mail that only mentions them. Off: the newest mail first, as before.
- The packages recommend KDE's hunspell spell-check plugin and the Dutch and English dictionaries;
  without a dictionary the language box says what to install.

**Fixed**
- Search matched only the last word as the start of a word, every earlier word had to be complete:
  "fact jans" found nothing. Every word now matches as the start of a word ("factuur Jansen"); put a
  word in quotes to match it whole.
- Search lists Microsoft 365's hits for mail that is not stored on this computer (older mail, a
  folder still copying) instead of leaving them out.
- The free/busy times of colleagues who did not share their calendar were only fetched again when
  the calendar was redrawn, at most every ten minutes, and never by Sync: Sync now fetches them, and
  an open calendar window asks again every five minutes. When Microsoft 365 cannot be reached, the
  times shown stay.
- A colleague's calendar added as a calendar of its own (shared with you, without their mail)
  could show nothing new after it was first copied, also not after Sync: it is now read whole on
  Sync and every quarter of an hour, besides the quick check for changes.
- Typing a font name in the composer's font box with text selected replaced the text from the second
  letter on: every letter was applied as a font and put the cursor back in the text. The box now
  keeps the cursor while you type and applies the font on Enter or a pick from its list.

## 0.14.3 (2026-11-19)

**Added**
- Reply, Reply All and Forward buttons at the top of the reading pane, as in Outlook, with a "…"
  button for the rest of the message menu. In a narrow reading pane they show only their icons.
- Quick buttons on the mail under the mouse in the message list: Mark as Read or Unread, Delete and
  Flag by default; the gear button chooses which ones (Archive too).

**Fixed**
- A sent message, and a reply or forward quoting it, could show an older version of its text: the
  body of the draft as it was at an autosave (for example cut off right after a path such as `C:\`
  that was still being typed). The body of a draft is now fetched again after every change and after
  sending, and so is the body of any message whose preview changed. A message cached with an old
  body before 0.14.3 keeps that copy; the version on the server and in the recipients' mailboxes
  was always complete.

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
