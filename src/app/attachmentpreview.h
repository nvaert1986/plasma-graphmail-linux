// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QPointer>

#include <memory>

class QLabel;
class QProcess;
class QStackedWidget;
class QTemporaryDir;

namespace KParts
{
class ReadOnlyPart;
}

namespace GraphMail
{

// Preview of an attachment without opening another program (docs/PHASE9B.md 9b.25): pictures and
// text always; PDF through Okular's viewer when Okular is installed; Word, Excel and PowerPoint
// files only when the user turned that on and LibreOffice is installed (it makes a PDF of them).
// What is missing is named in the dialog, and Open always remains.
class AttachmentPreview : public QDialog
{
    Q_OBJECT

public:
    enum class Kind { Image, Pdf, Text, Office, None };
    // What the preview can do with a file of this name and content
    static Kind kindOf(const QString &name, const QByteArray &data);
    // What the optional parts need: Okular (PDF), LibreOffice (Office documents)
    static bool hasOkular();
    static bool hasLibreOffice();
    // Settings > Preview Office Documents with LibreOffice (off unless the user turns it on)
    static bool officePreview();
    static void setOfficePreview(bool on);

    AttachmentPreview(const QString &name, const QByteArray &data, QWidget *parent = nullptr);
    ~AttachmentPreview() override;

Q_SIGNALS:
    // "Open" for what has no preview: the reading pane opens the attachment as before
    void openRequested();

private:
    void showPdf(const QString &file);
    void showMessage(const QString &text);
    void convertOffice(const QString &input);
    // The attachment as a private file for Okular or LibreOffice; empty on failure
    QString workFile(const QString &name, const QByteArray &data);

    QStackedWidget *m_stack;
    QLabel *m_message;
    KParts::ReadOnlyPart *m_part = nullptr;
    // LibreOffice while it converts: stopped (and disconnected) when the dialog closes first
    QPointer<QProcess> m_process;
    std::unique_ptr<QTemporaryDir> m_work;
};

}
