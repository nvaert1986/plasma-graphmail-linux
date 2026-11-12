// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

// Process-wide throttling state per key (see throttleKeyFor()). Thread-safe.
namespace GraphCore::Throttle
{

// Exchange Online allows four concurrent requests per app and mailbox
constexpr int maxConcurrentPerMailbox = 4;

// Microsoft asks clients to hold back every request to a throttled mailbox (or account) until
// Retry-After has passed; otherwise the other requests keep hitting it and earn more 429s
void note(const QString &key, qint64 milliseconds);
qint64 waitMs(const QString &key);

// Concurrency slots: takes `count` slots at once, or none (false) when that would exceed `limit`.
// A $batch takes all of a mailbox's slots, because Exchange runs its items concurrently.
bool tryAcquire(const QString &key, int limit, int count = 1);
void release(const QString &key, int count = 1);
int running(const QString &key);

void resetForTests();

}
