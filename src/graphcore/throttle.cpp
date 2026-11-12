// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "throttle.h"

#include <QDateTime>
#include <QHash>
#include <QMutex>

#include <algorithm>

namespace GraphCore::Throttle
{

namespace
{
struct State {
    QMutex mutex;
    QHash<QString, qint64> pausedUntilMs;
    QHash<QString, int> running;
};

State &state()
{
    static State instance;
    return instance;
}
}

void note(const QString &key, qint64 milliseconds)
{
    State &s = state();
    QMutexLocker locker(&s.mutex);
    const qint64 until = QDateTime::currentMSecsSinceEpoch() + milliseconds;
    qint64 &current = s.pausedUntilMs[key];
    current = std::max(current, until);
}

qint64 waitMs(const QString &key)
{
    State &s = state();
    QMutexLocker locker(&s.mutex);
    const auto it = s.pausedUntilMs.constFind(key);
    if (it == s.pausedUntilMs.cend()) {
        return 0;
    }
    const qint64 remaining = *it - QDateTime::currentMSecsSinceEpoch();
    if (remaining <= 0) {
        s.pausedUntilMs.remove(key);
        return 0;
    }
    return remaining;
}

bool tryAcquire(const QString &key, int limit, int count)
{
    State &s = state();
    QMutexLocker locker(&s.mutex);
    const int current = s.running.value(key);
    if (current + count > limit) {
        return false;
    }
    s.running[key] = current + count;
    return true;
}

void release(const QString &key, int count)
{
    State &s = state();
    QMutexLocker locker(&s.mutex);
    const auto it = s.running.find(key);
    if (it != s.running.end() && (*it -= count) <= 0) {
        s.running.erase(it);
    }
}

int running(const QString &key)
{
    State &s = state();
    QMutexLocker locker(&s.mutex);
    return s.running.value(key);
}

void resetForTests()
{
    State &s = state();
    QMutexLocker locker(&s.mutex);
    s.pausedUntilMs.clear();
    s.running.clear();
}

}
