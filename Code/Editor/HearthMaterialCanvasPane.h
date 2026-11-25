/*
 * Hearth Engine "Material canvas" (approved mockup "Material canvas", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * A new kind of surface made from blocks (Texture, Noise, Blend, Height and distance, Flicker with nearby light)
 * that feed the surface output (base color, roughness, normal). "Save as a material type" writes a real Atom
 * material type: the Base PBR inputs plus one setting per block, and shader code made from the blocks on top of
 * Base PBR's surface. The window then shows whether it compiled (Vulkan) and how heavy it is for this laptop, and
 * makes a material of the new type to put on things. The blocks are kept in <project>/Materials/Canvas/<name>.json.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QComboBox)
QT_FORWARD_DECLARE_CLASS(QHBoxLayout)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QScrollArea)

class HearthMaterialCanvasPane : public QWidget
{
    Q_OBJECT

public:
    explicit HearthMaterialCanvasPane(QWidget* parent = nullptr);
    static void RegisterViewClass();

private:
    QString ProjectFolder() const;
    QString Slug() const;
    void ListCanvases();
    void Load(const QString& slug);
    void SaveCanvas();
    void NewCanvas();
    void Rebuild();
    QWidget* BuildBlock(int index, QWidget* parent);
    void AddBlock(const QString& kind);
    void SaveAsMaterialType();
    QString ShaderCode() const;
    QJsonArray BlockProperties() const;
    QJsonObject BasePbrLayout(QString* problem) const;
    void CheckCompile();
    QString Weight() const;

    QJsonObject m_canvas; //!< { title, blocks: [ { kind, label, params: {} } ] }
    QString m_slug;
    int m_compileTries = 0;

    QLabel* m_subtitle = nullptr;
    QComboBox* m_pick = nullptr;
    QLabel* m_compile = nullptr;
    QScrollArea* m_scroll = nullptr;
    QLabel* m_status = nullptr;
};
