/*
 * Hearth Engine "Animation" (approved mockup "Animation", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * For the picked entity (an Actor with a Simple Motion component): its clips with their lengths, a preview with
 * Mesh / Skeleton / Both, scrubbing ("1.12 / 2.40 s") and speed, which part of the body a clip plays on (Body, Legs),
 * its events (footstep sounds), and "What it does when" (which clip follows which, and when). Clip names, parts,
 * events and the rules are kept in <project>/Animation/<entity>.json for the game's monster code and for Claude.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <AzCore/Asset/AssetCommon.h>
#include <AzCore/Component/EntityId.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>

#include <QJsonObject>
#include <QWidget>

#include <functional>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QSlider)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)
QT_FORWARD_DECLARE_CLASS(QComboBox)

class HearthAnimPreview;
class HearthAnimScrubber;

class HearthAnimationPane
    : public QWidget
    , private AzToolsFramework::ToolsApplicationNotificationBus::Handler
{
    Q_OBJECT

public:
    explicit HearthAnimationPane(QWidget* parent = nullptr);
    ~HearthAnimationPane() override;
    static void RegisterViewClass();

private:
    void AfterEntitySelectionChanged(const AzToolsFramework::EntityIdList& newlySelected, const AzToolsFramework::EntityIdList& newlyDeselected) override;

    struct Clip
    {
        AZ::Data::AssetId id;
        QString path;
        QString name;
        float seconds = 0.0f;
    };

    void PickEntity(AZ::EntityId entity);
    void FindClips();
    void ShowClips();
    void PlayClip(int index);
    void ShowRules();
    void SetView(int mode); //!< 0 Mesh, 1 Skeleton, 2 Both
    void Tick();
    void Capture();
    void SaveSidecar();
    QString SidecarPath() const;
    void ChangeClip(const QString& path, const std::function<void(QJsonObject&)>& change); //!< and saves

    AZ::EntityId m_entity;
    QString m_entityName;
    QVector<Clip> m_clips;
    int m_clip = -1;
    QJsonObject m_sidecar;
    float m_speed = 1.0f;
    bool m_playing = true;
    bool m_capturing = false;

    QLabel* m_subtitle = nullptr;
    QVBoxLayout* m_clipList = nullptr;
    HearthAnimPreview* m_preview = nullptr;
    HearthAnimScrubber* m_scrubber = nullptr;
    QLabel* m_time = nullptr;
    QComboBox* m_speedBox = nullptr;
    QPushButton* m_view[3] = {};
    QPushButton* m_body = nullptr;
    QPushButton* m_legs = nullptr;
    QPushButton* m_events = nullptr;
    QVBoxLayout* m_rules = nullptr;
    QLabel* m_status = nullptr;
};
