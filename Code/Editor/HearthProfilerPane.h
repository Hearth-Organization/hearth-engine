/*
 * Hearth Engine Profiler (approved mockup "Profiler", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * Records the engine's own timing regions (the Profiler part), shows the last frames as bars, where the time went
 * in the picked frame, what to fix, and lets Claude explain a capture.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <AzCore/Component/TickBus.h>

#include <QString>
#include <QVector>
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QTimer)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

class HearthFrameBars;
class HearthFlameView;

class HearthProfilerPane
    : public QWidget
    , private AZ::TickBus::Handler
{
    Q_OBJECT

public:
    explicit HearthProfilerPane(QWidget* parent = nullptr);
    ~HearthProfilerPane() override;
    static void RegisterViewClass();

    struct Region
    {
        QString name;
        int depth = 0;
        double startMs = 0.0;
        double endMs = 0.0;
    };
    struct Frame
    {
        double startMs = 0.0;
        double ms = 0.0;
        QVector<Region> regions; //!< main thread only
    };

private:
    void OnTick(float deltaTime, AZ::ScriptTimePoint time) override;
    void ToggleRecording();
    bool LoadCapture(const QString& path);
    void PickFrame(int index);
    void UpdateStats();
    void UpdateTips();

    QVector<Frame> m_frames;
    QVector<float> m_liveFrames;
    bool m_recording = false;
    QString m_capturePath;
    int m_picked = -1;

    QLabel* m_stat[5] = {};
    QLabel* m_recordingChip = nullptr;
    QPushButton* m_record = nullptr;
    HearthFrameBars* m_bars = nullptr;
    HearthFlameView* m_flame = nullptr;
    QLabel* m_frameTitle = nullptr;
    QLabel* m_framesLine = nullptr;  //!< "last 60 · frame 44 picked"
    QLabel* m_subtitle = nullptr;    //!< "Recording the game · Level0 · 2 players"
    void SaveCapture();
    QVBoxLayout* m_tips = nullptr;
    QTimer* m_timer = nullptr;
};
