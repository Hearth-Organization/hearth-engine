/*
 * Hearth Engine Material editor. Built on O3DE (Apache-2.0 OR MIT). See HearthMaterialPane.h.
 * The preview uses the engine's shared preview renderer (the one the Asset Browser thumbnails use) with its own
 * content: the picked shape, the material being edited and the picked lighting.
 */

#include "EditorDefs.h"

#include "HearthMaterialPane.h"
#include "HearthAssetBrowser.h"
#include "QtViewPaneManager.h"

#include <Atom/Feature/CoreLights/DirectionalLightFeatureProcessorInterface.h>
#include <Atom/Feature/CoreLights/PhotometricValue.h>
#include <Atom/Feature/ImageBasedLights/ImageBasedLightFeatureProcessorInterface.h>
#include <Atom/Feature/Mesh/MeshFeatureProcessorInterface.h>
#include <Atom/Feature/SkyBox/SkyBoxFeatureProcessorInterface.h>
#include <Atom/Feature/Utils/LightingPreset.h>
#include <Atom/RPI.Public/Image/StreamingImage.h>
#include <Atom/RPI.Public/Material/Material.h>
#include <Atom/RPI.Public/Model/Model.h>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/View.h>
#include <Atom/RPI.Reflect/Image/StreamingImageAsset.h>
#include <Atom/RPI.Reflect/Material/MaterialAsset.h>
#include <Atom/RPI.Reflect/Model/ModelAsset.h>
#include <Atom/RPI.Reflect/System/AnyAsset.h>
#include <AtomToolsFramework/PreviewRenderer/PreviewContent.h>
#include <AtomToolsFramework/PreviewRenderer/PreviewRendererCaptureRequest.h>
#include <AtomToolsFramework/PreviewRenderer/PreviewRendererInterface.h>
#include <AzCore/Asset/AssetManager.h>
#include <AzCore/Asset/AssetManagerBus.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/Entity.h>
#include <AzCore/Interface/Interface.h>
#include <AzCore/Math/Matrix3x4.h>
#include <AzCore/Math/Transform.h>
#include <AzCore/Serialization/Json/JsonSerialization.h>
#include <AzCore/Serialization/Json/JsonUtils.h>
#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>

#include <QColorDialog>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <functional>

//! The preview: drag to turn, scroll to zoom.
class HearthMaterialPreviewLabel : public QLabel
{
public:
    explicit HearthMaterialPreviewLabel(QWidget* parent)
        : QLabel(parent)
    {
        setAlignment(Qt::AlignCenter);
        setMinimumSize(360, 360);
        setCursor(Qt::OpenHandCursor);
    }

    std::function<void(float, float)> m_turned;
    std::function<void(float)> m_zoomed;
    std::function<void()> m_resized;

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        m_last = event->pos();
        setCursor(Qt::ClosedHandCursor);
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        setCursor(Qt::OpenHandCursor);
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!(event->buttons() & Qt::LeftButton))
        {
            return;
        }
        const QPoint delta = event->pos() - m_last;
        m_last = event->pos();
        if (m_turned)
        {
            m_turned(delta.x() * 0.01f, delta.y() * 0.01f);
        }
    }
    void wheelEvent(QWheelEvent* event) override
    {
        if (m_zoomed)
        {
            m_zoomed(event->angleDelta().y() > 0 ? 1.1f : 1.0f / 1.1f);
        }
    }
    void resizeEvent(QResizeEvent* event) override
    {
        QLabel::resizeEvent(event);
        if (m_resized)
        {
            m_resized();
        }
    }

private:
    QPoint m_last;
};

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    QString MaterialProject()
    {
        return QString::fromUtf8(AZ::Utils::GetProjectPath().c_str());
    }

    //! The texture slots, in the order they're shown.
    const QVector<QPair<QString, QString>>& MaterialSlots()
    {
        static const QVector<QPair<QString, QString>> materialSlots = {
            { QStringLiteral("baseColor.textureMap"), QObject::tr("Base color") },
            { QStringLiteral("normal.textureMap"), QObject::tr("Normal") },
            { QStringLiteral("roughness.textureMap"), QObject::tr("Roughness") },
            { QStringLiteral("metallic.textureMap"), QObject::tr("Metallic") },
            { QStringLiteral("occlusion.diffuseTextureMap"), QObject::tr("Ambient occlusion") },
            { QStringLiteral("emissive.textureMap"), QObject::tr("Glow") },
            { QStringLiteral("parallax.textureMap"), QObject::tr("Height") },
            { QStringLiteral("opacity.textureMap"), QObject::tr("Opacity") },
        };
        return materialSlots;
    }

    AZ::Color MaterialColor(const QJsonValue& value, const AZ::Color& fallback)
    {
        const QJsonArray rgb = value.toArray();
        if (rgb.size() < 3)
        {
            return fallback;
        }
        return AZ::Color(float(rgb[0].toDouble()), float(rgb[1].toDouble()), float(rgb[2].toDouble()), rgb.size() > 3 ? float(rgb[3].toDouble()) : 1.0f);
    }

    QJsonArray MaterialColorJson(const AZ::Color& c)
    {
        return QJsonArray{ double(c.GetR()), double(c.GetG()), double(c.GetB()), double(c.GetA()) };
    }

    QColor MaterialQColor(const AZ::Color& c)
    {
        return QColor::fromRgbF(qBound(0.0, double(c.GetR()), 1.0), qBound(0.0, double(c.GetG()), 1.0), qBound(0.0, double(c.GetB()), 1.0));
    }

    AZ::Color MaterialLerp(const AZ::Color& a, const AZ::Color& b, float t)
    {
        return AZ::Color(a.GetR() + (b.GetR() - a.GetR()) * t, a.GetG() + (b.GetG() - a.GetG()) * t, a.GetB() + (b.GetB() - a.GetB()) * t, a.GetA());
    }

    //! Absolute path of a texture named in a .material (relative to the material, or to the project).
    QString MaterialTexturePath(const QString& texture, const QString& materialPath)
    {
        if (texture.isEmpty())
        {
            return QString();
        }
        for (const QString& path : { QFileInfo(materialPath).dir().filePath(texture), QDir(MaterialProject()).filePath(texture),
                                     QDir(MaterialProject()).filePath(QStringLiteral("Assets/") + texture) })
        {
            if (QFileInfo::exists(path))
            {
                return QDir::cleanPath(path);
            }
        }
        return QString();
    }

    AZ::Data::AssetId MaterialProductId(const QString& absolute, const char* productSuffix, const AZ::Data::AssetType& type)
    {
        const QString product = QDir(MaterialProject()).relativeFilePath(absolute).toLower() + QString::fromLatin1(productSuffix);
        AZ::Data::AssetId id;
        AZ::Data::AssetCatalogRequestBus::BroadcastResult(id, &AZ::Data::AssetCatalogRequests::GetAssetIdByPath, product.toUtf8().constData(), type, false);
        return id;
    }

    //! The preview content: one mesh with the material, lights and the camera.
    class HearthMaterialPreviewContent final : public AtomToolsFramework::PreviewContent
    {
    public:
        AZ_CLASS_ALLOCATOR(HearthMaterialPreviewContent, AZ::SystemAllocator);

        HearthMaterialPreviewContent(AZ::RPI::ScenePtr scene, AZ::RPI::ViewPtr view, AZ::Data::Asset<AZ::RPI::ModelAsset> model,
                                     AZ::Data::Instance<AZ::RPI::Material> material, AZ::Data::Asset<AZ::RPI::AnyAsset> lighting, int lightingMode,
                                     int shape, float yaw, float pitch, float zoom)
            : m_scene(scene)
            , m_view(view)
            , m_model(model)
            , m_material(material)
            , m_lighting(lighting)
            , m_lightingMode(lightingMode)
            , m_shape(shape)
            , m_yaw(yaw)
            , m_pitch(pitch)
            , m_zoom(zoom)
        {
        }

        ~HearthMaterialPreviewContent() override
        {
            if (m_scene)
            {
                if (auto* meshes = m_scene->GetFeatureProcessor<AZ::Render::MeshFeatureProcessorInterface>())
                {
                    meshes->ReleaseMesh(m_mesh);
                }
            }
        }

        void Load() override
        {
            m_model.QueueLoad();
            if (m_lighting.GetId().IsValid())
            {
                m_lighting.QueueLoad();
            }
        }

        bool IsReady() const override
        {
            return m_model.IsReady() && (!m_lighting.GetId().IsValid() || m_lighting.IsReady());
        }

        bool IsError() const override
        {
            return m_model.IsError() || !m_model.GetId().IsValid();
        }

        void ReportErrors() override
        {
            AZ_Warning("Hearth", m_model.IsReady(), "Material preview: the preview shape didn't load (%s)", m_model.GetHint().c_str());
        }

        void Update() override
        {
            auto* meshes = m_scene->GetFeatureProcessor<AZ::Render::MeshFeatureProcessorInterface>();
            if (meshes && !m_mesh.IsValid())
            {
                m_mesh = meshes->AcquireMesh(AZ::Render::MeshHandleDescriptor(m_model, m_material));
            }
            const AZ::Vector3 scale = m_shape == 1 ? AZ::Vector3(1.6f, 0.12f, 1.6f) : AZ::Vector3::CreateOne();
            if (meshes)
            {
                meshes->SetTransform(m_mesh, AZ::Transform::CreateIdentity(), scale);
            }
            UpdateLighting();

            // Camera around the shape: yaw and pitch from dragging, distance from zooming.
            AZ::Vector3 center = AZ::Vector3::CreateZero();
            float radius = 0.6f;
            m_model->GetAabb().GetAsSphere(center, radius);
            radius *= m_shape == 1 ? 1.6f : 1.0f;
            const float fov = AZ::Constants::QuarterPi;
            const float distance = (radius / std::sin(fov * 0.5f)) * 1.08f / m_zoom;
            const AZ::Quaternion rotation = AZ::Quaternion::CreateRotationZ(m_yaw) * AZ::Quaternion::CreateRotationX(m_pitch);
            const AZ::Vector3 position = center + rotation.TransformVector(-AZ::Vector3::CreateAxisY() * distance);
            m_view->SetCameraTransform(AZ::Matrix3x4::CreateFromTransform(AZ::Transform::CreateLookAt(position, center)));
        }

        bool IsReadyToRender() override
        {
            auto* meshes = m_scene->GetFeatureProcessor<AZ::Render::MeshFeatureProcessorInterface>();
            return meshes && meshes->GetModel(m_mesh) && ++m_frames > 2;
        }

    private:
        void UpdateLighting()
        {
            auto* ibl = m_scene->GetFeatureProcessor<AZ::Render::ImageBasedLightFeatureProcessorInterface>();
            auto* sky = m_scene->GetFeatureProcessor<AZ::Render::SkyBoxFeatureProcessorInterface>();
            auto* lights = m_scene->GetFeatureProcessor<AZ::Render::DirectionalLightFeatureProcessorInterface>();
            const AZ::Render::LightingPreset* preset = m_lighting.IsReady() ? m_lighting->GetDataAs<AZ::Render::LightingPreset>() : nullptr;
            if (ibl && preset)
            {
                ibl->SetDiffuseImage(preset->m_iblDiffuseImageAsset);
                ibl->SetSpecularImage(preset->m_iblSpecularImageAsset);
                // Level 0: dim fluorescent room. Studio: the preset as it is. Dark zone: almost nothing.
                ibl->SetExposure(m_lightingMode == 0 ? preset->m_iblExposure - 3.0f : (m_lightingMode == 1 ? preset->m_iblExposure : preset->m_iblExposure - 7.0f));
            }
            if (sky)
            {
                sky->Enable(false);
            }
            if (!lights)
            {
                return;
            }
            // The preview scene is shared; put back only the lights this preview made last time.
            static AZStd::vector<AZ::Render::DirectionalLightFeatureProcessorInterface::LightHandle> previous;
            for (auto& handle : previous)
            {
                lights->ReleaseLight(handle);
            }
            previous.clear();
            auto handle = lights->AcquireLight();
            AZ::Color colour = AZ::Color(1.0f, 0.96f, 0.78f, 1.0f) * 5.0f; // warm overhead tubes
            AZ::Vector3 direction(0.15f, 0.25f, -1.0f);
            if (m_lightingMode == 1)
            {
                colour = AZ::Color::CreateOne() * 3.0f;
                direction = AZ::Vector3(1.0f, 1.0f, -1.0f);
            }
            else if (m_lightingMode == 2)
            {
                colour = AZ::Color(0.55f, 0.62f, 0.8f, 1.0f) * 0.25f;
                direction = AZ::Vector3(-0.4f, 0.3f, -1.0f);
            }
            lights->SetRgbIntensity(handle, AZ::Render::PhotometricColor<AZ::Render::PhotometricUnit::Lux>(colour));
            lights->SetDirection(handle, direction.GetNormalized());
            lights->SetShadowEnabled(handle, false);
            previous.push_back(handle);
        }

        AZ::RPI::ScenePtr m_scene;
        AZ::RPI::ViewPtr m_view;
        AZ::Data::Asset<AZ::RPI::ModelAsset> m_model;
        AZ::Data::Instance<AZ::RPI::Material> m_material;
        AZ::Data::Asset<AZ::RPI::AnyAsset> m_lighting;
        AZ::Render::MeshFeatureProcessorInterface::MeshHandle m_mesh;
        int m_lightingMode = 0;
        int m_shape = 0;
        float m_yaw = 0.0f;
        float m_pitch = 0.0f;
        float m_zoom = 1.0f;
        int m_frames = 0;
    };

    QWidget* MaterialSegmented(QWidget* parent, const QStringList& labels, int current, std::function<void(int)> changed)
    {
        auto* holder = new QWidget(parent);
        holder->setObjectName(QStringLiteral("MaterialSegmented"));
        holder->setAttribute(Qt::WA_StyledBackground);
        holder->setStyleSheet(QStringLiteral("QWidget#MaterialSegmented { background-color: rgba(13, 12, 11, 190); border-radius: 13px; }"
                                             "QPushButton { border: none; border-radius: 11px; padding: 3px 12px; color: #ECE6DE; background: transparent; }"
                                             "QPushButton:checked { background-color: #ECE6DE; color: #1A0F08; }"));
        auto* layout = new QHBoxLayout(holder);
        layout->setContentsMargins(2, 2, 2, 2);
        layout->setSpacing(2);
        auto buttons = std::make_shared<QList<QPushButton*>>();
        for (int i = 0; i < labels.size(); ++i)
        {
            auto* button = new QPushButton(labels[i], holder);
            button->setCheckable(true);
            button->setChecked(i == current);
            layout->addWidget(button);
            buttons->append(button);
            QObject::connect(button, &QPushButton::clicked, holder, [buttons, i, changed]()
            {
                for (int j = 0; j < buttons->size(); ++j)
                {
                    (*buttons)[j]->setChecked(j == i);
                }
                changed(i);
            });
        }
        return holder;
    }

    //! A slider row: name, slider, value text.
    QSlider* MaterialSlider(QWidget* parent, QVBoxLayout* into, const QString& name, int maximum, std::function<QString(int)> text)
    {
        auto* row = new QHBoxLayout();
        row->setSpacing(10);
        QLabel* label = Hearth::MakeLabel(name, QStringLiteral("body"), parent);
        label->setFixedWidth(130);
        row->addWidget(label);
        auto* slider = new QSlider(Qt::Horizontal, parent);
        slider->setRange(0, maximum);
        row->addWidget(slider, 1);
        QLabel* value = Hearth::MakeLabel(QString(), QStringLiteral("mono"), parent);
        value->setFixedWidth(58);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(value);
        QObject::connect(slider, &QSlider::valueChanged, value, [value, text](int v) { value->setText(text(v)); });
        into->addLayout(row);
        return slider;
    }

    QFrame* MaterialSection(QWidget* parent, const QString& title, QVBoxLayout** layout)
    {
        QFrame* card = Hearth::MakeCard(parent, layout);
        (*layout)->addWidget(Hearth::MakeLabel(title, QStringLiteral("title"), card));
        return card;
    }

    QString MaterialWearFile()
    {
        return QDir(MaterialProject()).filePath(QStringLiteral("Assets/Materials/hearth-wear.json"));
    }
} // namespace

void HearthMaterialPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(100, 70, 1360, 860);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthMaterialPane>("Material editor", "Tools", options);
}

HearthMaterialPane::HearthMaterialPane(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthMaterial"));
    setStyleSheet(QStringLiteral("QWidget#HearthMaterial { background-color: #0D0C0B; }"));
    m_root = new QVBoxLayout(this);
    m_root->setContentsMargins(10, 10, 10, 10);
    m_root->setSpacing(10);
    m_liveTimer = new QTimer(this);
    m_liveTimer->setSingleShot(true);
    m_liveTimer->setInterval(40);
    connect(m_liveTimer, &QTimer::timeout, this, &HearthMaterialPane::ApplyLive);
    ShowPicker();
}

HearthMaterialPane::~HearthMaterialPane() = default;

void HearthMaterialPane::ShowPicker()
{
    if (m_body)
    {
        m_body->deleteLater();
    }
    m_body = new QWidget(this);
    auto* layout = new QVBoxLayout(m_body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(Hearth::MakeHeader(m_body, tr("Material editor"), tr("Pick a material to change it live in the level")));
    QVBoxLayout* listLayout = nullptr;
    QFrame* card = Hearth::MakeCard(m_body, &listLayout);
    QDirIterator it(QDir(MaterialProject()).filePath(QStringLiteral("Assets")), { QStringLiteral("*.material") }, QDir::Files, QDirIterator::Subdirectories);
    QStringList found;
    while (it.hasNext())
    {
        found << it.next();
    }
    found.sort(Qt::CaseInsensitive);
    for (const QString& path : found)
    {
        auto* row = new QPushButton(QFileInfo(path).completeBaseName() + QStringLiteral("   ·   ") + QDir(MaterialProject()).relativeFilePath(QFileInfo(path).path()), card);
        row->setStyleSheet(QStringLiteral("QPushButton { text-align: left; background-color: rgba(255, 255, 255, 8); border: none; border-radius: 8px; padding: 8px 12px; color: #ECE6DE; }"
                                          "QPushButton:hover { background-color: rgba(232, 116, 59, 31); }"));
        connect(row, &QPushButton::clicked, this, [this, path]() { OpenMaterial(path); });
        listLayout->addWidget(row);
    }
    if (found.isEmpty())
    {
        listLayout->addWidget(Hearth::MakeLabel(tr("This project has no materials yet. Make one from Browse assets › New asset."), QStringLiteral("dim"), card));
    }
    listLayout->addStretch(1);
    auto* scroll = new QScrollArea(m_body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(card);
    layout->addWidget(scroll, 1);
    m_root->addWidget(m_body, 1);
}

HearthMaterialPane::Wear HearthMaterialPane::ReadWear() const
{
    Wear wear;
    QFile file(MaterialWearFile());
    if (file.open(QIODevice::ReadOnly))
    {
        const QJsonObject entry = QJsonDocument::fromJson(file.readAll()).object().value(QDir(MaterialProject()).relativeFilePath(m_path)).toObject();
        wear.yellowing = entry.value(QStringLiteral("yellowing")).toDouble();
        wear.damp = entry.value(QStringLiteral("damp")).toDouble();
        wear.fadeAtStains = entry.value(QStringLiteral("fadeAtStains")).toBool();
    }
    return wear;
}

void HearthMaterialPane::WriteWear(const Wear& wear) const
{
    // Wear is kept apart from the .material (which gets the worn result), with the clean values, so it can be changed again.
    QJsonObject all;
    QFile file(MaterialWearFile());
    if (file.open(QIODevice::ReadOnly))
    {
        all = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
    }
    all.insert(QDir(MaterialProject()).relativeFilePath(m_path),
               QJsonObject{ { QStringLiteral("yellowing"), wear.yellowing }, { QStringLiteral("damp"), wear.damp },
                            { QStringLiteral("fadeAtStains"), wear.fadeAtStains }, { QStringLiteral("clean"), m_values } });
    QSaveFile out(MaterialWearFile());
    if (out.open(QIODevice::WriteOnly))
    {
        out.write(QJsonDocument(all).toJson());
        out.commit();
    }
}

void HearthMaterialPane::OpenMaterial(const QString& path)
{
    if (m_dirty && m_path != path)
    {
        if (QMessageBox::question(this, tr("Material editor"), tr("Keep the changes to %1?").arg(QFileInfo(m_path).completeBaseName()),
                                  QMessageBox::Save | QMessageBox::Discard) == QMessageBox::Save)
        {
            Save();
        }
        else
        {
            Revert();
        }
    }
    m_path = path;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        QMessageBox::warning(this, tr("Material editor"), tr("Couldn't open %1.").arg(path));
        return;
    }
    m_file = QJsonDocument::fromJson(file.readAll()).object();
    m_values = m_file.value(QStringLiteral("propertyValues")).toObject();
    m_wear = ReadWear();
    // The clean values (before wear) come back from the wear file when the .material still matches what was saved.
    QFile wearFile(MaterialWearFile());
    if (wearFile.open(QIODevice::ReadOnly))
    {
        const QJsonObject entry = QJsonDocument::fromJson(wearFile.readAll()).object().value(QDir(MaterialProject()).relativeFilePath(m_path)).toObject();
        if (entry.contains(QStringLiteral("clean")) && (m_wear.yellowing > 0 || m_wear.damp > 0))
        {
            m_values = entry.value(QStringLiteral("clean")).toObject();
        }
    }
    m_savedWear = m_wear;
    m_dirty = false;
    BuildEditor();
    ApplyLive();
}

QString HearthMaterialPane::TextureSlotTitle(const QString& key) const
{
    for (const auto& slot : MaterialSlots())
    {
        if (slot.first == key)
        {
            return slot.second;
        }
    }
    return key;
}

void HearthMaterialPane::BuildEditor()
{
    if (m_body)
    {
        m_body->deleteLater();
    }
    m_body = new QWidget(this);
    auto* layout = new QVBoxLayout(m_body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    // Header.
    QHBoxLayout* header = Hearth::MakeHeader(m_body, QFileInfo(m_path).completeBaseName(), QString());
    if (auto* titles = header->itemAt(1) ? header->itemAt(1)->layout() : nullptr)
    {
        m_title = titles->count() > 0 ? qobject_cast<QLabel*>(titles->itemAt(0)->widget()) : nullptr;
        m_subtitle = titles->count() > 1 ? qobject_cast<QLabel*>(titles->itemAt(1)->widget()) : nullptr;
    }
    header->addStretch(1);
    QPushButton* other = Hearth::MakeButton(tr("Other materials"), false, m_body);
    connect(other, &QPushButton::clicked, this, [this]()
    {
        if (m_dirty && QMessageBox::question(this, tr("Material editor"), tr("Save the changes first?"), QMessageBox::Save | QMessageBox::Discard) == QMessageBox::Save)
        {
            Save();
        }
        else if (m_dirty)
        {
            Revert();
        }
        ShowPicker();
    });
    header->addWidget(other);
    header->addWidget(Hearth::MakeChip(tr("Live in the level"), QStringLiteral("good"), m_body));
    m_revert = Hearth::MakeButton(tr("Revert"), false, m_body);
    connect(m_revert, &QPushButton::clicked, this, &HearthMaterialPane::Revert);
    header->addWidget(m_revert);
    m_save = Hearth::MakeButton(tr("Save"), true, m_body);
    connect(m_save, &QPushButton::clicked, this, &HearthMaterialPane::Save);
    header->addWidget(m_save);
    layout->addLayout(header);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);

    // Preview.
    QVBoxLayout* previewLayout = nullptr;
    QFrame* previewCard = Hearth::MakeCard(m_body, &previewLayout);
    auto* previewTop = new QHBoxLayout();
    previewTop->addWidget(MaterialSegmented(previewCard, { tr("Sphere"), tr("Wall"), tr("Cube") }, m_shape, [this](int i) { m_shape = i; RequestPreview(); }));
    previewTop->addStretch(1);
    previewTop->addWidget(MaterialSegmented(previewCard, { tr("Level 0 lights"), tr("Studio"), tr("Dark zone") }, m_lighting, [this](int i) { m_lighting = i; RequestPreview(); }));
    previewLayout->addLayout(previewTop);
    m_preview = new HearthMaterialPreviewLabel(previewCard);
    m_preview->setStyleSheet(QStringLiteral("background-color: #121110; border-radius: 12px; color: #8D857B;"));
    m_preview->setText(tr("Drawing the preview…"));
    m_preview->m_turned = [this](float dx, float dy)
    {
        m_yaw += dx;
        m_pitch = qBound(-1.4f, m_pitch - dy, 1.4f);
        RequestPreview();
    };
    m_preview->m_zoomed = [this](float factor)
    {
        m_zoom = qBound(0.4f, m_zoom * factor, 4.0f);
        RequestPreview();
    };
    m_preview->m_resized = [this]() { RequestPreview(); };
    previewLayout->addWidget(m_preview, 1);
    previewLayout->addWidget(Hearth::MakeLabel(tr("Drag to turn · scroll to zoom"), QStringLiteral("small"), previewCard), 0, Qt::AlignHCenter);
    body->addWidget(previewCard, 1);

    // Controls.
    auto* side = new QWidget(m_body);
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(10);

    QFrame* textures = MaterialSection(side, tr("Textures"), &m_texturesLayout);
    sideLayout->addWidget(textures);
    FillTextures();

    QVBoxLayout* surface = nullptr;
    QFrame* surfaceCard = MaterialSection(side, tr("Surface"), &surface);
    {
        auto* tintRow = new QHBoxLayout();
        tintRow->setSpacing(10);
        QLabel* tintLabel = Hearth::MakeLabel(tr("Tint"), QStringLiteral("body"), surfaceCard);
        tintLabel->setFixedWidth(130);
        tintRow->addWidget(tintLabel);
        m_tintSwatch = new QPushButton(surfaceCard);
        m_tintSwatch->setFixedSize(28, 20);
        m_tintSwatch->setCursor(Qt::PointingHandCursor);
        tintRow->addWidget(m_tintSwatch);
        m_tintHex = Hearth::MakeLabel(QString(), QStringLiteral("mono"), surfaceCard);
        tintRow->addWidget(m_tintHex);
        m_tintFactor = new QSlider(Qt::Horizontal, surfaceCard);
        m_tintFactor->setRange(0, 1000);
        tintRow->addWidget(m_tintFactor, 1);
        QLabel* factorText = Hearth::MakeLabel(QString(), QStringLiteral("mono"), surfaceCard);
        factorText->setFixedWidth(58);
        factorText->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        tintRow->addWidget(factorText);
        connect(m_tintFactor, &QSlider::valueChanged, factorText, [factorText](int v) { factorText->setText(QString::number(v / 1000.0, 'f', 2)); });
        surface->addLayout(tintRow);
        connect(m_tintSwatch, &QPushButton::clicked, this, [this]()
        {
            const QColor start = MaterialQColor(MaterialColor(m_values.value(QStringLiteral("baseColor.color")), AZ::Color::CreateOne()));
            const QColor picked = QColorDialog::getColor(start, this, tr("Tint"));
            if (picked.isValid())
            {
                m_values.insert(QStringLiteral("baseColor.color"), QJsonArray{ picked.redF(), picked.greenF(), picked.blueF(), 1.0 });
                SetDirty(true);
            }
        });
    }
    m_metallic = MaterialSlider(surfaceCard, surface, tr("Metallic"), 1000, [](int v) { return QString::number(v / 1000.0, 'f', 2); });
    m_normalStrength = MaterialSlider(surfaceCard, surface, tr("Normal strength"), 2000, [](int v) { return QString::number(v / 1000.0, 'f', 2); });
    // Texture scale is shown as the size of one repeat (Level 0's meshes have one UV unit per metre).
    m_textureScale = MaterialSlider(surfaceCard, surface, tr("Texture scale"), 790, [](int v) { return QStringLiteral("%1 m").arg((v + 10) / 100.0, 0, 'f', 1); });
    sideLayout->addWidget(surfaceCard);

    QVBoxLayout* wear = nullptr;
    QFrame* wearCard = MaterialSection(side, tr("Wear"), &wear);
    m_yellowing = MaterialSlider(wearCard, wear, tr("Yellowing"), 1000, [](int v) { return QString::number(v / 1000.0, 'f', 2); });
    m_damp = MaterialSlider(wearCard, wear, tr("Damp patches"), 1000, [](int v) { return QString::number(v / 1000.0, 'f', 2); });
    m_fade = new Hearth::Toggle(wearCard);
    wear->addWidget(Hearth::MakeSettingRow(tr("Fade where stains are"), tr("The area's stains take on the same age, so they sink into the surface"), m_fade, wearCard));
    sideLayout->addWidget(wearCard);

    // Cost, folded away until asked for.
    QVBoxLayout* costOuter = nullptr;
    QFrame* costCard = Hearth::MakeCard(side, &costOuter);
    auto* costToggle = new QPushButton(tr("Cost on this laptop  ›"), costCard);
    costToggle->setStyleSheet(QStringLiteral("QPushButton { text-align: left; border: none; background: transparent; color: #ECE6DE; font-weight: 600; font-size: 14px; }"));
    costOuter->addWidget(costToggle);
    m_costBody = new QWidget(costCard);
    m_costLayout = new QVBoxLayout(m_costBody);
    m_costLayout->setContentsMargins(0, 0, 0, 0);
    costOuter->addWidget(m_costBody);
    connect(costToggle, &QPushButton::clicked, this, [this, costToggle]()
    {
        const bool show = !m_costBody->isVisible();
        m_costBody->setVisible(show);
        costToggle->setText(show ? tr("Cost on this laptop  ⌄") : tr("Cost on this laptop  ›"));
        if (show)
        {
            FillCost();
        }
    });
    m_costBody->setVisible(true);
    costToggle->setText(tr("Cost on this laptop  ⌄"));
    sideLayout->addWidget(costCard);
    sideLayout->addStretch(1);

    auto* scroll = new QScrollArea(m_body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setFixedWidth(470);
    scroll->setWidget(side);
    body->addWidget(scroll);
    layout->addLayout(body, 1);
    m_root->addWidget(m_body, 1);

    // Values into the controls (without counting as changes).
    auto block = [](QObject* o, bool b) { o->blockSignals(b); };
    for (QObject* o : std::initializer_list<QObject*>{ m_tintFactor, m_metallic, m_normalStrength, m_textureScale, m_yellowing, m_damp, m_fade })
    {
        block(o, true);
    }
    m_tintFactor->setValue(qRound(m_values.value(QStringLiteral("baseColor.factor")).toDouble(1.0) * 1000));
    m_metallic->setValue(qRound(m_values.value(QStringLiteral("metallic.factor")).toDouble(0.0) * 1000));
    m_normalStrength->setValue(qRound(m_values.value(QStringLiteral("normal.factor")).toDouble(1.0) * 1000));
    const double uvScale = qMax(0.0001, m_values.value(QStringLiteral("uv.scale")).toDouble(1.0));
    m_textureScale->setValue(qBound(0, qRound(100.0 / uvScale) - 10, 790));
    m_yellowing->setValue(qRound(m_wear.yellowing * 1000));
    m_damp->setValue(qRound(m_wear.damp * 1000));
    m_fade->setChecked(m_wear.fadeAtStains);
    for (QObject* o : std::initializer_list<QObject*>{ m_tintFactor, m_metallic, m_normalStrength, m_textureScale, m_yellowing, m_damp, m_fade })
    {
        block(o, false);
    }
    // The value labels show the starting values too.
    for (QSlider* s : { m_tintFactor, m_metallic, m_normalStrength, m_textureScale, m_yellowing, m_damp })
    {
        emit s->valueChanged(s->value());
    }

    connect(m_tintFactor, &QSlider::valueChanged, this, [this](int v) { m_values.insert(QStringLiteral("baseColor.factor"), v / 1000.0); SetDirty(true); });
    connect(m_metallic, &QSlider::valueChanged, this, [this](int v) { m_values.insert(QStringLiteral("metallic.factor"), v / 1000.0); SetDirty(true); });
    connect(m_normalStrength, &QSlider::valueChanged, this, [this](int v) { m_values.insert(QStringLiteral("normal.factor"), v / 1000.0); SetDirty(true); });
    connect(m_textureScale, &QSlider::valueChanged, this, [this](int v) { m_values.insert(QStringLiteral("uv.scale"), 100.0 / (v + 10)); SetDirty(true); });
    connect(m_yellowing, &QSlider::valueChanged, this, [this](int v) { m_wear.yellowing = v / 1000.0; SetDirty(true); });
    connect(m_damp, &QSlider::valueChanged, this, [this](int v) { m_wear.damp = v / 1000.0; SetDirty(true); });
    connect(m_fade, &QAbstractButton::toggled, this, [this](bool on) { m_wear.fadeAtStains = on; SetDirty(true); });
    SetDirty(false);
    FillCost();
}

void HearthMaterialPane::FillTextures()
{
    while (QLayoutItem* item = m_texturesLayout->takeAt(1))
    {
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
    QStringList missing;
    for (const auto& slot : MaterialSlots())
    {
        const QString texture = m_values.value(slot.first).toString();
        const bool always = slot.first == QLatin1String("baseColor.textureMap") || slot.first == QLatin1String("normal.textureMap")
            || slot.first == QLatin1String("roughness.textureMap");
        if (texture.isEmpty() && !always)
        {
            missing << slot.first;
            continue;
        }
        auto* row = new QPushButton(m_texturesLayout->parentWidget());
        row->setCursor(Qt::PointingHandCursor);
        row->setMinimumHeight(48);
        row->setStyleSheet(QStringLiteral("QPushButton { text-align: left; background-color: rgba(255, 255, 255, 8); border: none; border-radius: 10px; }"
                                          "QPushButton:hover { background-color: rgba(255, 255, 255, 16); }"));
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 6, 10, 6);
        rowLayout->setSpacing(10);
        auto* thumb = new QLabel(row);
        thumb->setFixedSize(34, 34);
        thumb->setAttribute(Qt::WA_TransparentForMouseEvents);
        const QString absolute = MaterialTexturePath(texture, m_path);
        QImageReader reader(absolute);
        reader.setScaledSize(QSize(68, 68));
        const QImage image = absolute.isEmpty() ? QImage() : reader.read();
        if (!image.isNull())
        {
            QPixmap rounded(68, 68);
            rounded.fill(Qt::transparent);
            QPainter p(&rounded);
            p.setRenderHint(QPainter::Antialiasing);
            QPainterPath clip;
            clip.addRoundedRect(QRectF(0, 0, 68, 68), 14, 14);
            p.setClipPath(clip);
            p.drawImage(QRect(0, 0, 68, 68), image);
            p.end();
            rounded.setDevicePixelRatio(2.0);
            thumb->setPixmap(rounded);
        }
        else
        {
            thumb->setStyleSheet(QStringLiteral("background-color: #24211E; border-radius: 7px; border: 1px dashed rgba(255, 255, 255, 30);"));
        }
        rowLayout->addWidget(thumb);
        auto* texts = new QVBoxLayout();
        texts->setSpacing(1);
        QLabel* title = Hearth::MakeLabel(slot.second, QStringLiteral("body"), row);
        title->setAttribute(Qt::WA_TransparentForMouseEvents);
        QLabel* name = Hearth::MakeLabel(texture.isEmpty() ? tr("none · pick one") : QFileInfo(texture).completeBaseName(), QStringLiteral("mono"), row);
        name->setAttribute(Qt::WA_TransparentForMouseEvents);
        if (!texture.isEmpty() && absolute.isEmpty())
        {
            name->setText(tr("%1 (missing)").arg(QFileInfo(texture).fileName()));
        }
        texts->addWidget(title);
        texts->addWidget(name);
        rowLayout->addLayout(texts, 1);
        const QString key = slot.first;
        connect(row, &QPushButton::clicked, this, [this, key, texture, row]()
        {
            QMenu menu(this);
            menu.addAction(tr("Pick a texture…"), this, [this, key]() { PickTexture(key); });
            if (!texture.isEmpty())
            {
                const QString absolute = MaterialTexturePath(texture, m_path);
                menu.addAction(tr("Show it in Browse assets"), this, [absolute]()
                {
                    if (const QtViewPane* pane = QtViewPaneManager::instance()->OpenPane(QStringLiteral("Browse assets")))
                    {
                        if (auto* browser = qobject_cast<HearthAssetBrowser*>(pane->Widget()))
                        {
                            browser->Rescan(absolute);
                        }
                    }
                });
                menu.addAction(tr("Take it off"), this, [this, key]()
                {
                    m_values.remove(key);
                    FillTextures();
                    SetDirty(true);
                });
            }
            menu.exec(row->mapToGlobal(QPoint(0, row->height())));
        });
        m_texturesLayout->addWidget(row);
    }
    QPushButton* add = Hearth::MakeButton(tr("Add a texture"), false, m_texturesLayout->parentWidget());
    add->setEnabled(!missing.isEmpty());
    connect(add, &QPushButton::clicked, this, [this, add, missing]()
    {
        QMenu menu(this);
        for (const QString& key : missing)
        {
            menu.addAction(TextureSlotTitle(key), this, [this, key]() { PickTexture(key); });
        }
        menu.exec(add->mapToGlobal(QPoint(0, add->height())));
    });
    m_texturesLayout->addWidget(add, 0, Qt::AlignLeft);
}

void HearthMaterialPane::PickTexture(const QString& key)
{
    const QString start = QDir(MaterialProject()).filePath(QStringLiteral("Assets/Textures"));
    const QString picked = QFileDialog::getOpenFileName(this, tr("Pick a texture for %1").arg(TextureSlotTitle(key)), start,
                                                        tr("Images (*.png *.jpg *.jpeg *.tga *.tif *.tiff *.exr *.bmp *.dds)"));
    if (picked.isEmpty())
    {
        return;
    }
    if (!QDir::cleanPath(picked).startsWith(QDir::cleanPath(MaterialProject()) + QLatin1Char('/')))
    {
        QMessageBox::information(this, tr("Pick a texture"), tr("That file isn't in the project yet. Bring it in with Import first, then pick it here."));
        return;
    }
    m_values.insert(key, QFileInfo(m_path).dir().relativeFilePath(picked));
    if (key == QLatin1String("emissive.textureMap"))
    {
        m_values.insert(QStringLiteral("emissive.enable"), true);
    }
    if (key == QLatin1String("parallax.textureMap"))
    {
        m_values.insert(QStringLiteral("parallax.enable"), true);
    }
    FillTextures();
    SetDirty(true);
}

void HearthMaterialPane::SetDirty(bool dirty)
{
    m_dirty = dirty;
    if (m_subtitle)
    {
        QString type = QFileInfo(m_file.value(QStringLiteral("materialType")).toString()).completeBaseName();
        type = type.isEmpty() ? tr("Material") : type;
        m_subtitle->setText(tr("Material · %1 · %2").arg(type, dirty ? tr("unsaved changes") : tr("saved")));
    }
    if (m_revert)
    {
        m_revert->setEnabled(dirty);
    }
    if (m_save)
    {
        m_save->setEnabled(dirty);
    }
    if (dirty)
    {
        m_liveTimer->start();
    }
}

void HearthMaterialPane::ApplyLive()
{
    if (m_path.isEmpty())
    {
        return;
    }
    // The values with wear on top: yellowing pulls the tint toward old paper, damp darkens it a little and makes it glossier.
    AZ::Color tint = MaterialColor(m_values.value(QStringLiteral("baseColor.color")), AZ::Color::CreateOne());
    tint = MaterialLerp(tint, AZ::Color(tint.GetR() * 0.93f, tint.GetG() * 0.84f, tint.GetB() * 0.52f, tint.GetA()), float(m_wear.yellowing));
    tint = MaterialLerp(tint, tint * 0.72f, float(m_wear.damp) * 0.35f);
    tint.SetA(1.0f);

    AZ::Data::AssetInfo info;
    AZStd::string watchFolder;
    bool found = false;
    AzToolsFramework::AssetSystemRequestBus::BroadcastResult(found, &AzToolsFramework::AssetSystemRequestBus::Events::GetSourceInfoBySourcePath,
                                                             m_path.toUtf8().constData(), info, watchFolder);
    if (!found)
    {
        RequestPreview();
        return;
    }
    AZ::Data::Asset<AZ::RPI::MaterialAsset> asset =
        AZ::Data::AssetManager::Instance().GetAsset<AZ::RPI::MaterialAsset>(AZ::Data::AssetId(info.m_assetId.m_guid, 0), AZ::Data::AssetLoadBehavior::PreLoad);
    asset.BlockUntilLoadComplete();
    if (!asset.IsReady())
    {
        if (m_subtitle)
        {
            m_subtitle->setText(tr("Material · not processed yet: changes show once the Asset Processor has made it"));
        }
        return;
    }
    // The level's own instance, so the change shows everywhere it's used.
    AZ::Data::Instance<AZ::RPI::Material> material = AZ::RPI::Material::FindOrCreate(asset);
    if (!material)
    {
        return;
    }
    auto set = [&material](const char* name, const AZ::RPI::MaterialPropertyValue& value)
    {
        const AZ::RPI::MaterialPropertyIndex index = material->FindPropertyIndex(AZ::Name(name));
        if (index.IsValid())
        {
            material->SetPropertyValue(index, value);
        }
    };
    set("baseColor.color", tint);
    set("baseColor.factor", float(m_values.value(QStringLiteral("baseColor.factor")).toDouble(1.0)));
    set("metallic.factor", float(m_values.value(QStringLiteral("metallic.factor")).toDouble(0.0)));
    set("normal.factor", float(m_values.value(QStringLiteral("normal.factor")).toDouble(1.0)));
    set("uv.scale", float(m_values.value(QStringLiteral("uv.scale")).toDouble(1.0)));
    const double damp = m_wear.damp;
    if (m_values.contains(QStringLiteral("roughness.textureMap")))
    {
        set("roughness.upperBound", float(m_values.value(QStringLiteral("roughness.upperBound")).toDouble(1.0) * (1.0 - damp * 0.5)));
    }
    else
    {
        set("roughness.factor", float(m_values.value(QStringLiteral("roughness.factor")).toDouble(1.0) * (1.0 - damp * 0.5)));
    }
    set("emissive.enable", m_values.value(QStringLiteral("emissive.enable")).toBool(false));
    set("parallax.enable", m_values.value(QStringLiteral("parallax.enable")).toBool(false));
    for (const auto& slot : MaterialSlots())
    {
        const QString absolute = MaterialTexturePath(m_values.value(slot.first).toString(), m_path);
        AZ::Data::Instance<AZ::RPI::Image> image;
        if (!absolute.isEmpty())
        {
            const AZ::Data::AssetId id = MaterialProductId(absolute, ".streamingimage", azrtti_typeid<AZ::RPI::StreamingImageAsset>());
            if (id.IsValid())
            {
                auto imageAsset = AZ::Data::AssetManager::Instance().GetAsset<AZ::RPI::StreamingImageAsset>(id, AZ::Data::AssetLoadBehavior::PreLoad);
                imageAsset.BlockUntilLoadComplete();
                if (imageAsset.IsReady())
                {
                    image = AZ::RPI::StreamingImage::FindOrCreate(imageAsset);
                }
            }
        }
        set(slot.first.toUtf8().constData(), image);
    }
    // Compiling can be refused for a frame while the material is in use; try again shortly.
    QPointer<HearthMaterialPane> self(this);
    auto attempts = std::make_shared<int>(0);
    auto compile = std::make_shared<std::function<void()>>();
    *compile = [self, material, attempts, compile]()
    {
        if (!self)
        {
            return;
        }
        if (!material->Compile() && ++*attempts < 30)
        {
            QTimer::singleShot(33, self, *compile);
            return;
        }
        self->RequestPreview();
    };
    (*compile)();
}

void HearthMaterialPane::RequestPreview()
{
    if (!m_preview || m_path.isEmpty())
    {
        return;
    }
    if (m_previewBusy)
    {
        m_previewAgain = true;
        return;
    }
    auto* renderer = AZ::Interface<AtomToolsFramework::PreviewRendererInterface>::Get();
    if (!renderer)
    {
        m_preview->setText(tr("The preview needs the renderer; it starts with the level."));
        return;
    }
    AZ::Data::AssetInfo info;
    AZStd::string watchFolder;
    bool found = false;
    AzToolsFramework::AssetSystemRequestBus::BroadcastResult(found, &AzToolsFramework::AssetSystemRequestBus::Events::GetSourceInfoBySourcePath,
                                                             m_path.toUtf8().constData(), info, watchFolder);
    AZ::Data::Asset<AZ::RPI::MaterialAsset> materialAsset =
        AZ::Data::AssetManager::Instance().GetAsset<AZ::RPI::MaterialAsset>(AZ::Data::AssetId(info.m_assetId.m_guid, 0), AZ::Data::AssetLoadBehavior::PreLoad);
    materialAsset.BlockUntilLoadComplete();
    if (!found || !materialAsset.IsReady())
    {
        m_preview->setText(tr("Waiting for the Asset Processor to make this material…"));
        QTimer::singleShot(2000, this, &HearthMaterialPane::RequestPreview);
        return;
    }
    const char* shapes[] = { "objects/_primitives/_sphere_1x1.fbx.azmodel", "objects/_primitives/_box_1x1.fbx.azmodel", "objects/_primitives/_box_1x1.fbx.azmodel" };
    AZ::Data::AssetId modelId;
    AZ::Data::AssetCatalogRequestBus::BroadcastResult(modelId, &AZ::Data::AssetCatalogRequests::GetAssetIdByPath, shapes[m_shape], azrtti_typeid<AZ::RPI::ModelAsset>(), false);
    AZ::Data::AssetId lightingId;
    AZ::Data::AssetCatalogRequestBus::BroadcastResult(lightingId, &AZ::Data::AssetCatalogRequests::GetAssetIdByPath, "lightingpresets/default.lightingpreset.azasset",
                                                      azrtti_typeid<AZ::RPI::AnyAsset>(), false);
    AZ::Data::Asset<AZ::RPI::ModelAsset> model(modelId, azrtti_typeid<AZ::RPI::ModelAsset>());
    AZ::Data::Asset<AZ::RPI::AnyAsset> lighting(lightingId, azrtti_typeid<AZ::RPI::AnyAsset>());

    AtomToolsFramework::PreviewRendererCaptureRequest request;
    const qreal ratio = devicePixelRatioF();
    request.m_size = qBound(128, int(qMin(m_preview->width(), m_preview->height()) * ratio), 768);
    request.m_content = AZStd::make_shared<HearthMaterialPreviewContent>(renderer->GetScene(), renderer->GetView(), model, AZ::RPI::Material::FindOrCreate(materialAsset),
                                                                        lighting, m_lighting, m_shape, m_yaw, m_pitch, m_zoom);
    QPointer<HearthMaterialPane> self(this);
    request.m_captureCompleteCallback = [self, ratio](const QPixmap& pixmap)
    {
        if (!self)
        {
            return;
        }
        QPixmap shown = pixmap;
        shown.setDevicePixelRatio(ratio);
        self->m_preview->setPixmap(shown);
        self->m_previewBusy = false;
        if (self->m_previewAgain)
        {
            self->m_previewAgain = false;
            self->RequestPreview();
        }
    };
    request.m_captureFailedCallback = [self]()
    {
        if (!self)
        {
            return;
        }
        self->m_previewBusy = false;
        if (self->m_preview->pixmap().isNull())
        {
            self->m_preview->setText(tr("The preview couldn't be drawn just now."));
        }
        if (self->m_previewAgain)
        {
            self->m_previewAgain = false;
            QTimer::singleShot(200, self, &HearthMaterialPane::RequestPreview);
        }
    };
    m_previewBusy = true;
    renderer->AddCaptureRequest(request);
}

void HearthMaterialPane::FillCost()
{
    if (!m_costLayout)
    {
        return;
    }
    while (QLayoutItem* item = m_costLayout->takeAt(0))
    {
        if (QLayout* inner = item->layout())
        {
            while (QLayoutItem* child = inner->takeAt(0))
            {
                if (child->widget())
                {
                    child->widget()->deleteLater();
                }
                delete child;
            }
        }
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
    auto row = [this](const QString& name, const QString& value, const QColor& colour)
    {
        auto* line = new QHBoxLayout();
        line->addWidget(Hearth::MakeLabel(name, QStringLiteral("dim"), m_costBody));
        line->addStretch(1);
        QLabel* text = Hearth::MakeLabel(value, QStringLiteral("body"), m_costBody);
        text->setStyleSheet(QStringLiteral("background: transparent; color: %1; font-weight: 500;").arg(colour.name()));
        line->addWidget(text);
        m_costLayout->addLayout(line);
    };

    // Shader: the expensive StandardPBR features this material switches on.
    const bool heavy = m_values.value(QStringLiteral("parallax.enable")).toBool() || m_values.value(QStringLiteral("subsurfaceScattering.enableSubsurfaceScattering")).toBool()
        || m_values.value(QStringLiteral("transmission.enable")).toBool();
    const bool medium = m_values.value(QStringLiteral("clearCoat.enable")).toBool() || m_values.value(QStringLiteral("emissive.enable")).toBool()
        || m_values.value(QStringLiteral("opacity.mode")).toString() == QLatin1String("Blended");
    row(tr("Shader"), heavy ? tr("Heavy") : (medium ? tr("Medium") : tr("Light")), heavy ? Hearth::Bad() : (medium ? Hearth::Warn() : Hearth::Good()));

    // Texture memory, as the image builder will store them (compressed formats, mips, size reduction).
    double bytes = 0.0;
    for (const auto& slot : MaterialSlots())
    {
        const QString absolute = MaterialTexturePath(m_values.value(slot.first).toString(), m_path);
        if (absolute.isEmpty())
        {
            continue;
        }
        QSize size = QImageReader(absolute).size();
        if (!size.isValid())
        {
            continue;
        }
        QString preset;
        int reduce = 0;
        QFile settings(absolute + QStringLiteral(".assetinfo"));
        if (settings.open(QIODevice::ReadOnly))
        {
            QJsonObject data = QJsonDocument::fromJson(settings.readAll()).object();
            data = data.contains(QStringLiteral("ClassData")) ? data.value(QStringLiteral("ClassData")).toObject() : data;
            reduce = data.value(QStringLiteral("SizeReduceLevel")).toInt();
            preset = data.value(QStringLiteral("Preset")).toString();
        }
        size = QSize(qMax(1, size.width() >> reduce), qMax(1, size.height() >> reduce));
        double perPixel = 0.5; // BC1 / BC4
        if (slot.first == QLatin1String("normal.textureMap"))
        {
            perPixel = 1.0; // BC5
        }
        if (preset == QLatin1String("ReferenceImage"))
        {
            perPixel = 4.0;
        }
        bytes += size.width() * double(size.height()) * perPixel * 4.0 / 3.0;
    }
    row(tr("Texture memory"), bytes >= 1024 * 1024 ? tr("%1 MB").arg(qRound(bytes / (1024 * 1024))) : tr("%1 KB").arg(qRound(bytes / 1024)), Hearth::Text());

    // Used on screen: entities in the open level that use it, and prefabs that use it.
    const QString needle = QDir(MaterialProject()).relativeFilePath(m_path).toLower().replace(QStringLiteral(".material"), QStringLiteral(".azmaterial"));
    int entities = 0;
    AZ::ComponentApplicationBus::Broadcast(&AZ::ComponentApplicationRequests::EnumerateEntities, [&entities, &needle](AZ::Entity* entity)
    {
        bool editorEntity = false;
        AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(editorEntity, &AzToolsFramework::EditorEntityContextRequests::IsEditorEntity, entity->GetId());
        if (!editorEntity)
        {
            return;
        }
        rapidjson::Document document;
        AZ::JsonSerialization::Store(document, document.GetAllocator(), *entity);
        AZStd::string text;
        if (AZ::JsonSerializationUtils::WriteJsonString(document, text).IsSuccess() && QString::fromUtf8(text.c_str()).toLower().contains(needle))
        {
            ++entities;
        }
    });
    int prefabs = 0;
    QDirIterator it(QDir(MaterialProject()).filePath(QStringLiteral("Prefabs")), { QStringLiteral("*.prefab") }, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        QFile prefab(it.next());
        if (prefab.open(QIODevice::ReadOnly) && QString::fromUtf8(prefab.readAll()).toLower().contains(needle))
        {
            ++prefabs;
        }
    }
    QStringList used;
    used << tr("~%n in the level", nullptr, entities);
    if (prefabs > 0)
    {
        used << tr("%n prefab(s)", nullptr, prefabs);
    }
    row(tr("Used on screen"), used.join(QStringLiteral(" · ")), Hearth::Text());
}

void HearthMaterialPane::Revert()
{
    if (m_path.isEmpty())
    {
        return;
    }
    // Put the saved values back on the level's material too.
    const QString path = m_path;
    m_dirty = false;
    OpenMaterial(path);
}

void HearthMaterialPane::Save()
{
    if (m_path.isEmpty())
    {
        return;
    }
    // The .material gets the worn result; the wear file keeps the clean values and the wear amounts.
    QJsonObject values = m_values;
    AZ::Color tint = MaterialColor(m_values.value(QStringLiteral("baseColor.color")), AZ::Color::CreateOne());
    tint = MaterialLerp(tint, AZ::Color(tint.GetR() * 0.93f, tint.GetG() * 0.84f, tint.GetB() * 0.52f, tint.GetA()), float(m_wear.yellowing));
    tint = MaterialLerp(tint, tint * 0.72f, float(m_wear.damp) * 0.35f);
    tint.SetA(1.0f);
    if (m_wear.yellowing > 0 || m_wear.damp > 0 || m_values.contains(QStringLiteral("baseColor.color")))
    {
        values.insert(QStringLiteral("baseColor.color"), MaterialColorJson(tint));
    }
    if (m_wear.damp > 0)
    {
        if (m_values.contains(QStringLiteral("roughness.textureMap")))
        {
            values.insert(QStringLiteral("roughness.upperBound"), m_values.value(QStringLiteral("roughness.upperBound")).toDouble(1.0) * (1.0 - m_wear.damp * 0.5));
        }
        else
        {
            values.insert(QStringLiteral("roughness.factor"), m_values.value(QStringLiteral("roughness.factor")).toDouble(1.0) * (1.0 - m_wear.damp * 0.5));
        }
    }
    QJsonObject file = m_file;
    file.insert(QStringLiteral("propertyValues"), values);
    QSaveFile out(m_path);
    if (!out.open(QIODevice::WriteOnly))
    {
        QMessageBox::warning(this, tr("Save"), tr("Couldn't write %1.").arg(m_path));
        return;
    }
    out.write(QJsonDocument(file).toJson(QJsonDocument::Indented));
    if (!out.commit())
    {
        QMessageBox::warning(this, tr("Save"), tr("Couldn't write %1.").arg(m_path));
        return;
    }
    WriteWear(m_wear);

    // Fade where stains are: the area's stain decals take the same yellowing, so they sink into the aged surface.
    if (m_wear.fadeAtStains != m_savedWear.fadeAtStains || (m_wear.fadeAtStains && !qFuzzyCompare(1.0 + m_wear.yellowing, 1.0 + m_savedWear.yellowing)))
    {
        const QDir folder = QFileInfo(m_path).dir();
        for (const QString& name : folder.entryList({ QStringLiteral("*decal*.material") }, QDir::Files))
        {
            const QString decalPath = folder.filePath(name);
            QFile decalFile(decalPath);
            if (!decalFile.open(QIODevice::ReadOnly))
            {
                continue;
            }
            QJsonObject decal = QJsonDocument::fromJson(decalFile.readAll()).object();
            decalFile.close();
            QJsonObject decalValues = decal.value(QStringLiteral("propertyValues")).toObject();
            // The decal's own colour before any fading is kept in the wear file, so turning this off puts it back.
            QJsonObject all;
            QFile wearFile(MaterialWearFile());
            if (wearFile.open(QIODevice::ReadOnly))
            {
                all = QJsonDocument::fromJson(wearFile.readAll()).object();
                wearFile.close();
            }
            const QString key = QDir(MaterialProject()).relativeFilePath(decalPath);
            QJsonObject entry = all.value(key).toObject();
            if (!entry.contains(QStringLiteral("originalColor")))
            {
                entry.insert(QStringLiteral("originalColor"), decalValues.value(QStringLiteral("baseColor.color")).isArray()
                                                                  ? decalValues.value(QStringLiteral("baseColor.color"))
                                                                  : QJsonValue(QJsonArray{ 1.0, 1.0, 1.0, 1.0 }));
            }
            const AZ::Color original = MaterialColor(entry.value(QStringLiteral("originalColor")), AZ::Color::CreateOne());
            const AZ::Color faded = m_wear.fadeAtStains
                ? MaterialLerp(original, AZ::Color(original.GetR() * 0.93f, original.GetG() * 0.84f, original.GetB() * 0.52f, original.GetA()), float(m_wear.yellowing))
                : original;
            decalValues.insert(QStringLiteral("baseColor.color"), MaterialColorJson(faded));
            decal.insert(QStringLiteral("propertyValues"), decalValues);
            QSaveFile decalOut(decalPath);
            if (decalOut.open(QIODevice::WriteOnly))
            {
                decalOut.write(QJsonDocument(decal).toJson(QJsonDocument::Indented));
                decalOut.commit();
            }
            all.insert(key, entry);
            QSaveFile wearOut(MaterialWearFile());
            if (wearOut.open(QIODevice::WriteOnly))
            {
                wearOut.write(QJsonDocument(all).toJson());
                wearOut.commit();
            }
        }
    }
    m_file = file;
    m_savedWear = m_wear;
    SetDirty(false);
    FillCost();
}
