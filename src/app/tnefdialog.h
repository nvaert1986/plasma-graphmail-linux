// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

#include <memory>

namespace KTnef
{
class KTNEFParser;
}
class QListWidget;

namespace GraphMail
{

// The files inside a winmail.dat (Outlook's rich-text "TNEF" wrapper, which mail from some Outlook
// setups carries instead of normal attachments): open or save each, or save all.
class TnefDialog : public QDialog
{
    Q_OBJECT

public:
    // Returns nullptr when the file is not a readable TNEF container
    static TnefDialog *open(const QString &path, const QString &extractDir, QWidget *parent);
    ~TnefDialog() override;

private:
    TnefDialog(std::unique_ptr<KTnef::KTNEFParser> parser, const QString &extractDir, QWidget *parent);
    void openFile(const QString &name);
    void saveFile(const QString &name);
    void saveAll();

    std::unique_ptr<KTnef::KTNEFParser> m_parser;
    QString m_extractDir;
    QListWidget *m_list;
};

}
