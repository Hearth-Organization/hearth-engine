/*
 * Hearth Engine Physics settings. Built on O3DE (Apache-2.0 OR MIT). See HearthPhysicsPane.h.
 */

#include "EditorDefs.h"

#include "HearthPhysicsPane.h"

#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/Entity.h>
#include <AzCore/Console/IConsole.h>
#include <AzCore/Interface/Interface.h>
#include <AzCore/Serialization/Json/JsonSerialization.h>
#include <AzCore/Serialization/Json/JsonUtils.h>
#include <AzCore/Utils/Utils.h>
#include <AzFramework/Physics/Collision/CollisionGroups.h>
#include <AzFramework/Physics/Collision/CollisionLayers.h>
#include <AzFramework/Physics/Common/PhysicsTypes.h>
#include <AzFramework/Physics/Configuration/CollisionConfiguration.h>
#include <AzFramework/Physics/Configuration/SceneConfiguration.h>
#include <AzFramework/Physics/Configuration/SystemConfiguration.h>
#include <AzFramework/Physics/PhysicsScene.h>
#include <AzFramework/Physics/PhysicsSystem.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>
#include <AzToolsFramework/Component/EditorComponentAPIBus.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>

#include <QCheckBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;
    using ComponentApi = AzToolsFramework::EditorComponentAPIBus;
    using ComponentRequests = AzToolsFramework::EditorComponentAPIRequests;

    const char* const PhysicsLayers[] = { "World", "Player", "Entities", "Items", "Triggers" };

    QString PhysicsRegistry(const QString& name)
    {
        return QDir(QString::fromUtf8(AZ::Utils::GetProjectPath().c_str())).filePath(QStringLiteral("Registry/%1").arg(name));
    }

    //! Writes value at /Amazon/Gems/<PhysX gem>/<section>/<key...> in one of the project's PhysX .setreg files.
    void PhysicsWrite(const QString& file, const QString& section, const QStringList& keys, const QJsonValue& value)
    {
        QJsonObject root;
        QFile in(PhysicsRegistry(file));
        if (in.open(QIODevice::ReadOnly))
        {
            root = QJsonDocument::fromJson(in.readAll()).object();
            in.close();
        }
        QJsonObject amazon = root.value(QStringLiteral("Amazon")).toObject();
        QJsonObject gems = amazon.value(QStringLiteral("Gems")).toObject();
        const QString gem = gems.contains(QStringLiteral("PhysX")) && !gems.contains(QStringLiteral("PhysX5")) ? QStringLiteral("PhysX") : QStringLiteral("PhysX5");
        QJsonObject physx = gems.value(gem).toObject();
        QJsonObject target = physx.value(section).toObject();
        // Walk down the keys, keeping everything else.
        std::function<QJsonObject(QJsonObject, int)> put = [&](QJsonObject node, int at) -> QJsonObject
        {
            if (at == keys.size() - 1)
            {
                node.insert(keys[at], value);
                return node;
            }
            node.insert(keys[at], put(node.value(keys[at]).toObject(), at + 1));
            return node;
        };
        target = put(target, 0);
        physx.insert(section, target);
        gems.insert(gem, physx);
        amazon.insert(QStringLiteral("Gems"), gems);
        root.insert(QStringLiteral("Amazon"), amazon);
        QSaveFile out(PhysicsRegistry(file));
        if (out.open(QIODevice::WriteOnly))
        {
            out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            out.commit();
        }
    }

    QDoubleSpinBox* PhysicsSpin(QWidget* parent, double low, double high, double step, int decimals, const QString& suffix)
    {
        auto* spin = new QDoubleSpinBox(parent);
        spin->setRange(low, high);
        spin->setSingleStep(step);
        spin->setDecimals(decimals);
        spin->setSuffix(suffix);
        spin->setFixedWidth(120);
        return spin;
    }

    QFrame* PhysicsCard(QWidget* parent, const QString& title, QVBoxLayout** layout)
    {
        QFrame* card = Hearth::MakeCard(parent, layout);
        (*layout)->addWidget(Hearth::MakeLabel(title, QStringLiteral("title"), card));
        return card;
    }

    //! The level's player: the editor entity with a PhysX Character Controller (and the First Person Controller).
    struct PhysicsPlayer
    {
        AZ::EntityComponentIdPair character;
        AZ::EntityComponentIdPair controller;
        AZStd::vector<AZStd::string> characterPaths;
        AZStd::vector<AZStd::string> controllerPaths;
    };

    PhysicsPlayer PhysicsFindPlayer()
    {
        PhysicsPlayer player;
        AZStd::vector<AZ::Uuid> characterType;
        AZStd::vector<AZ::Uuid> controllerType;
        ComponentApi::BroadcastResult(characterType, &ComponentRequests::FindComponentTypeIdsByEntityType,
                                      AZStd::vector<AZStd::string>{ "PhysX Character Controller" }, ComponentRequests::EntityType::Game);
        ComponentApi::BroadcastResult(controllerType, &ComponentRequests::FindComponentTypeIdsByEntityType,
                                      AZStd::vector<AZStd::string>{ "First Person Controller" }, ComponentRequests::EntityType::Game);
        AZ::ComponentApplicationBus::Broadcast(&AZ::ComponentApplicationRequests::EnumerateEntities, [&](AZ::Entity* entity)
        {
            if (player.character.GetEntityId().IsValid() || !entity)
            {
                return;
            }
            bool editorEntity = false;
            AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(editorEntity, &AzToolsFramework::EditorEntityContextRequests::IsEditorEntity, entity->GetId());
            if (!editorEntity || characterType.empty() || characterType.front().IsNull())
            {
                return;
            }
            ComponentRequests::GetComponentOutcome got = AZ::Failure(AZStd::string());
            ComponentApi::BroadcastResult(got, &ComponentRequests::GetComponentOfType, entity->GetId(), characterType.front());
            if (!got.IsSuccess())
            {
                return;
            }
            player.character = got.GetValue();
            if (!controllerType.empty() && !controllerType.front().IsNull())
            {
                ComponentRequests::GetComponentOutcome fpc = AZ::Failure(AZStd::string());
                ComponentApi::BroadcastResult(fpc, &ComponentRequests::GetComponentOfType, entity->GetId(), controllerType.front());
                if (fpc.IsSuccess())
                {
                    player.controller = fpc.GetValue();
                }
            }
        });
        if (player.character.GetEntityId().IsValid())
        {
            ComponentApi::BroadcastResult(player.characterPaths, &ComponentRequests::BuildComponentPropertyList, player.character);
        }
        if (player.controller.GetEntityId().IsValid())
        {
            ComponentApi::BroadcastResult(player.controllerPaths, &ComponentRequests::BuildComponentPropertyList, player.controller);
        }
        return player;
    }

    //! The first property path whose last part contains every word (case ignored), skipping paths with any "not" word.
    AZStd::string PhysicsPath(const AZStd::vector<AZStd::string>& paths, const QStringList& words, const QStringList& not_ = {})
    {
        for (const AZStd::string& path : paths)
        {
            const QString full = QString::fromUtf8(path.c_str());
            const QString last = full.section(QLatin1Char('|'), -1);
            bool all = true;
            for (const QString& word : words)
            {
                all = all && (last.contains(word, Qt::CaseInsensitive) || (word.startsWith(QLatin1Char('^')) && full.contains(word.mid(1), Qt::CaseInsensitive)));
            }
            for (const QString& word : not_)
            {
                all = all && !full.contains(word, Qt::CaseInsensitive);
            }
            if (all)
            {
                return path;
            }
        }
        return {};
    }

    bool PhysicsGetFloat(const AZ::EntityComponentIdPair& component, const AZStd::string& path, double& out)
    {
        if (path.empty())
        {
            return false;
        }
        ComponentRequests::PropertyOutcome got = AZ::Failure(AZStd::string());
        ComponentApi::BroadcastResult(got, &ComponentRequests::GetComponentProperty, component, AZStd::string_view(path));
        if (!got.IsSuccess())
        {
            return false;
        }
        const AZStd::any& value = got.GetValue();
        if (const float* f = AZStd::any_cast<float>(&value))
        {
            out = *f;
            return true;
        }
        if (const double* d = AZStd::any_cast<double>(&value))
        {
            out = *d;
            return true;
        }
        return false;
    }

    void PhysicsSetFloat(const AZ::EntityComponentIdPair& component, const AZStd::string& path, double value)
    {
        if (path.empty())
        {
            return;
        }
        ComponentRequests::PropertyOutcome done = AZ::Failure(AZStd::string());
        ComponentApi::BroadcastResult(done, &ComponentRequests::SetComponentProperty, component, AZStd::string_view(path), AZStd::any(float(value)));
    }
} // namespace

void HearthPhysicsPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(140, 90, 1180, 760);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthPhysicsPane>("Physics", "Tools", options);
}

HearthPhysicsPane::HearthPhysicsPane(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthPhysics"));
    setStyleSheet(QStringLiteral("QWidget#HearthPhysics { background-color: #0D0C0B; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);
    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Physics"), tr("Collisions, the player's body, gravity"));
    header->addStretch(1);
    m_showColliders = Hearth::MakeButton(tr("Show colliders in the viewport"), false, this);
    m_showColliders->setCheckable(true);
    {
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        m_showColliders->setChecked(settings->value(QStringLiteral("physics/showColliders"), false).toBool());
    }
    connect(m_showColliders, &QPushButton::toggled, this, [](bool on)
    {
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        settings->setValue(QStringLiteral("physics/showColliders"), on);
        if (auto* console = AZ::Interface<AZ::IConsole>::Get())
        {
            console->PerformCommand(on ? "physx_showColliders 1" : "physx_showColliders 0");
        }
    });
    header->addWidget(m_showColliders);
    root->addLayout(header);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);

    // What collides with what.
    QVBoxLayout* matrix = nullptr;
    QFrame* matrixCard = PhysicsCard(this, tr("What collides with what"), &matrix);
    matrix->addWidget(Hearth::MakeLabel(tr("Tick a box to make two layers bump into each other."), QStringLiteral("dim"), matrixCard));
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(10);
    for (int column = 0; column < LayerCount; ++column)
    {
        QLabel* name = Hearth::MakeLabel(tr(PhysicsLayers[LayerCount - 1 - column]), QStringLiteral("small"), matrixCard);
        grid->addWidget(name, 0, column + 1, Qt::AlignCenter);
    }
    for (int row = 0; row < LayerCount; ++row)
    {
        grid->addWidget(Hearth::MakeLabel(tr(PhysicsLayers[row]), QStringLiteral("body"), matrixCard), row + 1, 0);
        // A triangle, like the engine's own matrix: layer row against layer column, each pair once.
        for (int column = 0; column < LayerCount - row; ++column)
        {
            const int other = LayerCount - 1 - column;
            auto* box = new QCheckBox(matrixCard);
            m_matrix[row][other] = box;
            m_matrix[other][row] = box;
            connect(box, &QCheckBox::toggled, this, [this]()
            {
                if (!m_loading)
                {
                    SaveCollisions();
                }
            });
            grid->addWidget(box, row + 1, column + 1, Qt::AlignCenter);
        }
    }
    matrix->addLayout(grid);
    matrix->addWidget(Hearth::MakeLabel(tr("A collider on a layer uses the collision group with the same name."), QStringLiteral("small"), matrixCard));
    matrix->addSpacing(8);
    m_gravity = PhysicsSpin(matrixCard, 0.0, 100.0, 0.1, 1, tr(" m/s²"));
    matrix->addWidget(Hearth::MakeSettingRow(tr("Gravity"), QString(), m_gravity, matrixCard));
    m_steps = new QSpinBox(matrixCard);
    m_steps->setRange(10, 240);
    m_steps->setFixedWidth(120);
    matrix->addWidget(Hearth::MakeSettingRow(tr("Physics steps per second"), QString(), m_steps, matrixCard));
    connect(m_gravity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this]() { if (!m_loading) SaveWorld(); });
    connect(m_steps, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() { if (!m_loading) SaveWorld(); });
    matrix->addStretch(1);
    body->addWidget(matrixCard, 1);

    // Player body.
    QVBoxLayout* player = nullptr;
    QFrame* playerCard = PhysicsCard(this, tr("Player body"), &player);
    playerCard->setFixedWidth(440);
    m_height = PhysicsSpin(playerCard, 0.5, 4.0, 0.05, 2, tr(" m"));
    m_width = PhysicsSpin(playerCard, 0.1, 3.0, 0.05, 2, tr(" m"));
    m_stepUp = PhysicsSpin(playerCard, 0.0, 2.0, 0.05, 2, tr(" m"));
    m_slope = PhysicsSpin(playerCard, 0.0, 89.0, 1.0, 0, QStringLiteral("°"));
    m_walk = PhysicsSpin(playerCard, 0.1, 30.0, 0.1, 1, tr(" m/s"));
    m_sprint = PhysicsSpin(playerCard, 0.1, 30.0, 0.1, 1, tr(" m/s"));
    m_jump = PhysicsSpin(playerCard, 0.0, 10.0, 0.05, 2, tr(" m"));
    player->addWidget(Hearth::MakeSettingRow(tr("Height"), QString(), m_height, playerCard));
    player->addWidget(Hearth::MakeSettingRow(tr("Width"), QString(), m_width, playerCard));
    player->addWidget(Hearth::MakeSettingRow(tr("Step up"), QString(), m_stepUp, playerCard));
    player->addWidget(Hearth::MakeSettingRow(tr("Steepest slope"), QString(), m_slope, playerCard));
    auto* speeds = new QWidget(playerCard);
    auto* speedsLayout = new QHBoxLayout(speeds);
    speedsLayout->setContentsMargins(0, 0, 0, 0);
    m_walk->setFixedWidth(96);
    m_sprint->setFixedWidth(96);
    speedsLayout->addWidget(m_walk);
    speedsLayout->addWidget(Hearth::MakeLabel(QStringLiteral("/"), QStringLiteral("dim"), speeds));
    speedsLayout->addWidget(m_sprint);
    player->addWidget(Hearth::MakeSettingRow(tr("Walk / sprint"), QString(), speeds, playerCard));
    player->addWidget(Hearth::MakeSettingRow(tr("Jump"), tr("How high a jump goes"), m_jump, playerCard));
    m_playerNote = Hearth::MakeLabel(QString(), QStringLiteral("small"), playerCard);
    m_playerNote->setWordWrap(true);
    player->addWidget(m_playerNote);
    for (QDoubleSpinBox* spin : { m_height, m_width, m_stepUp, m_slope, m_walk, m_sprint, m_jump })
    {
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this]() { if (!m_loading) SavePlayer(); });
    }
    player->addStretch(1);
    body->addWidget(playerCard);
    root->addLayout(body, 1);
}

void HearthPhysicsPane::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    Load();
}

void HearthPhysicsPane::Load()
{
    m_loading = true;
    auto* system = AZ::Interface<AzPhysics::SystemInterface>::Get();
    if (system && system->GetConfiguration())
    {
        const AzPhysics::CollisionConfiguration& collision = system->GetConfiguration()->m_collisionConfig;
        for (int a = 0; a < LayerCount; ++a)
        {
            AzPhysics::CollisionGroup group;
            const bool named = collision.m_collisionGroups.TryFindGroupByName(PhysicsLayers[a], group);
            for (int b = a; b < LayerCount; ++b)
            {
                // Without the groups yet, everything collides (the engine's default).
                m_matrix[a][b]->setChecked(!named || group.IsSet(AzPhysics::CollisionLayer(AZ::u8(b))));
            }
        }
        const float step = system->GetConfiguration()->m_fixedTimestep;
        m_steps->setValue(step > 0 ? qRound(1.0f / step) : 60);
        m_gravity->setValue(-double(system->GetDefaultSceneConfiguration().m_gravity.GetZ()));
    }
    LoadPlayer();
    m_loading = false;
    if (m_showColliders->isChecked())
    {
        if (auto* console = AZ::Interface<AZ::IConsole>::Get())
        {
            console->PerformCommand("physx_showColliders 1");
        }
    }
}

void HearthPhysicsPane::SaveCollisions()
{
    auto* system = AZ::Interface<AzPhysics::SystemInterface>::Get();
    if (!system || !system->GetConfiguration())
    {
        return;
    }
    // The live configuration is changed in place (the collision filters read it), then saved for next time.
    auto* live = const_cast<AzPhysics::SystemConfiguration*>(system->GetConfiguration());
    AzPhysics::CollisionConfiguration& collision = live->m_collisionConfig;
    for (int a = 0; a < LayerCount; ++a)
    {
        collision.m_collisionLayers.SetName(AZ::u64(a), a == 0 ? AZStd::string("Default") : AZStd::string(PhysicsLayers[a]));
    }
    for (int a = 0; a < LayerCount; ++a)
    {
        AzPhysics::CollisionGroup group(AZ::u64(0));
        for (int b = 0; b < LayerCount; ++b)
        {
            group.SetLayer(AzPhysics::CollisionLayer(AZ::u8(b)), m_matrix[a][b]->isChecked());
        }
        // Layers past the five keep colliding with everything, as before.
        for (int b = LayerCount; b < int(AzPhysics::CollisionLayers::MaxCollisionLayers); ++b)
        {
            group.SetLayer(AzPhysics::CollisionLayer(AZ::u8(b)), true);
        }
        const AzPhysics::CollisionGroups::Id id = collision.m_collisionGroups.FindGroupIdByName(PhysicsLayers[a]);
        if (id.m_id.IsNull())
        {
            collision.m_collisionGroups.CreateGroup(PhysicsLayers[a], group);
        }
        else
        {
            for (int b = 0; b < int(AzPhysics::CollisionLayers::MaxCollisionLayers); ++b)
            {
                collision.m_collisionGroups.SetLayer(id, AzPhysics::CollisionLayer(AZ::u8(b)), group.IsSet(AzPhysics::CollisionLayer(AZ::u8(b))));
            }
        }
    }
    rapidjson::Document document;
    AZ::JsonSerialization::Store(document, document.GetAllocator(), collision);
    AZStd::string text;
    if (AZ::JsonSerializationUtils::WriteJsonString(document, text).IsSuccess())
    {
        PhysicsWrite(QStringLiteral("physxsystemconfiguration.setreg"), QStringLiteral("PhysXSystemConfiguration"), { QStringLiteral("CollisionConfig") },
                     QJsonDocument::fromJson(QByteArray(text.c_str())).object());
    }
}

void HearthPhysicsPane::SaveWorld()
{
    auto* system = AZ::Interface<AzPhysics::SystemInterface>::Get();
    const double gravity = m_gravity->value();
    if (system)
    {
        AzPhysics::SceneConfiguration scene = system->GetDefaultSceneConfiguration();
        scene.m_gravity = AZ::Vector3(0.0f, 0.0f, -float(gravity));
        system->UpdateDefaultSceneConfiguration(scene);
        if (auto* scenes = AZ::Interface<AzPhysics::SceneInterface>::Get())
        {
            for (const char* name : { AzPhysics::DefaultPhysicsSceneName, AzPhysics::EditorPhysicsSceneName })
            {
                const AzPhysics::SceneHandle handle = scenes->GetSceneHandle(name);
                if (handle != AzPhysics::InvalidSceneHandle)
                {
                    scenes->SetGravity(handle, scene.m_gravity);
                }
            }
        }
        if (system->GetConfiguration())
        {
            const_cast<AzPhysics::SystemConfiguration*>(system->GetConfiguration())->m_fixedTimestep = 1.0f / float(m_steps->value());
        }
    }
    PhysicsWrite(QStringLiteral("physxdefaultsceneconfiguration.setreg"), QStringLiteral("DefaultSceneConfiguration"), { QStringLiteral("Gravity") },
                 QJsonArray{ 0.0, 0.0, -gravity });
    PhysicsWrite(QStringLiteral("physxsystemconfiguration.setreg"), QStringLiteral("PhysXSystemConfiguration"), { QStringLiteral("FixedTimeStep") },
                 1.0 / m_steps->value());
    // The player falls with the same gravity.
    const PhysicsPlayer player = PhysicsFindPlayer();
    PhysicsSetFloat(player.controller, PhysicsPath(player.controllerPaths, { QStringLiteral("Gravity") }), gravity);
}

void HearthPhysicsPane::LoadPlayer()
{
    const PhysicsPlayer player = PhysicsFindPlayer();
    const bool found = player.character.GetEntityId().IsValid();
    for (QDoubleSpinBox* spin : { m_height, m_width, m_stepUp, m_slope, m_walk, m_sprint, m_jump })
    {
        spin->setEnabled(found);
    }
    if (!found)
    {
        m_playerNote->setText(tr("Open a level with a player (an entity with a PhysX Character Controller) to set its body."));
        return;
    }
    double value = 0.0;
    if (PhysicsGetFloat(player.character, PhysicsPath(player.characterPaths, { QStringLiteral("Height") }, { QStringLiteral("Step"), QStringLiteral("Contact") }), value))
    {
        m_height->setValue(value);
    }
    if (PhysicsGetFloat(player.character, PhysicsPath(player.characterPaths, { QStringLiteral("Radius") }), value))
    {
        m_width->setValue(value * 2.0);
    }
    if (PhysicsGetFloat(player.character, PhysicsPath(player.characterPaths, { QStringLiteral("Step") }), value))
    {
        m_stepUp->setValue(value);
    }
    if (PhysicsGetFloat(player.character, PhysicsPath(player.characterPaths, { QStringLiteral("slope") }), value))
    {
        m_slope->setValue(value);
    }
    double gravity = m_gravity->value();
    PhysicsGetFloat(player.controller, PhysicsPath(player.controllerPaths, { QStringLiteral("Gravity") }), gravity);
    if (PhysicsGetFloat(player.controller, PhysicsPath(player.controllerPaths, { QStringLiteral("Walk") }), value))
    {
        m_walk->setValue(value);
    }
    if (PhysicsGetFloat(player.controller, PhysicsPath(player.controllerPaths, { QStringLiteral("Sprint") }), value))
    {
        m_sprint->setValue(value);
    }
    if (PhysicsGetFloat(player.controller, PhysicsPath(player.controllerPaths, { QStringLiteral("Jump") }), value) && gravity > 0)
    {
        m_jump->setValue(value * value / (2.0 * gravity)); // jump speed -> how high it goes
    }
    m_playerNote->setText(player.controller.GetEntityId().IsValid() ? tr("From the level's player. Changes go into the level (save it to keep them).")
                                                                    : tr("This player has no First Person Controller: walk, sprint and jump aren't used."));
}

void HearthPhysicsPane::SavePlayer()
{
    const PhysicsPlayer player = PhysicsFindPlayer();
    if (!player.character.GetEntityId().IsValid())
    {
        return;
    }
    AzToolsFramework::ScopedUndoBatch undo("Hearth: player body");
    undo.MarkEntityDirty(player.character.GetEntityId());
    PhysicsSetFloat(player.character, PhysicsPath(player.characterPaths, { QStringLiteral("Height") }, { QStringLiteral("Step"), QStringLiteral("Contact") }), m_height->value());
    PhysicsSetFloat(player.character, PhysicsPath(player.characterPaths, { QStringLiteral("Radius") }), m_width->value() / 2.0);
    PhysicsSetFloat(player.character, PhysicsPath(player.characterPaths, { QStringLiteral("Step") }), m_stepUp->value());
    PhysicsSetFloat(player.character, PhysicsPath(player.characterPaths, { QStringLiteral("slope") }), m_slope->value());
    PhysicsSetFloat(player.controller, PhysicsPath(player.controllerPaths, { QStringLiteral("Walk") }), m_walk->value());
    PhysicsSetFloat(player.controller, PhysicsPath(player.controllerPaths, { QStringLiteral("Sprint") }), m_sprint->value());
    // How high a jump goes -> the jump speed that reaches it with this gravity.
    PhysicsSetFloat(player.controller, PhysicsPath(player.controllerPaths, { QStringLiteral("Jump") }), std::sqrt(2.0 * m_gravity->value() * m_jump->value()));
}
