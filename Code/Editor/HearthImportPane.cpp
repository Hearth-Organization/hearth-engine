/*
 * Hearth Engine Import. Built on O3DE (Apache-2.0 OR MIT). See HearthImportPane.h.
 */

#include "EditorDefs.h"

#include "HearthImportPane.h"
#include "HearthAssetBrowser.h"
#include "QtViewPaneManager.h"

#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtEndian>
#include <QtMath>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    QString ImportProject()
    {
        return QString::fromUtf8(AZ::Utils::GetProjectPath().c_str());
    }

    QString ImportKind(const QString& suffix)
    {
        static const QStringList textures = { QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("tga"), QStringLiteral("tif"),
                                              QStringLiteral("tiff"), QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("exr"), QStringLiteral("dds") };
        static const QStringList models = { QStringLiteral("fbx"), QStringLiteral("gltf"), QStringLiteral("glb"), QStringLiteral("obj") };
        static const QStringList sounds = { QStringLiteral("wav"), QStringLiteral("ogg"), QStringLiteral("mp3"), QStringLiteral("flac") };
        const QString s = suffix.toLower();
        if (textures.contains(s)) return QStringLiteral("TEXTURE");
        if (models.contains(s)) return QStringLiteral("MODEL");
        if (sounds.contains(s)) return QStringLiteral("SOUND");
        if (s == QLatin1String("material")) return QStringLiteral("MATERIAL");
        if (s == QLatin1String("prefab")) return QStringLiteral("PREFAB");
        return QString();
    }

    //! Image formats the engine can't read but ffmpeg can turn into a PNG.
    bool ImportConvertible(const QString& suffix)
    {
        static const QStringList list = { QStringLiteral("webp"), QStringLiteral("psd"), QStringLiteral("dds"), QStringLiteral("avif"),
                                          QStringLiteral("heic"), QStringLiteral("jxl"), QStringLiteral("pcx"), QStringLiteral("ppm") };
        return list.contains(suffix.toLower());
    }

    QString ImportFolderFor(const QString& kind)
    {
        if (kind == QLatin1String("TEXTURE")) return QStringLiteral("Assets/Textures");
        if (kind == QLatin1String("MODEL")) return QStringLiteral("Assets/Models");
        if (kind == QLatin1String("SOUND")) return QStringLiteral("Assets/Sounds");
        if (kind == QLatin1String("MATERIAL")) return QStringLiteral("Assets/Materials");
        if (kind == QLatin1String("PREFAB")) return QStringLiteral("Prefabs");
        return QStringLiteral("Assets");
    }

    //! The engine's image preset for a texture name (the same name endings the image builder uses).
    QString ImportPreset(const QString& stem, QString* channel = nullptr)
    {
        static const QVector<QPair<QString, QStringList>> presets = {
            { QStringLiteral("Normals"), { "_ddn", "_n", "_nm", "_nor", "_norm", "_normal", "_normalmap", "_normals", "_nrm" } },
            { QStringLiteral("Reflectance"), { "_r", "_rough", "_roughness", "_rgh", "_m", "_metal", "_metallic", "_metalness", "_gloss", "_g", "_mtl" } },
            { QStringLiteral("AmbientOcclusion"), { "_ao", "_amb", "_ambocc", "_ambientocclusion", "_occlusion" } },
            { QStringLiteral("Emissive"), { "_e", "_em", "_emissive", "_emit", "_glow" } },
            { QStringLiteral("Displacement"), { "_h", "_height", "_disp", "_displacement", "_d" } },
            { QStringLiteral("Opacity"), { "_o", "_op", "_opacity", "_mask", "_alpha" } },
            { QStringLiteral("Albedo"), { "_alb", "_albedo", "_basecolor", "_basecolour", "_bc", "_col", "_color", "_colour", "_diff", "_diffuse" } },
        };
        const QString lower = stem.toLower();
        for (const auto& preset : presets)
        {
            for (const QString& ending : preset.second)
            {
                if (lower.endsWith(ending))
                {
                    if (channel)
                    {
                        *channel = ending;
                    }
                    return preset.first;
                }
            }
        }
        if (channel)
        {
            channel->clear();
        }
        return QStringLiteral("Albedo");
    }

    //! "carpet_damp_01_normal" -> "carpet_damp" (the material a set of textures belongs to).
    QString ImportMaterialName(const QString& stem)
    {
        QString channel;
        ImportPreset(stem, &channel);
        QString base = channel.isEmpty() ? stem : stem.left(stem.size() - channel.size());
        base.remove(QRegularExpression(QStringLiteral("[_-]?\\d+$")));
        return base.isEmpty() ? stem : base;
    }

    quint32 ImportU32(const QByteArray& data, int at)
    {
        return at + 4 <= data.size() ? qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + at)) : 0;
    }

    struct ImportModelInfo
    {
        int meshes = 0;
        int materials = 0;
        int animations = 0;
        bool rig = false;
        double sizeMetres = 0.0;
        bool read = false;
    };

    void ImportGrow(double (&low)[3], double (&high)[3], double x, double y, double z)
    {
        const double v[3] = { x, y, z };
        for (int axis = 0; axis < 3; ++axis)
        {
            low[axis] = qMin(low[axis], v[axis]);
            high[axis] = qMax(high[axis], v[axis]);
        }
    }

    double ImportLargest(const double (&low)[3], const double (&high)[3])
    {
        double size = 0.0;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (high[axis] >= low[axis])
            {
                size = qMax(size, high[axis] - low[axis]);
            }
        }
        return size;
    }

    ImportModelInfo ImportReadFbx(const QByteArray& data)
    {
        ImportModelInfo info;
        double low[3] = { 1e30, 1e30, 1e30 };
        double high[3] = { -1e30, -1e30, -1e30 };
        double unit = 1.0; // centimetres per file unit
        if (data.startsWith("Kaydara FBX Binary"))
        {
            info.meshes = data.count(QByteArray("\x00\x01Geometry", 10));
            info.materials = data.count(QByteArray("\x00\x01Material", 10));
            info.animations = data.count(QByteArray("\x00\x01" "AnimStack", 11));
            info.rig = data.contains(QByteArray("\x00\x01" "Deformer", 10));
            const int unitAt = data.indexOf("UnitScaleFactor");
            if (unitAt >= 0)
            {
                const int valueAt = data.indexOf(QByteArray("S\x00\x00\x00\x00" "D", 6), unitAt);
                if (valueAt > 0 && valueAt - unitAt < 96 && valueAt + 14 <= data.size())
                {
                    unit = qFromLittleEndian<double>(reinterpret_cast<const uchar*>(data.constData() + valueAt + 6));
                }
            }
            int at = 0;
            while ((at = data.indexOf(QByteArray("\x08Vertices", 9), at)) >= 0)
            {
                at += 9;
                if (at >= data.size() || data[at] != 'd')
                {
                    continue;
                }
                const quint32 count = ImportU32(data, at + 1);
                const quint32 encoding = ImportU32(data, at + 5);
                const quint32 compressed = ImportU32(data, at + 9);
                QByteArray values = data.mid(at + 13, int(compressed));
                if (encoding == 1)
                {
                    QByteArray prefixed(4, '\0');
                    qToBigEndian<quint32>(count * 8, reinterpret_cast<uchar*>(prefixed.data()));
                    values = qUncompress(prefixed + values);
                }
                const int doubles = qMin(int(count), values.size() / 8);
                for (int i = 0; i + 2 < doubles; i += 3)
                {
                    const auto* p = reinterpret_cast<const uchar*>(values.constData()) + i * 8;
                    ImportGrow(low, high, qFromLittleEndian<double>(p), qFromLittleEndian<double>(p + 8), qFromLittleEndian<double>(p + 16));
                }
            }
        }
        else
        {
            const QString text = QString::fromUtf8(data);
            info.meshes = text.count(QStringLiteral("\"Geometry::"));
            info.materials = text.count(QStringLiteral("\"Material::"));
            info.animations = text.count(QStringLiteral("\"AnimStack::"));
            info.rig = text.contains(QStringLiteral("\"Deformer::"));
            const QRegularExpressionMatch unitMatch = QRegularExpression(QStringLiteral("\"UnitScaleFactor\",\\s*\"double\",\\s*\"Number\",\\s*\"\",\\s*([-0-9.eE]+)")).match(text);
            if (unitMatch.hasMatch())
            {
                unit = unitMatch.captured(1).toDouble();
            }
            QRegularExpressionMatchIterator blocks = QRegularExpression(QStringLiteral("Vertices:\\s*\\*\\d+\\s*\\{\\s*a:\\s*([^}]*)\\}")).globalMatch(text);
            while (blocks.hasNext())
            {
                const QStringList numbers = blocks.next().captured(1).split(QLatin1Char(','), Qt::SkipEmptyParts);
                for (int i = 0; i + 2 < numbers.size(); i += 3)
                {
                    ImportGrow(low, high, numbers[i].trimmed().toDouble(), numbers[i + 1].trimmed().toDouble(), numbers[i + 2].trimmed().toDouble());
                }
            }
        }
        info.sizeMetres = ImportLargest(low, high) * (unit > 0 ? unit : 1.0) / 100.0;
        info.read = info.meshes > 0 || info.sizeMetres > 0;
        return info;
    }

    ImportModelInfo ImportReadGltf(const QByteArray& data)
    {
        ImportModelInfo info;
        QJsonObject json;
        if (data.startsWith("glTF"))
        {
            const quint32 length = ImportU32(data, 12);
            json = QJsonDocument::fromJson(data.mid(20, int(length))).object();
        }
        else
        {
            json = QJsonDocument::fromJson(data).object();
        }
        const QJsonArray meshes = json.value(QStringLiteral("meshes")).toArray();
        const QJsonArray accessors = json.value(QStringLiteral("accessors")).toArray();
        info.meshes = meshes.size();
        info.materials = json.value(QStringLiteral("materials")).toArray().size();
        info.animations = json.value(QStringLiteral("animations")).toArray().size();
        info.rig = !json.value(QStringLiteral("skins")).toArray().isEmpty();
        double low[3] = { 1e30, 1e30, 1e30 };
        double high[3] = { -1e30, -1e30, -1e30 };
        for (const QJsonValue mesh : meshes)
        {
            for (const QJsonValue primitive : mesh.toObject().value(QStringLiteral("primitives")).toArray())
            {
                const int index = primitive.toObject().value(QStringLiteral("attributes")).toObject().value(QStringLiteral("POSITION")).toInt(-1);
                const QJsonObject accessor = accessors.at(index).toObject();
                const QJsonArray min = accessor.value(QStringLiteral("min")).toArray();
                const QJsonArray max = accessor.value(QStringLiteral("max")).toArray();
                if (min.size() == 3 && max.size() == 3)
                {
                    ImportGrow(low, high, min[0].toDouble(), min[1].toDouble(), min[2].toDouble());
                    ImportGrow(low, high, max[0].toDouble(), max[1].toDouble(), max[2].toDouble());
                }
            }
        }
        info.sizeMetres = ImportLargest(low, high);
        info.read = !json.isEmpty();
        return info;
    }

    ImportModelInfo ImportReadObj(const QByteArray& data)
    {
        ImportModelInfo info;
        double low[3] = { 1e30, 1e30, 1e30 };
        double high[3] = { -1e30, -1e30, -1e30 };
        QSet<QByteArray> materials;
        int groups = 0;
        for (const QByteArray& line : data.split('\n'))
        {
            if (line.startsWith("v "))
            {
                const QList<QByteArray> parts = line.simplified().split(' ');
                if (parts.size() >= 4)
                {
                    ImportGrow(low, high, parts[1].toDouble(), parts[2].toDouble(), parts[3].toDouble());
                }
            }
            else if (line.startsWith("o ") || line.startsWith("g "))
            {
                ++groups;
            }
            else if (line.startsWith("usemtl "))
            {
                materials.insert(line.mid(7).trimmed());
            }
        }
        info.meshes = qMax(1, groups);
        info.materials = materials.size();
        info.sizeMetres = ImportLargest(low, high);
        info.read = true;
        return info;
    }

    QString ImportSoundAbout(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return QObject::tr("Sound");
        }
        const QByteArray head = file.read(65536);
        double rate = 0.0;
        double seconds = 0.0;
        if (head.startsWith("RIFF") && head.mid(8, 4) == "WAVE")
        {
            quint32 byteRate = 0;
            int at = 12;
            while (at + 8 <= head.size())
            {
                const QByteArray id = head.mid(at, 4);
                const quint32 size = ImportU32(head, at + 4);
                if (id == "fmt ")
                {
                    rate = ImportU32(head, at + 12);
                    byteRate = ImportU32(head, at + 16);
                }
                if (id == "data")
                {
                    seconds = byteRate ? double(size) / byteRate : 0.0;
                    break;
                }
                at += 8 + int(size) + int(size & 1);
            }
        }
        else if (head.startsWith("OggS"))
        {
            const int vorbis = head.indexOf("\x01vorbis");
            rate = vorbis >= 0 ? ImportU32(head, vorbis + 12) : 0;
            const qint64 size = file.size();
            file.seek(qMax<qint64>(0, size - 65536));
            const QByteArray tail = file.readAll();
            const int last = tail.lastIndexOf("OggS");
            if (last >= 0 && last + 14 <= tail.size() && rate > 0)
            {
                seconds = double(qFromLittleEndian<qint64>(reinterpret_cast<const uchar*>(tail.constData() + last + 6))) / rate;
            }
        }
        else if (head.startsWith("fLaC") && head.size() > 26)
        {
            const auto* b = reinterpret_cast<const uchar*>(head.constData());
            rate = (quint32(b[18]) << 12) | (quint32(b[19]) << 4) | (quint32(b[20]) >> 4);
            const quint64 samples = (quint64(b[21] & 0x0F) << 32) | (quint64(b[22]) << 24) | (quint64(b[23]) << 16) | (quint64(b[24]) << 8) | quint64(b[25]);
            seconds = rate > 0 ? double(samples) / rate : 0.0;
        }
        QStringList parts = { QObject::tr("Sound") };
        if (rate > 0)
        {
            parts << QObject::tr("%1 kHz").arg(rate / 1000.0, 0, 'g', 3);
        }
        const bool loop = QFileInfo(path).completeBaseName().contains(QLatin1String("loop"), Qt::CaseInsensitive);
        if (seconds > 0)
        {
            const QString length = seconds >= 60 ? QObject::tr("%n min", nullptr, qRound(seconds / 60.0)) : QObject::tr("%1 s").arg(seconds, 0, 'f', 1);
            parts << (loop ? QObject::tr("%1 loop").arg(length) : length);
        }
        else if (loop)
        {
            parts << QObject::tr("loop");
        }
        return parts.join(QStringLiteral(" · "));
    }

    QSize ImportImageSize(const QString& path)
    {
        QSize size = QImageReader(path).size();
        if (!size.isValid() && path.endsWith(QLatin1String(".dds"), Qt::CaseInsensitive))
        {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly))
            {
                const QByteArray head = file.read(32);
                if (head.startsWith("DDS "))
                {
                    size = QSize(int(ImportU32(head, 16)), int(ImportU32(head, 12)));
                }
            }
        }
        return size;
    }

    qint64 ImportId()
    {
        return qint64(QRandomGenerator::global()->generate64() & ((quint64(1) << 53) - 1));
    }

    QJsonObject ImportComponent(const QString& type)
    {
        return QJsonObject{ { QStringLiteral("$type"), type }, { QStringLiteral("Id"), ImportId() } };
    }

    //! The editor components every prefab entity carries.
    void ImportBasics(QJsonObject& components, const QString& parent, double scale)
    {
        for (const char* type : { "EditorEntitySortComponent", "EditorInspectorComponent", "EditorLockComponent", "EditorOnlyEntityComponent",
                                  "EditorPendingCompositionComponent", "EditorVisibilityComponent", "EditorDisabledCompositionComponent",
                                  "EditorEntityIconComponent" })
        {
            components.insert(QString::fromLatin1(type), ImportComponent(QString::fromLatin1(type)));
        }
        QJsonObject transform = ImportComponent(QStringLiteral("{27F1E1A1-8D9D-4C3B-BD3A-AFB9762449C0} TransformComponent"));
        transform.insert(QStringLiteral("Parent Entity"), parent);
        if (!qFuzzyCompare(scale, 1.0))
        {
            transform.insert(QStringLiteral("Transform Data"), QJsonObject{ { QStringLiteral("UniformScale"), scale } });
        }
        components.insert(QStringLiteral("TransformComponent"), transform);
    }

    bool ImportWriteJson(const QString& path, const QJsonObject& json)
    {
        QDir().mkpath(QFileInfo(path).path());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            return false;
        }
        file.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
        return true;
    }

    //! A prefab holding one entity with the model on it (scaled when the import fixes the scale).
    bool ImportWritePrefab(const QString& prefabPath, const QString& name, const QString& modelHint, double scale)
    {
        const QString entityId = QStringLiteral("Entity_[%1]").arg(100000000000 + (ImportId() % 899999999999));
        QJsonObject container;
        container.insert(QStringLiteral("Id"), QStringLiteral("ContainerEntity"));
        container.insert(QStringLiteral("Name"), name);
        QJsonObject containerComponents;
        ImportBasics(containerComponents, QString(), 1.0);
        QJsonObject sort = containerComponents.value(QStringLiteral("EditorEntitySortComponent")).toObject();
        sort.insert(QStringLiteral("Child Entity Order"), QJsonArray{ entityId });
        containerComponents.insert(QStringLiteral("EditorEntitySortComponent"), sort);
        containerComponents.insert(QStringLiteral("EditorPrefabComponent"), ImportComponent(QStringLiteral("EditorPrefabComponent")));
        container.insert(QStringLiteral("Components"), containerComponents);

        QJsonObject entity;
        entity.insert(QStringLiteral("Id"), entityId);
        entity.insert(QStringLiteral("Name"), name);
        QJsonObject components;
        ImportBasics(components, QStringLiteral("ContainerEntity"), scale);
        QJsonObject mesh = ImportComponent(QStringLiteral("AZ::Render::EditorMeshComponent"));
        // No id yet (the model hasn't been processed): the prefab loader finds it by its hint.
        mesh.insert(QStringLiteral("Controller"),
                    QJsonObject{ { QStringLiteral("Configuration"),
                                   QJsonObject{ { QStringLiteral("ModelAsset"),
                                                  QJsonObject{ { QStringLiteral("assetId"),
                                                                 QJsonObject{ { QStringLiteral("guid"), QStringLiteral("{00000000-0000-0000-0000-000000000000}") },
                                                                              { QStringLiteral("subId"), 0 } } },
                                                               { QStringLiteral("assetHint"), modelHint } } } } } });
        components.insert(QStringLiteral("AZ::Render::EditorMeshComponent"), mesh);
        entity.insert(QStringLiteral("Components"), components);

        QJsonObject prefab;
        prefab.insert(QStringLiteral("ContainerEntity"), container);
        prefab.insert(QStringLiteral("Entities"), QJsonObject{ { entityId, entity } });
        return ImportWriteJson(prefabPath, prefab);
    }

    bool ImportJobFailed(const QString& path)
    {
        using namespace AzToolsFramework::AssetSystem;
        AZ::Outcome<JobInfoContainer> jobs = AZ::Failure();
        AzToolsFramework::AssetSystemJobRequestBus::BroadcastResult(jobs, &AzToolsFramework::AssetSystemJobRequestBus::Events::GetAssetJobsInfo,
                                                                    AZStd::string(path.toUtf8().constData()), false);
        if (!jobs.IsSuccess())
        {
            return false;
        }
        for (const JobInfo& job : jobs.GetValue())
        {
            if (job.m_status == JobStatus::Failed || job.m_status == JobStatus::Failed_InvalidSourceNameExceedsMaxLimit)
            {
                return true;
            }
        }
        return false;
    }

    QStringList ImportAreas()
    {
        QStringList areas;
        const QDir project(ImportProject());
        for (const QString& folder : { QStringLiteral("Assets/Materials"), QStringLiteral("Assets/Textures"), QStringLiteral("Assets/Models"),
                                       QStringLiteral("Assets/Sounds"), QStringLiteral("Prefabs"), QStringLiteral("Levels") })
        {
            areas << QDir(project.filePath(folder)).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        }
        areas.removeDuplicates();
        areas.sort(Qt::CaseInsensitive);
        return areas;
    }

    QString ImportChipTone(const QString& state)
    {
        if (state == QLatin1String("ready")) return QStringLiteral("good");
        if (state == QLatin1String("wont")) return QStringLiteral("bad");
        return QStringLiteral("warn");
    }
} // namespace

void HearthImportPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(140, 90, 1200, 760);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthImportPane>("Import", "Tools", options);
}

HearthImportPane::HearthImportPane(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthImport"));
    setStyleSheet(QStringLiteral("QWidget#HearthImport { background-color: #0D0C0B; }"));
    setAcceptDrops(true);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Import"), tr("Pick files, or drop them here"));
    if (auto* titles = header->itemAt(1) ? header->itemAt(1)->layout() : nullptr)
    {
        if (titles->count() > 1)
        {
            m_subtitle = qobject_cast<QLabel*>(titles->itemAt(1)->widget());
        }
    }
    header->addStretch(1);
    QPushButton* more = Hearth::MakeButton(tr("Add more files"), false, this);
    connect(more, &QPushButton::clicked, this, [this]()
    {
        const QString start = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        const QStringList picked = QFileDialog::getOpenFileNames(this, tr("Add files"), start);
        if (!picked.isEmpty())
        {
            AddFiles(picked);
        }
    });
    header->addWidget(more);
    root->addLayout(header);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);

    // Files.
    QVBoxLayout* filesLayout = nullptr;
    QFrame* filesCard = Hearth::MakeCard(this, &filesLayout);
    auto* filesTitle = new QHBoxLayout();
    filesTitle->addWidget(Hearth::MakeLabel(tr("Files"), QStringLiteral("title"), filesCard));
    filesTitle->addStretch(1);
    m_count = Hearth::MakeLabel(QString(), QStringLiteral("dim"), filesCard);
    filesTitle->addWidget(m_count);
    filesLayout->addLayout(filesTitle);
    auto* drop = new QPushButton(tr("Drop files or folders here"), filesCard);
    drop->setCursor(Qt::PointingHandCursor);
    drop->setStyleSheet(QStringLiteral("QPushButton { border: 1.5px dashed rgba(232, 116, 59, 115); border-radius: 12px; padding: 16px; color: #8D857B; background: transparent; }"
                                       "QPushButton:hover { background-color: rgba(232, 116, 59, 15); color: #FFCFB4; }"));
    connect(drop, &QPushButton::clicked, more, &QPushButton::click);
    filesLayout->addWidget(drop);
    auto* scroll = new QScrollArea(filesCard);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; } QScrollArea > QWidget > QWidget { background: transparent; }"));
    auto* rowsHolder = new QWidget(scroll);
    m_rows = new QVBoxLayout(rowsHolder);
    m_rows->setContentsMargins(0, 0, 0, 0);
    m_rows->setSpacing(4);
    m_rows->addStretch(1);
    scroll->setWidget(rowsHolder);
    filesLayout->addWidget(scroll, 1);
    body->addWidget(filesCard, 1);

    // Where they go and options.
    QVBoxLayout* sideLayout = nullptr;
    QFrame* side = Hearth::MakeCard(this, &sideLayout);
    side->setFixedWidth(440);
    sideLayout->addWidget(Hearth::MakeLabel(tr("Where they go"), QStringLiteral("title"), side));
    m_area = new QComboBox(side);
    m_area->setEditable(true);
    m_area->setStyleSheet(QStringLiteral("QComboBox { font-family: \"Geist Mono\"; }"));
    m_areaHint = Hearth::MakeLabel(QString(), QStringLiteral("small"), side);
    m_areaHint->setWordWrap(true);
    sideLayout->addWidget(m_area);
    sideLayout->addWidget(m_areaHint);
    connect(m_area, &QComboBox::currentTextChanged, this, [this]() { Fill(); });
    sideLayout->addSpacing(6);
    sideLayout->addWidget(Hearth::MakeLabel(tr("Options"), QStringLiteral("title"), side));

    QScopedPointer<QSettings> settings(Hearth::NewSettings());
    auto toggle = [this, side, &settings](const QString& key, bool fallback)
    {
        auto* t = new Hearth::Toggle(side);
        t->setChecked(settings->value(key, fallback).toBool());
        connect(t, &QAbstractButton::toggled, this, [this, key](bool on)
        {
            QScopedPointer<QSettings> s(Hearth::NewSettings());
            s->setValue(key, on);
            Fill();
        });
        return t;
    };
    m_scale = new QDoubleSpinBox(side);
    m_scale->setDecimals(4);
    m_scale->setRange(0.0001, 1000.0);
    m_scale->setSuffix(QStringLiteral(" ×"));
    m_scale->setValue(1.0);
    m_scale->setStyleSheet(QStringLiteral("QDoubleSpinBox { color: #F0D17A; font-family: \"Geist Mono\"; }"));
    m_scale->hide(); // only shown in its row, when a model looks the wrong size (it sat over the heading otherwise)
    m_makeMaterials = toggle(QStringLiteral("import/makeMaterials"), true);
    m_makePrefabs = toggle(QStringLiteral("import/makePrefabs"), true);
    m_compress = toggle(QStringLiteral("import/compressTextures"), true);
    m_maxSize = new QComboBox(side);
    m_maxSize->addItem(tr("Full size"), 0);
    for (int size : { 4096, 2048, 1024, 512 })
    {
        m_maxSize->addItem(QString::number(size), size);
    }
    const bool laptop = settings->value(QStringLiteral("laptop/preset"), QStringLiteral("light")).toString() == QLatin1String("light");
    const int savedMax = settings->value(QStringLiteral("import/maxTextureSize"), laptop ? 1024 : 0).toInt();
    m_maxSize->setCurrentIndex(qMax(0, m_maxSize->findData(savedMax)));
    connect(m_maxSize, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]()
    {
        QScopedPointer<QSettings> s(Hearth::NewSettings());
        s->setValue(QStringLiteral("import/maxTextureSize"), m_maxSize->currentData().toInt());
    });
    auto* optionsHolder = new QWidget(side);
    m_options = new QVBoxLayout(optionsHolder);
    m_options->setContentsMargins(0, 0, 0, 0);
    m_options->setSpacing(0);
    sideLayout->addWidget(optionsHolder);
    sideLayout->addStretch(1);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    QPushButton* cancel = Hearth::MakeButton(tr("Cancel"), false, side);
    connect(cancel, &QPushButton::clicked, this, [this]()
    {
        m_files.clear();
        Fill();
        QtViewPaneManager::instance()->ClosePane(QStringLiteral("Import"));
    });
    m_import = Hearth::MakeButton(tr("Import"), true, side);
    connect(m_import, &QPushButton::clicked, this, &HearthImportPane::Import);
    buttons->addWidget(cancel);
    buttons->addWidget(m_import);
    sideLayout->addLayout(buttons);
    body->addWidget(side);
    root->addLayout(body, 1);

    m_areas = ImportAreas();
    m_area->addItems(m_areas);
    const QString lastArea = settings->value(QStringLiteral("import/area")).toString();
    if (!lastArea.isEmpty())
    {
        m_area->setCurrentText(lastArea);
    }
    Fill();
}

void HearthImportPane::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
    {
        event->acceptProposedAction();
    }
}

void HearthImportPane::dropEvent(QDropEvent* event)
{
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls())
    {
        if (url.isLocalFile())
        {
            paths << url.toLocalFile();
        }
    }
    AddFiles(paths, true);
    event->acceptProposedAction();
}

void HearthImportPane::AddFiles(const QStringList& paths, bool fromDrop)
{
    m_fromDrop = m_fromDrop || fromDrop;
    for (const QString& path : paths)
    {
        QStringList found;
        if (QFileInfo(path).isDir())
        {
            QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext())
            {
                const QString file = it.next();
                const QString suffix = QFileInfo(file).suffix();
                if (!ImportKind(suffix).isEmpty() || ImportConvertible(suffix))
                {
                    found << file;
                }
            }
        }
        else
        {
            found << path;
        }
        for (const QString& file : found)
        {
            const bool already = std::any_of(m_files.begin(), m_files.end(), [&file](const HearthImportFile& f) { return f.source == file; });
            if (!already)
            {
                HearthImportFile entry;
                entry.source = QFileInfo(file).absoluteFilePath();
                m_files << entry;
            }
        }
    }
    Fill();
}

QString HearthImportPane::Destination(const HearthImportFile& file) const
{
    const QString area = m_area->currentText().trimmed().remove(QLatin1Char('/'));
    QString name = QFileInfo(file.source).fileName();
    if (file.convertible && file.state == QLatin1String("wont"))
    {
        name = QFileInfo(file.source).completeBaseName() + QStringLiteral(".png");
    }
    const QString kind = file.kind.isEmpty() && file.convertible ? QStringLiteral("TEXTURE") : file.kind;
    return QDir(ImportProject()).filePath(ImportFolderFor(kind) + (area.isEmpty() ? QString() : QLatin1Char('/') + area) + QLatin1Char('/') + name);
}

void HearthImportPane::Check(HearthImportFile& file) const
{
    const QFileInfo info(file.source);
    const QString suffix = info.suffix().toLower();
    file.kind = ImportKind(suffix);
    file.convertible = ImportConvertible(suffix);
    file.state = QStringLiteral("ready");
    file.problem.clear();
    file.scaleFix = 1.0;
    file.scaleTimes = 1;
    const QString goes = QFileInfo(QDir(ImportProject()).relativeFilePath(Destination(file))).path().section(QLatin1Char('/'), file.kind == QLatin1String("PREFAB") ? 0 : 1);

    // The same file already in the project that the Asset Processor keeps failing on.
    const QString existing = Destination(file);
    file.failingInProject = QFileInfo::exists(existing) && ImportJobFailed(existing);

    if (file.kind.isEmpty())
    {
        file.state = QStringLiteral("wont");
        file.problem = tr("format not supported");
        file.about = (file.convertible ? tr("Image") : tr("File")) + QStringLiteral(" · ") + file.problem;
        return;
    }
    if (file.kind == QLatin1String("TEXTURE"))
    {
        const QSize size = ImportImageSize(file.source);
        file.textureSize = qMax(size.width(), size.height());
        const QString sizeText = !size.isValid() ? QString() : (size.width() == size.height() ? QStringLiteral("%1²").arg(size.width()) : QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()));
        if (file.failingInProject)
        {
            file.state = QStringLiteral("wont");
            file.problem = tr("format not supported");
            file.about = tr("Texture · %1").arg(file.problem);
            return;
        }
        const QString materialName = ImportMaterialName(info.completeBaseName());
        const bool noMaterialYet = !QDirIterator(QDir(ImportProject()).filePath(QStringLiteral("Assets/Materials")), { materialName + QStringLiteral(".material") },
                                               QDir::Files, QDirIterator::Subdirectories).hasNext();
        const bool colourMap = ImportPreset(info.completeBaseName()) == QLatin1String("Albedo");
        QStringList parts = { tr("Texture") };
        if (!sizeText.isEmpty())
        {
            parts << sizeText;
        }
        parts << (m_makeMaterials->isChecked() && colourMap && noMaterialYet ? tr("makes a %1 material").arg(materialName) : tr("goes to %1").arg(goes));
        file.about = parts.join(QStringLiteral(" · "));
    }
    else if (file.kind == QLatin1String("MODEL"))
    {
        // Reading a big model takes a moment: once per file (until it changes).
        static QHash<QString, QPair<QDateTime, ImportModelInfo>> cache;
        ImportModelInfo read;
        const auto hit = cache.constFind(file.source);
        if (hit != cache.constEnd() && hit.value().first == info.lastModified())
        {
            read = hit.value().second;
        }
        else
        {
            QFile model(file.source);
            if (model.size() < 512ll * 1024 * 1024 && model.open(QIODevice::ReadOnly))
            {
                const QByteArray data = model.readAll();
                read = suffix == QLatin1String("fbx") ? ImportReadFbx(data) : (suffix == QLatin1String("obj") ? ImportReadObj(data) : ImportReadGltf(data));
            }
            cache.insert(file.source, { info.lastModified(), read });
        }
        QStringList parts = { tr("Model") };
        if (read.rig)
        {
            parts << (read.animations > 0 ? tr("rig + %n animation(s)", nullptr, read.animations) : tr("rig"));
        }
        else if (read.read)
        {
            parts << tr("%n mesh(es)", nullptr, qMax(1, read.meshes));
            if (read.materials > 0)
            {
                parts << tr("%n material(s)", nullptr, read.materials);
            }
        }
        file.about = parts.join(QStringLiteral(" · "));
        if (read.sizeMetres > 40.0)
        {
            file.scaleTimes = int(qPow(10.0, qRound(std::log10(read.sizeMetres / 2.0))));
            file.scaleFix = 1.0 / file.scaleTimes;
        }
        else if (read.sizeMetres > 0.0 && read.sizeMetres < 0.02)
        {
            file.scaleTimes = -int(qPow(10.0, qRound(std::log10(2.0 / read.sizeMetres))));
            file.scaleFix = -1.0 * file.scaleTimes;
        }
        if (qAbs(file.scaleTimes) >= 10)
        {
            file.state = QStringLiteral("scale");
        }
    }
    else if (file.kind == QLatin1String("SOUND"))
    {
        file.about = ImportSoundAbout(file.source);
    }
    else if (file.kind == QLatin1String("MATERIAL"))
    {
        file.about = tr("Material · goes to %1").arg(goes);
    }
    else
    {
        file.about = tr("Prefab · goes to %1").arg(goes);
    }
    if (file.state == QLatin1String("ready") && QFileInfo::exists(existing))
    {
        file.state = QStringLiteral("replace");
    }
}

void HearthImportPane::Fill()
{
    for (HearthImportFile& file : m_files)
    {
        Check(file);
    }
    // Rows.
    while (m_rows->count() > 1)
    {
        QLayoutItem* item = m_rows->takeAt(0);
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
    int ready = 0;
    int check = 0;
    int wont = 0;
    for (int i = 0; i < m_files.size(); ++i)
    {
        const HearthImportFile& file = m_files[i];
        auto* row = new QFrame();
        row->setObjectName(QStringLiteral("ImportRow"));
        row->setStyleSheet(QStringLiteral("QFrame#ImportRow { border-radius: 10px; } QFrame#ImportRow:hover { background-color: rgba(255, 255, 255, 8); }"));
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(12);
        auto* swatch = new QLabel(row);
        swatch->setFixedSize(34, 34);
        QPixmap thumb;
        if (file.kind == QLatin1String("TEXTURE"))
        {
            QImageReader reader(file.source);
            reader.setScaledSize(QSize(68, 68));
            thumb = QPixmap::fromImage(reader.read());
        }
        if (!thumb.isNull())
        {
            QPixmap rounded(68, 68);
            rounded.fill(Qt::transparent);
            QPainter painter(&rounded);
            painter.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            path.addRoundedRect(QRectF(0, 0, 68, 68), 16, 16);
            painter.setClipPath(path);
            painter.drawPixmap(0, 0, thumb.scaled(68, 68, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
            painter.end();
            rounded.setDevicePixelRatio(2.0);
            swatch->setPixmap(rounded);
        }
        else
        {
            const QString colour = file.state == QLatin1String("wont") ? QStringLiteral("#3a1d14") : (file.kind == QLatin1String("SOUND") ? QStringLiteral("#3a3530") : QStringLiteral("#2c2824"));
            swatch->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 8px;").arg(colour));
        }
        layout->addWidget(swatch);
        auto* texts = new QVBoxLayout();
        texts->setSpacing(2);
        texts->addWidget(Hearth::MakeLabel(QFileInfo(file.source).fileName(), QStringLiteral("mono"), row));
        texts->addWidget(Hearth::MakeLabel(file.about, QStringLiteral("small"), row));
        layout->addLayout(texts, 1);
        QString chip = tr("Ready");
        if (file.state == QLatin1String("scale"))
        {
            chip = tr("Check scale");
            ++check;
        }
        else if (file.state == QLatin1String("wont"))
        {
            chip = tr("Won't import");
            ++wont;
        }
        else if (file.state == QLatin1String("replace"))
        {
            chip = tr("Replaces the old one");
            ++check;
        }
        else
        {
            ++ready;
        }
        layout->addWidget(Hearth::MakeChip(chip, ImportChipTone(file.state), row));
        auto* remove = new QPushButton(QStringLiteral("×"), row);
        remove->setToolTip(tr("Leave this one out"));
        remove->setFixedSize(22, 22);
        remove->setStyleSheet(QStringLiteral("QPushButton { border: none; color: #8D857B; background: transparent; font-size: 15px; } QPushButton:hover { color: #ECE6DE; }"));
        connect(remove, &QPushButton::clicked, this, [this, i]()
        {
            if (i < m_files.size())
            {
                m_files.remove(i);
                QTimer::singleShot(0, this, &HearthImportPane::Fill);
            }
        });
        layout->addWidget(remove);
        m_rows->insertWidget(m_rows->count() - 1, row);
    }
    QStringList counts;
    if (ready) counts << tr("%1 ready").arg(ready);
    if (check) counts << tr("%1 to check").arg(check);
    if (wont) counts << tr("%1 won't import").arg(wont);
    m_count->setText(counts.join(QStringLiteral(" · ")));
    if (m_subtitle)
    {
        m_subtitle->setText(m_files.isEmpty() ? tr("Pick files, or drop them here")
                                              : (m_fromDrop ? tr("%n file(s) dropped onto Assets", nullptr, m_files.size()) : tr("%n file(s) picked", nullptr, m_files.size())));
    }
    const QString area = m_area->currentText().trimmed().remove(QLatin1Char('/'));
    m_areaHint->setText(area.isEmpty() ? tr("Textures go to Assets/Textures, models to Assets/Models, sounds to Assets/Sounds.")
                                       : tr("Textures go to Assets/Textures/%1, models to Assets/Models/%1, sounds to Assets/Sounds/%1, prefabs to Prefabs/%1.").arg(area));
    FillOptions();
    int importable = 0;
    for (const HearthImportFile& file : m_files)
    {
        const bool converting = file.convertible && file.state == QLatin1String("wont")
            && std::any_of(m_converts.begin(), m_converts.end(), [&file](const auto& c) { return c.first == file.source && c.second->isChecked(); });
        importable += (file.state != QLatin1String("wont") || converting) ? 1 : 0;
    }
    m_import->setText(tr("Import %n file(s)", nullptr, importable));
    m_import->setEnabled(importable > 0);
}

void HearthImportPane::FillOptions()
{
    // Remember which conversions were switched off before rebuilding the rows.
    QSet<QString> convertOff;
    for (const auto& convert : m_converts)
    {
        if (!convert.second->isChecked())
        {
            convertOff.insert(convert.first);
        }
    }
    m_converts.clear();
    while (QLayoutItem* item = m_options->takeAt(0))
    {
        if (QWidget* widget = item->widget())
        {
            // Keep the reusable controls; only the rows around them go.
            for (QWidget* keep : std::initializer_list<QWidget*>{ m_scale, m_makeMaterials, m_makePrefabs, m_compress, m_maxSize })
            {
                if (keep->parentWidget() && keep->parentWidget()->parentWidget() == widget)
                {
                    keep->setParent(m_options->parentWidget());
                    keep->hide();
                }
            }
            widget->deleteLater();
        }
        delete item;
    }
    auto addRow = [this](const QString& label, const QString& hint, QWidget* control)
    {
        auto* row = new QFrame(m_options->parentWidget());
        row->setObjectName(QStringLiteral("ImportOption"));
        row->setStyleSheet(QStringLiteral("QFrame#ImportOption { border-top: 1px solid rgba(255, 255, 255, 15); }"));
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(4, 9, 4, 9);
        auto* texts = new QVBoxLayout();
        texts->setSpacing(2);
        texts->addWidget(Hearth::MakeLabel(label, QStringLiteral("body"), row));
        if (!hint.isEmpty())
        {
            texts->addWidget(Hearth::MakeLabel(hint, QStringLiteral("small"), row));
        }
        layout->addLayout(texts, 1);
        auto* holder = new QWidget(row);
        auto* holderLayout = new QHBoxLayout(holder);
        holderLayout->setContentsMargins(0, 0, 0, 0);
        control->setParent(holder);
        control->show();
        holderLayout->addWidget(control);
        layout->addWidget(holder);
        m_options->addWidget(row);
    };

    // Scale: the model that looks the wrongest decides the suggestion.
    const HearthImportFile* worst = nullptr;
    for (const HearthImportFile& file : m_files)
    {
        if (file.state == QLatin1String("scale") && (!worst || qAbs(file.scaleTimes) > qAbs(worst->scaleTimes)))
        {
            worst = &file;
        }
    }
    if (worst)
    {
        static QString suggestedFor;
        if (suggestedFor != worst->source)
        {
            suggestedFor = worst->source;
            m_scale->setValue(worst->scaleFix);
        }
        const QString hint = worst->scaleTimes > 0 ? tr("%1 looks %2× too big").arg(QFileInfo(worst->source).fileName()).arg(worst->scaleTimes)
                                                   : tr("%1 looks %2× too small").arg(QFileInfo(worst->source).fileName()).arg(-worst->scaleTimes);
        addRow(tr("Scale"), hint + (m_makePrefabs->isChecked() ? QString() : tr(" (applied in its prefab: turn on Make prefabs)")), m_scale);
    }
    addRow(tr("Make materials from textures"), tr("Finds base colour, normal and roughness"), m_makeMaterials);
    addRow(tr("Make prefabs for models"), QString(), m_makePrefabs);
    addRow(tr("Compress textures"), tr("Smaller, a little softer"), m_compress);
    addRow(tr("Max texture size on laptop"), QString(), m_maxSize);
    for (const HearthImportFile& file : m_files)
    {
        if (file.convertible && file.state == QLatin1String("wont"))
        {
            auto* toggle = new Hearth::Toggle(m_options->parentWidget());
            toggle->setChecked(!convertOff.contains(file.source));
            connect(toggle, &QAbstractButton::toggled, this, [this]() { QTimer::singleShot(0, this, &HearthImportPane::Fill); });
            m_converts.append({ file.source, toggle });
            addRow(tr("Convert %1 to PNG").arg(QFileInfo(file.source).completeBaseName()),
                   file.failingInProject ? tr("Fixes the file that keeps failing") : tr("The engine can't read .%1 files").arg(QFileInfo(file.source).suffix().toLower()), toggle);
        }
    }
}

void HearthImportPane::Import()
{
    const QString project = ImportProject();
    const QString area = m_area->currentText().trimmed().remove(QLatin1Char('/'));
    {
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        settings->setValue(QStringLiteral("import/area"), area);
    }
    QStringList done;
    QStringList problems;
    QStringList importedTextures;
    QString firstImported;
    for (const HearthImportFile& file : m_files)
    {
        const bool converting = file.convertible && file.state == QLatin1String("wont")
            && std::any_of(m_converts.begin(), m_converts.end(), [&file](const auto& c) { return c.first == file.source && c.second->isChecked(); });
        if (file.state == QLatin1String("wont") && !converting)
        {
            continue;
        }
        const QString target = Destination(file);
        QDir().mkpath(QFileInfo(target).path());
        if (converting)
        {
            // Keep the project's failing copy out of the Asset Processor's way (moved, not deleted).
            const QString failing = QDir(QFileInfo(target).path()).filePath(QFileInfo(file.source).fileName());
            if (QFileInfo::exists(failing))
            {
                const QString backup = QDir(project).filePath(QStringLiteral("user/ImportBackup/") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
                QDir().mkpath(backup);
                QFile::rename(failing, QDir(backup).filePath(QFileInfo(failing).fileName()));
            }
            QProcess ffmpeg;
            ffmpeg.start(QStringLiteral("ffmpeg"), { QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-i"), file.source,
                                                     QStringLiteral("-frames:v"), QStringLiteral("1"), target });
            if (!ffmpeg.waitForFinished(120000) || ffmpeg.exitCode() != 0 || !QFileInfo::exists(target))
            {
                problems << tr("%1: couldn't convert (%2)").arg(QFileInfo(file.source).fileName(), QString::fromUtf8(ffmpeg.readAllStandardError()).trimmed().left(160));
                continue;
            }
        }
        else
        {
            if (QFileInfo::exists(target))
            {
                QFile::remove(target);
            }
            if (!QFile::copy(file.source, target))
            {
                problems << tr("%1: couldn't copy it into the project").arg(QFileInfo(file.source).fileName());
                continue;
            }
        }
        done << target;
        firstImported = firstImported.isEmpty() ? target : firstImported;
        const QString kind = converting ? QStringLiteral("TEXTURE") : file.kind;
        if (kind == QLatin1String("TEXTURE"))
        {
            importedTextures << target;
            // Texture settings: compression and the laptop's max size, written next to the texture for the image builder.
            const int maxSize = m_maxSize->currentData().toInt();
            const int textureSize = converting ? qMax(ImportImageSize(target).width(), ImportImageSize(target).height()) : file.textureSize;
            int reduce = 0;
            while (maxSize > 0 && textureSize > 0 && (textureSize >> reduce) > maxSize && reduce < 6)
            {
                ++reduce;
            }
            const QString preset = ImportPreset(QFileInfo(target).completeBaseName());
            const bool uncompressed = !m_compress->isChecked() && preset == QLatin1String("Albedo");
            const QString infoPath = target + QStringLiteral(".assetinfo");
            if ((reduce > 0 || uncompressed) && !QFileInfo::exists(infoPath))
            {
                QJsonObject data{ { QStringLiteral("Preset"), uncompressed ? QStringLiteral("ReferenceImage") : preset }, { QStringLiteral("SizeReduceLevel"), reduce } };
                ImportWriteJson(infoPath, QJsonObject{ { QStringLiteral("Type"), QStringLiteral("JsonSerialization") }, { QStringLiteral("Version"), 1 },
                                                       { QStringLiteral("ClassName"), QStringLiteral("TextureSettings") }, { QStringLiteral("ClassData"), data } });
            }
        }
        else if (kind == QLatin1String("MODEL") && m_makePrefabs->isChecked())
        {
            const QString name = QFileInfo(target).completeBaseName();
            const QString prefab = QDir(project).filePath(QStringLiteral("Prefabs") + (area.isEmpty() ? QString() : QLatin1Char('/') + area) + QLatin1Char('/') + name + QStringLiteral(".prefab"));
            if (!QFileInfo::exists(prefab))
            {
                const double scale = file.state == QLatin1String("scale") ? m_scale->value() : 1.0;
                const QString hint = QDir(project).relativeFilePath(target).toLower() + QStringLiteral(".azmodel");
                if (!ImportWritePrefab(prefab, name, hint, scale))
                {
                    problems << tr("%1: couldn't write its prefab").arg(name);
                }
            }
        }
    }

    // Materials from textures: one StandardPBR material per set (base colour, normal, roughness, metallic, occlusion).
    if (m_makeMaterials->isChecked())
    {
        QMap<QString, QMap<QString, QString>> sets;
        for (const QString& texture : importedTextures)
        {
            const QString stem = QFileInfo(texture).completeBaseName();
            QString channel;
            const QString preset = ImportPreset(stem, &channel);
            const QString relative = QDir(project).relativeFilePath(texture);
            const QString name = ImportMaterialName(stem);
            if (preset == QLatin1String("Albedo"))
            {
                sets[name][QStringLiteral("baseColor.textureMap")] = relative;
            }
            else if (preset == QLatin1String("Normals"))
            {
                sets[name][QStringLiteral("normal.textureMap")] = relative;
            }
            else if (preset == QLatin1String("Reflectance"))
            {
                const bool metal = channel.contains(QLatin1String("m")) && !channel.contains(QLatin1String("rough"));
                sets[name][metal ? QStringLiteral("metallic.textureMap") : QStringLiteral("roughness.textureMap")] = relative;
            }
            else if (preset == QLatin1String("AmbientOcclusion"))
            {
                sets[name][QStringLiteral("occlusion.diffuseTextureMap")] = relative;
            }
            else if (preset == QLatin1String("Emissive"))
            {
                sets[name][QStringLiteral("emissive.textureMap")] = relative;
                sets[name][QStringLiteral("emissive.enable")] = QStringLiteral("true");
            }
        }
        for (auto it = sets.begin(); it != sets.end(); ++it)
        {
            if (!it.value().contains(QStringLiteral("baseColor.textureMap")))
            {
                continue; // a normal map alone doesn't make a material
            }
            const QString path = QDir(project).filePath(QStringLiteral("Assets/Materials") + (area.isEmpty() ? QString() : QLatin1Char('/') + area) + QLatin1Char('/') + it.key() + QStringLiteral(".material"));
            if (QFileInfo::exists(path))
            {
                continue;
            }
            QJsonObject values;
            for (auto value = it.value().begin(); value != it.value().end(); ++value)
            {
                values.insert(value.key(), value.value() == QLatin1String("true") ? QJsonValue(true) : QJsonValue(value.value()));
            }
            ImportWriteJson(path, QJsonObject{ { QStringLiteral("materialType"), QStringLiteral("@gemroot:Atom_Feature_Common@/Assets/Materials/Types/StandardPBR.materialtype") },
                                               { QStringLiteral("materialTypeVersion"), 5 },
                                               { QStringLiteral("propertyValues"), values } });
            done << path;
        }
    }

    if (!problems.isEmpty())
    {
        QMessageBox::warning(this, tr("Import"), tr("Imported %n file(s). Some didn't go in:", nullptr, done.size()) + QStringLiteral("\n\n") + problems.join(QLatin1Char('\n')));
    }
    m_files.clear();
    m_fromDrop = false;
    m_areas = ImportAreas();
    const QString keep = m_area->currentText();
    m_area->blockSignals(true);
    m_area->clear();
    m_area->addItems(m_areas);
    m_area->setCurrentText(keep);
    m_area->blockSignals(false);
    Fill();

    // Show what came in, in the asset browser.
    if (const QtViewPane* pane = QtViewPaneManager::instance()->OpenPane(QStringLiteral("Browse assets")))
    {
        if (auto* browser = qobject_cast<HearthAssetBrowser*>(pane->Widget()))
        {
            browser->Rescan(firstImported);
        }
    }
}
