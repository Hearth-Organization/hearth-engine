/*
 * Hearth Engine Asset processing. Built on O3DE (Apache-2.0 OR MIT). See HearthProcessingPane.h.
 */

#include "EditorDefs.h"

#include "HearthProcessingPane.h"
#include "QtViewPaneManager.h"

#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>
#include <AzToolsFramework/AssetDatabase/AssetDatabaseConnection.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;
    using namespace AzToolsFramework::AssetDatabase;

    QString ProcessingProject()
    {
        return QString::fromUtf8(AZ::Utils::GetProjectPath().c_str());
    }

    QString ProcessingFolder()
    {
        return QDir(ProcessingProject()).filePath(QStringLiteral("user/AssetProcessor"));
    }

    QString ProcessingKind(const QString& path)
    {
        const QString s = QFileInfo(path).suffix().toLower();
        static const QStringList textures = { "png", "jpg", "jpeg", "tga", "tif", "tiff", "bmp", "gif", "exr", "dds" };
        if (textures.contains(s)) return QStringLiteral("TEXTURE");
        if (s == QLatin1String("fbx") || s == QLatin1String("gltf") || s == QLatin1String("glb") || s == QLatin1String("obj")) return QStringLiteral("MODEL");
        if (s == QLatin1String("wav") || s == QLatin1String("ogg") || s == QLatin1String("mp3") || s == QLatin1String("flac")) return QStringLiteral("SOUND");
        if (s == QLatin1String("material") || s == QLatin1String("materialtype")) return QStringLiteral("MATERIAL");
        if (s == QLatin1String("prefab")) return QStringLiteral("PREFAB");
        if (s == QLatin1String("shader") || s == QLatin1String("azsl") || s == QLatin1String("azsli") || s == QLatin1String("shadervariantlist")) return QStringLiteral("SHADER");
        if (s == QLatin1String("lua") || s == QLatin1String("scriptcanvas") || s == QLatin1String("py")) return QStringLiteral("SCRIPT");
        if (s == QLatin1String("pass") || s == QLatin1String("attimage") || s == QLatin1String("azasset")) return QStringLiteral("RENDER");
        return s.isEmpty() ? QStringLiteral("FILE") : s.toUpper();
    }

    QString ProcessingBuilder(const QString& kind)
    {
        if (kind == QLatin1String("TEXTURE")) return QObject::tr("image builder");
        if (kind == QLatin1String("MODEL")) return QObject::tr("scene builder");
        if (kind == QLatin1String("MATERIAL")) return QObject::tr("material builder");
        if (kind == QLatin1String("SHADER")) return QObject::tr("shader builder");
        if (kind == QLatin1String("PREFAB")) return QObject::tr("prefab builder");
        if (kind == QLatin1String("SOUND")) return QObject::tr("audio builder");
        return QObject::tr("builder");
    }

    QString ProcessingThing(const QString& kind)
    {
        if (kind == QLatin1String("TEXTURE")) return QObject::tr("texture");
        if (kind == QLatin1String("MODEL")) return QObject::tr("model");
        if (kind == QLatin1String("MATERIAL")) return QObject::tr("material");
        if (kind == QLatin1String("SHADER")) return QObject::tr("shader");
        if (kind == QLatin1String("PREFAB")) return QObject::tr("prefab");
        if (kind == QLatin1String("SOUND")) return QObject::tr("sound");
        return QObject::tr("file");
    }

    //! Memory used by the Asset Processor and its builders, in MB.
    double ProcessingMemoryMb()
    {
        double total = 0.0;
        const QStringList pids = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& pid : pids)
        {
            bool number = false;
            pid.toInt(&number);
            if (!number)
            {
                continue;
            }
            QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
            if (!comm.open(QIODevice::ReadOnly))
            {
                continue;
            }
            const QString name = QString::fromUtf8(comm.readAll()).trimmed();
            if (!name.startsWith(QLatin1String("AssetProcesso")) && !name.startsWith(QLatin1String("AssetBuilder")))
            {
                continue;
            }
            QFile status(QStringLiteral("/proc/%1/status").arg(pid));
            if (status.open(QIODevice::ReadOnly))
            {
                for (const QString& line : QString::fromUtf8(status.readAll()).split(QLatin1Char('\n')))
                {
                    if (line.startsWith(QLatin1String("VmRSS:")))
                    {
                        total += line.section(QLatin1Char(':'), 1).simplified().section(QLatin1Char(' '), 0, 0).toDouble() / 1024.0;
                    }
                }
            }
        }
        return total;
    }

    double ProcessingTotalMemoryMb()
    {
        QFile meminfo(QStringLiteral("/proc/meminfo"));
        if (meminfo.open(QIODevice::ReadOnly))
        {
            return QString::fromUtf8(meminfo.readLine()).simplified().section(QLatin1Char(' '), 1, 1).toDouble() / 1024.0;
        }
        return 8000.0;
    }

    QString ProcessingSize(double mb)
    {
        return mb >= 1000.0 ? QObject::tr("%1 GB").arg(mb / 1024.0, 0, 'f', 1) : QObject::tr("%1 MB").arg(qRound(mb));
    }

    QFrame* ProcessingStat(QWidget* parent, const QString& title, QLabel** value, QLabel** hint)
    {
        QVBoxLayout* layout = nullptr;
        QFrame* card = Hearth::MakeCard(parent, &layout);
        layout->setSpacing(2);
        layout->addWidget(Hearth::MakeLabel(title, QStringLiteral("small"), card));
        *value = Hearth::MakeLabel(QStringLiteral("…"), QStringLiteral("big"), card);
        layout->addWidget(*value);
        QLabel* below = Hearth::MakeLabel(QString(), QStringLiteral("small"), card);
        layout->addWidget(below);
        if (hint)
        {
            *hint = below;
        }
        return card;
    }

    void ProcessingTone(QLabel* label, const QColor& colour)
    {
        label->setStyleSheet(QStringLiteral("background: transparent; color: %1; font-size: 24px; font-weight: 600;").arg(colour.name()));
    }
} // namespace

void HearthProcessingPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(120, 80, 1320, 820);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthProcessingPane>("Asset processing", "Tools", options);
}

HearthProcessingPane::HearthProcessingPane(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthProcessing"));
    setStyleSheet(QStringLiteral("QWidget#HearthProcessing { background-color: #0D0C0B; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Asset processing"), tr("Turns your files into game-ready assets in the background"));
    header->addStretch(1);
    m_state = Hearth::MakeChip(tr("Looking…"), QStringLiteral("plain"), this);
    header->addWidget(m_state);
    m_pause = Hearth::MakeButton(tr("Pause"), false, this);
    connect(m_pause, &QPushButton::clicked, this, [this]()
    {
        if (!m_running)
        {
            // Not running: start it (it lives next to the editor).
            QProcess::startDetached(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("AssetProcessor")),
                                    { QStringLiteral("--project-path=%1").arg(ProcessingProject()) });
            return;
        }
        Send(QJsonObject{ { QStringLiteral("pause"), !m_status.value(QStringLiteral("paused")).toBool() } });
    });
    header->addWidget(m_pause);
    QPushButton* now = Hearth::MakeButton(tr("Process changed files now"), true, this);
    connect(now, &QPushButton::clicked, this, [this]() { Send(QJsonObject{ { QStringLiteral("processChanges"), true } }); });
    header->addWidget(now);
    root->addLayout(header);

    auto* stats = new QHBoxLayout();
    stats->setSpacing(10);
    stats->addWidget(ProcessingStat(this, tr("Done"), &m_doneValue, nullptr));
    stats->addWidget(ProcessingStat(this, tr("Left to do"), &m_leftValue, &m_leftHint));
    stats->addWidget(ProcessingStat(this, tr("Failed"), &m_failedValue, nullptr));
    stats->addWidget(ProcessingStat(this, tr("Memory used"), &m_memoryValue, &m_memoryHint));
    root->addLayout(stats);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);
    QVBoxLayout* queueLayout = nullptr;
    QFrame* queueCard = Hearth::MakeCard(this, &queueLayout);
    auto* queueTitle = new QHBoxLayout();
    queueTitle->addWidget(Hearth::MakeLabel(tr("Queue"), QStringLiteral("title"), queueCard));
    queueTitle->addStretch(1);
    const QString chipStyle = QStringLiteral("QPushButton { background-color: rgba(255, 255, 255, 10); border: 1px solid rgba(255, 255, 255, 15); border-radius: 13px; padding: 4px 12px; color: #ECE6DE; }"
                                             "QPushButton:checked { background-color: #ECE6DE; color: #1A0F08; border-color: #ECE6DE; }");
    const QVector<QPair<QString, QString>> chips = { { tr("All"), QString() }, { tr("Working"), QStringLiteral("working") }, { tr("Waiting"), QStringLiteral("waiting") } };
    for (const auto& chip : chips)
    {
        auto* button = new QPushButton(chip.first, queueCard);
        button->setCheckable(true);
        button->setChecked(chip.second.isEmpty());
        button->setStyleSheet(chipStyle);
        const QString filter = chip.second;
        connect(button, &QPushButton::clicked, this, [this, button, filter]()
        {
            for (QPushButton* other : m_queueChips)
            {
                other->setChecked(other == button);
            }
            m_queueFilter = filter;
            FillQueue();
        });
        m_queueChips << button;
        queueTitle->addWidget(button);
    }
    queueLayout->addLayout(queueTitle);
    m_queue = new QListWidget(queueCard);
    m_queue->setFrameShape(QFrame::NoFrame);
    m_queue->setSelectionMode(QAbstractItemView::NoSelection);
    m_queue->setStyleSheet(QStringLiteral("QListWidget { background: transparent; outline: none; } QListWidget::item { border: none; }"));
    queueLayout->addWidget(m_queue, 1);
    body->addWidget(queueCard, 1);

    m_failedCard = Hearth::MakeCard(this, &m_failedLayout);
    m_failedCard->setFixedWidth(400);
    body->addWidget(m_failedCard);
    root->addLayout(body, 1);

    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &HearthProcessingPane::Refresh);
}

HearthProcessingPane::~HearthProcessingPane()
{
    if (m_database)
    {
        m_database->CloseDatabase();
    }
}

void HearthProcessingPane::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    m_ticks = 0;
    Refresh();
    m_timer->start();
}

void HearthProcessingPane::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_timer->stop();
}

void HearthProcessingPane::Send(const QJsonObject& request)
{
    QDir().mkpath(ProcessingFolder());
    // Merge with a request the Asset Processor hasn't read yet.
    QJsonObject merged;
    QFile pending(QDir(ProcessingFolder()).filePath(QStringLiteral("hearth-control.json")));
    if (pending.open(QIODevice::ReadOnly))
    {
        merged = QJsonDocument::fromJson(pending.readAll()).object();
        pending.close();
    }
    for (auto it = request.begin(); it != request.end(); ++it)
    {
        if (it.key() == QLatin1String("reprocess"))
        {
            QJsonArray list = merged.value(QStringLiteral("reprocess")).toArray();
            for (const QJsonValue value : it.value().toArray())
            {
                list.append(value);
            }
            merged.insert(it.key(), list);
        }
        else
        {
            merged.insert(it.key(), it.value());
        }
    }
    QSaveFile out(pending.fileName());
    if (out.open(QIODevice::WriteOnly))
    {
        out.write(QJsonDocument(merged).toJson(QJsonDocument::Compact));
        out.commit();
    }
    QTimer::singleShot(1200, this, &HearthProcessingPane::Refresh);
}

void HearthProcessingPane::Refresh()
{
    // The Asset Processor's own status file.
    QFile file(QDir(ProcessingFolder()).filePath(QStringLiteral("hearth-status.json")));
    const QDateTime written = QFileInfo(file).lastModified();
    if (file.open(QIODevice::ReadOnly))
    {
        m_status = QJsonDocument::fromJson(file.readAll()).object();
    }
    const qint64 pid = m_status.value(QStringLiteral("pid")).toVariant().toLongLong();
    m_running = written.isValid() && written.secsTo(QDateTime::currentDateTime()) < 20 && pid > 0 && QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid));

    const bool paused = m_status.value(QStringLiteral("paused")).toBool();
    const int working = m_status.value(QStringLiteral("working")).toInt();
    const int waiting = m_status.value(QStringLiteral("waiting")).toInt();
    const int left = working + waiting;
    if (!m_running)
    {
        m_state->setText(tr("Not running"));
        m_pause->setText(tr("Start it"));
    }
    else if (paused)
    {
        m_state->setText(tr("Paused · %n waiting", nullptr, left));
        m_pause->setText(tr("Carry on"));
    }
    else
    {
        m_state->setText(working > 0 ? tr("Working · %1 at a time").arg(working) : tr("All done"));
        m_pause->setText(tr("Pause"));
    }
    m_state->setStyleSheet(!m_running || paused ? QStringLiteral("background-color: rgba(240, 209, 122, 36); color: #F0D17A; border-radius: 9px; padding: 3px 10px;")
                                                : (working > 0 ? QStringLiteral("background-color: rgba(232, 116, 59, 41); color: #FFCFB4; border-radius: 9px; padding: 3px 10px;")
                                                               : QStringLiteral("background-color: rgba(166, 221, 151, 31); color: #A6DD97; border-radius: 9px; padding: 3px 10px;")));

    // Left to do, and about how long that takes at the speed of the last few minutes.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_leftHistory.append({ now, left });
    while (m_leftHistory.size() > 2 && now - m_leftHistory.first().first > 180000)
    {
        m_leftHistory.removeFirst();
    }
    m_leftValue->setText(m_running ? QLocale().toString(left) : QStringLiteral("–"));
    QString estimate;
    if (left == 0)
    {
        estimate = m_running ? tr("nothing waiting") : QString();
    }
    else if (m_leftHistory.size() >= 5)
    {
        const double minutes = (now - m_leftHistory.first().first) / 60000.0;
        const int finished = m_leftHistory.first().second - left;
        if (finished > 0 && minutes > 0.05)
        {
            const double perMinute = finished / minutes;
            const int eta = qMax(1, qRound(left / perMinute));
            estimate = eta >= 90 ? tr("about %n hour(s)", nullptr, qRound(eta / 60.0)) : tr("about %n minute(s)", nullptr, eta);
        }
        else
        {
            estimate = paused ? tr("paused") : tr("working out how long");
        }
    }
    else
    {
        estimate = tr("working out how long");
    }
    m_leftHint->setText(estimate);

    // Memory, against the same safe limit builds use (45% of this computer's memory).
    const double memory = ProcessingMemoryMb();
    const double safe = ProcessingTotalMemoryMb() * 0.45;
    m_memoryValue->setText(ProcessingSize(memory));
    m_memoryHint->setText(memory <= safe ? tr("kept under the safe limit") : tr("over the safe limit (%1)").arg(ProcessingSize(safe)));
    ProcessingTone(m_memoryValue, memory <= safe ? Hearth::Text() : Hearth::Warn());

    // The database changes slower; look every few seconds.
    if (m_ticks++ % 5 == 0)
    {
        RefreshDatabase();
    }
    FillQueue();
}

void HearthProcessingPane::RefreshDatabase()
{
    if (!m_database)
    {
        m_database = std::make_unique<AssetDatabaseConnection>();
        if (!m_database->OpenDatabase(true))
        {
            m_database.reset();
        }
    }
    const char* platform = AzToolsFramework::AssetSystem::GetHostAssetPlatform();
    int done = 0;
    QVector<HearthFailedAsset> failed;
    if (m_database)
    {
        m_database->QueryJobsTable([&done](JobDatabaseEntry&) { ++done; return true; }, AZ::Uuid::CreateNull(), nullptr, platform,
                                   AzToolsFramework::AssetSystem::JobStatus::Completed);
        QSet<qint64> seen;
        m_database->QueryJobsTable(
            [this, &failed, &seen](JobDatabaseEntry& job)
            {
                if (seen.contains(job.m_sourcePK))
                {
                    return true;
                }
                seen.insert(job.m_sourcePK);
                HearthFailedAsset entry;
                entry.sourceId = job.m_sourcePK;
                entry.jobRunKey = job.m_jobRunKey;
                entry.logFile = QString::fromUtf8(job.m_lastFailLogFile.c_str());
                m_database->QuerySourceBySourceID(job.m_sourcePK, [this, &entry](SourceDatabaseEntry& source)
                {
                    entry.relative = QString::fromUtf8(source.m_sourceName.c_str());
                    m_database->QueryScanFolderByScanFolderID(source.m_scanFolderPK, [&entry](ScanFolderDatabaseEntry& folder)
                    {
                        entry.path = QDir(QString::fromUtf8(folder.m_scanFolder.c_str())).filePath(entry.relative);
                        return false;
                    });
                    return false;
                });
                entry.kind = ProcessingKind(entry.relative);
                failed << entry;
                return true;
            },
            AZ::Uuid::CreateNull(), nullptr, platform, AzToolsFramework::AssetSystem::JobStatus::Failed);
    }
    m_done = done;
    m_doneValue->setText(m_database ? QLocale().toString(done) : QStringLiteral("–"));
    m_failedValue->setText(QLocale().toString(failed.size()));
    ProcessingTone(m_failedValue, failed.isEmpty() ? Hearth::Text() : Hearth::Bad());
    const bool changed = failed.size() != m_failed.size()
        || !std::equal(failed.begin(), failed.end(), m_failed.begin(), [](const HearthFailedAsset& a, const HearthFailedAsset& b) { return a.path == b.path && a.jobRunKey == b.jobRunKey; });
    m_failed = failed;
    if (changed)
    {
        m_failedPick = qBound(0, m_failedPick, qMax(0, int(m_failed.size()) - 1));
        FillFailed();
    }
}

void HearthProcessingPane::FillQueue()
{
    const QJsonArray jobs = m_status.value(QStringLiteral("jobs")).toArray();
    m_queue->setUpdatesEnabled(false);
    m_queue->clear();
    // Working first, then the order the Asset Processor will take them in.
    QJsonArray sorted;
    for (const QJsonValue job : jobs)
    {
        if (job.toObject().value(QStringLiteral("state")).toString() == QLatin1String("working"))
        {
            sorted.append(job);
        }
    }
    for (const QJsonValue job : jobs)
    {
        if (job.toObject().value(QStringLiteral("state")).toString() != QLatin1String("working"))
        {
            sorted.append(job);
        }
    }
    int shown = 0;
    for (const QJsonValue value : sorted)
    {
        const QJsonObject job = value.toObject();
        const QString state = job.value(QStringLiteral("state")).toString();
        if (!m_queueFilter.isEmpty() && state != m_queueFilter)
        {
            continue;
        }
        if (++shown > 300)
        {
            break;
        }
        auto* row = new QWidget();
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(6, 4, 6, 4);
        layout->setSpacing(10);
        const QString relative = job.value(QStringLiteral("relative")).toString();
        QLabel* kind = Hearth::MakeChip(ProcessingKind(relative), QStringLiteral("plain"), row);
        kind->setFixedWidth(86);
        kind->setAlignment(Qt::AlignCenter);
        layout->addWidget(kind);
        QLabel* name = Hearth::MakeLabel(QFileInfo(relative).fileName(), QStringLiteral("body"), row);
        name->setToolTip(relative);
        layout->addWidget(name, 1);
        layout->addWidget(Hearth::MakeLabel(QFileInfo(relative).path(), QStringLiteral("small"), row));
        const bool working = state == QLatin1String("working");
        layout->addWidget(Hearth::MakeChip(working ? tr("Working") : tr("Waiting"), working ? QStringLiteral("ember") : QStringLiteral("plain"), row));
        auto* item = new QListWidgetItem(m_queue);
        item->setSizeHint(QSize(10, 36));
        m_queue->setItemWidget(item, row);
    }
    if (shown == 0)
    {
        auto* item = new QListWidgetItem(m_running ? (m_queueFilter.isEmpty() ? tr("Nothing to do: every file is processed.") : tr("Nothing here right now."))
                                                   : tr("The Asset Processor isn't running. Start it to process files."),
                                         m_queue);
        item->setForeground(Hearth::Dim());
    }
    m_queue->setUpdatesEnabled(true);
}

QString HearthProcessingPane::JobLog(const HearthFailedAsset& failed) const
{
    AZ::Outcome<AZStd::string> log = AZ::Failure();
    AzToolsFramework::AssetSystemJobRequestBus::BroadcastResult(log, &AzToolsFramework::AssetSystemJobRequestBus::Events::GetJobLog, failed.jobRunKey);
    if (log.IsSuccess() && !log.GetValue().empty())
    {
        return QString::fromUtf8(log.GetValue().c_str());
    }
    QFile file(failed.logFile);
    if (!failed.logFile.isEmpty() && file.open(QIODevice::ReadOnly))
    {
        return QString::fromUtf8(file.readAll());
    }
    return QString();
}

QStringList HearthProcessingPane::NeededBy(const HearthFailedAsset& failed) const
{
    QStringList names;
    if (!m_database)
    {
        return names;
    }
    QSet<qint64> sources;
    // Products that depend on this file's products, and sources that name it as a source dependency.
    m_database->QueryProductDependenciesThatDependOnProductBySourceId(failed.sourceId, [this, &sources](ProductDependencyDatabaseEntry& dependency)
    {
        m_database->QueryJobByProductID(dependency.m_productPK, [&sources](JobDatabaseEntry& job)
        {
            sources.insert(job.m_sourcePK);
            return false;
        });
        return true;
    });
    AZ::Uuid guid = AZ::Uuid::CreateNull();
    QString scanFolder;
    m_database->QuerySourceBySourceID(failed.sourceId, [this, &guid, &scanFolder](SourceDatabaseEntry& source)
    {
        guid = source.m_sourceGuid;
        m_database->QueryScanFolderByScanFolderID(source.m_scanFolderPK, [&scanFolder](ScanFolderDatabaseEntry& folder)
        {
            scanFolder = QString::fromUtf8(folder.m_scanFolder.c_str());
            return false;
        });
        return false;
    });
    QVector<AZ::Uuid> sourceGuids;
    m_database->QuerySourceDependencyByDependsOnSource(guid, failed.relative.toUtf8().constData(), scanFolder.toUtf8().constData(),
                                                        SourceFileDependencyEntry::DEP_Any, [&sourceGuids](SourceFileDependencyEntry& entry)
    {
        sourceGuids << entry.m_sourceGuid;
        return true;
    });
    for (const AZ::Uuid& sourceGuid : sourceGuids)
    {
        m_database->QuerySourceBySourceGuid(sourceGuid, [&sources](SourceDatabaseEntry& source)
        {
            sources.insert(source.m_sourceID);
            return false;
        });
    }
    sources.remove(failed.sourceId);
    for (qint64 id : sources)
    {
        m_database->QuerySourceBySourceID(id, [&names](SourceDatabaseEntry& source)
        {
            names << QString::fromUtf8(source.m_sourceName.c_str());
            return false;
        });
    }
    names.removeDuplicates();
    names.sort(Qt::CaseInsensitive);
    return names;
}

void HearthProcessingPane::FillFailed()
{
    while (QLayoutItem* item = m_failedLayout->takeAt(0))
    {
        if (QWidget* widget = item->widget())
        {
            widget->deleteLater();
        }
        else if (QLayout* layout = item->layout())
        {
            while (QLayoutItem* inner = layout->takeAt(0))
            {
                if (inner->widget())
                {
                    inner->widget()->deleteLater();
                }
                delete inner;
            }
        }
        delete item;
    }
    if (m_failed.isEmpty())
    {
        m_failedLayout->addWidget(Hearth::MakeLabel(tr("Nothing failed"), QStringLiteral("title"), m_failedCard));
        m_failedLayout->addWidget(Hearth::MakeLabel(tr("When a file can't be processed it shows up here, with what to do about it."), QStringLiteral("dim"), m_failedCard));
        m_failedLayout->addStretch(1);
        return;
    }
    const HearthFailedAsset failed = m_failed[m_failedPick];
    auto* title = new QHBoxLayout();
    title->addWidget(Hearth::MakeChip(tr("%n failed", nullptr, int(m_failed.size())), QStringLiteral("bad"), m_failedCard));
    title->addStretch(1);
    if (m_failed.size() > 1)
    {
        // Step through the failed files.
        QPushButton* previous = Hearth::MakeButton(QStringLiteral("‹"), false, m_failedCard);
        QPushButton* next = Hearth::MakeButton(QStringLiteral("›"), false, m_failedCard);
        previous->setEnabled(m_failedPick > 0);
        next->setEnabled(m_failedPick + 1 < m_failed.size());
        connect(previous, &QPushButton::clicked, this, [this]() { --m_failedPick; FillFailed(); });
        connect(next, &QPushButton::clicked, this, [this]() { ++m_failedPick; FillFailed(); });
        title->addWidget(Hearth::MakeLabel(tr("%1 of %2").arg(m_failedPick + 1).arg(m_failed.size()), QStringLiteral("small"), m_failedCard));
        title->addWidget(previous);
        title->addWidget(next);
    }
    m_failedLayout->addLayout(title);
    QLabel* name = Hearth::MakeLabel(QFileInfo(failed.relative).fileName(), QStringLiteral("mono"), m_failedCard);
    name->setToolTip(failed.path);
    name->setStyleSheet(QStringLiteral("background: transparent; color: #ECE6DE; font-family: \"Geist Mono\"; font-size: 14px; font-weight: 500;"));
    m_failedLayout->addWidget(name);
    QLabel* what = Hearth::MakeLabel(tr("The %1 stopped on this %2. Everything else keeps processing.").arg(ProcessingBuilder(failed.kind), ProcessingThing(failed.kind)),
                                     QStringLiteral("body"), m_failedCard);
    what->setWordWrap(true);
    m_failedLayout->addWidget(what);

    // The first error in its log, so it's clear what went wrong.
    const QString log = JobLog(failed);
    QString firstError;
    for (const QString& line : log.split(QLatin1Char('\n')))
    {
        if (line.contains(QLatin1String("Error"), Qt::CaseInsensitive) || line.contains(QLatin1String("failed"), Qt::CaseInsensitive))
        {
            firstError = line.section(QLatin1Char('|'), -1).trimmed();
            break;
        }
    }
    if (!firstError.isEmpty())
    {
        QLabel* error = Hearth::MakeLabel(firstError.left(300), QStringLiteral("mono"), m_failedCard);
        error->setWordWrap(true);
        error->setStyleSheet(QStringLiteral("background-color: rgba(255, 122, 102, 20); color: #FFB38F; border-radius: 8px; padding: 6px 8px; font-family: \"Geist Mono\"; font-size: 11px;"));
        m_failedLayout->addWidget(error);
    }

    const int tried = m_status.value(QStringLiteral("failures")).toObject().value(failed.path).toInt(1);
    const QStringList needed = NeededBy(failed);
    QString level;
    QString levelText;
    if (GetIEditor())
    {
        level = GetIEditor()->GetLevelName();
        if (!level.isEmpty())
        {
            QFile prefab(QDir(ProcessingProject()).filePath(QStringLiteral("Levels/%1/%1.prefab").arg(level)));
            if (prefab.open(QIODevice::ReadOnly))
            {
                levelText = QString::fromUtf8(prefab.readAll()).toLower();
            }
        }
    }
    auto usedInLevel = [&levelText](const QString& relative)
    {
        const QString stem = QFileInfo(relative).completeBaseName().toLower();
        return !levelText.isEmpty() && (levelText.contains(QLatin1Char('/') + stem + QLatin1Char('.')) || levelText.contains(QLatin1Char('"') + stem + QLatin1Char('.')));
    };
    QString neededText = needed.isEmpty() ? tr("Nothing yet") : QFileInfo(needed.first()).completeBaseName();
    if (needed.size() > 1)
    {
        neededText += tr(" and %n more", nullptr, int(needed.size() - 1));
    }
    if (!level.isEmpty())
    {
        bool used = usedInLevel(failed.relative);
        for (const QString& user : needed)
        {
            used = used || usedInLevel(user);
        }
        neededText += used ? tr(" (used in %1)").arg(level) : tr(" (not used in %1)").arg(level);
    }
    auto fact = [this](const QString& label, const QString& value)
    {
        auto* row = new QHBoxLayout();
        row->addWidget(Hearth::MakeLabel(label, QStringLiteral("dim"), m_failedCard));
        row->addStretch(1);
        QLabel* text = Hearth::MakeLabel(value, QStringLiteral("body"), m_failedCard);
        text->setWordWrap(true);
        text->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(text);
        m_failedLayout->addLayout(row);
    };
    fact(tr("Tried"), tr("%n time(s)", nullptr, qMax(1, tried)));
    fact(tr("Needed by"), neededText);
    m_failedLayout->addStretch(1);

    QPushButton* claude = Hearth::MakeButton(tr("Let Claude fix it"), true, m_failedCard);
    connect(claude, &QPushButton::clicked, this, [failed, log, firstError]()
    {
        const QString details = QStringLiteral("The Asset Processor keeps failing on %1 (%2).\nFirst error: %3\n\nIts log:\n%4")
                                    .arg(failed.relative, failed.path, firstError.isEmpty() ? QStringLiteral("(none found)") : firstError, log.right(12000));
        Hearth::AskClaude(QStringLiteral("Asset failed: %1").arg(QFileInfo(failed.relative).fileName()), details, ProcessingProject());
    });
    m_failedLayout->addWidget(claude);
    auto* buttons = new QHBoxLayout();
    QPushButton* openLog = Hearth::MakeButton(tr("Open its log"), false, m_failedCard);
    connect(openLog, &QPushButton::clicked, this, [this, failed, log]()
    {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Log for %1").arg(QFileInfo(failed.relative).fileName()));
        dialog.resize(900, 600);
        auto* layout = new QVBoxLayout(&dialog);
        auto* text = new QPlainTextEdit(&dialog);
        text->setReadOnly(true);
        text->setPlainText(log.isEmpty() ? tr("No log was kept for this job.") : log);
        text->setStyleSheet(QStringLiteral("QPlainTextEdit { font-family: \"Geist Mono\"; font-size: 11px; }"));
        layout->addWidget(text);
        if (!failed.logFile.isEmpty() && QFileInfo::exists(failed.logFile))
        {
            QPushButton* folder = Hearth::MakeButton(tr("Show the log file"), false, &dialog);
            const QString path = failed.logFile;
            connect(folder, &QPushButton::clicked, &dialog, [path]() { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).path())); });
            layout->addWidget(folder, 0, Qt::AlignLeft);
        }
        dialog.exec();
    });
    QPushButton* again = Hearth::MakeButton(tr("Try again"), false, m_failedCard);
    connect(again, &QPushButton::clicked, this, [this, failed]() { Send(QJsonObject{ { QStringLiteral("reprocess"), QJsonArray{ failed.path } } }); });
    buttons->addWidget(openLog);
    buttons->addWidget(again);
    m_failedLayout->addLayout(buttons);
}
