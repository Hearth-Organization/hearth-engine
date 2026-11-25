/*
 * Hearth Engine Profiler. Built on O3DE (Apache-2.0 OR MIT). See HearthProfilerPane.h.
 */

#include "EditorDefs.h"

#include "HearthProfilerPane.h"
#include "HearthAgentClient.h"

#include <AzCore/Debug/ProfilerBus.h>
#include <Atom/RPI.Public/Pass/PassSystemInterface.h>
#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFile>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;
    constexpr int ShownFrames = 60;
    constexpr double TargetMs = 33.3;

    QString CaptureFolder()
    {
        QDir folder(QDir(Hearth::HomeFolder()).filePath(QStringLiteral("captures")));
        folder.mkpath(QStringLiteral("."));
        return folder.absolutePath();
    }

    double MemoryUsedGb(double* totalGb)
    {
        QFile meminfo(QStringLiteral("/proc/meminfo"));
        double total = 0.0;
        double available = 0.0;
        if (meminfo.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            for (const QByteArray& line : meminfo.readAll().split('\n'))
            {
                const QList<QByteArray> parts = line.simplified().split(' ');
                if (parts.size() >= 2 && parts[0] == "MemTotal:")
                {
                    total = parts[1].toDouble() / (1024.0 * 1024.0);
                }
                else if (parts.size() >= 2 && parts[0] == "MemAvailable:")
                {
                    available = parts[1].toDouble() / (1024.0 * 1024.0);
                }
            }
        }
        *totalGb = total;
        return total - available;
    }

    //! Busy percentage of the graphics chip. AMD tells us directly. Intel (i915 and xe) doesn't without root, but it
    //! does say how long the chip has been asleep (rc6 / idle residency), so busy = 1 - asleep time / wall time.
    QString GpuBusy()
    {
        for (const QString& card : { QStringLiteral("card0"), QStringLiteral("card1") })
        {
            QFile busy(QStringLiteral("/sys/class/drm/%1/device/gpu_busy_percent").arg(card));
            if (busy.open(QIODevice::ReadOnly))
            {
                return QString::fromUtf8(busy.readAll()).trimmed() + QLatin1Char('%');
            }
        }
        static QString idleFile;
        static double lastIdleMs = -1.0;
        static QElapsedTimer clock;
        static QString lastText = QStringLiteral("—");
        if (idleFile.isEmpty())
        {
            for (const QString& card : { QStringLiteral("card0"), QStringLiteral("card1") })
            {
                for (const QString& path : { QStringLiteral("/sys/class/drm/%1/gt/gt0/rc6_residency_ms"),
                                             QStringLiteral("/sys/class/drm/%1/power/rc6_residency_ms"),
                                             QStringLiteral("/sys/class/drm/%1/device/tile0/gt0/gtidle/idle_residency_ms") })
                {
                    if (idleFile.isEmpty() && QFileInfo(path.arg(card)).isReadable())
                    {
                        idleFile = path.arg(card);
                    }
                }
            }
            if (idleFile.isEmpty())
            {
                idleFile = QStringLiteral("none");
            }
        }
        if (idleFile == QLatin1String("none"))
        {
            return lastText;
        }
        QFile idle(idleFile);
        if (!idle.open(QIODevice::ReadOnly))
        {
            return lastText;
        }
        const double idleMs = QString::fromUtf8(idle.readAll()).trimmed().toDouble();
        if (lastIdleMs >= 0.0 && clock.isValid() && clock.elapsed() > 200)
        {
            const double wall = static_cast<double>(clock.elapsed());
            const double busy = qBound(0.0, 1.0 - (idleMs - lastIdleMs) / wall, 1.0);
            lastText = QStringLiteral("%1%").arg(qRound(busy * 100.0));
        }
        lastIdleMs = idleMs;
        clock.restart();
        return lastText;
    }

    //! What a profiler region is about, for the Show chips: 0 CPU, 1 GPU, 2 Memory, 3 Network, 4 Asset loads.
    int ProfilerRegionKind(const QString& name)
    {
        static const QStringList gpu = { QStringLiteral("render"), QStringLiteral("pass"), QStringLiteral("rhi"), QStringLiteral("atom"),
                                         QStringLiteral("draw"), QStringLiteral("shadow"), QStringLiteral("light"), QStringLiteral("post"),
                                         QStringLiteral("cull"), QStringLiteral("shader"), QStringLiteral("present"), QStringLiteral("gpu") };
        static const QStringList memory = { QStringLiteral("alloc"), QStringLiteral("memory"), QStringLiteral("garbage"), QStringLiteral("free") };
        static const QStringList network = { QStringLiteral("net"), QStringLiteral("multiplayer"), QStringLiteral("replica"),
                                             QStringLiteral("socket"), QStringLiteral("packet") };
        static const QStringList assets = { QStringLiteral("asset"), QStringLiteral("load"), QStringLiteral("stream"),
                                            QStringLiteral("spawn"), QStringLiteral("prefab") };
        const QString lower = name.toLower();
        auto any = [&lower](const QStringList& words)
        {
            return std::any_of(words.begin(), words.end(), [&lower](const QString& word) { return lower.contains(word); });
        };
        if (any(assets))
        {
            return 4;
        }
        if (any(network))
        {
            return 3;
        }
        if (any(memory))
        {
            return 2;
        }
        if (any(gpu))
        {
            return 1;
        }
        return 0;
    }
} // namespace

//! The last frames as bars (ember, red when over twice the target), the picked one highlighted.
class HearthFrameBars : public QWidget
{
public:
    explicit HearthFrameBars(QWidget* parent)
        : QWidget(parent)
    {
        setMinimumHeight(150);
        setCursor(Qt::PointingHandCursor);
    }
    QVector<double> m_ms;
    int m_picked = -1;
    std::function<void(int)> m_onPick;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        const int count = m_ms.size();
        if (count == 0)
        {
            g.setPen(Hearth::Dim());
            g.drawText(rect(), Qt::AlignCenter, QObject::tr("Press Record, play for a moment, then Pause."));
            return;
        }
        const double top = qMax(TargetMs * 2.0, *std::max_element(m_ms.begin(), m_ms.end()) * 1.1);
        const double w = static_cast<double>(width()) / count;
        for (int i = 0; i < count; ++i)
        {
            const double h = height() * m_ms[i] / top;
            QColor color = m_ms[i] > TargetMs * 2.0 ? QColor(0xEF, 0x6B, 0x5B) : QColor(232, 116, 59, i == m_picked ? 230 : 115);
            g.setPen(Qt::NoPen);
            g.setBrush(color);
            g.drawRoundedRect(QRectF(i * w + 1, height() - h, w - 2, h), 2, 2);
        }
        const double targetY = height() - height() * TargetMs / top;
        g.setPen(QPen(QColor(166, 221, 151, 160), 1, Qt::DashLine));
        g.drawLine(QPointF(0, targetY), QPointF(width(), targetY));
        g.setPen(QColor(0xA6, 0xDD, 0x97));
        g.drawText(QRectF(0, targetY - 16, width() - 4, 14), Qt::AlignRight, QObject::tr("33 ms · 30 fps target"));
    }
    void mousePressEvent(QMouseEvent* event) override
    {
        if (!m_ms.isEmpty() && m_onPick)
        {
            m_onPick(qBound(0, static_cast<int>(event->pos().x() * m_ms.size() / qMax(1, width())), m_ms.size() - 1));
        }
    }
};

//! Where the time went in one frame: rows by depth, blocks by time (like the mockup's flame rows).
class HearthFlameView : public QWidget
{
public:
    explicit HearthFlameView(QWidget* parent)
        : QWidget(parent)
    {
        setMinimumHeight(4 * 28);
    }
    HearthProfilerPane::Frame m_frame;
    bool m_show[5] = { true, true, true, true, true }; //!< CPU, GPU, Memory, Network, Asset loads

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        if (m_frame.ms <= 0.0)
        {
            return;
        }
        static const QColor depthColors[] = { QColor(0x3D, 0x34, 0x2B), QColor(0x6B, 0x44, 0x30), QColor(0xE8, 0x74, 0x3B), QColor(0xFF, 0x9A, 0x63) };
        QFont font = g.font();
        font.setFamily(QStringLiteral("Geist Mono"));
        font.setPixelSize(11);
        g.setFont(font);
        for (const HearthProfilerPane::Region& region : m_frame.regions)
        {
            if (region.depth > 3)
            {
                continue;
            }
            const double x = (region.startMs - m_frame.startMs) / m_frame.ms * width();
            const double w = (region.endMs - region.startMs) / m_frame.ms * width();
            if (w < 2.0)
            {
                continue;
            }
            const QRectF box(x, region.depth * 28, w - 2, 26);
            // Frame-level blocks always show; the rest follow the Show chips (hidden kinds are drawn faint).
            const bool shown = region.depth == 0 || m_show[ProfilerRegionKind(region.name)];
            g.setOpacity(shown ? 1.0 : 0.15);
            g.setPen(Qt::NoPen);
            g.setBrush(depthColors[region.depth]);
            g.drawRoundedRect(box, 5, 5);
            g.setPen(region.depth >= 2 ? Hearth::Ink() : Hearth::Text());
            const QString text = QStringLiteral("%1 %2").arg(region.name).arg(region.endMs - region.startMs, 0, 'f', 1);
            g.drawText(box.adjusted(6, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, g.fontMetrics().elidedText(text, Qt::ElideRight, static_cast<int>(box.width()) - 10));
        }
        g.setOpacity(1.0);
    }
};

void HearthProfilerPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(100, 80, 1300, 820);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthProfilerPane>("Profiler", "Tools", options);
}

HearthProfilerPane::HearthProfilerPane(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthProfiler"));
    setStyleSheet(QStringLiteral("QWidget#HearthProfiler { background-color: #0D0C0B; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Profiler"), tr("Records the editor and the game while it plays"));
    if (auto* titles = qobject_cast<QLayout*>(header->itemAt(1) ? header->itemAt(1)->layout() : nullptr))
    {
        m_subtitle = titles->itemAt(1) ? qobject_cast<QLabel*>(titles->itemAt(1)->widget()) : nullptr;
    }
    m_recordingChip = Hearth::MakeChip(tr("Not recording"), QStringLiteral("plain"), this);
    m_record = Hearth::MakeButton(tr("Record"), false, this);
    QPushButton* save = Hearth::MakeButton(tr("Save capture"), false, this);
    save->setToolTip(tr("Keep this capture with a name, in the captures folder"));
    QPushButton* explain = Hearth::MakeButton(tr("Ask Claude to explain"), true, this);
    header->addWidget(m_recordingChip);
    header->addWidget(m_record);
    header->addWidget(save);
    header->addWidget(explain);
    root->addLayout(header);
    connect(m_record, &QPushButton::clicked, this, &HearthProfilerPane::ToggleRecording);
    connect(save, &QPushButton::clicked, this, &HearthProfilerPane::SaveCapture);
    connect(explain, &QPushButton::clicked, this, [this]()
    {
        QStringList report;
        report << tr("Capture: %1").arg(m_capturePath);
        for (int i = 0; i < 5; ++i)
        {
            report << m_stat[i]->text();
        }
        Hearth::AskClaude(tr("Explain this Hearth Engine profiler capture and what to fix"), report.join(QLatin1Char('\n')));
    });

    auto* body = new QHBoxLayout();
    body->setSpacing(10);
    QVBoxLayout* main = nullptr;
    QFrame* mainCard = Hearth::MakeCard(this, &main);
    auto* stats = new QHBoxLayout();
    stats->setSpacing(8);
    const QStringList names = { tr("Frame rate"), tr("Worst frame"), tr("RAM"), tr("GPU"), tr("Draw calls") };
    for (int i = 0; i < 5; ++i)
    {
        auto* tile = new QFrame(mainCard);
        tile->setObjectName(QStringLiteral("ProfStat"));
        tile->setStyleSheet(QStringLiteral("QFrame#ProfStat { background-color: rgba(255, 255, 255, 8); border: 1px solid rgba(255, 255, 255, 15); border-radius: 12px; }"));
        auto* layout = new QVBoxLayout(tile);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(4);
        layout->addWidget(Hearth::MakeLabel(names[i], QStringLiteral("small"), tile));
        m_stat[i] = Hearth::MakeLabel(QStringLiteral("—"), QStringLiteral("mono"), tile);
        m_stat[i]->setStyleSheet(QStringLiteral("background: transparent; color: #ECE6DE; font-family: \"Geist Mono\"; font-size: 18px; font-weight: 600;"));
        layout->addWidget(m_stat[i]);
        stats->addWidget(tile);
    }
    main->addLayout(stats);
    auto* framesRow = new QHBoxLayout();
    framesRow->addWidget(Hearth::MakeLabel(tr("Frames"), QStringLiteral("title"), mainCard));
    m_framesLine = Hearth::MakeLabel(tr("live"), QStringLiteral("dim"), mainCard);
    framesRow->addWidget(m_framesLine);
    framesRow->addStretch(1);
    main->addLayout(framesRow);
    m_bars = new HearthFrameBars(mainCard);
    m_bars->m_onPick = [this](int index) { PickFrame(index); };
    main->addWidget(m_bars);
    m_frameTitle = Hearth::MakeLabel(tr("Where the time went"), QStringLiteral("title"), mainCard);
    main->addWidget(m_frameTitle);
    m_flame = new HearthFlameView(mainCard);
    main->addWidget(m_flame);
    auto* showRow = new QHBoxLayout();
    showRow->setSpacing(6);
    showRow->addWidget(Hearth::MakeLabel(tr("Show"), QStringLiteral("small"), mainCard));
    const QStringList kinds = { tr("CPU"), tr("GPU"), tr("Memory"), tr("Network"), tr("Asset loads") };
    for (int i = 0; i < kinds.size(); ++i)
    {
        QPushButton* chip = Hearth::MakeButton(kinds[i], false, mainCard);
        chip->setCheckable(true);
        chip->setChecked(true);
        chip->setStyleSheet(QStringLiteral("QPushButton { border-radius: 12px; padding: 3px 12px; font-size: 12px; background-color: rgba(255, 255, 255, 10); color: #8D857B; }"
                                           "QPushButton:checked { background-color: rgba(232, 116, 59, 46); color: #ECE6DE; }"));
        connect(chip, &QPushButton::toggled, this, [this, i](bool on)
        {
            m_flame->m_show[i] = on;
            m_flame->update();
        });
        showRow->addWidget(chip);
    }
    showRow->addStretch(1);
    main->addLayout(showRow);
    main->addStretch(1);
    body->addWidget(mainCard, 1);

    QVBoxLayout* side = nullptr;
    QFrame* sideCard = Hearth::MakeCard(this, &side);
    sideCard->setFixedWidth(380);
    side->addWidget(Hearth::MakeLabel(tr("What to fix"), QStringLiteral("title"), sideCard));
    m_tips = new QVBoxLayout();
    m_tips->setSpacing(8);
    side->addLayout(m_tips);
    side->addStretch(1);
    body->addWidget(sideCard);
    root->addLayout(body, 1);

    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &HearthProfilerPane::UpdateStats);
    m_timer->start();
    AZ::TickBus::Handler::BusConnect();
    UpdateTips();
}

HearthProfilerPane::~HearthProfilerPane()
{
    AZ::TickBus::Handler::BusDisconnect();
    if (m_recording)
    {
        if (auto* profiler = AZ::Debug::ProfilerSystemInterface::Get())
        {
            profiler->EndCapture();
        }
    }
}

void HearthProfilerPane::OnTick(float deltaTime, [[maybe_unused]] AZ::ScriptTimePoint time)
{
    m_liveFrames.push_back(deltaTime * 1000.0f);
    if (m_liveFrames.size() > 600)
    {
        m_liveFrames.remove(0, m_liveFrames.size() - 600);
    }
}

void HearthProfilerPane::UpdateStats()
{
    if (!isVisible())
    {
        return;
    }
    // Live numbers from the last second of frames.
    const int recent = qMin(m_liveFrames.size(), 60);
    double sum = 0.0;
    double worst = 0.0;
    for (int i = m_liveFrames.size() - recent; i < m_liveFrames.size(); ++i)
    {
        sum += m_liveFrames[i];
        worst = qMax(worst, static_cast<double>(m_liveFrames[i]));
    }
    m_stat[0]->setText(recent ? tr("%1 fps").arg(qRound(1000.0 * recent / qMax(1.0, sum))) : QStringLiteral("—"));
    m_stat[1]->setText(recent ? tr("%1 ms").arg(qRound(worst)) : QStringLiteral("—"));
    m_stat[1]->setStyleSheet(QStringLiteral("background: transparent; font-family: \"Geist Mono\"; font-size: 18px; font-weight: 600; color: %1;")
                                 .arg(worst > TargetMs * 2 ? QStringLiteral("#FF9F8F") : QStringLiteral("#ECE6DE")));
    double total = 0.0;
    const double used = MemoryUsedGb(&total);
    m_stat[2]->setText(QStringLiteral("%1 / %2 GB").arg(used, 0, 'f', 1).arg(total, 0, 'f', 1));
    m_stat[2]->setStyleSheet(QStringLiteral("background: transparent; font-family: \"Geist Mono\"; font-size: 18px; font-weight: 600; color: %1;")
                                 .arg(total > 0 && used / total > 0.8 ? QStringLiteral("#F0D17A") : QStringLiteral("#ECE6DE")));
    m_stat[3]->setText(GpuBusy());
    // Draw calls: everything the renderer drew last frame.
    if (auto* passes = AZ::RPI::PassSystemInterface::Get())
    {
        const AZ::RPI::PassSystemFrameStatistics frame = passes->GetFrameStatistics();
        m_stat[4]->setText(QLocale().toString(static_cast<qlonglong>(frame.m_totalDrawItemsRendered)));
    }
    if (m_subtitle)
    {
        const bool inGame = GetIEditor() && GetIEditor()->IsInGameMode();
        const QString level = GetIEditor() ? GetIEditor()->GetLevelName() : QString();
        const QString players = m_players > 1 ? tr(" · %1 players").arg(m_players) : QString();
        m_subtitle->setText(m_recording ? (inGame ? tr("Recording the game · %1%2").arg(level, players) : tr("Recording the editor · %1").arg(level))
                                        : tr("Records the editor and the game while it plays"));
        // How many players the co-op test has (every few seconds while recording the game).
        static int ask = 0;
        if (m_recording && inGame && (ask++ % 5) == 0)
        {
            if (!m_agent)
            {
                m_agent = std::make_unique<HearthAgentClient>(this);
            }
            m_agent->Post(QStringLiteral("coop_status"), {}, [this](const QJsonObject& answer, bool reached)
            {
                m_players = reached && answer.value(QStringLiteral("running")).toBool() ? answer.value(QStringLiteral("players")).toArray().size() : 0;
            }, 3000);
        }
    }
    if (m_frames.isEmpty())
    {
        m_framesLine->setText(tr("live · last %1").arg(qMin(ShownFrames, m_liveFrames.size())));
    }
    if (!m_recording && m_frames.isEmpty())
    {
        // Before a capture, show the live frames as bars.
        m_bars->m_ms.clear();
        for (int i = qMax(0, m_liveFrames.size() - ShownFrames); i < m_liveFrames.size(); ++i)
        {
            m_bars->m_ms.push_back(m_liveFrames[i]);
        }
        m_bars->update();
    }
}

void HearthProfilerPane::ToggleRecording()
{
    auto* profiler = AZ::Debug::ProfilerSystemInterface::Get();
    if (!profiler)
    {
        m_recordingChip->setText(tr("Turn on the Profiler engine part (Hub) and rebuild"));
        m_recordingChip->setStyleSheet(QStringLiteral("background-color: rgba(232, 190, 59, 36); color: #F0D17A; border-radius: 10px; padding: 3px 10px; font-size: 11px;"));
        return;
    }
    if (!m_recording)
    {
        m_capturePath = QDir(CaptureFolder()).filePath(QStringLiteral("capture_%1.json").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"))));
        profiler->SetActive(true);
        m_recording = profiler->StartCapture(AZStd::string(m_capturePath.toUtf8().constData()));
        if (m_recording)
        {
            m_record->setText(tr("Pause"));
            m_recordingChip->setText(tr("● Recording"));
            m_recordingChip->setStyleSheet(QStringLiteral("background-color: rgba(232, 80, 59, 41); color: #FF9F8F; border-radius: 10px; padding: 3px 10px; font-size: 11px;"));
        }
        return;
    }
    profiler->EndCapture();
    m_recording = false;
    m_record->setText(tr("Record"));
    m_recordingChip->setText(tr("Reading the capture…"));
    // The capture is written in the background; read it when it's there.
    auto* poll = new QTimer(this);
    poll->setInterval(500);
    int tries = 0;
    connect(poll, &QTimer::timeout, this, [this, poll, tries]() mutable
    {
        if (LoadCapture(m_capturePath) || ++tries > 40)
        {
            poll->deleteLater();
            m_recordingChip->setText(m_frames.isEmpty() ? tr("Capture was empty") : tr("Capture: %n frame(s)", nullptr, m_frames.size()));
            m_recordingChip->setStyleSheet(QStringLiteral("background-color: rgba(255, 255, 255, 15); color: #8D857B; border-radius: 10px; padding: 3px 10px; font-size: 11px;"));
        }
    });
    poll->start();
}

bool HearthProfilerPane::LoadCapture(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return false;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject data = root.contains(QStringLiteral("ClassData")) ? root.value(QStringLiteral("ClassData")).toObject() : root;
    const QJsonArray entries = data.value(QStringLiteral("cpuProfilingStatisticsSerializerEntries")).toArray();
    const double ticksPerMs = data.value(QStringLiteral("timeTicksPerSecond")).toDouble(1e9) / 1000.0;
    if (entries.isEmpty())
    {
        return false;
    }
    // The main thread is the one with the most depth-0 regions; each of its depth-0 regions is a frame.
    QHash<QString, int> topCounts;
    for (const QJsonValue value : entries)
    {
        const QJsonObject e = value.toObject();
        if (e.value(QStringLiteral("stackDepth")).toInt() == 0)
        {
            topCounts[e.value(QStringLiteral("threadId")).toVariant().toString()]++;
        }
    }
    QString mainThread;
    int most = 0;
    for (auto it = topCounts.begin(); it != topCounts.end(); ++it)
    {
        if (it.value() > most)
        {
            most = it.value();
            mainThread = it.key();
        }
    }
    QVector<Region> regions;
    double origin = -1.0;
    for (const QJsonValue value : entries)
    {
        const QJsonObject e = value.toObject();
        if (e.value(QStringLiteral("threadId")).toVariant().toString() != mainThread)
        {
            continue;
        }
        Region region;
        region.name = e.value(QStringLiteral("regionName")).toString();
        region.depth = e.value(QStringLiteral("stackDepth")).toInt();
        region.startMs = e.value(QStringLiteral("startTick")).toVariant().toDouble() / ticksPerMs;
        region.endMs = e.value(QStringLiteral("endTick")).toVariant().toDouble() / ticksPerMs;
        origin = origin < 0 ? region.startMs : qMin(origin, region.startMs);
        regions.push_back(region);
    }
    std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) { return a.startMs < b.startMs; });
    m_frames.clear();
    for (const Region& region : regions)
    {
        if (region.depth == 0)
        {
            Frame frame;
            frame.startMs = region.startMs;
            frame.ms = region.endMs - region.startMs;
            m_frames.push_back(frame);
        }
        if (!m_frames.isEmpty() && region.startMs >= m_frames.last().startMs)
        {
            m_frames.last().regions.push_back(region);
        }
    }
    m_bars->m_ms.clear();
    const int first = qMax(0, m_frames.size() - ShownFrames);
    for (int i = first; i < m_frames.size(); ++i)
    {
        m_bars->m_ms.push_back(m_frames[i].ms);
    }
    m_bars->update();
    // Pick the worst frame to start with.
    int worst = -1;
    for (int i = 0; i < m_bars->m_ms.size(); ++i)
    {
        if (worst < 0 || m_bars->m_ms[i] > m_bars->m_ms[worst])
        {
            worst = i;
        }
    }
    PickFrame(worst);
    UpdateTips();
    return true;
}

void HearthProfilerPane::SaveCapture()
{
    if (m_capturePath.isEmpty() || !QFile::exists(m_capturePath))
    {
        m_recordingChip->setText(tr("Record something first"));
        return;
    }
    const QString level = GetIEditor() && !GetIEditor()->GetLevelName().isEmpty() ? GetIEditor()->GetLevelName() : QStringLiteral("editor");
    const QString name = QStringLiteral("%1-%2.json").arg(level.toLower(), QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd-HHmm")));
    const QString target = QDir(CaptureFolder()).filePath(QStringLiteral("saved/") + name);
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile::remove(target);
    if (QFile::copy(m_capturePath, target))
    {
        m_recordingChip->setText(tr("Saved as %1").arg(name));
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(target).absolutePath()));
    }
    else
    {
        m_recordingChip->setText(tr("Couldn't save the capture"));
    }
}

void HearthProfilerPane::PickFrame(int index)
{
    const int first = qMax(0, m_frames.size() - ShownFrames);
    if (index < 0 || first + index >= m_frames.size())
    {
        return;
    }
    m_picked = first + index;
    m_bars->m_picked = index;
    m_bars->update();
    m_flame->m_frame = m_frames[m_picked];
    m_flame->update();
    m_frameTitle->setText(tr("Frame %1, where the time went (%2 ms)").arg(index + 1).arg(m_frames[m_picked].ms, 0, 'f', 1));
    m_framesLine->setText(tr("last %1 · frame %2 picked").arg(qMin(ShownFrames, m_frames.size())).arg(index + 1));
}

void HearthProfilerPane::UpdateTips()
{
    while (QLayoutItem* item = m_tips->takeAt(0))
    {
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
    auto addTip = [this](const QString& color, const QString& title, const QString& text)
    {
        auto* tip = new QFrame();
        tip->setObjectName(QStringLiteral("ProfTip"));
        tip->setStyleSheet(QStringLiteral("QFrame#ProfTip { background-color: rgba(255, 255, 255, 8); border: 1px solid rgba(255, 255, 255, 15); border-radius: 10px; }"));
        auto* layout = new QHBoxLayout(tip);
        layout->setContentsMargins(10, 10, 10, 10);
        auto* dot = new QLabel(QStringLiteral("●"), tip);
        dot->setStyleSheet(QStringLiteral("background: transparent; color: %1;").arg(color));
        layout->addWidget(dot, 0, Qt::AlignTop);
        auto* texts = new QVBoxLayout();
        texts->addWidget(Hearth::MakeLabel(title, QStringLiteral("body"), tip));
        texts->addWidget(Hearth::MakeLabel(text, QStringLiteral("small"), tip));
        layout->addLayout(texts, 1);
        m_tips->addWidget(tip);
    };
    if (m_frames.isEmpty())
    {
        addTip(QStringLiteral("#8D857B"), tr("Nothing recorded yet"), tr("Press Record, play the part that feels slow, then Pause. The worst frames get explained here."));
        return;
    }
    // Spikes: frames over twice the target, and the biggest region in them.
    QHash<QString, double> spikeTime;
    QHash<QString, double> totalTime;
    int spikes = 0;
    for (const Frame& frame : m_frames)
    {
        for (const Region& region : frame.regions)
        {
            if (region.depth == 1 || region.depth == 2)
            {
                totalTime[region.name] += region.endMs - region.startMs;
                if (frame.ms > TargetMs * 2)
                {
                    spikeTime[region.name] += region.endMs - region.startMs;
                }
            }
        }
        spikes += frame.ms > TargetMs * 2 ? 1 : 0;
    }
    auto biggest = [](const QHash<QString, double>& times) -> QPair<QString, double>
    {
        QPair<QString, double> best;
        for (auto it = times.begin(); it != times.end(); ++it)
        {
            if (it.value() > best.second)
            {
                best = { it.key(), it.value() };
            }
        }
        return best;
    };
    if (spikes > 0)
    {
        const auto culprit = biggest(spikeTime);
        addTip(QStringLiteral("#FF9F8F"), tr("%n frame(s) took over 66 ms", nullptr, spikes),
               tr("Most of that time was in %1 (%2 ms over those frames). Spreading that work over several frames stops the hitch.")
                   .arg(culprit.first).arg(culprit.second, 0, 'f', 0));
    }
    const auto heaviest = biggest(totalTime);
    if (!heaviest.first.isEmpty())
    {
        addTip(QStringLiteral("#F0D17A"), tr("%1 is the heaviest part").arg(heaviest.first),
               tr("%1 ms on average per frame.").arg(heaviest.second / qMax(1, m_frames.size()), 0, 'f', 1));
    }
    double average = 0.0;
    for (const Frame& frame : m_frames)
    {
        average += frame.ms;
    }
    average /= m_frames.size();
    addTip(average <= TargetMs ? QStringLiteral("#A6DD97") : QStringLiteral("#F0D17A"),
           average <= TargetMs ? tr("On target") : tr("Below 30 fps on average"),
           tr("Average frame %1 ms over %n frame(s).", nullptr, m_frames.size()).arg(average, 0, 'f', 1));
}
