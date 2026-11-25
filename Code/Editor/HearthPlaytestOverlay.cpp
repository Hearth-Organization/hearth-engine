/*
 * Hearth Engine playtest overlay. Built on O3DE (Apache-2.0 OR MIT). See HearthPlaytestOverlay.h.
 */

#include "EditorDefs.h"

#include "HearthPlaytestOverlay.h"

#include <AzCore/Console/IConsole.h>
#include <AzCore/Interface/Interface.h>
#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/ActionManager/Action/ActionManagerInterface.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdio>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    const char* PlaytestPillStyle = "background-color: rgba(30, 28, 26, 225); border: 1px solid rgba(255, 255, 255, 26); border-radius: 18px;";
    const char* PlaytestKeyStyle = "background-color: rgba(255, 255, 255, 20); color: #ECE6DE; border-radius: 6px; padding: 3px 7px;"
                           " font-family: \"Geist Mono\"; font-size: 11px;";
    const char* PlaytestKeyOnStyle = "background-color: #E8743B; color: #1A0F08; border-radius: 6px; padding: 3px 7px;"
                             " font-family: \"Geist Mono\"; font-size: 11px;";
    const char* PlaytestKeyNames[] = { "W", "A", "S", "D", "Shift", "Space", "F" };

    QLabel* PlaytestKey(const QString& text, QWidget* parent)
    {
        auto* key = new QLabel(text, parent);
        key->setAlignment(Qt::AlignCenter);
        key->setStyleSheet(QLatin1String(PlaytestKeyStyle));
        return key;
    }

    QWidget* PlaytestPill(QWidget* parent)
    {
        auto* pill = new QWidget(parent);
        pill->setAttribute(Qt::WA_StyledBackground);
        pill->setObjectName(QStringLiteral("PlaytestPill"));
        pill->setStyleSheet(QStringLiteral("QWidget#PlaytestPill { ") + QLatin1String(PlaytestPillStyle) + QStringLiteral(" }"));
        return pill;
    }

    QLabel* PlaytestMono(const QString& text, QWidget* parent, const QString& color = QStringLiteral("#8D857B"))
    {
        auto* label = new QLabel(text, parent);
        label->setStyleSheet(QStringLiteral("background: transparent; color: %1; font-family: \"Geist Mono\"; font-size: 11px;").arg(color));
        return label;
    }
} // namespace

HearthPlaytestOverlay::HearthPlaytestOverlay(QMainWindow* mainWindow, std::function<void()> toggleWindowed)
    : QWidget(mainWindow, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus)
    , m_mainWindow(mainWindow)
    , m_toggleWindowed(std::move(toggleWindowed))
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);

    // Top: what's being tested, seed, fps, Back to editor.
    m_top = PlaytestPill(this);
    auto* top = new QHBoxLayout(m_top);
    top->setContentsMargins(14, 6, 6, 6);
    top->setSpacing(10);
    auto* dot = new QLabel(m_top);
    dot->setFixedSize(8, 8);
    dot->setStyleSheet(QStringLiteral("background-color: #E8743B; border-radius: 4px;"));
    top->addWidget(dot);
    m_title = new QLabel(m_top);
    m_title->setStyleSheet(QStringLiteral("background: transparent; color: #ECE6DE; font-size: 13px;"));
    top->addWidget(m_title);
    m_seed = PlaytestMono(QString(), m_top);
    top->addWidget(m_seed);
    m_fps = PlaytestMono(QString(), m_top);
    top->addWidget(m_fps);
    auto* back = new QLabel(tr("Back to editor  <span style=\"background-color: rgba(255,255,255,26); border-radius: 4px; font-family: 'Geist Mono'; font-size: 11px;\">&nbsp;Esc&nbsp;</span>"), m_top);
    back->setTextFormat(Qt::RichText);
    back->setStyleSheet(QStringLiteral("background-color: rgba(255, 255, 255, 15); color: #ECE6DE; border-radius: 14px; padding: 6px 12px;"));
    top->addWidget(back);

    // Right: Claude is driving.
    m_claude = new QWidget(this);
    m_claude->setAttribute(Qt::WA_StyledBackground);
    m_claude->setObjectName(QStringLiteral("PlaytestClaude"));
    m_claude->setStyleSheet(QStringLiteral("QWidget#PlaytestClaude { background-color: rgba(30, 28, 26, 235); border: 1px solid rgba(255, 255, 255, 26); border-radius: 14px; }"));
    m_claude->setFixedWidth(290);
    auto* claude = new QVBoxLayout(m_claude);
    claude->setContentsMargins(14, 12, 14, 12);
    claude->setSpacing(8);
    auto* claudeTop = new QHBoxLayout();
    claudeTop->addWidget(Hearth::MakeLabel(tr("Claude is driving"), QStringLiteral("title"), m_claude), 1);
    QLabel* live = Hearth::MakeLabel(tr("live"), QStringLiteral("small"), m_claude);
    live->setStyleSheet(QStringLiteral("background: transparent; color: #A6DD97; font-size: 11px;"));
    claudeTop->addWidget(live);
    claude->addLayout(claudeTop);
    m_claudeText = Hearth::MakeLabel(QString(), QStringLiteral("dim"), m_claude);
    claude->addWidget(m_claudeText);
    auto* keysTop = new QHBoxLayout();
    keysTop->addStretch(1);
    m_keys[0] = PlaytestKey(QStringLiteral("W"), m_claude);
    keysTop->addWidget(m_keys[0]);
    keysTop->addStretch(1);
    claude->addLayout(keysTop);
    auto* keysMid = new QHBoxLayout();
    keysMid->addStretch(1);
    for (int i = 1; i <= 3; ++i)
    {
        m_keys[i] = PlaytestKey(QLatin1String(PlaytestKeyNames[i]), m_claude);
        keysMid->addWidget(m_keys[i]);
    }
    keysMid->addStretch(1);
    claude->addLayout(keysMid);
    auto* keysLow = new QHBoxLayout();
    keysLow->addStretch(1);
    for (int i = 4; i <= 6; ++i)
    {
        m_keys[i] = PlaytestKey(QLatin1String(PlaytestKeyNames[i]), m_claude);
        keysLow->addWidget(m_keys[i]);
    }
    keysLow->addStretch(1);
    claude->addLayout(keysLow);
    QPushButton* takeBack = Hearth::MakeButton(tr("Take back control"), false, m_claude);
    connect(takeBack, &QPushButton::clicked, this, [this]()
    {
        // The Claude bridge stops sending game input when this is off (the Play menu's "Let Claude drive").
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        settings->setValue(QStringLiteral("claude/drive"), false);
        m_claude->hide();
    });
    claude->addWidget(takeBack);
    m_claude->hide();

    // Bottom left: frame time and memory.
    m_stats = new QWidget(this);
    auto* stats = new QHBoxLayout(m_stats);
    stats->setContentsMargins(0, 0, 0, 0);
    stats->setSpacing(6);
    const QString chip = QStringLiteral("background-color: rgba(30, 28, 26, 225); color: #ECE6DE; border-radius: 12px; padding: 4px 10px;"
                                        " font-family: \"Geist Mono\"; font-size: 11px;");
    m_frameMs = new QLabel(m_stats);
    m_frameMs->setStyleSheet(chip);
    m_memory = new QLabel(m_stats);
    m_memory->setStyleSheet(chip);
    stats->addWidget(m_frameMs);
    stats->addWidget(m_memory);

    // Bottom right: keys.
    m_shortcuts = PlaytestPill(this);
    auto* shortcuts = new QHBoxLayout(m_shortcuts);
    shortcuts->setContentsMargins(12, 6, 14, 6);
    shortcuts->setSpacing(14);
    const QVector<QPair<QString, QString>> keys = { { QStringLiteral("F8"), tr("Pause") }, { QStringLiteral("F9"), tr("Screenshot") },
                                                    { QStringLiteral("F10"), tr("Free camera") }, { QStringLiteral("F11"), tr("Windowed") } };
    for (const auto& key : keys)
    {
        auto* pair = new QHBoxLayout();
        pair->setSpacing(6);
        pair->addWidget(PlaytestKey(key.first, m_shortcuts));
        pair->addWidget(Hearth::MakeLabel(key.second, QStringLiteral("dim"), m_shortcuts));
        shortcuts->addLayout(pair);
    }

    qApp->installEventFilter(this);
    m_timer = new QTimer(this);
    m_timer->setInterval(250);
    connect(m_timer, &QTimer::timeout, this, &HearthPlaytestOverlay::Tick);
    m_timer->start();
    m_clock.start();
    AZ::TickBus::Handler::BusConnect();
    hide();
}

HearthPlaytestOverlay::~HearthPlaytestOverlay()
{
    AZ::TickBus::Handler::BusDisconnect();
}

void HearthPlaytestOverlay::OnTick(float deltaTime, [[maybe_unused]] AZ::ScriptTimePoint time)
{
    ++m_frames;
    m_frameTimeSum += deltaTime;
    m_frameSeconds = deltaTime;
}

void HearthPlaytestOverlay::SetSeed(const QString& seed)
{
    m_seed->setText(seed.isEmpty() ? QString() : tr("seed %1").arg(seed));
}

QString HearthPlaytestOverlay::BridgeFolder() const
{
    const auto project = AZ::Utils::GetProjectPath();
    return QDir(QString::fromUtf8(project.c_str())).filePath(QStringLiteral("user/ClaudeBridge"));
}

void HearthPlaytestOverlay::Tick()
{
    const bool inGame = GetIEditor() && GetIEditor()->IsInGameMode();
    if (inGame != m_inGame)
    {
        m_inGame = inGame;
        m_paused = false;
        if (inGame)
        {
            const QString level = GetIEditor()->GetLevelName();
            m_title->setText(tr("Testing · %1").arg(level.isEmpty() ? tr("the level") : level));
            Place();
            show();
            raise();
        }
        else
        {
            hide();
        }
    }
    if (!m_inGame)
    {
        return;
    }
    Place();

    if (m_clock.elapsed() >= 1000)
    {
        const float average = m_frames > 0 ? m_frameTimeSum / m_frames : 0.0f;
        m_fps->setText(tr("%1 fps").arg(m_frames * 1000 / qMax<qint64>(1, m_clock.elapsed())));
        m_frameMs->setText(QStringLiteral("%1 ms").arg(average * 1000.0f, 0, 'f', 1));
        m_frames = 0;
        m_frameTimeSum = 0.0f;
        m_clock.restart();
        if (FILE* statm = fopen("/proc/self/statm", "r"))
        {
            long pages = 0;
            long resident = 0;
            if (fscanf(statm, "%ld %ld", &pages, &resident) == 2)
            {
                m_memory->setText(tr("game %1 MB").arg(resident * 4096 / (1024 * 1024)));
            }
            fclose(statm);
        }
    }

    // Claude is driving: the bridge did something in the last few seconds and driving is allowed.
    QScopedPointer<QSettings> settings(Hearth::NewSettings());
    const bool allowed = settings->value(QStringLiteral("claude/drive"), true).toBool();
    QFile activity(QDir(BridgeFolder()).filePath(QStringLiteral("activity.jsonl")));
    const QFileInfo info(activity);
    const bool recent = info.exists() && info.lastModified().msecsTo(QDateTime::currentDateTime()) < 5000;
    if (allowed && recent && activity.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        if (activity.size() > 4096)
        {
            activity.seek(activity.size() - 4096);
        }
        const QList<QByteArray> lines = activity.readAll().split('\n');
        QJsonObject last;
        for (int i = lines.size() - 1; i >= 0 && last.isEmpty(); --i)
        {
            last = QJsonDocument::fromJson(lines[i]).object();
        }
        const QString what = last.value(QStringLiteral("what")).toString();
        m_claudeText->setText(what);
        for (int i = 0; i < 7; ++i)
        {
            const bool on = what.contains(QStringLiteral("'%1'").arg(QLatin1String(PlaytestKeyNames[i])), Qt::CaseInsensitive) ||
                            what.contains(QStringLiteral("\"%1\"").arg(QLatin1String(PlaytestKeyNames[i])), Qt::CaseInsensitive);
            m_keys[i]->setStyleSheet(QLatin1String(on ? PlaytestKeyOnStyle : PlaytestKeyStyle));
        }
        m_claude->show();
    }
    else
    {
        m_claude->hide();
    }
}

void HearthPlaytestOverlay::Place()
{
    const QRect area = m_mainWindow->geometry();
    setGeometry(area);
    m_top->adjustSize();
    m_top->move((width() - m_top->width()) / 2, 16);
    m_claude->adjustSize();
    m_claude->move(width() - m_claude->width() - 18, 76);
    m_stats->adjustSize();
    m_stats->move(18, height() - m_stats->height() - 18);
    m_shortcuts->adjustSize();
    m_shortcuts->move(width() - m_shortcuts->width() - 18, height() - m_shortcuts->height() - 18);
    // Only the pills take the mouse; the game gets everything else.
    QRegion region(m_top->geometry());
    region += m_stats->geometry();
    region += m_shortcuts->geometry();
    if (m_claude->isVisible())
    {
        region += m_claude->geometry();
    }
    setMask(region);
}

bool HearthPlaytestOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (m_inGame && event->type() == QEvent::KeyPress)
    {
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key())
        {
        case Qt::Key_F8:
            Pause();
            return true;
        case Qt::Key_F9:
            Screenshot();
            return true;
        case Qt::Key_F10:
            FreeCamera();
            return true;
        case Qt::Key_F11:
            if (m_toggleWindowed)
            {
                m_toggleWindowed();
            }
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void HearthPlaytestOverlay::Pause()
{
    m_paused = !m_paused;
    if (auto* console = AZ::Interface<AZ::IConsole>::Get())
    {
        console->PerformCommand(m_paused ? "t_simulationTickScale 0" : "t_simulationTickScale 1");
    }
    m_title->setText(m_paused ? tr("Paused · %1").arg(GetIEditor()->GetLevelName()) : tr("Testing · %1").arg(GetIEditor()->GetLevelName()));
}

void HearthPlaytestOverlay::Screenshot()
{
    // Saved with Claude's screenshots, so the Hub can use the newest one as the game's cover.
    QDir shots(QDir(BridgeFolder()).filePath(QStringLiteral("shots")));
    shots.mkpath(QStringLiteral("."));
    const QString path = shots.filePath(QStringLiteral("playtest_%1.png").arg(QDateTime::currentMSecsSinceEpoch()));
    hide();
    QTimer::singleShot(120, this, [this, path]()
    {
        if (QScreen* screen = m_mainWindow->screen())
        {
            const QRect area = m_mainWindow->frameGeometry();
            screen->grabWindow(0, area.x(), area.y(), area.width(), area.height()).save(path);
        }
        if (m_inGame)
        {
            show();
        }
    });
}

void HearthPlaytestOverlay::FreeCamera()
{
    if (auto* actions = AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get())
    {
        actions->TriggerAction("o3de.action.game.movePlayerAndCameraSeparately");
    }
}
