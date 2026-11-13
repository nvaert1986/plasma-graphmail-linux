// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

// Compound files ([MS-CFB], docs/PHASE9B.md): the container of .msg files. Read whole into a tree
// of storages and streams; written as version 3 (512-byte sectors), streams under 4 KB in the
// mini stream, siblings as a balanced binary tree (all nodes black, which the spec allows).
namespace GraphMail::Cfb
{

struct Entry {
    QString name;
    bool storage = false;
    // A stream's bytes
    QByteArray data;
    // A storage's children
    QList<Entry> children;

    // A child by name (compared as the spec does: case-insensitive); nullptr when absent
    const Entry *child(const QString &name) const;
    // The new storage; the reference holds until the next child is added here
    Entry &addStorage(const QString &name);
    void addStream(const QString &name, const QByteArray &data);
};

// The root storage of a compound file; nullopt with the reason for a damaged or foreign file
std::optional<Entry> read(const QByteArray &file, QString *error);
// A compound file holding the root's children
QByteArray write(const Entry &root);

// The spec's order of sibling names (2.6.4): shorter first, then by uppercased UTF-16 units
bool lessThan(const QString &a, const QString &b);

}
