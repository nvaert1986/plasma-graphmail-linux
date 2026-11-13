// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pstmessaging.h"

#include <QJsonObject>
#include <QStringList>

// From MAPI properties (a PST or .msg message) to what Graph takes to create the message in a
// mailbox (docs/PHASE9B.md): received mail, not a draft, with its dates, read state, flag,
// categories and attachments. Property IDs from [MS-OXPROPS], values from [MS-OXCMSG] and
// [MS-OXOFLAG].
namespace GraphMail::MapiGraph
{

struct File {
    QString name;
    QString contentType;
    QString contentId;
    bool isInline = false;
    QByteArray data;
};

struct Import {
    // For POST /mailFolders/{id}/messages
    QJsonObject message;
    // fileAttachments, sent after the message (upload sessions over 3 MB)
    QList<File> files;
    // itemAttachments: attached messages, {"name", "item"}
    QList<QJsonObject> items;
    // What did not come along, for the import's log
    QStringList skipped;
};

// The body to show or send on: PidTagHtml, else HTML encapsulated in the RTF, else PidTagBody,
// else the RTF's text ([MS-OXRTFEX]); note says what was lost, if anything
struct Body {
    bool html = false;
    QString content;
    QString note;
};
Body bestBody(const Pst::Message &message);

// Mail Graph can hold: notes, meeting messages, reports, posts; not contacts, appointments,
// tasks or notes (those are other kinds of items)
bool isMail(const QString &messageClass);

Import toGraph(const Pst::Message &message, const QHash<quint16, Pst::NamedProperty> &names);

// An SMTP address of a sender or recipient: the SMTP property, else the address when its type is
// SMTP; empty for Exchange-internal (EX) addresses without an SMTP form
QString smtpAddress(const QString &smtp, const QString &address, const QString &addressType);

}
