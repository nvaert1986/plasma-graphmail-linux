// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pstimport.h"
#include "mimeexport.h"
#include "daemonclient.h"
#include "mimemapi.h"
#include "msg/msgfile.h"
#include "pst/mapigraph.h"
#include "pst/pstalgorithms.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QProgressDialog>
#include <QPushButton>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

namespace GraphMail
{

namespace
{
constexpr int nidRole = Qt::UserRole;
constexpr quint16 propSubject = 0x0037;
constexpr quint16 propMessageClass = 0x001A;
constexpr quint16 propDisplayName = 0x3001;
// Folders nested deeper than this are left out (a damaged file can loop)
constexpr int maxDepth = 64;

constexpr quint16 propContainerClass = 0x3613;

// The daemon's word for an item kind
QString kindName(MapiItems::Kind kind)
{
    switch (kind) {
    case MapiItems::Kind::Contact:
        return QStringLiteral("contact");
    case MapiItems::Kind::Event:
        return QStringLiteral("event");
    case MapiItems::Kind::Task:
        return QStringLiteral("task");
    default:
        return {};
    }
}

MapiItems::Item convertItem(MapiItems::Kind kind, const Pst::Message &m, const QHash<quint16, Pst::NamedProperty> &names)
{
    switch (kind) {
    case MapiItems::Kind::Contact:
        return MapiItems::contact(m, names);
    case MapiItems::Kind::Event:
        return MapiItems::event(m, names);
    default:
        return MapiItems::task(m, names);
    }
}

// Why a request failed, as the daemon told; no answer at all has its own words
QString reason(const QJsonObject &answer)
{
    const QString detail = answer.value(QLatin1String("detail")).toString();
    return detail.isEmpty() ? i18nc("@info", "Microsoft 365 or the Graph Mail service did not answer.") : detail;
}
}

bool PstImport::passwordMatches(const QString &password, quint32 storedCrc)
{
    const QList<QByteArray> encodings{QByteArray(reinterpret_cast<const char *>(password.utf16()), password.size() * 2), password.toLatin1(), password.toUtf8()};
    for (const QByteArray &bytes : encodings) {
        // The PST's own CRC (seed 0, no final inversion) and the common CRC-32
        if (Pst::crc(bytes.constData(), bytes.size()) == storedCrc || ~Pst::crc(bytes.constData(), bytes.size(), 0xFFFFFFFFu) == storedCrc) {
            return true;
        }
    }
    return false;
}

QString PstImport::writeRequest(const Pst::Message &message, const QHash<quint16, Pst::NamedProperty> &names, const QString &dir, QStringList *skipped)
{
    const MapiGraph::Import import = MapiGraph::toGraph(message, names);
    *skipped = import.skipped;
    QDir().mkpath(dir);
    QJsonArray files;
    for (int i = 0; i < import.files.size(); ++i) {
        const MapiGraph::File &f = import.files.at(i);
        QFile out(dir + QStringLiteral("/%1").arg(i));
        if (!out.open(QIODevice::WriteOnly) || out.write(f.data) != f.data.size()) {
            return {};
        }
        files.append(QJsonObject{{QStringLiteral("name"), f.name},
                                 {QStringLiteral("contentType"), f.contentType},
                                 {QStringLiteral("contentId"), f.contentId},
                                 {QStringLiteral("isInline"), f.isInline},
                                 {QStringLiteral("path"), out.fileName()}});
    }
    QJsonArray items;
    for (const QJsonObject &item : import.items) {
        items.append(item);
    }
    QFile request(dir + QStringLiteral("/request.json"));
    if (!request.open(QIODevice::WriteOnly)) {
        return {};
    }
    const QByteArray json =
        QJsonDocument(QJsonObject{{QStringLiteral("message"), import.message}, {QStringLiteral("files"), files}, {QStringLiteral("items"), items}}).toJson(QJsonDocument::Compact);
    if (request.write(json) != json.size()) {
        return {};
    }
    return request.fileName();
}

PstImportDialog::PstImportDialog(Pst::PstFile *pst, const QString &fileName, const QString &mailboxLabel, QWidget *parent)
    : QDialog(parent)
    , m_pst(pst)
    , m_fileName(fileName)
{
    setWindowTitle(i18nc("@title:window", "Import Outlook Data File"));
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->addRow(i18nc("@label", "File:"), new QLabel(fileName, this));
    form->addRow(i18nc("@label", "Into mailbox:"), new QLabel(mailboxLabel, this));
    m_target = new QComboBox(this);
    m_target->addItem(i18nc("@item:inlistbox %1 a file name", "A new folder \"%1\"", QFileInfo(fileName).completeBaseName()));
    m_target->addItem(i18nc("@item:inlistbox", "The mailbox's own folders (same names are used; contacts, calendar items and tasks go to the defaults)"));
    form->addRow(i18nc("@label", "Import into:"), m_target);
    layout->addLayout(form);

    layout->addWidget(new QLabel(i18nc("@label", "Folders to import (mail, contacts, calendar items and tasks; notes and journal entries are left out):"), this));
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabels({i18nc("@title:column", "Folder"), i18nc("@title:column", "Items")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(m_tree);
    addChildren(m_tree->invisibleRootItem(), m_pst->ipmSubtree(), 0);
    m_tree->expandAll();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Import"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    resize(520, 480);
}

void PstImportDialog::addChildren(QTreeWidgetItem *item, quint32 nid, int depth)
{
    QString error;
    const std::optional<QList<Pst::FolderInfo>> children = depth < maxDepth ? m_pst->subfolders(nid, &error) : std::nullopt;
    if (!children) {
        return;
    }
    for (const Pst::FolderInfo &f : *children) {
        // Outlook's hidden folders (Recipient Cache, Yammer Root, Quick Step Settings) stay out,
        // as in Outlook's own import
        if (f.hidden) {
            continue;
        }
        auto *child = new QTreeWidgetItem(item, {f.name, QString::number(f.count)});
        child->setData(0, nidRole, f.nid);
        child->setCheckState(0, Qt::Checked);
        child->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        addChildren(child, f.nid, depth + 1);
    }
}

QList<quint32> PstImportDialog::folders() const
{
    QList<quint32> out;
    std::function<void(QTreeWidgetItem *)> walk = [&](QTreeWidgetItem *item) {
        for (int i = 0; i < item->childCount(); ++i) {
            QTreeWidgetItem *child = item->child(i);
            if (child->checkState(0) == Qt::Checked) {
                out.append(child->data(0, nidRole).toUInt());
            }
            walk(child);
        }
    };
    walk(m_tree->invisibleRootItem());
    return out;
}

QHash<quint32, quint32> PstImportDialog::parents() const
{
    // Each chosen folder under its nearest chosen ancestor; 0 when none is chosen
    QHash<quint32, quint32> out;
    std::function<void(QTreeWidgetItem *, quint32)> walk = [&](QTreeWidgetItem *item, quint32 chosenAbove) {
        for (int i = 0; i < item->childCount(); ++i) {
            QTreeWidgetItem *child = item->child(i);
            const quint32 nid = child->data(0, nidRole).toUInt();
            const bool chosen = child->checkState(0) == Qt::Checked;
            if (chosen) {
                out.insert(nid, chosenAbove);
            }
            walk(child, chosen ? nid : chosenAbove);
        }
    };
    walk(m_tree->invisibleRootItem(), 0);
    return out;
}

QString PstImportDialog::newFolderName() const
{
    return m_target->currentIndex() == 0 ? QFileInfo(m_fileName).completeBaseName() : QString();
}

PstImporter::PstImporter(DaemonClient *daemon, const QString &mailboxKey, std::shared_ptr<Pst::PstFile> pst, const QList<quint32> &folders, const QHash<quint32, quint32> &parents,
                         const QString &targetParent, const QString &newFolderName, QWidget *parent)
    : QObject(parent)
    , m_daemon(daemon)
    , m_mailboxKey(mailboxKey)
    , m_pst(std::move(pst))
    , m_folders(folders)
    , m_parents(parents)
    , m_targetParent(targetParent)
    , m_newFolderName(newFolderName)
    , m_parent(parent)
{
}

void PstImporter::query(const QString &kind, const QJsonObject &args, const std::function<void(const QJsonObject &)> &done)
{
    m_daemon->query(m_mailboxKey.section(QLatin1Char('/'), 0, 0), m_mailboxKey.section(QLatin1Char('/'), 1), kind,
                    QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this, [done](const QString &json) {
                        done(QJsonDocument::fromJson(json.toUtf8()).object());
                    });
}

void PstImporter::start()
{
    QString error;
    for (const quint32 nid : std::as_const(m_folders)) {
        if (const std::optional<QList<quint32>> ids = m_pst->messageIds(nid, false, &error)) {
            m_total += int(ids->size());
        }
    }
    m_tempDir = MimeExport::workDirectory(QStringLiteral("import"));
    if (m_parent) {
        m_progress = new QProgressDialog(i18nc("@info:progress", "Importing the Outlook data file..."), i18nc("@action:button", "Cancel"), 0, std::max(1, m_total), m_parent);
        m_progress->setWindowModality(Qt::WindowModal);
        m_progress->setMinimumDuration(0);
        connect(m_progress, &QProgressDialog::canceled, this, [this] {
            m_cancelled = true;
        });
    }
    if (m_newFolderName.isEmpty()) {
        m_top = m_targetParent;
        nextFolder();
        return;
    }
    query(QStringLiteral("folderCreate"), {{QStringLiteral("parent"), m_targetParent}, {QStringLiteral("name"), m_newFolderName}}, [this](const QJsonObject &answer) {
        if (!answer.value(QLatin1String("ok")).toBool()) {
            m_log.append(i18nc("@info %1 a folder, %2 the reason", "The folder %1 could not be made: %2", m_newFolderName, reason(answer)));
            finish(false);
            return;
        }
        m_top = answer.value(QLatin1String("id")).toString();
        nextFolder();
    });
}

void PstImporter::nextFolder()
{
    if (m_cancelled) {
        finish(false);
        return;
    }
    if (m_folderIndex >= m_folders.size()) {
        finish(true);
        return;
    }
    const quint32 nid = m_folders.at(m_folderIndex++);
    QString error;
    const std::optional<Pst::PropertyMap> props = m_pst->folder(nid, &error);
    m_currentName = props ? props->value(propDisplayName).toString() : QString();
    m_currentKind = MapiItems::folderKind(props ? props->value(propContainerClass).toString() : QString());
    if (m_currentKind == MapiItems::Kind::Other) {
        // Notes, journal and the like: Graph has no place for them
        const std::optional<QList<quint32>> ids = m_pst->messageIds(nid, false, &error);
        const int count = ids ? int(ids->size()) : 0;
        m_skipped += count;
        m_done += count;
        if (count > 0) {
            m_log.append(i18ncp("@info %2 a folder, %3 its kind", "%2: %1 item left out (%3 cannot be imported)", "%2: %1 items left out (%3 cannot be imported)", count,
                                m_currentName, props ? props->value(propContainerClass).toString() : QString()));
        }
        // Mail folders below it go to the top
        m_made.insert(nid, m_top);
        QMetaObject::invokeMethod(this, &PstImporter::nextFolder, Qt::QueuedConnection);
        return;
    }
    if (m_currentKind != MapiItems::Kind::Mail) {
        m_made.insert(nid, m_top);
        // An empty contact folder, calendar or task list (Outlook makes several) is not made
        const std::optional<QList<quint32>> ids = m_pst->messageIds(nid, false, &error);
        if (ids && ids->isEmpty()) {
            QMetaObject::invokeMethod(this, &PstImporter::nextFolder, Qt::QueuedConnection);
            return;
        }
        startItemFolder(nid, m_currentKind);
        return;
    }
    const quint32 above = m_parents.value(nid);
    const QString parent = above != 0 ? m_made.value(above) : m_top;
    if (parent.isEmpty()) {
        // Its parent folder could not be made
        m_log.append(i18nc("@info %1 a folder", "The folder %1 was left out: its parent could not be made.", m_currentName));
        nextFolder();
        return;
    }
    query(QStringLiteral("folderCreate"), {{QStringLiteral("parent"), parent}, {QStringLiteral("name"), m_currentName}}, [this, nid](const QJsonObject &answer) {
        QString error;
        const std::optional<QList<quint32>> ids = m_pst->messageIds(nid, false, &error);
        if (!answer.value(QLatin1String("ok")).toBool()) {
            m_log.append(i18nc("@info %1 a folder, %2 the reason", "The folder %1 could not be made: %2", m_currentName, reason(answer)));
            m_failed += ids ? int(ids->size()) : 0;
            m_done += ids ? int(ids->size()) : 0;
            if (answer.isEmpty()) {
                // No daemon answer at all: Microsoft 365 or the daemon is away, stop here
                finish(false);
                return;
            }
            nextFolder();
            return;
        }
        m_currentFolder = answer.value(QLatin1String("id")).toString();
        m_made.insert(nid, m_currentFolder);
        if (!ids) {
            m_log.append(i18nc("@info %1 a folder, %2 the reason", "The messages of %1 could not be read: %2", m_currentName, error));
        }
        m_messages = ids ? *ids : QList<quint32>();
        m_messageIndex = 0;
        nextMessage();
    });
}

void PstImporter::startItemFolder(quint32 nid, MapiItems::Kind kind)
{
    const auto begin = [this, nid](const QString &container) {
        QString error;
        const std::optional<QList<quint32>> ids = m_pst->messageIds(nid, false, &error);
        if (!ids) {
            m_log.append(i18nc("@info %1 a folder, %2 the reason", "The messages of %1 could not be read: %2", m_currentName, error));
        }
        m_currentFolder = container;
        m_messages = ids ? *ids : QList<quint32>();
        m_messageIndex = 0;
        nextMessage();
    };
    const auto failed = [this, nid](const QJsonObject &answer) {
        QString error;
        const std::optional<QList<quint32>> ids = m_pst->messageIds(nid, false, &error);
        m_log.append(i18nc("@info %1 a folder, %2 the reason", "The folder %1 could not be made: %2", m_currentName, reason(answer)));
        m_failed += ids ? int(ids->size()) : 0;
        m_done += ids ? int(ids->size()) : 0;
        if (answer.isEmpty()) {
            finish(false);
            return;
        }
        nextFolder();
    };
    if (m_newFolderName.isEmpty()) {
        // Into the mailbox's own: the default contacts and calendar, the default task list
        if (kind != MapiItems::Kind::Task) {
            begin(QString());
            return;
        }
        query(QStringLiteral("defaultTaskList"), {}, [begin, failed](const QJsonObject &answer) {
            if (answer.value(QLatin1String("ok")).toBool()) {
                begin(answer.value(QLatin1String("id")).toString());
            } else {
                failed(answer);
            }
        });
        return;
    }
    // Contact folders, calendars and task lists are flat: named after the file and the folder
    const QString name = m_newFolderName + QStringLiteral(" - ") + m_currentName;
    query(QStringLiteral("containerCreate"), {{QStringLiteral("kind"), kindName(kind)}, {QStringLiteral("name"), name}}, [begin, failed](const QJsonObject &answer) {
        if (answer.value(QLatin1String("ok")).toBool()) {
            begin(answer.value(QLatin1String("id")).toString());
        } else {
            failed(answer);
        }
    });
}

void PstImporter::importItem(const Pst::Message &m)
{
    const MapiItems::Item item = convertItem(m_currentKind, m, m_pst->namedProperties());
    const QString subject = m.string(0x0037).isEmpty() ? m.string(propDisplayName) : m.string(0x0037);
    const QStringList skipped = item.skipped;
    query(QStringLiteral("importItem"),
          {{QStringLiteral("kind"), kindName(m_currentKind)}, {QStringLiteral("container"), m_currentFolder}, {QStringLiteral("item"), item.json}},
          [this, subject, skipped](const QJsonObject &answer) {
              if (answer.value(QLatin1String("ok")).toBool()) {
                  ++m_imported;
                  for (const QString &part : skipped) {
                      m_log.append(i18nc("@info %1 a folder, %2 a subject, %3 what was left out", "%1: \"%2\" imported without %3", m_currentName, subject, part));
                  }
              } else {
                  ++m_failed;
                  m_log.append(i18nc("@info %1 a folder, %2 a subject, %3 the reason", "%1: \"%2\" not imported: %3", m_currentName, subject, reason(answer)));
                  if (answer.isEmpty()) {
                      finish(false);
                      return;
                  }
              }
              nextMessage();
          });
}

void PstImporter::nextMessage()
{
    if (m_progress) {
        m_progress->setValue(m_done);
    }
    if (m_cancelled) {
        finish(false);
        return;
    }
    if (m_messageIndex >= m_messages.size()) {
        nextFolder();
        return;
    }
    const quint32 nid = m_messages.at(m_messageIndex++);
    ++m_done;
    QString error;
    const std::optional<Pst::Message> m = m_pst->message(nid, &error);
    if (!m) {
        ++m_failed;
        m_log.append(i18nc("@info %1 a folder, %2 the reason", "A message in %1 could not be read: %2", m_currentName, error));
        QMetaObject::invokeMethod(this, &PstImporter::nextMessage, Qt::QueuedConnection);
        return;
    }
    const QString subject = m->string(propSubject);
    const QString messageClass = m->string(propMessageClass);
    // Items go where their folder's kind goes: a contact in a mail folder is left out
    if (MapiItems::kindOf(messageClass) != m_currentKind) {
        ++m_skipped;
        m_log.append(i18nc("@info %1 a folder, %2 a subject, %3 an item type", "%1: \"%2\" left out (%3 in this folder)", m_currentName, subject, messageClass));
        QMetaObject::invokeMethod(this, &PstImporter::nextMessage, Qt::QueuedConnection);
        return;
    }
    if (m_currentKind != MapiItems::Kind::Mail) {
        importItem(*m);
        return;
    }

    // The request for the daemon: the message, its attachments as files beside it
    const QString dir = m_tempDir + QStringLiteral("/%1").arg(nid);
    QStringList skipped;
    const QString requestFile = PstImport::writeRequest(*m, m_pst->namedProperties(), dir, &skipped);
    if (requestFile.isEmpty()) {
        ++m_failed;
        m_log.append(i18nc("@info %1 a folder", "%1: a message could not be prepared (disk full?).", m_currentName));
        QDir(dir).removeRecursively();
        QMetaObject::invokeMethod(this, &PstImporter::nextMessage, Qt::QueuedConnection);
        return;
    }
    // A message already in the folder (the same Message-ID) is left out, as Outlook's "Do not
    // import duplicates": importing the same file twice, or into the mailbox's own folders
    query(QStringLiteral("importMessage"), {{QStringLiteral("folder"), m_currentFolder}, {QStringLiteral("file"), requestFile}, {QStringLiteral("skipDuplicates"), true}},
          [this, dir, subject, skipped](const QJsonObject &answer) {
              QDir(dir).removeRecursively();
              if (answer.value(QLatin1String("duplicate")).toBool()) {
                  ++m_skipped;
                  m_log.append(i18nc("@info %1 a folder, %2 a subject", "%1: \"%2\" left out: it is in the folder already", m_currentName, subject));
              } else if (answer.value(QLatin1String("ok")).toBool()) {
                  ++m_imported;
                  for (const QString &part : skipped) {
                      m_log.append(i18nc("@info %1 a folder, %2 a subject, %3 what was left out", "%1: \"%2\" imported without %3", m_currentName, subject, part));
                  }
              } else {
                  ++m_failed;
                  m_log.append(i18nc("@info %1 a folder, %2 a subject, %3 the reason", "%1: \"%2\" not imported: %3", m_currentName, subject, reason(answer)));
                  if (answer.isEmpty()) {
                      finish(false);
                      return;
                  }
              }
              nextMessage();
          });
}

void PstImporter::finish(bool ok)
{
    if (m_progress) {
        m_progress->deleteLater();
        m_progress = nullptr;
    }
    QDir(m_tempDir).removeRecursively();
    qWarning() << "pst import test:" << ok << m_imported << "imported," << m_skipped << "skipped," << m_failed << "failed";
    Q_EMIT finished(ok && !m_cancelled, m_imported, m_skipped, m_failed, m_log);
    deleteLater();
}

MessageFileImporter::MessageFileImporter(DaemonClient *daemon, const QString &mailboxKey, const QStringList &files, const QString &folderGraphId, QWidget *parent)
    : QObject(parent)
    , m_daemon(daemon)
    , m_mailboxKey(mailboxKey)
    , m_files(files)
    , m_folder(folderGraphId)
    , m_parent(parent)
{
}

void MessageFileImporter::start()
{
    m_tempDir = MimeExport::workDirectory(QStringLiteral("import"));
    if (m_parent && m_files.size() > 1) {
        m_progress = new QProgressDialog(i18nc("@info:progress", "Importing messages..."), i18nc("@action:button", "Cancel"), 0, int(m_files.size()), m_parent);
        m_progress->setWindowModality(Qt::WindowModal);
        m_progress->setMinimumDuration(500);
        connect(m_progress, &QProgressDialog::canceled, this, [this] {
            m_cancelled = true;
        });
    }
    next();
}

void MessageFileImporter::next()
{
    if (m_progress) {
        m_progress->setValue(m_index);
    }
    if (m_cancelled) {
        finish(false);
        return;
    }
    if (m_index >= m_files.size()) {
        finish(true);
        return;
    }
    const QString path = m_files.at(m_index++);
    const QString shown = QFileInfo(path).fileName();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        ++m_failed;
        m_log.append(i18nc("@info %1 a file, %2 the reason", "%1 could not be read: %2", shown, file.errorString()));
        QMetaObject::invokeMethod(this, &MessageFileImporter::next, Qt::QueuedConnection);
        return;
    }
    const QByteArray bytes = file.readAll();
    // A .msg is a compound file; anything else is taken as Internet mail (.eml)
    QString error;
    std::optional<Msg::File> msg = bytes.startsWith(QByteArray::fromHex("d0cf11e0a1b11ae1")) ? Msg::read(bytes, &error) : std::nullopt;
    if (!msg && error.isEmpty()) {
        msg = Msg::File{MimeMapi::fromMime(bytes, {}, 0), {}};
    }
    if (!msg) {
        ++m_failed;
        m_log.append(i18nc("@info %1 a file, %2 the reason", "%1 could not be read: %2", shown, error));
        QMetaObject::invokeMethod(this, &MessageFileImporter::next, Qt::QueuedConnection);
        return;
    }
    const MapiItems::Kind kind = MapiItems::kindOf(msg->message.string(propMessageClass));
    if (kind == MapiItems::Kind::Other) {
        ++m_skipped;
        m_log.append(i18nc("@info %1 a file, %2 an item type", "%1 left out (not mail: %2)", shown, msg->message.string(propMessageClass)));
        QMetaObject::invokeMethod(this, &MessageFileImporter::next, Qt::QueuedConnection);
        return;
    }
    if (kind != MapiItems::Kind::Mail) {
        // A contact, appointment or task: to the mailbox's default contacts, calendar or list
        const MapiItems::Item item = convertItem(kind, msg->message, msg->names);
        const QJsonObject args{{QStringLiteral("kind"), kindName(kind)}, {QStringLiteral("item"), item.json}};
        if (kind == MapiItems::Kind::Task && m_taskList.isEmpty()) {
            m_daemon->query(m_mailboxKey.section(QLatin1Char('/'), 0, 0), m_mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("defaultTaskList"), QStringLiteral("{}"), this,
                            [this, args, shown, item](const QString &json) {
                                const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                                m_taskList = answer.value(QLatin1String("id")).toString();
                                if (m_taskList.isEmpty()) {
                                    ++m_failed;
                                    m_log.append(i18nc("@info %1 a file, %2 the reason", "%1 not imported: %2", shown, reason(answer)));
                                    next();
                                    return;
                                }
                                QJsonObject withList = args;
                                withList.insert(QStringLiteral("container"), m_taskList);
                                send(QStringLiteral("importItem"), withList, QString(), shown, item.skipped);
                            });
            return;
        }
        QJsonObject withContainer = args;
        withContainer.insert(QStringLiteral("container"), kind == MapiItems::Kind::Task ? m_taskList : QString());
        send(QStringLiteral("importItem"), withContainer, QString(), shown, item.skipped);
        return;
    }
    const QString dir = m_tempDir + QStringLiteral("/%1").arg(m_index);
    QStringList skipped;
    const QString request = PstImport::writeRequest(msg->message, msg->names, dir, &skipped);
    if (request.isEmpty()) {
        ++m_failed;
        m_log.append(i18nc("@info %1 a file", "%1 could not be prepared (disk full?).", shown));
        QDir(dir).removeRecursively();
        QMetaObject::invokeMethod(this, &MessageFileImporter::next, Qt::QueuedConnection);
        return;
    }
    send(QStringLiteral("importMessage"), {{QStringLiteral("folder"), m_folder}, {QStringLiteral("file"), request}}, dir, shown, skipped);
}

void MessageFileImporter::send(const QString &kind, const QJsonObject &args, const QString &dir, const QString &shown, const QStringList &skipped)
{
    m_daemon->query(m_mailboxKey.section(QLatin1Char('/'), 0, 0), m_mailboxKey.section(QLatin1Char('/'), 1), kind,
                    QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this, [this, dir, shown, skipped](const QString &json) {
                        if (!dir.isEmpty()) {
                            QDir(dir).removeRecursively();
                        }
                        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                        if (answer.value(QLatin1String("ok")).toBool()) {
                            ++m_imported;
                            for (const QString &part : skipped) {
                                m_log.append(i18nc("@info %1 a file, %2 what was left out", "%1 imported without %2", shown, part));
                            }
                        } else {
                            ++m_failed;
                            m_log.append(i18nc("@info %1 a file, %2 the reason", "%1 not imported: %2", shown, reason(answer)));
                            if (answer.isEmpty()) {
                                finish(false);
                                return;
                            }
                        }
                        next();
                    });
}

void MessageFileImporter::finish(bool ok)
{
    if (m_progress) {
        m_progress->deleteLater();
        m_progress = nullptr;
    }
    QDir(m_tempDir).removeRecursively();
    qWarning() << "message file import test:" << ok << m_imported << "imported," << m_skipped << "skipped," << m_failed << "failed";
    Q_EMIT finished(ok && !m_cancelled, m_imported, m_skipped, m_failed, m_log);
    deleteLater();
}

}
