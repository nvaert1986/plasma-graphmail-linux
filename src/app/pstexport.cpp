// SPDX-FileCopyrightText: 2026 plasma-graphmail-linux contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pstexport.h"
#include "daemonclient.h"
#include "mailboxset.h"
#include "mimeexport.h"
#include "mimemapi.h"
#include "pst/mapiitems.h"
#include "store.h"

#include <KLocalizedString>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QUuid>

namespace GraphMail
{

PstExporter::PstExporter(MailboxSet *mailboxes, DaemonClient *daemon, const QString &mailboxKey, const QList<qint64> &folders, const QString &file, QWidget *parent)
    : QObject(parent)
    , m_mailboxes(mailboxes)
    , m_daemon(daemon)
    , m_mailboxKey(mailboxKey)
    , m_folders(folders)
    , m_file(file)
    , m_part(file + QStringLiteral(".part"))
    , m_parent(parent)
{
}

void PstExporter::plan(qint64 folder, quint32 parent)
{
    const Store *store = m_mailboxes->store(m_mailboxKey);
    const std::optional<FolderRow> row = store->folder(folder);
    if (!row) {
        return;
    }
    // Deleted Items exists in every PST: the mailbox's goes there instead of beside it
    const quint32 nid = parent == Pst::PstWriter::nidIpmSubtree && row->wellKnown == QLatin1String("deleteditems")
        ? Pst::PstWriter::nidDeletedItems
        : m_writer.addFolder(parent, row->name);
    QList<qint64> ids;
    std::optional<PageCursor> cursor;
    for (;;) {
        const QList<ListRow> page = store->listPage(folder, cursor, 1000);
        for (const ListRow &r : page) {
            ids.append(r.id);
        }
        if (page.size() < 1000) {
            break;
        }
        cursor = PageCursor{page.constLast().received, page.constLast().id};
    }
    // Oldest first, as Outlook adds them
    for (auto it = ids.crbegin(); it != ids.crend(); ++it) {
        m_jobs.append({*it, nid});
    }
    const QList<FolderRow> all = store->folders();
    for (const FolderRow &child : all) {
        if (child.parentGraphId == row->graphId) {
            plan(child.id, nid);
        }
    }
}

void PstExporter::addItems()
{
    const Store *store = m_mailboxes->store(m_mailboxKey);
    const std::optional<MailboxInfo> box = m_mailboxes->mailbox(m_mailboxKey);
    const MapiItems::NameId name = [this](const QUuid &set, quint32 lid) {
        return m_writer.namedProperty(set, lid);
    };
    const auto parse = [](const QString &json) {
        return QJsonDocument::fromJson(json.toUtf8()).object();
    };
    const QList<ParentRow> contactFolders = store->parents(ItemKind::Contact);
    for (const ParentRow &folder : contactFolders) {
        const quint32 nid = m_writer.addFolder(Pst::PstWriter::nidIpmSubtree, folder.name, QStringLiteral("IPF.Contact"));
        const QList<StoredItem> items = store->items(ItemKind::Contact, folder.id);
        for (const StoredItem &item : items) {
            m_items += m_writer.addMessage(nid, MapiItems::contactToMapi(parse(item.json), name, m_keywords)) != 0 ? 1 : 0;
        }
    }
    // The mailbox's own calendars; colleagues' calendars shown here belong to their mailboxes
    const QList<CalendarRow> calendars = store->calendars();
    for (const CalendarRow &calendar : calendars) {
        if (!calendar.owner.isEmpty() && box && calendar.owner.compare(box->address, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const quint32 nid = m_writer.addFolder(Pst::PstWriter::nidIpmSubtree, calendar.name, QStringLiteral("IPF.Appointment"));
        // Series with their changed occurrences apart; plain occurrences come from their series
        QHash<QString, QList<QDate>> changed;
        QList<QJsonObject> events;
        const QList<StoredItem> items = store->items(ItemKind::Event, calendar.id);
        for (const StoredItem &item : items) {
            QJsonObject e = parse(item.json);
            const QString type = e.value(QLatin1String("type")).toString();
            if (type == QLatin1String("occurrence")) {
                continue;
            }
            if (type == QLatin1String("exception")) {
                // occurrenceId ends with the original date of the occurrence
                const QDate original = QDate::fromString(e.value(QLatin1String("occurrenceId")).toString().section(QLatin1Char('.'), -1), Qt::ISODate);
                if (original.isValid()) {
                    changed[e.value(QLatin1String("seriesMasterId")).toString()].append(original);
                }
                e.remove(QStringLiteral("recurrence"));
            }
            events.append(e);
        }
        for (const QJsonObject &e : std::as_const(events)) {
            const QList<QDate> deleted = e.value(QLatin1String("type")).toString() == QLatin1String("seriesMaster") ? changed.value(e.value(QLatin1String("id")).toString()) : QList<QDate>();
            m_items += m_writer.addMessage(nid, MapiItems::eventToMapi(e, name, m_keywords, deleted)) != 0 ? 1 : 0;
        }
    }
    const QList<ParentRow> taskLists = store->parents(ItemKind::Task);
    for (const ParentRow &list : taskLists) {
        const quint32 nid = m_writer.addFolder(Pst::PstWriter::nidIpmSubtree, list.name, QStringLiteral("IPF.Task"));
        const QList<StoredItem> items = store->items(ItemKind::Task, list.id);
        for (const StoredItem &item : items) {
            m_items += m_writer.addMessage(nid, MapiItems::taskToMapi(parse(item.json), name, m_keywords)) != 0 ? 1 : 0;
        }
    }
}

void PstExporter::start()
{
    const Store *store = m_mailboxes->store(m_mailboxKey);
    const std::optional<MailboxInfo> box = m_mailboxes->mailbox(m_mailboxKey);
    QFile::remove(m_part);
    if (!store || !m_writer.create(m_part, box ? box->label() : m_mailboxKey, &m_error)) {
        finish();
        return;
    }
    m_keywords = m_writer.namedProperty(Pst::psPublicStrings, QStringLiteral("Keywords"));
    QList<qint64> roots = m_folders;
    if (roots.isEmpty()) {
        // The whole mailbox: the folders whose parent is not a folder here (the top of the tree)
        const QList<FolderRow> all = store->folders();
        QSet<QString> known;
        for (const FolderRow &f : all) {
            known.insert(f.graphId);
        }
        for (const FolderRow &f : all) {
            if (!known.contains(f.parentGraphId)) {
                roots.append(f.id);
            }
        }
    }
    if (m_itemsOnly) {
        roots.clear();
    }
    for (const qint64 folder : std::as_const(roots)) {
        plan(folder, Pst::PstWriter::nidIpmSubtree);
    }
    if (m_folders.isEmpty()) {
        addItems();
    }
    m_tempDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/export-") + QUuid::createUuid().toString(QUuid::Id128);
    QDir().mkpath(m_tempDir);
    if (m_parent) {
        m_progress = new QProgressDialog(i18nc("@info:progress", "Exporting to an Outlook data file..."), i18nc("@action:button", "Cancel"), 0, int(m_jobs.size()), m_parent);
        m_progress->setWindowModality(Qt::WindowModal);
        m_progress->setMinimumDuration(500);
        connect(m_progress, &QProgressDialog::canceled, this, [this] {
            m_cancelled = true;
        });
    }
    next();
}

void PstExporter::next()
{
    if (m_progress) {
        m_progress->setValue(m_index);
    }
    if (m_cancelled || m_index >= m_jobs.size() || !m_writer.error().isEmpty()) {
        finish();
        return;
    }
    const Job job = m_jobs.at(m_index++);
    if (m_offline || !m_daemon->isRunning()) {
        add(MimeExport::rebuild(m_mailboxes->store(m_mailboxKey), m_mailboxes->blobs(m_mailboxKey), job.message), job, false);
        QMetaObject::invokeMethod(this, &PstExporter::next, Qt::QueuedConnection);
        return;
    }
    const QString temp = m_tempDir + QStringLiteral("/%1.eml").arg(job.message);
    const QJsonObject args{{QStringLiteral("message"), job.message}, {QStringLiteral("file"), temp}};
    m_daemon->query(m_mailboxKey.section(QLatin1Char('/'), 0, 0), m_mailboxKey.section(QLatin1Char('/'), 1), QStringLiteral("messageMime"),
                    QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), this, [this, job, temp](const QString &json) {
                        const QJsonObject answer = QJsonDocument::fromJson(json.toUtf8()).object();
                        QFile file(temp);
                        if (answer.value(QLatin1String("ok")).toBool() && file.open(QIODevice::ReadOnly)) {
                            add(file.readAll(), job, true);
                            file.close();
                            file.remove();
                        } else {
                            // No answer (offline, an outage): the rest from the cache at once
                            m_offline = true;
                            add(MimeExport::rebuild(m_mailboxes->store(m_mailboxKey), m_mailboxes->blobs(m_mailboxKey), job.message), job, false);
                        }
                        next();
                    });
}

void PstExporter::add(const QByteArray &mime, const Job &job, bool original)
{
    if (mime.isEmpty()) {
        return;
    }
    const Store *store = m_mailboxes->store(m_mailboxKey);
    const std::optional<MessageRow> row = store ? store->message(job.message) : std::nullopt;
    MimeMapi::Extras extras;
    if (row) {
        extras.read = row->isRead;
        extras.flag = row->flag;
        extras.importance = row->importance;
        extras.categories = row->categories;
        extras.received = row->received;
    }
    if (m_writer.addMessage(job.folder, MimeMapi::fromMime(mime, extras, m_keywords)) != 0) {
        ++(original ? m_original : m_rebuilt);
    }
}

void PstExporter::finish()
{
    if (m_progress) {
        m_progress->deleteLater();
    }
    if (!m_tempDir.isEmpty()) {
        QDir(m_tempDir).removeRecursively();
    }
    if (m_error.isEmpty() && !m_writer.error().isEmpty()) {
        m_error = m_writer.error();
    }
    bool ok = !m_cancelled && m_error.isEmpty();
    if (ok) {
        ok = m_writer.finish(&m_error);
    }
    if (ok) {
        QFile::remove(m_file);
        ok = QFile::rename(m_part, m_file);
        if (!ok) {
            m_error = i18nc("@info", "The file could not be renamed to %1.", m_file);
        }
    }
    if (!ok) {
        QFile::remove(m_part);
    }
    qWarning() << "pst export test:" << ok << m_original << "original," << m_rebuilt << "rebuilt," << m_items << "items" << m_error;
    Q_EMIT finished(ok, m_original, m_rebuilt, m_items, m_file, m_error);
    deleteLater();
}

}
