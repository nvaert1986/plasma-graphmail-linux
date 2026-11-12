// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QCheckBox;
class QDateEdit;
class QLineEdit;
class QRadioButton;

namespace GraphMail
{

// A new or changed Outlook search folder (docs/PHASE8.md): a name and simple rules that become
// Exchange's filter (OData). The filter line shows the result and may be edited by hand; rules on
// recipients are not possible (Exchange refuses them in a search folder).
class SearchFolderDialog : public QDialog
{
    Q_OBJECT

public:
    // folderName: the folder shown now ("Look in"), empty when only the whole mailbox fits
    SearchFolderDialog(const QString &name, const QString &filter, const QString &folderName, bool editing, QWidget *parent = nullptr);

    QString name() const;
    QString filter() const;
    // Only the folder shown now (with its subfolders) instead of the whole mailbox
    bool currentFolderOnly() const;

    // The rules as Exchange's filter (also used by the tests)
    static QString buildFilter(const QString &from, const QString &subject, bool unread, bool flagged, bool attachments, bool important, const QDate &since);

private:
    void rebuild();
    // Puts a filter this dialog made back into the rules; anything else stays as typed
    void parse(const QString &filter);

    QLineEdit *m_name;
    QCheckBox *m_fromOn;
    QLineEdit *m_from;
    QCheckBox *m_subjectOn;
    QLineEdit *m_subject;
    QCheckBox *m_unread;
    QCheckBox *m_flagged;
    QCheckBox *m_attachments;
    QCheckBox *m_important;
    QCheckBox *m_sinceOn;
    QDateEdit *m_since;
    QRadioButton *m_everywhere;
    QRadioButton *m_here;
    QLineEdit *m_filter;
    bool m_building = false;
};

}
