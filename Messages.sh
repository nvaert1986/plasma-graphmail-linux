#!/bin/sh
# SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Extracts translatable strings into $podir (KDE convention; XGETTEXT is set by the caller).
# The menu titles of the .rc file: the same as extractrc does, without needing it
sed -n 's|.*<text>\(.*\)</text>.*|i18n("\1");|p' src/app/*.rc | sed 's/&amp;/\&/g' > rc.cpp
$XGETTEXT rc.cpp $(find src -name '*.cpp' -o -name '*.h') -o $podir/plasma-graphmail-linux.pot
rm -f rc.cpp
