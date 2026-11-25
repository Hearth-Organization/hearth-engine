/*
 * Hearth Engine Game UI editor (approved mockup "Game UI editor", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * Lays out the game's HUD: layers, things to add (text, image, bar, button, list, meter), a canvas at the size of the
 * device you pick (1920 × 1080, Steam Deck, ROG Ally, vertical) with 8 px snapping, and for the picked element where
 * it sticks when the screen changes size, its position from there, size, fill colour, the game value it shows,
 * pulse when low and whether it shows on gamepad. Saved as <project>/UI/HUD.uicanvas (Hearth's JSON layout,
 * "hearth_ui": 1), which the game's HUD draws.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QCheckBox)
QT_FORWARD_DECLARE_CLASS(QComboBox)
QT_FORWARD_DECLARE_CLASS(QDoubleSpinBox)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QSpinBox)
QT_FORWARD_DECLARE_CLASS(QTreeWidget)
QT_FORWARD_DECLARE_CLASS(QTreeWidgetItem)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

namespace AzQtComponents
{
    namespace Hearth
    {
        class Toggle;
    }
}

class HearthUiCanvasView;

class HearthUiEditor : public QWidget
{
    Q_OBJECT

public:
    explicit HearthUiEditor(QWidget* parent = nullptr);
    static void RegisterViewClass();

    //! Every element, children after their parent (the canvas draws in this order); "parent" names the parent's id.
    QVector<QJsonObject>& Elements() { return m_elements; }
    QJsonObject* Find(const QString& id);
    QRect ElementRect(const QJsonObject& element) const; //!< on the canvas
    QString Selected() const { return m_selected; }
    void Select(const QString& id);
    void Changed(bool structure = false);
    QSize CanvasSize() const;
    void SetCanvasTopLeft(QJsonObject& element, const QPoint& topLeft); //!< sets x/y from a canvas position

private:
    void Load();
    void Save();
    void FillLayers();
    void FillProperties();
    void Add(const QString& type);
    void MakeDefault();
    QRect ParentRect(const QJsonObject& element) const;

    QString m_path;
    QJsonObject m_canvas;
    QVector<QJsonObject> m_elements;
    QString m_selected;
    bool m_dirty = false;
    bool m_filling = false;
    int m_device = 0;

    QLabel* m_subtitle = nullptr;
    QList<QPushButton*> m_devices;
    QPushButton* m_save = nullptr;
    QTreeWidget* m_layers = nullptr;
    HearthUiCanvasView* m_view = nullptr;
    QLabel* m_snapLabel = nullptr;
    QLabel* m_selectedLabel = nullptr;
    QList<QPushButton*> m_anchors;
    QSpinBox* m_x = nullptr;
    QSpinBox* m_y = nullptr;
    QSpinBox* m_w = nullptr;
    QSpinBox* m_h = nullptr;
    QPushButton* m_fill = nullptr;
    QComboBox* m_bind = nullptr;
    QLineEdit* m_text = nullptr;
    AzQtComponents::Hearth::Toggle* m_pulse = nullptr;
    QSpinBox* m_pulseBelow = nullptr;
    AzQtComponents::Hearth::Toggle* m_gamepad = nullptr;
    QWidget* m_propertiesBody = nullptr;
};
