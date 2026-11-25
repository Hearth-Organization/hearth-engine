/*
 * Hearth Engine "Lighting and look" window (approved mockup "Lighting", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * One place for the level's look: exposure, colour temperature, fog, film grain, vignette, bloom, ambient occlusion,
 * shadows; four looks (Level 0, Blackout, Foundation site, Containment breach) plus saved ones; Before / after;
 * and the level's lights by state. The settings live on a "Hearth Look" entity in the level (post-process
 * components), so they save with the level and work in the game too.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <AzCore/Component/EntityId.h>

#include <QMap>
#include <QWidget>

#include <functional>
#include <memory>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QSlider)
QT_FORWARD_DECLARE_CLASS(QHBoxLayout)

class HearthAgentClient;
namespace AzQtComponents { namespace Hearth { class Toggle; } }

class HearthLookPane : public QWidget
{
    Q_OBJECT

public:
    explicit HearthLookPane(QWidget* parent = nullptr);
    ~HearthLookPane() override;
    static void RegisterViewClass();

private:
    void ApplyPreset(const QMap<QString, double>& values);
    void Apply(bool showBefore = false);
    AZ::EntityId FindOrCreateLookEntity();
    void UpdateLights();
    void SavePreset();
    void RebuildPresets();
    //! The window's own view (mockup "Lighting"): the main viewport captured with the look off (before) and on
    //! (after), shown side by side with a line to drag. "Live" refreshes the after side every couple of seconds.
    void CaptureBefore();
    void CaptureAfter();
    void Capture(const QString& path, std::function<void()> then);

    std::unique_ptr<HearthAgentClient> m_agent;
    QMap<QString, QSlider*> m_sliders;
    QMap<QString, QLabel*> m_values;
    AzQtComponents::Hearth::Toggle* m_ao = nullptr;
    AzQtComponents::Hearth::Toggle* m_shadows = nullptr;
    QPushButton* m_beforeAfter = nullptr;
    QHBoxLayout* m_presets = nullptr;
    QLabel* m_working = nullptr;
    QLabel* m_flickering = nullptr;
    QLabel* m_dead = nullptr;
    QLabel* m_status = nullptr;
    class HearthLookPreview* m_preview = nullptr;
    QPushButton* m_live = nullptr;
    bool m_capturing = false;
};
