/*
 * Hearth Engine Material editor (approved mockup "Material editor", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * Edits a .material live in the level (the level's own material instance changes as you drag), with a preview on a
 * sphere, wall or cube under Level 0 lights, studio light or a dark zone, its textures, surface values, wear, and
 * what it costs on this laptop. Revert puts back what's saved; Save writes the .material.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <AzCore/Math/Color.h>
#include <QJsonObject>
#include <QPixmap>
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QSlider)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)
QT_FORWARD_DECLARE_CLASS(QTimer)

namespace AzQtComponents
{
    namespace Hearth
    {
        class Toggle;
    }
}

class HearthMaterialPreviewLabel;

class HearthMaterialPane : public QWidget
{
    Q_OBJECT

public:
    explicit HearthMaterialPane(QWidget* parent = nullptr);
    ~HearthMaterialPane() override;
    static void RegisterViewClass();

    //! Opens a .material (full path).
    void OpenMaterial(const QString& path);

private:
    struct Wear
    {
        double yellowing = 0.0;
        double damp = 0.0;
        bool fadeAtStains = false;
    };

    void ShowPicker();
    void BuildEditor();
    void FillTextures();
    void FillCost();
    void ApplyLive();
    void SetDirty(bool dirty);
    void Save();
    void Revert();
    void RequestPreview();
    QString TextureSlotTitle(const QString& key) const;
    void PickTexture(const QString& key);
    Wear ReadWear() const;
    void WriteWear(const Wear& wear) const;

    QString m_path;
    QJsonObject m_file;          //!< the .material as saved
    QJsonObject m_values;        //!< propertyValues being edited (clean values, before wear)
    Wear m_wear;
    Wear m_savedWear;
    bool m_dirty = false;

    // Preview
    int m_shape = 0;       //!< 0 sphere, 1 wall, 2 cube
    int m_lighting = 0;    //!< 0 Level 0 lights, 1 studio, 2 dark zone
    float m_yaw = 0.6f;
    float m_pitch = -0.25f;
    float m_zoom = 1.0f;
    bool m_previewBusy = false;
    bool m_previewAgain = false;

    QVBoxLayout* m_root = nullptr;
    QWidget* m_body = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_subtitle = nullptr;
    QPushButton* m_revert = nullptr;
    QPushButton* m_save = nullptr;
    HearthMaterialPreviewLabel* m_preview = nullptr;
    QVBoxLayout* m_texturesLayout = nullptr;
    QVBoxLayout* m_costLayout = nullptr;
    QWidget* m_costBody = nullptr;
    QPushButton* m_tintSwatch = nullptr;
    QLabel* m_tintHex = nullptr;
    QSlider* m_tintFactor = nullptr;
    QSlider* m_metallic = nullptr;
    QSlider* m_normalStrength = nullptr;
    QSlider* m_textureScale = nullptr;
    QSlider* m_yellowing = nullptr;
    QSlider* m_damp = nullptr;
    AzQtComponents::Hearth::Toggle* m_fade = nullptr;
    QTimer* m_liveTimer = nullptr;
};
