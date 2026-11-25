/*
 * Hearth Engine Physics settings (approved mockup "Physics settings", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * What collides with what (layers World, Player, Entities, Items, Triggers, and a collision group of the same name for
 * each), gravity, physics steps per second, the player's body (from the level's player: its character controller and
 * first person controller) and Show colliders in the viewport. Changes apply right away and are saved in the
 * project's PhysX settings (Registry/physx*.setreg).
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QCheckBox)
QT_FORWARD_DECLARE_CLASS(QDoubleSpinBox)
QT_FORWARD_DECLARE_CLASS(QGridLayout)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QSpinBox)

class HearthPhysicsPane : public QWidget
{
    Q_OBJECT

public:
    explicit HearthPhysicsPane(QWidget* parent = nullptr);
    static void RegisterViewClass();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void Load();
    void SaveCollisions();
    void SaveWorld();
    void LoadPlayer();
    void SavePlayer();

    static constexpr int LayerCount = 5;
    QCheckBox* m_matrix[LayerCount][LayerCount] = {};
    QDoubleSpinBox* m_gravity = nullptr;
    QSpinBox* m_steps = nullptr;
    QDoubleSpinBox* m_height = nullptr;
    QDoubleSpinBox* m_width = nullptr;
    QDoubleSpinBox* m_stepUp = nullptr;
    QDoubleSpinBox* m_slope = nullptr;
    QDoubleSpinBox* m_walk = nullptr;
    QDoubleSpinBox* m_sprint = nullptr;
    QDoubleSpinBox* m_jump = nullptr;
    QLabel* m_playerNote = nullptr;
    QPushButton* m_showColliders = nullptr;
    bool m_loading = false;
};
