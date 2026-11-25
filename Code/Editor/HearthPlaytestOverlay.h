/*
 * Hearth Engine playtest overlay (approved mockup "Playtest", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * While the game runs in the editor: "Testing · <level>", seed, fps, Back to editor (Esc); "Claude is driving" with
 * Take back control; frame time, memory; F8 Pause, F9 Screenshot, F10 Free camera, F11 Windowed.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <AzCore/Component/TickBus.h>

#include <QElapsedTimer>
#include <QPointer>
#include <QWidget>

#include <functional>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QMainWindow)
QT_FORWARD_DECLARE_CLASS(QTimer)

class HearthPlaytestOverlay
    : public QWidget
    , private AZ::TickBus::Handler
{
    Q_OBJECT

public:
    //! toggleWindowed: called by F11 (the shell knows how the game was started).
    HearthPlaytestOverlay(QMainWindow* mainWindow, std::function<void()> toggleWindowed);
    ~HearthPlaytestOverlay() override;

    void SetSeed(const QString& seed);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void OnTick(float deltaTime, AZ::ScriptTimePoint time) override;
    void Tick();
    void Place();
    void Pause();
    void Screenshot();
    void FreeCamera();
    QString BridgeFolder() const;

    QMainWindow* m_mainWindow = nullptr;
    std::function<void()> m_toggleWindowed;
    QWidget* m_top = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_seed = nullptr;
    QLabel* m_fps = nullptr;
    QWidget* m_claude = nullptr;
    QLabel* m_claudeText = nullptr;
    QLabel* m_keys[7] = {};
    QWidget* m_stats = nullptr;
    QLabel* m_frameMs = nullptr;
    QLabel* m_memory = nullptr;
    QWidget* m_shortcuts = nullptr;
    QTimer* m_timer = nullptr;
    QElapsedTimer m_clock;
    int m_frames = 0;
    float m_frameSeconds = 0.0f;
    float m_frameTimeSum = 0.0f;
    bool m_paused = false;
    bool m_inGame = false;
};
