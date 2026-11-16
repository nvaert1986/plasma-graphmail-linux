// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "attachmentpreview.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KParts/PartLoader>
#include <KParts/ReadOnlyPart>
#include <KPluginMetaData>
#include <KSharedConfig>

#include <QBuffer>
#include <QUrl>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLabel>
#include <QMimeDatabase>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
// Larger text is shown up to here: a preview, not an editor
constexpr qsizetype textLimit = 2 * 1024 * 1024;

KPluginMetaData okularPart()
{
    return KPluginMetaData::findPluginById(QStringLiteral("kf6/parts"), QStringLiteral("okularpart"));
}

QString officeProgram()
{
    for (const char *name : {"soffice", "libreoffice"}) {
        if (const QString path = QStandardPaths::findExecutable(QLatin1String(name)); !path.isEmpty()) {
            return path;
        }
    }
    return {};
}
}

AttachmentPreview::Kind AttachmentPreview::kindOf(const QString &name, const QByteArray &data)
{
    const QString suffix = QFileInfo(name).suffix().toLower();
    if (data.startsWith("%PDF")) {
        return Kind::Pdf;
    }
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    if (!QImageReader::imageFormat(&buffer).isEmpty()) {
        return Kind::Image;
    }
    static const QStringList office{QStringLiteral("doc"), QStringLiteral("docx"), QStringLiteral("xls"), QStringLiteral("xlsx"), QStringLiteral("ppt"),
                                    QStringLiteral("pptx"), QStringLiteral("odt"), QStringLiteral("ods"), QStringLiteral("odp"), QStringLiteral("rtf")};
    if (office.contains(suffix)) {
        return Kind::Office;
    }
    const QMimeType type = QMimeDatabase().mimeTypeForFileNameAndData(name, data);
    if (type.inherits(QStringLiteral("text/plain"))) {
        return Kind::Text;
    }
    return Kind::None;
}

bool AttachmentPreview::hasOkular()
{
    return okularPart().isValid();
}

bool AttachmentPreview::hasLibreOffice()
{
    return !officeProgram().isEmpty();
}

bool AttachmentPreview::officePreview()
{
    return KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Reading")).readEntry("OfficePreview", false);
}

void AttachmentPreview::setOfficePreview(bool on)
{
    KConfigGroup group = KSharedConfig::openConfig(QStringLiteral("graphmailrc"), KConfig::NoGlobals)->group(QStringLiteral("Reading"));
    group.writeEntry("OfficePreview", on);
    group.sync();
}

AttachmentPreview::AttachmentPreview(const QString &name, const QByteArray &data, QWidget *parent)
    : QDialog(parent)
    , m_stack(new QStackedWidget(this))
    , m_message(new QLabel(this))
{
    setWindowTitle(i18nc("@title:window %1 a file name", "Preview of %1", name));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(900, 760);
    auto *layout = new QVBoxLayout(this);
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);
    m_stack->addWidget(m_message);
    layout->addWidget(m_stack, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton *open = buttons->addButton(i18nc("@action:button", "Open"), QDialogButtonBox::ActionRole);
    open->setIcon(QIcon::fromTheme(QStringLiteral("document-open")));
    connect(open, &QPushButton::clicked, this, [this] {
        Q_EMIT openRequested();
        close();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);

    switch (kindOf(name, data)) {
    case Kind::Pdf:
        if (!hasOkular()) {
            showMessage(i18nc("@info", "PDF files are previewed with Okular. Install Okular (the package \"okular\") to see them here, or use Open."));
        } else if (const QString file = workFile(name, data); !file.isEmpty()) {
            showPdf(file);
        }
        break;
    case Kind::Image: {
        QImage image = QImage::fromData(data);
        auto *area = new QScrollArea(this);
        auto *label = new QLabel(area);
        // Big pictures fit the window, small ones keep their size
        const QSize room(860, 680);
        if (image.width() > room.width() || image.height() > room.height()) {
            image = image.scaled(room, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        label->setPixmap(QPixmap::fromImage(image));
        label->setAlignment(Qt::AlignCenter);
        area->setWidget(label);
        area->setWidgetResizable(true);
        m_stack->addWidget(area);
        m_stack->setCurrentWidget(area);
        break;
    }
    case Kind::Text: {
        auto *text = new QPlainTextEdit(this);
        text->setReadOnly(true);
        text->setPlainText(QString::fromUtf8(data.left(textLimit)));
        m_stack->addWidget(text);
        m_stack->setCurrentWidget(text);
        break;
    }
    case Kind::Office:
        // Optional: not every office uses LibreOffice (OnlyOffice, Microsoft 365 on the web)
        if (!officePreview()) {
            showMessage(i18nc("@info", "Previews of Word, Excel and PowerPoint files are off. They need LibreOffice (the package \"libreoffice\") and Okular, "
                                       "and are turned on under Settings > Preview Office Documents with LibreOffice. Open shows the file in the "
                                       "program you use for it, such as OnlyOffice or Microsoft 365 on the web."));
        } else if (!hasLibreOffice() || !hasOkular()) {
            showMessage(i18nc("@info", "Previews of Word, Excel and PowerPoint files need LibreOffice (the package \"libreoffice\") and Okular (the package "
                                       "\"okular\"). Install what is missing, or use Open."));
        } else if (const QString file = workFile(name, data); !file.isEmpty()) {
            convertOffice(file);
        }
        break;
    case Kind::None:
        showMessage(i18nc("@info", "There is no preview for this kind of file. Open it with another program instead."));
        break;
    }
}

AttachmentPreview::~AttachmentPreview()
{
    // Closed while LibreOffice still works: its finished signal must not reach this half-destroyed
    // dialog (the child process is deleted after the members), and LibreOffice stops
    if (m_process) {
        disconnect(m_process, nullptr, this, nullptr);
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

void AttachmentPreview::showMessage(const QString &text)
{
    m_message->setText(text);
    m_stack->setCurrentWidget(m_message);
}

QString AttachmentPreview::workFile(const QString &name, const QByteArray &data)
{
    // A private copy in the app's cache, gone when the dialog closes
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cache);
    m_work = std::make_unique<QTemporaryDir>(cache + QStringLiteral("/preview-XXXXXX"));
    const QString suffix = QFileInfo(name).suffix().toLower();
    const QString path = m_work->filePath(QStringLiteral("document") + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix));
    QFile file(path);
    if (!m_work->isValid() || !file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
        showMessage(i18nc("@info", "The preview could not be made."));
        return {};
    }
    return path;
}

void AttachmentPreview::showPdf(const QString &file)
{
    // Okular's viewer, as KDE programs embed it
    const auto result = KParts::PartLoader::instantiatePart<KParts::ReadOnlyPart>(okularPart(), this, this);
    if (!result) {
        showMessage(i18nc("@info", "Okular could not be loaded for the preview. Use Open instead."));
        return;
    }
    m_part = result.plugin;
    m_stack->addWidget(m_part->widget());
    m_stack->setCurrentWidget(m_part->widget());
    m_part->openUrl(QUrl::fromLocalFile(file));
}

void AttachmentPreview::convertOffice(const QString &input)
{
    // A private LibreOffice profile: a LibreOffice the user has open does not take the job over
    showMessage(i18nc("@info", "Preparing the preview with LibreOffice..."));
    auto *process = new QProcess(this);
    m_process = process;
    process->setProgram(officeProgram());
    process->setArguments({QStringLiteral("-env:UserInstallation=") + QUrl::fromLocalFile(m_work->filePath(QStringLiteral("profile"))).toString(),
                           QStringLiteral("--headless"),
                           QStringLiteral("--convert-to"),
                           QStringLiteral("pdf"),
                           QStringLiteral("--outdir"),
                           m_work->path(),
                           input});
    connect(process, &QProcess::finished, this, [this, process] {
        process->deleteLater();
        const QString pdf = m_work->filePath(QStringLiteral("document.pdf"));
        if (process->exitStatus() != QProcess::NormalExit || !QFileInfo::exists(pdf)) {
            showMessage(i18nc("@info", "LibreOffice could not make a preview of this document. Use Open instead."));
            return;
        }
        showPdf(pdf);
    });
    // A document that hangs LibreOffice does not keep the dialog waiting
    QTimer::singleShot(60000, process, [process] {
        process->kill();
    });
    process->start();
}

}
