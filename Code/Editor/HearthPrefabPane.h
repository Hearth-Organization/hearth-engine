/*
 * Hearth Engine Prefab editing (approved mockup "Prefab editing", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * Opens when you go into a prefab: where you are (Level 0 / L0_LightFlicker), how many times it's used, what's in it,
 * its light and flicker, what you changed here (before → after), Flicker once, Show around it, Back to the level and
 * Save to all. Built on the editor's prefab focus mode.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <AzCore/JSON/document.h>
#include <AzToolsFramework/Prefab/PrefabFocusNotificationBus.h>
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QListWidget)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QTimer)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

class HearthPrefabPane
    : public QWidget
    , public AzToolsFramework::Prefab::PrefabFocusNotificationBus::Handler
{
    Q_OBJECT

public:
    explicit HearthPrefabPane(QWidget* parent = nullptr);
    ~HearthPrefabPane() override;
    static void RegisterViewClass();

    // PrefabFocusNotificationBus
    void OnPrefabFocusChanged(AZ::EntityId previousContainerEntityId, AZ::EntityId newContainerEntityId) override;

protected:
    void showEvent(QShowEvent* event) override;

private:
    void Refresh();
    void FillContents();
    void FillDetails();
    void FillChanges();
    void FlickerOnce();
    void SetShowAround(bool show);
    bool InPrefab() const;

    rapidjson::Document m_saved; //!< the prefab as it was when you went in (or last saved)
    AZ::u64 m_savedTemplate = 0;
    bool m_showAround = false;
    int m_uses = 0;

    QLabel* m_path = nullptr;
    QLabel* m_subtitle = nullptr;
    QPushButton* m_back = nullptr;
    QPushButton* m_save = nullptr;
    QPushButton* m_around = nullptr;
    QListWidget* m_contents = nullptr;
    QVBoxLayout* m_detailsLayout = nullptr;
    QVBoxLayout* m_changesLayout = nullptr;
    QLabel* m_footer = nullptr;
    QTimer* m_changesTimer = nullptr;
};
