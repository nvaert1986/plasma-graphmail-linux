// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "store.h"

#include <QLatin1StringView>

class QSqlQuery;

// What store.cpp and storeextra.cpp both read: the columns of a ListRow and the row itself, kept
// in one place so a new ListRow field cannot be missed in one of the queries
namespace GraphMail::StoreRows
{
extern const QLatin1StringView listColumns;
ListRow readListRow(const QSqlQuery &q);
}
