// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pstltpwriter.h"
#include "pstmessaging.h"

#include <QMap>

// Writes a new Unicode PST at the messaging layer ([MS-PST] 2.4, 2.6.3 and 2.7, docs/PST-NOTES.md):
// the minimum PST (store, name map, root folder, IPM subtree, Search Root, Deleted Items, the spam
// search folder, the table templates, the search queues), then folders and messages as they are
// added. Messages go to the file at once; folders, tables and the name map follow in finish().
namespace GraphMail::Pst
{

class PstWriter
{
public:
    // Fixed folders of the minimum PST
    static constexpr quint32 nidIpmSubtree = 0x8022;
    static constexpr quint32 nidSearchRoot = 0x8042;
    static constexpr quint32 nidDeletedItems = 0x8062;
    static constexpr quint32 nidSpamSearchFolder = 0x2223;

    bool create(const QString &path, const QString &displayName, QString *error);
    // A folder under the IPM subtree or a folder made here; 0 when the parent is not such a folder
    quint32 addFolder(quint32 parent, const QString &name, const QString &containerClass = QStringLiteral("IPF.Note"));
    // A message (or a folder associated, FAI, message) with its recipients and attachments;
    // attached messages are written with it. Returns its NID; 0 when the folder is not one made
    // here or the IPM subtree's, or after a write failure (then error() says why).
    quint32 addMessage(quint32 folder, const Message &message, bool associated = false);
    // The property ID (0x8000 and up) of a named property in this file
    quint16 namedProperty(const QUuid &guid, quint32 id);
    quint16 namedProperty(const QUuid &guid, const QString &name);
    bool finish(QString *error);

    // Why writing failed; the writer is of no further use then
    const QString &error() const
    {
        return m_ndb.failed() ? m_ndb.error() : m_error;
    }

private:
    struct Folder {
        quint32 nid = 0;
        quint32 parent = 0;
        QString name;
        QString containerClass;
        bool search = false;
        QList<quint32> children;
        QList<QHash<quint32, PropValue>> contents;
        QList<QHash<quint32, PropValue>> associated;
        qint64 unread = 0;
    };
    quint32 folderNode(quint32 nid, quint32 parent, const QString &name, const QString &containerClass);
    struct Written {
        quint64 data = 0;
        quint64 sub = 0;
        // PidTagMessageSize: the data bytes of the node and all its subnodes, as Outlook counts
        qint64 bytes = 0;
        // The properties as written (defaults and sizes included), for the folder's table row
        PropertyMap properties;
    };
    Written writeMessage(const Message &message, int depth, bool associated = false);
    PropertyMap folderProperties(const Folder &folder) const;
    void writeTable(quint32 nid, const QList<quint32> &columns, const QList<QHash<quint32, PropValue>> &rows);
    void writePc(quint32 nid, quint32 parent, const PropertyMap &properties);
    PropertyMap nameMap() const;
    QByteArray entryId(quint32 nid) const;
    quint16 addName(const NamedProperty &name);

    NdbWriter m_ndb;
    QString m_error;
    QString m_displayName;
    QByteArray m_recordKey;
    QMap<quint32, Folder> m_folders;
    QList<NamedProperty> m_names;
    quint32 m_rowVersion = 0;
};

}
