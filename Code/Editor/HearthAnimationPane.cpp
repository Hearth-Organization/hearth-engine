/*
 * Hearth Engine "Animation". Built on O3DE (Apache-2.0 OR MIT). See HearthAnimationPane.h.
 */

#include "EditorDefs.h"

#include "HearthAnimationPane.h"
#include "EditorViewportCamera.h"
#include "QtViewPaneManager.h"

#include <Atom/Feature/Utils/FrameCaptureBus.h>
#include <AzCore/Asset/AssetManagerBus.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/Entity.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>
#include <AzToolsFramework/ActionManager/Action/ActionManagerInterface.h>
#include <AzToolsFramework/Component/EditorComponentAPIBus.h>
#include <Integration/EditorSimpleMotionComponentBus.h>
#include <Integration/SimpleMotionComponentBus.h>

#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <memory>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;
    using Components = AzToolsFramework::EditorComponentAPIRequests;
    using SimpleMotion = EMotionFX::Integration::SimpleMotionComponentRequestBus;

    const char* const AnimWhenChoices[] = { "when it hears you", "when it sees you", "within 3 m", "in a working light", "when cornered",
                                            "when it loses you", "when hurt", "after it finishes" };

    AZ::Uuid AnimComponentType(const char* name)
    {
        AZStd::vector<AZ::Uuid> types;
        AzToolsFramework::EditorComponentAPIBus::BroadcastResult(types, &Components::FindComponentTypeIdsByEntityType,
                                                                 AZStd::vector<AZStd::string>{ name }, Components::EntityType::Game);
        return types.empty() ? AZ::Uuid::CreateNull() : types.front();
    }

    //! Sets a property on one of the entity's components, found by the end of its path ("Draw skeleton").
    bool AnimSetProperty(AZ::EntityId entity, const char* componentName, const char* pathEnd, const AZStd::any& value)
    {
        Components::GetComponentOutcome component = AZ::Failure(AZStd::string());
        AzToolsFramework::EditorComponentAPIBus::BroadcastResult(component, &Components::GetComponentOfType, entity, AnimComponentType(componentName));
        if (!component.IsSuccess())
        {
            return false;
        }
        AZStd::vector<AZStd::string> paths;
        AzToolsFramework::EditorComponentAPIBus::BroadcastResult(paths, &Components::BuildComponentPropertyList, component.GetValue());
        for (const AZStd::string& path : paths)
        {
            if (path.ends_with(pathEnd))
            {
                Components::PropertyOutcome set = AZ::Failure(AZStd::string());
                AzToolsFramework::EditorComponentAPIBus::BroadcastResult(set, &Components::SetComponentProperty, component.GetValue(), AZStd::string_view(path), value);
                return set.IsSuccess();
            }
        }
        return false;
    }

    QString AnimPrettyName(const QString& file, const QString& entity)
    {
        QString name = QFileInfo(file).completeBaseName();
        name = name.section(QLatin1Char('.'), 0, 0);
        // hound_idle_sniffing -> "Idle sniffing"
        const QString prefix = entity.toLower() + QLatin1Char('_');
        if (name.toLower().startsWith(prefix))
        {
            name = name.mid(prefix.size());
        }
        name.replace(QRegularExpression(QStringLiteral("[_\\-]+")), QStringLiteral(" "));
        name = name.trimmed();
        if (!name.isEmpty())
        {
            name[0] = name[0].toUpper();
        }
        return name;
    }
} // namespace

//! The viewport looking at the entity (captured), with a label for what's shown.
class HearthAnimPreview : public QWidget
{
public:
    explicit HearthAnimPreview(QWidget* parent)
        : QWidget(parent)
    {
        setMinimumSize(360, 240);
    }
    QImage m_image;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()), 12, 12);
        p.setClipPath(clip);
        p.fillRect(rect(), QColor(0x0D, 0x0C, 0x0B));
        if (m_image.isNull())
        {
            p.setPen(Hearth::Dim());
            p.drawText(rect(), Qt::AlignCenter, QObject::tr("Pick an entity with an Actor to see it move here"));
            return;
        }
        const QSizeF fill = QSizeF(m_image.size()).scaled(QSizeF(size()), Qt::KeepAspectRatioByExpanding);
        p.drawImage(QRectF(QPointF((width() - fill.width()) / 2.0, (height() - fill.height()) / 2.0), fill), m_image);
    }
};

//! The clip's time line: position, event marks (footsteps), click or drag to move, Shift+click to add or remove a mark.
class HearthAnimScrubber : public QWidget
{
public:
    explicit HearthAnimScrubber(QWidget* parent)
        : QWidget(parent)
    {
        setFixedHeight(34);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QObject::tr("Click or drag to move in the clip. Shift+click adds or removes a footstep sound there."));
    }
    float m_time = 0.0f;
    float m_length = 0.0f;
    bool m_showEvents = true;
    QVector<double> m_events;
    std::function<void(float)> m_onTime;
    std::function<void(float)> m_onToggleEvent;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF track(8, height() / 2.0 - 3, width() - 16, 6);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x2C, 0x28, 0x24));
        p.drawRoundedRect(track, 3, 3);
        if (m_length <= 0.0f)
        {
            return;
        }
        const double share = qBound(0.0, static_cast<double>(m_time / m_length), 1.0);
        p.setBrush(QColor(0xE8, 0x74, 0x3B));
        p.drawRoundedRect(QRectF(track.left(), track.top(), track.width() * share, track.height()), 3, 3);
        if (m_showEvents)
        {
            p.setBrush(QColor(0x9A, 0xB8, 0xE8));
            for (double t : m_events)
            {
                const double x = track.left() + track.width() * qBound(0.0, t / m_length, 1.0);
                p.drawRoundedRect(QRectF(x - 2, 4, 4, height() - 8), 2, 2);
            }
        }
        p.setBrush(QColor(0xEC, 0xE6, 0xDE));
        p.drawEllipse(QPointF(track.left() + track.width() * share, height() / 2.0), 7, 7);
    }
    float TimeAt(double x) const
    {
        return static_cast<float>(qBound(0.0, (x - 8.0) / qMax(1.0, width() - 16.0), 1.0)) * m_length;
    }
    void mousePressEvent(QMouseEvent* event) override
    {
        if ((event->modifiers() & Qt::ShiftModifier) && m_onToggleEvent)
        {
            m_onToggleEvent(TimeAt(event->pos().x()));
        }
        else if (m_onTime)
        {
            m_onTime(TimeAt(event->pos().x()));
        }
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if ((event->buttons() & Qt::LeftButton) && !(event->modifiers() & Qt::ShiftModifier) && m_onTime)
        {
            m_onTime(TimeAt(event->pos().x()));
        }
    }
};

void HearthAnimationPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(130, 80, 1240, 800);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthAnimationPane>("Animation", "Tools", options);
}

HearthAnimationPane::HearthAnimationPane(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthAnimation"));
    setStyleSheet(QStringLiteral("QWidget#HearthAnimation { background-color: #0D0C0B; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Animation"), tr("Pick an entity with an Actor"));
    if (QLayoutItem* titles = header->itemAt(1); titles && titles->layout() && titles->layout()->itemAt(1))
    {
        m_subtitle = qobject_cast<QLabel*>(titles->layout()->itemAt(1)->widget());
    }
    QPushButton* import = Hearth::MakeButton(tr("Import clips"), false, this);
    QPushButton* tryIt = Hearth::MakeButton(tr("Try it in Level 0"), true, this);
    header->addWidget(import);
    header->addWidget(tryIt);
    root->addLayout(header);
    connect(import, &QPushButton::clicked, this, []() { QtViewPaneManager::instance()->OpenPane(QStringLiteral("Import")); });
    connect(tryIt, &QPushButton::clicked, this, [this]()
    {
        // Play the level with this entity in it (Level 0 is where the monsters live).
        const QString level = GetIEditor() ? GetIEditor()->GetLevelName() : QString();
        if (level.compare(QStringLiteral("Level0"), Qt::CaseInsensitive) != 0)
        {
            m_status->setText(tr("Open Level 0 first (File > Open level), with %1 in it.").arg(m_entityName.isEmpty() ? tr("the entity") : m_entityName));
            return;
        }
        SaveSidecar();
        if (auto* actions = AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get())
        {
            actions->TriggerAction("o3de.action.game.play");
        }
    });

    auto* body = new QHBoxLayout();
    body->setSpacing(10);
    root->addLayout(body, 1);

    // Left: clips.
    QVBoxLayout* clipsLayout = nullptr;
    QFrame* clips = Hearth::MakeCard(this, &clipsLayout);
    clips->setFixedWidth(250);
    clipsLayout->addWidget(Hearth::MakeLabel(tr("Clips"), QStringLiteral("title"), clips));
    m_clipList = new QVBoxLayout();
    m_clipList->setSpacing(4);
    clipsLayout->addLayout(m_clipList);
    clipsLayout->addStretch(1);
    clipsLayout->addWidget(Hearth::MakeLabel(tr("Double-click a clip to rename it."), QStringLiteral("small"), clips));
    body->addWidget(clips);

    // Middle: the preview and its controls.
    QVBoxLayout* viewLayout = nullptr;
    QFrame* view = Hearth::MakeCard(this, &viewLayout);
    auto* viewTop = new QHBoxLayout();
    const QStringList modes = { tr("Mesh"), tr("Skeleton"), tr("Both") };
    for (int i = 0; i < 3; ++i)
    {
        m_view[i] = Hearth::MakeButton(modes[i], false, view);
        m_view[i]->setCheckable(true);
        connect(m_view[i], &QPushButton::clicked, this, [this, i]() { SetView(i); });
        viewTop->addWidget(m_view[i]);
    }
    m_view[0]->setChecked(true);
    viewTop->addStretch(1);
    viewLayout->addLayout(viewTop);
    m_preview = new HearthAnimPreview(view);
    viewLayout->addWidget(m_preview, 1);
    auto* controls = new QHBoxLayout();
    QPushButton* play = Hearth::MakeButton(QStringLiteral("❚❚"), true, view);
    play->setFixedWidth(44);
    connect(play, &QPushButton::clicked, this, [this, play]()
    {
        m_playing = !m_playing;
        play->setText(m_playing ? QStringLiteral("❚❚") : QStringLiteral("▶"));
        SimpleMotion::Event(m_entity, &SimpleMotion::Events::SetPlaySpeed, m_playing ? m_speed : 0.0f);
    });
    controls->addWidget(play);
    m_scrubber = new HearthAnimScrubber(view);
    m_scrubber->m_onTime = [this](float seconds)
    {
        SimpleMotion::Event(m_entity, &SimpleMotion::Events::PlayTime, seconds);
        m_scrubber->m_time = seconds;
        m_scrubber->update();
    };
    m_scrubber->m_onToggleEvent = [this](float seconds)
    {
        if (m_clip < 0)
        {
            return;
        }
        QJsonArray events = m_sidecar.value(QStringLiteral("clips")).toObject().value(m_clips[m_clip].path).toObject().value(QStringLiteral("footsteps")).toArray();
        bool removed = false;
        for (int i = 0; i < events.size(); ++i)
        {
            if (qAbs(events[i].toDouble() - seconds) < m_clips[m_clip].seconds * 0.03)
            {
                events.removeAt(i);
                removed = true;
                break;
            }
        }
        if (!removed)
        {
            events.append(qRound(seconds * 100.0) / 100.0);
        }
        ChangeClip(m_clips[m_clip].path, [&events](QJsonObject& data) { data.insert(QStringLiteral("footsteps"), events); });
        PlayClip(m_clip);
    };
    controls->addWidget(m_scrubber, 1);
    m_time = Hearth::MakeLabel(QStringLiteral("0.00 / 0.00 s"), QStringLiteral("mono"), view);
    controls->addWidget(m_time);
    m_speedBox = new QComboBox(view);
    for (const double speed : { 0.25, 0.5, 1.0, 1.5, 2.0 })
    {
        m_speedBox->addItem(tr("Speed %1×").arg(speed), speed);
    }
    m_speedBox->setCurrentIndex(2);
    connect(m_speedBox, qOverload<int>(&QComboBox::activated), this, [this](int)
    {
        m_speed = static_cast<float>(m_speedBox->currentData().toDouble());
        if (m_playing)
        {
            SimpleMotion::Event(m_entity, &SimpleMotion::Events::SetPlaySpeed, m_speed);
        }
    });
    controls->addWidget(m_speedBox);
    viewLayout->addLayout(controls);
    // Which part of the body the clip plays on, and its events.
    auto* parts = new QHBoxLayout();
    m_body = Hearth::MakeButton(tr("Body"), false, view);
    m_legs = Hearth::MakeButton(tr("Legs"), false, view);
    m_events = Hearth::MakeButton(tr("Events: footstep sounds"), false, view);
    for (QPushButton* toggle : { m_body, m_legs, m_events })
    {
        toggle->setCheckable(true);
        toggle->setChecked(true);
        parts->addWidget(toggle);
    }
    parts->addStretch(1);
    viewLayout->addLayout(parts);
    auto partsChanged = [this]()
    {
        if (m_clip < 0)
        {
            return;
        }
        QJsonArray on;
        if (m_body->isChecked())
        {
            on.append(QStringLiteral("body"));
        }
        if (m_legs->isChecked())
        {
            on.append(QStringLiteral("legs"));
        }
        ChangeClip(m_clips[m_clip].path, [&on](QJsonObject& data) { data.insert(QStringLiteral("parts"), on); });
    };
    connect(m_body, &QPushButton::toggled, this, partsChanged);
    connect(m_legs, &QPushButton::toggled, this, partsChanged);
    connect(m_events, &QPushButton::toggled, this, [this](bool on)
    {
        m_scrubber->m_showEvents = on;
        m_scrubber->update();
    });
    body->addWidget(view, 1);

    // Right: what it does when.
    QVBoxLayout* rulesLayout = nullptr;
    QFrame* rules = Hearth::MakeCard(this, &rulesLayout);
    rules->setFixedWidth(340);
    rulesLayout->addWidget(Hearth::MakeLabel(tr("What it does when"), QStringLiteral("title"), rules));
    m_rules = new QVBoxLayout();
    m_rules->setSpacing(6);
    rulesLayout->addLayout(m_rules);
    QPushButton* addRule = Hearth::MakeButton(tr("+ Add a rule"), false, rules);
    connect(addRule, &QPushButton::clicked, this, [this]()
    {
        QJsonArray list = m_sidecar.value(QStringLiteral("rules")).toArray();
        list.append(QJsonObject{ { QStringLiteral("from"), QStringLiteral("Any") },
                                 { QStringLiteral("to"), m_clip >= 0 ? m_clips[m_clip].name : QString() },
                                 { QStringLiteral("when"), QString::fromLatin1(AnimWhenChoices[0]) } });
        m_sidecar.insert(QStringLiteral("rules"), list);
        SaveSidecar();
        ShowRules();
    });
    rulesLayout->addWidget(addRule);
    rulesLayout->addStretch(1);
    QPushButton* describe = Hearth::MakeButton(tr("Describe a behaviour for Claude"), true, rules);
    connect(describe, &QPushButton::clicked, this, [this]()
    {
        bool ok = false;
        const QString what = QInputDialog::getMultiLineText(this, tr("Describe a behaviour for Claude"),
            tr("How should %1 behave? For example: it stalks you when it hears footsteps and flinches at working lights.").arg(m_entityName), QString(), &ok);
        if (!ok || what.trimmed().isEmpty())
        {
            return;
        }
        Hearth::AskClaude(tr("Animation: %1").arg(m_entityName),
                          tr("Entity: %1. Its clips, parts, footstep events and rules are in %2 (\"rules\": from, to, when).\nWhat the owner wants:\n%3")
                              .arg(m_entityName, SidecarPath(), what.trimmed()));
    });
    rulesLayout->addWidget(describe);
    m_status = Hearth::MakeLabel(QString(), QStringLiteral("small"), rules);
    m_status->setWordWrap(true);
    rulesLayout->addWidget(m_status);
    body->addWidget(rules);

    auto* clock = new QTimer(this);
    clock->setInterval(50);
    connect(clock, &QTimer::timeout, this, &HearthAnimationPane::Tick);
    clock->start();
    auto* captures = new QTimer(this);
    captures->setInterval(250);
    connect(captures, &QTimer::timeout, this, &HearthAnimationPane::Capture);
    captures->start();

    AzToolsFramework::ToolsApplicationNotificationBus::Handler::BusConnect();
    AzToolsFramework::EntityIdList selected;
    AzToolsFramework::ToolsApplicationRequestBus::BroadcastResult(selected, &AzToolsFramework::ToolsApplicationRequests::GetSelectedEntities);
    if (!selected.empty())
    {
        PickEntity(selected.front());
    }
}

HearthAnimationPane::~HearthAnimationPane()
{
    AzToolsFramework::ToolsApplicationNotificationBus::Handler::BusDisconnect();
}

void HearthAnimationPane::AfterEntitySelectionChanged(const AzToolsFramework::EntityIdList& newlySelected, const AzToolsFramework::EntityIdList&)
{
    if (newlySelected.size() == 1 && newlySelected.front() != m_entity)
    {
        PickEntity(newlySelected.front());
    }
}

QString HearthAnimationPane::SidecarPath() const
{
    const auto project = AZ::Utils::GetProjectPath();
    QString name = m_entityName.toLower();
    name.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    return QDir(QString::fromUtf8(project.c_str())).filePath(QStringLiteral("Animation/%1.json").arg(name.isEmpty() ? QStringLiteral("entity") : name));
}

void HearthAnimationPane::ChangeClip(const QString& path, const std::function<void(QJsonObject&)>& change)
{
    QJsonObject clips = m_sidecar.value(QStringLiteral("clips")).toObject();
    QJsonObject clip = clips.value(path).toObject();
    change(clip);
    clips.insert(path, clip);
    m_sidecar.insert(QStringLiteral("clips"), clips);
    SaveSidecar();
}

void HearthAnimationPane::SaveSidecar()
{
    if (m_entityName.isEmpty())
    {
        return;
    }
    QDir().mkpath(QFileInfo(SidecarPath()).absolutePath());
    m_sidecar.insert(QStringLiteral("entity"), m_entityName);
    QFile file(SidecarPath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        file.write(QJsonDocument(m_sidecar).toJson(QJsonDocument::Indented));
    }
}

void HearthAnimationPane::PickEntity(AZ::EntityId entity)
{
    const AZ::Uuid motionType = AnimComponentType("Simple Motion");
    bool hasMotion = false;
    if (!motionType.IsNull())
    {
        AzToolsFramework::EditorComponentAPIBus::BroadcastResult(hasMotion, &Components::HasComponentOfType, entity, motionType);
    }
    bool hasActor = false;
    const AZ::Uuid actorType = AnimComponentType("Actor");
    if (!actorType.IsNull())
    {
        AzToolsFramework::EditorComponentAPIBus::BroadcastResult(hasActor, &Components::HasComponentOfType, entity, actorType);
    }
    if (!hasActor)
    {
        return; // keep showing the last one
    }
    if (!hasMotion && !motionType.IsNull())
    {
        // The window plays clips through a Simple Motion component: add one (an Anim Graph still wins in the game).
        AzToolsFramework::ScopedUndoBatch undo("Add Simple Motion for the Animation window");
        Components::AddComponentsOutcome added = AZ::Failure(AZStd::string());
        AzToolsFramework::EditorComponentAPIBus::BroadcastResult(added, &Components::AddComponentOfType, entity, motionType);
    }
    m_entity = entity;
    AZStd::string name;
    AZ::ComponentApplicationBus::BroadcastResult(name, &AZ::ComponentApplicationRequests::GetEntityName, entity);
    m_entityName = QString::fromUtf8(name.c_str());
    m_sidecar = {};
    QFile file(SidecarPath());
    if (file.open(QIODevice::ReadOnly))
    {
        m_sidecar = QJsonDocument::fromJson(file.readAll()).object();
    }
    EMotionFX::Integration::EditorSimpleMotionComponentRequestBus::Event(entity, &EMotionFX::Integration::EditorSimpleMotionComponentRequests::SetPreviewInEditor, true);
    // Look at it from the front, a few metres away.
    AZ::Transform at = AZ::Transform::CreateIdentity();
    AZ::TransformBus::EventResult(at, entity, &AZ::TransformBus::Events::GetWorldTM);
    const AZ::Vector3 eye = at.GetTranslation() + at.GetBasisY().GetNormalized() * 4.0f + AZ::Vector3(0.0f, 0.0f, 1.4f);
    const AZ::Vector3 look = (at.GetTranslation() + AZ::Vector3(0.0f, 0.0f, 0.8f) - eye).GetNormalized();
    SandboxEditor::InterpolateDefaultViewportCameraToTransform(AZ::Transform::CreateLookAt(eye, eye + look), 0.4f);
    FindClips();
    ShowRules();
    PlayClip(m_clips.isEmpty() ? -1 : 0);
}

void HearthAnimationPane::FindClips()
{
    // The entity's clips: motions named after it (hound_run.motion for "Hound"), or the ones listed in its file;
    // when none match, every motion in the project so one can be picked.
    m_clips.clear();
    const QString key = m_entityName.toLower().section(QLatin1Char(' '), 0, 0);
    const QJsonObject known = m_sidecar.value(QStringLiteral("clips")).toObject();
    QVector<Clip> all;
    AZ::Data::AssetCatalogRequests::AssetEnumerationCB collect = [this, &all, &key, &known](const AZ::Data::AssetId id, const AZ::Data::AssetInfo& info)
    {
        const QString path = QString::fromUtf8(info.m_relativePath.c_str());
        if (!path.endsWith(QLatin1String(".motion"), Qt::CaseInsensitive))
        {
            return;
        }
        Clip clip;
        clip.id = id;
        clip.path = path;
        all.push_back(clip);
        if ((!key.isEmpty() && QFileInfo(path).fileName().toLower().contains(key)) || known.contains(path))
        {
            m_clips.push_back(clip);
        }
    };
    AZ::Data::AssetCatalogRequestBus::Broadcast(&AZ::Data::AssetCatalogRequests::EnumerateAssets, nullptr, collect, nullptr);
    if (m_clips.isEmpty())
    {
        m_clips = all;
    }
    const QJsonObject names = m_sidecar.value(QStringLiteral("clips")).toObject();
    for (Clip& clip : m_clips)
    {
        clip.name = names.value(clip.path).toObject().value(QStringLiteral("name")).toString(AnimPrettyName(clip.path, m_entityName));
        EMotionFX::Integration::EditorSimpleMotionComponentRequestBus::EventResult(
            clip.seconds, m_entity, &EMotionFX::Integration::EditorSimpleMotionComponentRequests::GetAssetDuration, clip.id);
    }
    std::sort(m_clips.begin(), m_clips.end(), [](const Clip& a, const Clip& b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
    if (m_subtitle)
    {
        m_subtitle->setText(tr("%1 (entity) · %n clip(s)", nullptr, m_clips.size()).arg(m_entityName));
    }
    ShowClips();
}

void HearthAnimationPane::ShowClips()
{
    while (QLayoutItem* item = m_clipList->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    for (int i = 0; i < m_clips.size(); ++i)
    {
        auto* row = new QPushButton(this);
        row->setCheckable(true);
        row->setChecked(i == m_clip);
        row->setCursor(Qt::PointingHandCursor);
        row->setMinimumHeight(38);
        row->setStyleSheet(QStringLiteral("QPushButton { background-color: rgba(255, 255, 255, 6); border: none; border-radius: 10px; text-align: left; padding: 0 12px; color: #ECE6DE; }"
                                          "QPushButton:checked { background-color: rgba(232, 116, 59, 46); }"));
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(12, 0, 12, 0);
        QLabel* name = Hearth::MakeLabel(m_clips[i].name, QStringLiteral("body"), row);
        name->setAttribute(Qt::WA_TransparentForMouseEvents);
        QLabel* length = Hearth::MakeLabel(m_clips[i].seconds > 0.0f ? tr("%1 s").arg(m_clips[i].seconds, 0, 'f', 1) : QStringLiteral("…"), QStringLiteral("mono"), row);
        length->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(name, 1);
        layout->addWidget(length);
        connect(row, &QPushButton::clicked, this, [this, i]() { PlayClip(i); });
        m_clipList->addWidget(row);
        // Double-click renames: a second click on the playing clip.
        connect(row, &QPushButton::pressed, this, [this, i]()
        {
            static QElapsedTimer last;
            static int lastIndex = -1;
            if (lastIndex == i && last.isValid() && last.elapsed() < 400)
            {
                bool ok = false;
                const QString name = QInputDialog::getText(this, tr("Rename clip"), tr("Name"), QLineEdit::Normal, m_clips[i].name, &ok).trimmed();
                if (ok && !name.isEmpty())
                {
                    ChangeClip(m_clips[i].path, [&name](QJsonObject& data) { data.insert(QStringLiteral("name"), name); });
                    m_clips[i].name = name;
                    QTimer::singleShot(0, this, &HearthAnimationPane::ShowClips);
                    ShowRules();
                }
            }
            lastIndex = i;
            last.restart();
        });
    }
    if (m_clips.isEmpty())
    {
        m_clipList->addWidget(Hearth::MakeLabel(m_entity.IsValid() ? tr("No clips next to this actor yet. Import clips brings them in.")
                                                                   : tr("Pick an entity with an Actor."),
                                                QStringLiteral("small"), this));
    }
}

void HearthAnimationPane::PlayClip(int index)
{
    m_clip = index;
    if (index < 0 || index >= m_clips.size())
    {
        m_scrubber->m_length = 0.0f;
        m_scrubber->update();
        ShowClips();
        return;
    }
    const Clip& clip = m_clips[index];
    SimpleMotion::Event(m_entity, &SimpleMotion::Events::Motion, clip.id);
    SimpleMotion::Event(m_entity, &SimpleMotion::Events::LoopMotion, true);
    SimpleMotion::Event(m_entity, &SimpleMotion::Events::SetPlaySpeed, m_playing ? m_speed : 0.0f);
    SimpleMotion::Event(m_entity, &SimpleMotion::Events::PlayMotion);
    const QJsonObject data = m_sidecar.value(QStringLiteral("clips")).toObject().value(clip.path).toObject();
    const QJsonArray parts = data.value(QStringLiteral("parts")).toArray(QJsonArray{ QStringLiteral("body"), QStringLiteral("legs") });
    {
        const QSignalBlocker b1(m_body), b2(m_legs);
        m_body->setChecked(parts.contains(QStringLiteral("body")));
        m_legs->setChecked(parts.contains(QStringLiteral("legs")));
    }
    m_scrubber->m_events.clear();
    for (const QJsonValue t : data.value(QStringLiteral("footsteps")).toArray())
    {
        m_scrubber->m_events.push_back(t.toDouble());
    }
    m_scrubber->m_length = clip.seconds;
    m_scrubber->update();
    ShowClips();
}

void HearthAnimationPane::ShowRules()
{
    while (QLayoutItem* item = m_rules->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    QStringList names = { tr("Any") };
    for (const Clip& clip : m_clips)
    {
        names << clip.name;
    }
    const QJsonArray rules = m_sidecar.value(QStringLiteral("rules")).toArray();
    for (int i = 0; i < rules.size(); ++i)
    {
        const QJsonObject rule = rules[i].toObject();
        auto* row = new QWidget(this);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        auto* from = new QComboBox(row);
        from->addItems(names);
        from->setCurrentText(rule.value(QStringLiteral("from")).toString());
        auto* to = new QComboBox(row);
        to->addItems(names.mid(1));
        to->setCurrentText(rule.value(QStringLiteral("to")).toString());
        auto* when = new QComboBox(row);
        when->setEditable(true);
        for (const char* choice : AnimWhenChoices)
        {
            when->addItem(tr(choice));
        }
        when->setCurrentText(rule.value(QStringLiteral("when")).toString());
        QPushButton* remove = Hearth::MakeButton(QStringLiteral("✕"), false, row);
        remove->setFixedWidth(30);
        layout->addWidget(from);
        layout->addWidget(Hearth::MakeLabel(QStringLiteral("→"), QStringLiteral("dim"), row));
        layout->addWidget(to);
        auto* column = new QVBoxLayout();
        column->setSpacing(2);
        auto* top = new QHBoxLayout();
        top->addWidget(row);
        top->addWidget(remove);
        auto* holder = new QWidget(this);
        auto* holderLayout = new QVBoxLayout(holder);
        holderLayout->setContentsMargins(0, 0, 0, 6);
        holderLayout->setSpacing(2);
        holderLayout->addLayout(top);
        holderLayout->addWidget(when);
        m_rules->addWidget(holder);
        auto changed = [this, i, from, to, when]()
        {
            QJsonArray list = m_sidecar.value(QStringLiteral("rules")).toArray();
            if (i < list.size())
            {
                list[i] = QJsonObject{ { QStringLiteral("from"), from->currentText() }, { QStringLiteral("to"), to->currentText() }, { QStringLiteral("when"), when->currentText() } };
                m_sidecar.insert(QStringLiteral("rules"), list);
                SaveSidecar();
            }
        };
        connect(from, qOverload<int>(&QComboBox::activated), this, changed);
        connect(to, qOverload<int>(&QComboBox::activated), this, changed);
        connect(when, &QComboBox::currentTextChanged, this, changed);
        connect(remove, &QPushButton::clicked, this, [this, i]()
        {
            QJsonArray list = m_sidecar.value(QStringLiteral("rules")).toArray();
            if (i < list.size())
            {
                list.removeAt(i);
                m_sidecar.insert(QStringLiteral("rules"), list);
                SaveSidecar();
                QTimer::singleShot(0, this, &HearthAnimationPane::ShowRules);
            }
        });
    }
    if (rules.isEmpty())
    {
        m_rules->addWidget(Hearth::MakeLabel(tr("No rules yet. Each rule says which clip comes next, and when."), QStringLiteral("small"), this));
    }
    m_status->setText(tr("Rules, clip names, parts and footsteps are saved in %1. The monster's code reads them in the game.")
                          .arg(QDir(QString::fromUtf8(AZ::Utils::GetProjectPath().c_str())).relativeFilePath(SidecarPath())));
}

void HearthAnimationPane::SetView(int mode)
{
    for (int i = 0; i < 3; ++i)
    {
        m_view[i]->setChecked(i == mode);
    }
    if (!m_entity.IsValid())
    {
        return;
    }
    AzToolsFramework::ScopedUndoBatch undo("Animation view");
    AnimSetProperty(m_entity, "Actor", "Draw character", AZStd::any(mode != 1));
    AnimSetProperty(m_entity, "Actor", "Draw skeleton", AZStd::any(mode != 0));
}

void HearthAnimationPane::Tick()
{
    if (!m_entity.IsValid() || m_clip < 0 || !isVisible())
    {
        return;
    }
    float time = 0.0f;
    float length = 0.0f;
    SimpleMotion::EventResult(time, m_entity, &SimpleMotion::Events::GetPlayTime);
    SimpleMotion::EventResult(length, m_entity, &SimpleMotion::Events::GetDuration);
    if (length > 0.0f && m_clips[m_clip].seconds <= 0.0f)
    {
        m_clips[m_clip].seconds = length;
        ShowClips();
    }
    m_scrubber->m_length = length > 0.0f ? length : m_clips[m_clip].seconds;
    m_scrubber->m_time = time;
    m_scrubber->update();
    m_time->setText(QStringLiteral("%1 / %2 s").arg(time, 0, 'f', 2).arg(m_scrubber->m_length, 0, 'f', 2));
}

void HearthAnimationPane::Capture()
{
    if (m_capturing || !isVisible() || !m_entity.IsValid() || !GetIEditor() || GetIEditor()->GetLevelName().isEmpty())
    {
        return;
    }
    bool canCapture = false;
    AZ::Render::FrameCaptureRequestBus::BroadcastResult(canCapture, &AZ::Render::FrameCaptureRequestBus::Events::CanCapture);
    if (!canCapture)
    {
        return;
    }
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/hearth-animation");
    QDir().mkpath(folder);
    const QString path = folder + QStringLiteral("/frame.png");
    QFile::remove(path);
    AZ::Render::FrameCaptureOutcome outcome;
    AZ::Render::FrameCaptureRequestBus::BroadcastResult(outcome, &AZ::Render::FrameCaptureRequestBus::Events::CaptureScreenshot, AZStd::string(path.toUtf8().constData()));
    if (!outcome.IsSuccess())
    {
        return;
    }
    m_capturing = true;
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto* poll = new QTimer(this);
    poll->setInterval(40);
    connect(poll, &QTimer::timeout, this, [this, poll, path, clock]()
    {
        if (QFileInfo(path).size() > 0 || clock->elapsed() > 2000)
        {
            poll->stop();
            poll->deleteLater();
            m_capturing = false;
            const QImage image(path);
            if (!image.isNull())
            {
                m_preview->m_image = image;
                m_preview->update();
            }
        }
    });
    poll->start();
}
