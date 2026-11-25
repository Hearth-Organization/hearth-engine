/*
 * Hearth Engine "Material canvas". Built on O3DE (Apache-2.0 OR MIT). See HearthMaterialCanvasPane.h.
 */

#include "EditorDefs.h"

#include "HearthMaterialCanvasPane.h"
#include "QtViewPaneManager.h"

#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>
#include <AzToolsFramework/Asset/AssetProcessorMessages.h>

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    struct CanvasParam
    {
        const char* key;
        const char* label;
        double min;
        double max;
        double fallback;
        const char* unit;
    };
    struct CanvasKind
    {
        const char* kind;
        const char* palette;   // the mockup's Blocks list
        const char* label;     // a new block's name
        const char* hint;
        CanvasParam params[3];
    };
    const CanvasKind CanvasKinds[] = {
        { "texture", "Texture", "Texture", "The surface's picture (Base color texture of the material)",
          { { "brightness", "Brightness", 0.2, 2.0, 1.0, "" }, {}, {} } },
        { "noise", "Noise", "Noise", "Patches spread over the surface, the same size wherever it is",
          { { "scale", "Patches per metre", 0.1, 10.0, 1.5, "" }, { "softness", "Softness", 0.01, 0.5, 0.15, "" }, {} } },
        { "blend", "Blend", "Darken", "Darkens the surface where the patches are, and makes it shinier (wet)",
          { { "amount", "Amount", 0.0, 1.0, 0.45, "" }, { "softness", "Softness", 0.0, 1.0, 0.3, "" }, { "shine", "Wet shine", 0.0, 1.0, 0.5, "" } } },
        { "height", "Height and distance", "Height from floor", "Only up to a height above the object's floor (or near the camera)",
          { { "rise", "Rises", 0.0, 5.0, 0.6, " m" }, { "softness", "Fade", 0.01, 2.0, 0.3, " m" }, { "fromCamera", "From the camera instead (0/1)", 0.0, 1.0, 0.0, "" } } },
        { "flicker", "Flicker with nearby light", "Flicker with nearby light", "Brightness stutters like the fluorescent lights",
          { { "amount", "Amount", 0.0, 1.0, 0.3, "" }, { "speed", "Speed", 0.5, 20.0, 8.0, "" }, {} } },
    };

    const CanvasKind* CanvasFind(const QString& kind)
    {
        for (const CanvasKind& k : CanvasKinds)
        {
            if (kind == QLatin1String(k.kind))
            {
                return &k;
            }
        }
        return nullptr;
    }

    //! Replaces every {"$import": "file.json"} with the file's JSON (relative to the file that imports it).
    QJsonValue CanvasResolveImports(const QJsonValue& value, const QDir& from, int depth = 0)
    {
        if (depth > 8)
        {
            return value;
        }
        if (value.isObject())
        {
            const QJsonObject object = value.toObject();
            if (object.size() == 1 && object.contains(QStringLiteral("$import")))
            {
                const QString path = from.filePath(object.value(QStringLiteral("$import")).toString());
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly))
                {
                    return value;
                }
                return CanvasResolveImports(QJsonDocument::fromJson(file.readAll()).object(), QFileInfo(path).dir(), depth + 1);
            }
            QJsonObject out;
            for (auto it = object.begin(); it != object.end(); ++it)
            {
                out.insert(it.key(), CanvasResolveImports(it.value(), from, depth + 1));
            }
            return out;
        }
        if (value.isArray())
        {
            QJsonArray out;
            for (const QJsonValue& item : value.toArray())
            {
                out.append(CanvasResolveImports(item, from, depth + 1));
            }
            return out;
        }
        return value;
    }
} // namespace

void HearthMaterialCanvasPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(150, 90, 1240, 760);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthMaterialCanvasPane>("Material canvas", "Tools", options);
}

HearthMaterialCanvasPane::HearthMaterialCanvasPane(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthMaterialCanvas"));
    setStyleSheet(QStringLiteral("QWidget#HearthMaterialCanvas { background-color: #0D0C0B; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Material canvas"), tr("A new surface type, made from blocks"));
    if (QLayoutItem* titles = header->itemAt(1); titles && titles->layout() && titles->layout()->itemAt(1))
    {
        m_subtitle = qobject_cast<QLabel*>(titles->layout()->itemAt(1)->widget());
    }
    m_pick = new QComboBox(this);
    m_pick->setMinimumWidth(180);
    header->addWidget(m_pick);
    m_compile = Hearth::MakeChip(tr("Not saved yet"), QStringLiteral("plain"), this);
    header->addWidget(m_compile);
    QPushButton* save = Hearth::MakeButton(tr("Save as a material type"), true, this);
    header->addWidget(save);
    root->addLayout(header);
    connect(save, &QPushButton::clicked, this, &HearthMaterialCanvasPane::SaveAsMaterialType);
    connect(m_pick, qOverload<int>(&QComboBox::activated), this, [this](int index)
    {
        const QString slug = m_pick->itemData(index).toString();
        if (slug.isEmpty())
        {
            NewCanvas();
        }
        else
        {
            Load(slug);
        }
    });

    auto* body = new QHBoxLayout();
    body->setSpacing(10);
    root->addLayout(body, 1);
    QVBoxLayout* canvasLayout = nullptr;
    QFrame* canvas = Hearth::MakeCard(this, &canvasLayout);
    m_scroll = new QScrollArea(canvas);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
    canvasLayout->addWidget(m_scroll, 1);
    m_status = Hearth::MakeLabel(QString(), QStringLiteral("small"), canvas);
    m_status->setWordWrap(true);
    canvasLayout->addWidget(m_status);
    body->addWidget(canvas, 1);

    QVBoxLayout* paletteLayout = nullptr;
    QFrame* palette = Hearth::MakeCard(this, &paletteLayout);
    palette->setFixedWidth(260);
    paletteLayout->addWidget(Hearth::MakeLabel(tr("Blocks"), QStringLiteral("title"), palette));
    for (const CanvasKind& kind : CanvasKinds)
    {
        QPushButton* add = Hearth::MakeButton(tr(kind.palette), false, palette);
        add->setToolTip(tr(kind.hint));
        const QString name = QLatin1String(kind.kind);
        connect(add, &QPushButton::clicked, this, [this, name]() { AddBlock(name); });
        paletteLayout->addWidget(add);
    }
    paletteLayout->addStretch(1);
    QPushButton* claude = Hearth::MakeButton(tr("Describe a look for Claude"), true, palette);
    connect(claude, &QPushButton::clicked, this, [this]()
    {
        bool ok = false;
        const QString what = QInputDialog::getMultiLineText(this, tr("Describe a look for Claude"),
            tr("What should the surface look like? For example: wallpaper with damp rising from the floor, darker and shinier where it's wet."), QString(), &ok);
        if (!ok || what.trimmed().isEmpty())
        {
            return;
        }
        Hearth::AskClaude(tr("Material canvas: %1").arg(m_canvas.value(QStringLiteral("title")).toString()),
                          tr("Blocks file: %1 (kinds: texture, noise, blend, height, flicker; params as in the file). After changing it, the "
                             "owner presses Save as a material type, which writes Materials/Types/Hearth/%2.*.\nWhat the owner wants:\n%3\n\nNow:\n%4")
                              .arg(QDir(ProjectFolder()).filePath(QStringLiteral("Materials/Canvas/%1.json").arg(m_slug)), m_slug, what.trimmed(),
                                   QString::fromUtf8(QJsonDocument(m_canvas).toJson(QJsonDocument::Indented))),
                          ProjectFolder());
    });
    paletteLayout->addWidget(claude);
    body->addWidget(palette);

    ListCanvases();
}

QString HearthMaterialCanvasPane::ProjectFolder() const
{
    const auto project = AZ::Utils::GetProjectPath();
    return QString::fromUtf8(project.c_str());
}

QString HearthMaterialCanvasPane::Slug() const
{
    QString slug = m_canvas.value(QStringLiteral("title")).toString().toLower();
    slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
    slug = slug.section(QLatin1Char('_'), 0, -1, QString::SectionSkipEmpty);
    return slug.isEmpty() ? QStringLiteral("surface") : slug;
}

void HearthMaterialCanvasPane::ListCanvases()
{
    m_pick->clear();
    const QStringList files = QDir(QDir(ProjectFolder()).filePath(QStringLiteral("Materials/Canvas"))).entryList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
    for (const QString& file : files)
    {
        m_pick->addItem(QFileInfo(file).completeBaseName(), QFileInfo(file).completeBaseName());
    }
    m_pick->addItem(tr("+ New surface type…"), QString());
    if (!files.isEmpty())
    {
        Load(QFileInfo(m_slug.isEmpty() || !files.contains(m_slug + QStringLiteral(".json")) ? files.first() : m_slug + QStringLiteral(".json")).completeBaseName());
    }
    else
    {
        // Start from the mockup's example so the window shows how blocks work.
        m_canvas = QJsonObject{ { QStringLiteral("title"), tr("Damp wallpaper") },
                                { QStringLiteral("blocks"), QJsonArray{
                                      QJsonObject{ { QStringLiteral("kind"), QStringLiteral("texture") }, { QStringLiteral("label"), tr("Wallpaper texture") } },
                                      QJsonObject{ { QStringLiteral("kind"), QStringLiteral("noise") }, { QStringLiteral("label"), tr("Damp map (noise)") } },
                                      QJsonObject{ { QStringLiteral("kind"), QStringLiteral("height") }, { QStringLiteral("label"), tr("Height from floor") } },
                                      QJsonObject{ { QStringLiteral("kind"), QStringLiteral("blend") }, { QStringLiteral("label"), tr("Darken where damp") } } } } };
        m_slug.clear();
        Rebuild();
    }
}

void HearthMaterialCanvasPane::Load(const QString& slug)
{
    QFile file(QDir(ProjectFolder()).filePath(QStringLiteral("Materials/Canvas/%1.json").arg(slug)));
    if (!file.open(QIODevice::ReadOnly))
    {
        return;
    }
    m_canvas = QJsonDocument::fromJson(file.readAll()).object();
    m_slug = slug;
    m_pick->setCurrentIndex(qMax(0, m_pick->findData(slug)));
    m_compileTries = 0;
    CheckCompile();
    Rebuild();
}

void HearthMaterialCanvasPane::SaveCanvas()
{
    if (m_slug.isEmpty())
    {
        m_slug = Slug();
    }
    const QString path = QDir(ProjectFolder()).filePath(QStringLiteral("Materials/Canvas/%1.json").arg(m_slug));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        file.write(QJsonDocument(m_canvas).toJson(QJsonDocument::Indented));
    }
}

void HearthMaterialCanvasPane::NewCanvas()
{
    bool ok = false;
    const QString title = QInputDialog::getText(this, tr("New surface type"), tr("Name"), QLineEdit::Normal, tr("Damp wallpaper"), &ok).trimmed();
    if (!ok || title.isEmpty())
    {
        ListCanvases();
        return;
    }
    m_canvas = QJsonObject{ { QStringLiteral("title"), title },
                            { QStringLiteral("blocks"), QJsonArray{ QJsonObject{ { QStringLiteral("kind"), QStringLiteral("texture") },
                                                                                 { QStringLiteral("label"), tr("Texture") } } } } };
    m_slug = Slug();
    SaveCanvas();
    ListCanvases();
}

void HearthMaterialCanvasPane::AddBlock(const QString& kind)
{
    const CanvasKind* spec = CanvasFind(kind);
    if (!spec)
    {
        return;
    }
    QJsonArray blocks = m_canvas.value(QStringLiteral("blocks")).toArray();
    blocks.append(QJsonObject{ { QStringLiteral("kind"), kind }, { QStringLiteral("label"), tr(spec->label) } });
    m_canvas.insert(QStringLiteral("blocks"), blocks);
    SaveCanvas();
    Rebuild();
}

QWidget* HearthMaterialCanvasPane::BuildBlock(int index, QWidget* parent)
{
    const QJsonArray blocks = m_canvas.value(QStringLiteral("blocks")).toArray();
    const QJsonObject block = blocks[index].toObject();
    const CanvasKind* spec = CanvasFind(block.value(QStringLiteral("kind")).toString());
    QVBoxLayout* layout = nullptr;
    QFrame* card = Hearth::MakeCard(parent, &layout);
    card->setFixedWidth(250);
    auto* top = new QHBoxLayout();
    QLabel* title = Hearth::MakeLabel(block.value(QStringLiteral("label")).toString(), QStringLiteral("title"), card);
    top->addWidget(title, 1);
    QPushButton* more = Hearth::MakeButton(QStringLiteral("⋯"), false, card);
    more->setFixedWidth(30);
    top->addWidget(more);
    layout->addLayout(top);
    connect(more, &QPushButton::clicked, this, [this, index, more]()
    {
        QMenu menu;
        menu.addAction(tr("Rename…"), [this, index]()
        {
            QJsonArray list = m_canvas.value(QStringLiteral("blocks")).toArray();
            QJsonObject b = list[index].toObject();
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Rename block"), tr("Name"), QLineEdit::Normal, b.value(QStringLiteral("label")).toString(), &ok).trimmed();
            if (ok && !name.isEmpty())
            {
                b.insert(QStringLiteral("label"), name);
                list[index] = b;
                m_canvas.insert(QStringLiteral("blocks"), list);
                SaveCanvas();
                Rebuild();
            }
        });
        if (index > 0)
        {
            menu.addAction(tr("Move earlier"), [this, index]()
            {
                QJsonArray list = m_canvas.value(QStringLiteral("blocks")).toArray();
                const QJsonValue moved = list.takeAt(index);
                list.insert(index - 1, moved);
                m_canvas.insert(QStringLiteral("blocks"), list);
                SaveCanvas();
                Rebuild();
            });
        }
        menu.addAction(tr("Remove"), [this, index]()
        {
            QJsonArray list = m_canvas.value(QStringLiteral("blocks")).toArray();
            list.removeAt(index);
            m_canvas.insert(QStringLiteral("blocks"), list);
            SaveCanvas();
            Rebuild();
        });
        menu.exec(more->mapToGlobal(QPoint(0, more->height())));
    });
    if (!spec)
    {
        return card;
    }
    QLabel* hint = Hearth::MakeLabel(tr(spec->hint), QStringLiteral("small"), card);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    const QJsonObject params = block.value(QStringLiteral("params")).toObject();
    QStringList summary;
    for (const CanvasParam& param : spec->params)
    {
        if (!param.key)
        {
            continue;
        }
        const QString key = QLatin1String(param.key);
        const double value = params.value(key).toDouble(param.fallback);
        auto* row = new QHBoxLayout();
        row->addWidget(Hearth::MakeLabel(tr(param.label), QStringLiteral("small"), card));
        QLabel* shown = Hearth::MakeLabel(QStringLiteral("%1%2").arg(value, 0, 'f', 2).arg(QLatin1String(param.unit)), QStringLiteral("mono"), card);
        row->addStretch(1);
        row->addWidget(shown);
        layout->addLayout(row);
        auto* slider = new QSlider(Qt::Horizontal, card);
        slider->setRange(0, 100);
        slider->setValue(qRound((value - param.min) / (param.max - param.min) * 100.0));
        layout->addWidget(slider);
        const double low = param.min;
        const double high = param.max;
        const QString unit = QLatin1String(param.unit);
        connect(slider, &QSlider::valueChanged, this, [shown, low, high, unit](int position)
        {
            shown->setText(QStringLiteral("%1%2").arg(low + (high - low) * position / 100.0, 0, 'f', 2).arg(unit));
        });
        connect(slider, &QSlider::sliderReleased, this, [this, index, key, slider, low, high]()
        {
            QJsonArray list = m_canvas.value(QStringLiteral("blocks")).toArray();
            QJsonObject b = list[index].toObject();
            QJsonObject p = b.value(QStringLiteral("params")).toObject();
            p.insert(key, low + (high - low) * slider->value() / 100.0);
            b.insert(QStringLiteral("params"), p);
            list[index] = b;
            m_canvas.insert(QStringLiteral("blocks"), list);
            SaveCanvas();
            m_compile->setText(tr("Changed · save to compile"));
        });
        summary << QStringLiteral("%1 %2%3").arg(tr(param.label).toLower()).arg(value, 0, 'f', 2).arg(QLatin1String(param.unit));
    }
    card->setToolTip(summary.join(QStringLiteral(" · ")));
    return card;
}

void HearthMaterialCanvasPane::Rebuild()
{
    if (m_subtitle)
    {
        m_subtitle->setText(tr("%1 (a new surface type)").arg(m_canvas.value(QStringLiteral("title")).toString()));
    }
    auto* page = new QWidget();
    auto* row = new QHBoxLayout(page);
    row->setContentsMargins(16, 16, 16, 16);
    row->setSpacing(0);
    row->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    const QJsonArray blocks = m_canvas.value(QStringLiteral("blocks")).toArray();
    // Blocks run in order, left to right, into the surface output.
    auto* chain = new QVBoxLayout();
    chain->setSpacing(0);
    for (int i = 0; i < blocks.size(); ++i)
    {
        chain->addWidget(BuildBlock(i, page));
        if (i + 1 < blocks.size())
        {
            auto* line = new QFrame(page);
            line->setFixedSize(2, 16);
            line->setStyleSheet(QStringLiteral("background-color: rgba(232, 116, 59, 140);"));
            chain->addWidget(line, 0, Qt::AlignHCenter);
        }
    }
    chain->addStretch(1);
    row->addLayout(chain);
    auto* arrow = Hearth::MakeLabel(QStringLiteral("  ⟶  "), QStringLiteral("title"), page);
    arrow->setStyleSheet(QStringLiteral("color: #E8743B; font-size: 22px; background: transparent;"));
    row->addWidget(arrow, 0, Qt::AlignVCenter);
    QVBoxLayout* outLayout = nullptr;
    QFrame* output = Hearth::MakeCard(page, &outLayout);
    output->setFixedWidth(220);
    outLayout->addWidget(Hearth::MakeLabel(tr("Surface output"), QStringLiteral("title"), output));
    for (const QString& slot : { tr("Base color"), tr("Roughness"), tr("Normal") })
    {
        outLayout->addWidget(Hearth::MakeChip(slot, QStringLiteral("ember"), output));
    }
    row->addWidget(output, 0, Qt::AlignVCenter);
    row->addStretch(1);
    m_scroll->setWidget(page);
    m_status->setText(tr("Blocks work top to bottom: Noise and Height make the patches, Blend darkens where they are, Flicker makes it "
                         "stutter. Saved blocks: Materials/Canvas/%1.json").arg(m_slug.isEmpty() ? Slug() : m_slug));
}

QJsonArray HearthMaterialCanvasPane::BlockProperties() const
{
    QJsonArray properties;
    const QJsonArray blocks = m_canvas.value(QStringLiteral("blocks")).toArray();
    for (int i = 0; i < blocks.size(); ++i)
    {
        const QJsonObject block = blocks[i].toObject();
        const CanvasKind* spec = CanvasFind(block.value(QStringLiteral("kind")).toString());
        if (!spec)
        {
            continue;
        }
        for (const CanvasParam& param : spec->params)
        {
            if (!param.key)
            {
                continue;
            }
            const QString name = QStringLiteral("b%1_%2").arg(i).arg(QLatin1String(param.key));
            properties.append(QJsonObject{
                { QStringLiteral("name"), name },
                { QStringLiteral("displayName"), QStringLiteral("%1: %2").arg(block.value(QStringLiteral("label")).toString(), tr(param.label)) },
                { QStringLiteral("type"), QStringLiteral("Float") },
                { QStringLiteral("defaultValue"), block.value(QStringLiteral("params")).toObject().value(QLatin1String(param.key)).toDouble(param.fallback) },
                { QStringLiteral("min"), param.min },
                { QStringLiteral("max"), param.max },
                { QStringLiteral("connection"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("ShaderInput") },
                                                            { QStringLiteral("name"), QStringLiteral("m_hearth_%1").arg(name) } } } });
        }
    }
    return properties;
}

QString HearthMaterialCanvasPane::ShaderCode() const
{
    QString blocksCode;
    const QJsonArray blocks = m_canvas.value(QStringLiteral("blocks")).toArray();
    auto p = [](int i, const char* key) { return QStringLiteral("params.m_hearth_b%1_%2").arg(i).arg(QLatin1String(key)); };
    for (int i = 0; i < blocks.size(); ++i)
    {
        const QString kind = blocks[i].toObject().value(QStringLiteral("kind")).toString();
        blocksCode += QStringLiteral("        // %1\n").arg(blocks[i].toObject().value(QStringLiteral("label")).toString().remove(QLatin1Char('\n')));
        if (kind == QLatin1String("texture"))
        {
            blocksCode += QStringLiteral("        albedo *= real(%1);\n").arg(p(i, "brightness"));
        }
        else if (kind == QLatin1String("noise"))
        {
            blocksCode += QStringLiteral("        {\n            float n = HearthNoise(positionWS * %1);\n"
                                         "            mask *= smoothstep(0.5 - %2, 0.5 + %2, n);\n        }\n").arg(p(i, "scale"), p(i, "softness"));
        }
        else if (kind == QLatin1String("height"))
        {
            blocksCode += QStringLiteral("        {\n            float4x4 world = SceneSrg::GetObjectToWorldMatrix(ObjectSrg::m_objectId);\n"
                                         "            float h = %3 > 0.5 ? distance(positionWS, ViewSrg::m_worldPosition) : positionWS.z - world[2][3];\n"
                                         "            mask *= saturate((%1 - h) / max(%2, 0.01));\n        }\n").arg(p(i, "rise"), p(i, "softness"), p(i, "fromCamera"));
        }
        else if (kind == QLatin1String("blend"))
        {
            blocksCode += QStringLiteral("        {\n            float m = smoothstep(0.0, max(%2, 0.001), mask);\n"
                                         "            albedo *= real(1.0 - %1 * m);\n"
                                         "            roughness = lerp(roughness, roughness * real(1.0 - %3), real(m));\n        }\n").arg(p(i, "amount"), p(i, "softness"), p(i, "shine"));
        }
        else if (kind == QLatin1String("flicker"))
        {
            blocksCode += QStringLiteral("        {\n            float t = SceneSrg::m_time * %2;\n"
                                         "            float stutter = step(0.7, HearthNoise(float3(t, floor(t * 0.37), 0.0)));\n"
                                         "            albedo *= real(1.0 - %1 * stutter);\n        }\n").arg(p(i, "amount"), p(i, "speed"));
        }
    }
    return QStringLiteral(
        "// Made by Hearth Engine's Material canvas from Materials/Canvas/%1.json. Saving there writes this file again.\n"
        "#include <Atom/Feature/Common/Assets/Shaders/Materials/BasePBR/BasePBR_MaterialInputs.azsli>\n\n"
        "#if MATERIALPIPELINE_SHADER_HAS_PIXEL_STAGE\n\n"
        "    COMMON_OPTIONS_BASE_COLOR()\n    COMMON_OPTIONS_VERTEX_COLOR()\n    COMMON_OPTIONS_ROUGHNESS()\n"
        "    COMMON_OPTIONS_METALLIC()\n    COMMON_OPTIONS_SPECULAR_F0()\n    COMMON_OPTIONS_NORMAL()\n\n"
        "    bool NeedsTangentFrame()\n    {\n        return o_normal_useTexture;\n    }\n\n"
        "    #include <Atom/Feature/Common/Assets/Shaders/Materials/BasePBR/BasePBR_VertexEval.azsli>\n"
        "    #include <Atom/Feature/Common/Assets/Shaders/Materials/BasePBR/BasePBR_PixelGeometryEval.azsli>\n\n"
        "    // The blocks go on top of Base PBR's surface.\n"
        "    #define EvaluateSurface EvaluateSurface_Hearth\n"
        "    #include <Atom/Feature/Common/Assets/Shaders/Materials/BasePBR/BasePBR_SurfaceEval.azsli>\n\n"
        "    float HearthHash(float3 p)\n    {\n        p = frac(p * 0.3183099 + 0.1);\n        p *= 17.0;\n"
        "        return frac(p.x * p.y * p.z * (p.x + p.y + p.z));\n    }\n\n"
        "    float HearthNoise(float3 x)\n    {\n        float3 i = floor(x);\n        float3 f = frac(x);\n        f = f * f * (3.0 - 2.0 * f);\n"
        "        return lerp(lerp(lerp(HearthHash(i), HearthHash(i + float3(1, 0, 0)), f.x),\n"
        "                         lerp(HearthHash(i + float3(0, 1, 0)), HearthHash(i + float3(1, 1, 0)), f.x), f.y),\n"
        "                    lerp(lerp(HearthHash(i + float3(0, 0, 1)), HearthHash(i + float3(1, 0, 1)), f.x),\n"
        "                         lerp(HearthHash(i + float3(0, 1, 1)), HearthHash(i + float3(1, 1, 1)), f.x), f.y), f.z);\n    }\n\n"
        "    void HearthBlocks(const MaterialParameters params, float3 positionWS, inout Surface surface)\n    {\n"
        "        float mask = 1.0;\n        real3 albedo = surface.albedo;\n        real roughness = surface.roughnessLinear;\n\n"
        "%2\n"
        "        surface.albedo = albedo;\n        surface.roughnessLinear = roughness;\n        surface.CalculateRoughnessA();\n    }\n\n"
        "    Surface EvaluateSurface_Hearth(const MaterialParameters params, float3 positionWS, real3 vertexNormal, float3 tangents[UvSetCount],\n"
        "        float3 bitangents[UvSetCount], float2 uvs[UvSetCount], bool isFrontFace, float4 uvDxDy, bool customDerivatives, float4 vertexColor, uint lightingChannels)\n    {\n"
        "        Surface surface = EvaluateSurface_BasePBR(params, positionWS, vertexNormal, tangents, bitangents, uvs, isFrontFace, uvDxDy, customDerivatives, vertexColor, lightingChannels);\n"
        "        HearthBlocks(params, positionWS, surface);\n        return surface;\n    }\n\n"
        "    Surface EvaluateSurface_Hearth(VsOutput IN, VsSystemValues SV, PixelGeometryData geoData, const MaterialParameters params, float4 uvDxDy, bool customDerivatives)\n    {\n"
        "        Surface surface = EvaluateSurface_BasePBR(IN, SV, geoData, params, uvDxDy, customDerivatives);\n"
        "        HearthBlocks(params, geoData.positionWS, surface);\n        return surface;\n    }\n\n"
        "    Surface EvaluateSurface_Hearth(VsOutput IN, VsSystemValues SV, PixelGeometryData geoData, const MaterialParameters params)\n    {\n"
        "        Surface surface = EvaluateSurface_BasePBR(IN, SV, geoData, params);\n"
        "        HearthBlocks(params, geoData.positionWS, surface);\n        return surface;\n    }\n\n"
        "    #include <Atom/Feature/Common/Assets/Shaders/Materials/BasePBR/BasePBR_LightingBrdf.azsli>\n"
        "    #include <Atom/Feature/Common/Assets/Shaders/Materials/BasePBR/BasePBR_LightingEval.azsli>\n\n"
        "#else\n\n"
        "    #include <Atom/Feature/Common/Assets/Shaders/Materials/DepthPass_VertexEval.azsli>\n\n"
        "#endif\n").arg(m_slug, blocksCode);
}

QJsonObject HearthMaterialCanvasPane::BasePbrLayout(QString* problem) const
{
    const QString types = QDir(Hearth::EngineRoot()).filePath(QStringLiteral("Gems/Atom/Feature/Common/Assets/Materials/Types"));
    QFile file(QDir(types).filePath(QStringLiteral("BasePBR.materialtype")));
    if (!file.open(QIODevice::ReadOnly))
    {
        *problem = tr("Can't read Base PBR from the engine (%1).").arg(QDir::toNativeSeparators(file.fileName()));
        return {};
    }
    return CanvasResolveImports(QJsonDocument::fromJson(file.readAll()).object(), QDir(types)).toObject();
}

void HearthMaterialCanvasPane::SaveAsMaterialType()
{
    if (m_slug.isEmpty())
    {
        m_slug = Slug();
    }
    SaveCanvas();
    QString problem;
    QJsonObject type = BasePbrLayout(&problem);
    if (type.isEmpty())
    {
        m_status->setText(problem);
        return;
    }
    // Base PBR's settings, then a group with one setting per block.
    QJsonObject layout = type.value(QStringLiteral("propertyLayout")).toObject();
    QJsonArray groups = layout.value(QStringLiteral("propertyGroups")).toArray();
    groups.append(QJsonObject{ { QStringLiteral("name"), QStringLiteral("hearthBlocks") },
                               { QStringLiteral("displayName"), tr("Blocks (Material canvas)") },
                               { QStringLiteral("description"), tr("Made from the blocks in Materials/Canvas/%1.json").arg(m_slug) },
                               { QStringLiteral("properties"), BlockProperties() } });
    layout.insert(QStringLiteral("propertyGroups"), groups);
    type.insert(QStringLiteral("propertyLayout"), layout);
    type.insert(QStringLiteral("description"), tr("%1, made in Hearth Engine's Material canvas").arg(m_canvas.value(QStringLiteral("title")).toString()));
    type.insert(QStringLiteral("materialShaderDefines"), m_slug + QStringLiteral("_Defines.azsli"));
    type.insert(QStringLiteral("materialShaderCode"), m_slug + QStringLiteral(".azsli"));
    type.remove(QStringLiteral("versionUpdates"));

    const QDir folder(QDir(ProjectFolder()).filePath(QStringLiteral("Materials/Types/Hearth")));
    QDir().mkpath(folder.absolutePath());
    auto write = [](const QString& path, const QByteArray& data)
    {
        QFile out(path);
        return out.open(QIODevice::WriteOnly | QIODevice::Truncate) && out.write(data) == data.size();
    };
    // The defines are Base PBR's own.
    QFile defines(QDir(QDir(Hearth::EngineRoot()).filePath(QStringLiteral("Gems/Atom/Feature/Common/Assets/Materials/Types"))).filePath(QStringLiteral("BasePBR_Defines.azsli")));
    const QByteArray definesText = defines.open(QIODevice::ReadOnly) ? defines.readAll() : QByteArray();
    const bool ok = write(folder.filePath(m_slug + QStringLiteral(".materialtype")), QJsonDocument(type).toJson(QJsonDocument::Indented))
        && write(folder.filePath(m_slug + QStringLiteral("_Defines.azsli")), definesText)
        && write(folder.filePath(m_slug + QStringLiteral(".azsli")), ShaderCode().toUtf8());
    if (!ok || definesText.isEmpty())
    {
        m_status->setText(tr("Couldn't write the material type into %1.").arg(QDir::toNativeSeparators(folder.absolutePath())));
        return;
    }
    // A material of the new type to put on things (kept if it's there already, so its textures stay).
    const QString materialPath = QDir(ProjectFolder()).filePath(QStringLiteral("Materials/Hearth/%1.material").arg(m_slug));
    if (!QFileInfo::exists(materialPath))
    {
        QDir().mkpath(QFileInfo(materialPath).absolutePath());
        write(materialPath, QJsonDocument(QJsonObject{ { QStringLiteral("materialType"), QStringLiteral("../Types/Hearth/%1.materialtype").arg(m_slug) },
                                                       { QStringLiteral("materialTypeVersion"), 1 },
                                                       { QStringLiteral("propertyValues"), QJsonObject() } })
                                .toJson(QJsonDocument::Indented));
    }
    m_compile->setText(tr("Compiling…"));
    m_status->setText(tr("Saved Materials/Types/Hearth/%1.materialtype and a material to use it, Materials/Hearth/%1.material.").arg(m_slug));
    m_compileTries = 0;
    QTimer::singleShot(1500, this, &HearthMaterialCanvasPane::CheckCompile);
}

QString HearthMaterialCanvasPane::Weight() const
{
    // Rough cost per pixel: noise is the heavy part (8 hashes), the rest is a few instructions.
    int cost = 0;
    for (const QJsonValue& value : m_canvas.value(QStringLiteral("blocks")).toArray())
    {
        const QString kind = value.toObject().value(QStringLiteral("kind")).toString();
        cost += kind == QLatin1String("noise") || kind == QLatin1String("flicker") ? 3 : 1;
    }
    return cost <= 6 ? tr("light on this laptop") : (cost <= 12 ? tr("medium on this laptop") : tr("heavy on this laptop"));
}

void HearthMaterialCanvasPane::CheckCompile()
{
    if (m_slug.isEmpty())
    {
        return;
    }
    const QString source = QDir(ProjectFolder()).filePath(QStringLiteral("Materials/Types/Hearth/%1.materialtype").arg(m_slug));
    if (!QFileInfo::exists(source))
    {
        m_compile->setText(tr("Not saved as a material type yet"));
        return;
    }
    using namespace AzToolsFramework::AssetSystem;
    AZ::Outcome<JobInfoContainer> jobs = AZ::Failure();
    AzToolsFramework::AssetSystemJobRequestBus::BroadcastResult(jobs, &AzToolsFramework::AssetSystemJobRequestBus::Events::GetAssetJobsInfo,
                                                                AZStd::string(source.toUtf8().constData()), false);
    bool failed = false;
    bool working = !jobs.IsSuccess() || jobs.GetValue().empty();
    AZ::u64 failedRun = 0;
    if (jobs.IsSuccess())
    {
        for (const JobInfo& job : jobs.GetValue())
        {
            if (job.m_status == JobStatus::Failed)
            {
                failed = true;
                failedRun = job.m_jobRunKey;
            }
            working |= job.m_status == JobStatus::InProgress || job.m_status == JobStatus::Queued;
        }
    }
    if (failed)
    {
        m_compile->setText(tr("Didn't compile"));
        AZ::Outcome<AZStd::string> log = AZ::Failure();
        AzToolsFramework::AssetSystemJobRequestBus::BroadcastResult(log, &AzToolsFramework::AssetSystemJobRequestBus::Events::GetJobLog, failedRun);
        const QString text = log.IsSuccess() ? QString::fromUtf8(log.GetValue().c_str()) : QString();
        QStringList errors;
        for (const QString& line : text.split(QLatin1Char('\n')))
        {
            if (line.contains(QLatin1String("error"), Qt::CaseInsensitive) && errors.size() < 3)
            {
                errors << line.trimmed().left(200);
            }
        }
        m_status->setText(tr("The shader didn't compile. %1\nDescribe a look for Claude can fix it: it reads the same log.").arg(errors.join(QLatin1Char('\n'))));
        return;
    }
    if (working && ++m_compileTries < 120)
    {
        m_compile->setText(tr("Compiling…"));
        QTimer::singleShot(2000, this, &HearthMaterialCanvasPane::CheckCompile);
        return;
    }
    m_compile->setText(tr("Compiles for Vulkan · %1").arg(Weight()));
}
