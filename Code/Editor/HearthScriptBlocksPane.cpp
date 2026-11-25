/*
 * Hearth Engine "Script Canvas" (blocks). Built on O3DE (Apache-2.0 OR MIT). See HearthScriptBlocksPane.h.
 */

#include "EditorDefs.h"

#include "HearthScriptBlocksPane.h"
#include "QtViewPaneManager.h"

#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/Entity.h>
#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/EditorPythonRunnerRequestsBus.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>
#include <AzToolsFramework/Component/EditorComponentAPIBus.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;
    using Components = AzToolsFramework::EditorComponentAPIRequests;

    constexpr const char* BlocksComponentName = "Hearth Blocks";

    struct BlocksKind
    {
        const char* kind;
        const char* palette;   // as in the mockup's Blocks list
        const char* group;     // When / Check / Do
    };
    const BlocksKind BlocksKinds[] = {
        { "when.enter", "Player enters an area", "When" },
        { "when.lightDies", "A light dies", "When" },
        { "when.seen", "An entity sees the player", "When" },
        { "check.item", "The player has an item", "Check" },
        { "do.play", "Play a sound or line", "Do" },
        { "do.move", "Open, close, move", "Do" },
        { "do.level", "Send to another level", "Do" },
        { "do.shift", "Shift the rooms", "Do" },
    };

    AZ::Uuid BlocksComponentType(const char* name)
    {
        AZStd::vector<AZ::Uuid> types;
        AzToolsFramework::EditorComponentAPIBus::BroadcastResult(types, &Components::FindComponentTypeIdsByEntityType,
                                                                 AZStd::vector<AZStd::string>{ name }, Components::EntityType::Game);
        return types.empty() ? AZ::Uuid::CreateNull() : types.front();
    }

    QJsonArray BlocksIds(const QJsonObject& block, const char* key)
    {
        return block.value(QLatin1String(key)).toArray();
    }

    //! A card you can click (pick) and double-click (edit); right-click gives the menu.
    class BlocksCard : public QFrame
    {
    public:
        explicit BlocksCard(QWidget* parent)
            : QFrame(parent)
        {
            setCursor(Qt::PointingHandCursor);
        }
        std::function<void()> m_onClick;
        std::function<void()> m_onEdit;
        std::function<void(const QPoint&)> m_onMenu;

    protected:
        void mousePressEvent(QMouseEvent* event) override
        {
            if (event->button() == Qt::LeftButton && m_onClick)
            {
                m_onClick();
            }
            else if (event->button() == Qt::RightButton && m_onMenu)
            {
                m_onMenu(event->globalPos());
            }
        }
        void mouseDoubleClickEvent(QMouseEvent*) override
        {
            if (m_onEdit)
            {
                m_onEdit();
            }
        }
    };
} // namespace

void HearthScriptBlocksPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(140, 90, 1180, 780);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthScriptBlocksPane>("Script Canvas", "Tools", options);
}

HearthScriptBlocksPane::HearthScriptBlocksPane(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthBlocks"));
    setStyleSheet(QStringLiteral("QWidget#HearthBlocks { background-color: #0D0C0B; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Script Canvas"), tr("No script yet"));
    if (QLayoutItem* titles = header->itemAt(1); titles && titles->layout() && titles->layout()->itemAt(1))
    {
        m_subtitle = qobject_cast<QLabel*>(titles->layout()->itemAt(1)->widget());
    }
    m_scripts = new QComboBox(this);
    m_scripts->setMinimumWidth(200);
    header->addWidget(m_scripts);
    QPushButton* graphs = Hearth::MakeButton(tr("Graph editor"), false, this);
    graphs->setToolTip(tr("The full node-graph Script Canvas, for scripts blocks can't say"));
    connect(graphs, &QPushButton::clicked, this, []() { QtViewPaneManager::instance()->OpenPane(QStringLiteral("Script Canvas graphs")); });
    header->addWidget(graphs);
    QPushButton* claude = Hearth::MakeButton(tr("Describe it, Claude builds it"), true, this);
    header->addWidget(claude);
    root->addLayout(header);
    connect(m_scripts, qOverload<int>(&QComboBox::activated), this, [this](int index)
    {
        const QString id = m_scripts->itemData(index).toString();
        if (id.isEmpty())
        {
            NewScript();
        }
        else
        {
            LoadScript(AZ::EntityId(id.toULongLong()));
        }
    });
    connect(claude, &QPushButton::clicked, this, [this]()
    {
        bool ok = false;
        const QString what = QInputDialog::getMultiLineText(this, tr("Describe it, Claude builds it"),
            tr("What should happen? For example: when the player walks into the exit zone without the keycard, Reyes says it's locked."), QString(), &ok);
        if (!ok || what.trimmed().isEmpty())
        {
            return;
        }
        const QString details = tr("Build this as Hearth Blocks on the entity \"%1\" (component \"Hearth Blocks\", property \"Blocks\", JSON).\n"
                                   "Block kinds: when.enter (target = trigger entity, who = player/anyone), when.lightDies (target = light entity), "
                                   "when.seen (target = watcher entity, distance), check.item (target = item, next = yes, no = no), do.play (target = entity "
                                   "with MiniAudio Playback), do.move (target, mode open/close/move, x y z, seconds, sound), do.level (target = level), do.shift.\n"
                                   "Each block: id, kind, next (ids). What the owner wants:\n%2\n\nThe script now:\n%3")
                                    .arg(m_name, what.trimmed(), QString::fromUtf8(QJsonDocument(m_graph).toJson(QJsonDocument::Indented)));
        Hearth::AskClaude(tr("Script Canvas: %1").arg(m_name.isEmpty() ? tr("new script") : m_name), details);
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
    m_running = Hearth::MakeLabel(tr("Running in Play: the highlighted block just ran"), QStringLiteral("small"), canvas);
    canvasLayout->addWidget(m_running);
    body->addWidget(canvas, 1);

    // Blocks: click one to add it after the picked block (a When starts a new chain).
    QVBoxLayout* paletteLayout = nullptr;
    QFrame* palette = Hearth::MakeCard(this, &paletteLayout);
    palette->setFixedWidth(260);
    paletteLayout->addWidget(Hearth::MakeLabel(tr("Blocks"), QStringLiteral("title"), palette));
    QString group;
    for (const BlocksKind& kind : BlocksKinds)
    {
        if (QLatin1String(kind.group) == QLatin1String("Check"))
        {
            continue; // checks are added from a block's menu ("Add a check after"), like the mockup's list
        }
        if (group != QLatin1String(kind.group))
        {
            group = QLatin1String(kind.group);
            paletteLayout->addWidget(Hearth::MakeLabel(tr(kind.group), QStringLiteral("small"), palette));
        }
        QPushButton* add = Hearth::MakeButton(tr(kind.palette), false, palette);
        add->setStyleSheet(add->styleSheet() + QStringLiteral(" QPushButton { text-align: left; }"));
        const QString name = QLatin1String(kind.kind);
        connect(add, &QPushButton::clicked, this, [this, name]() { AddBlock(name, name.startsWith(QLatin1String("when")) ? -1 : m_selected); });
        paletteLayout->addWidget(add);
    }
    paletteLayout->addStretch(1);
    paletteLayout->addWidget(Hearth::MakeLabel(tr("Click a block to pick it, double-click to change it, right-click for more."), QStringLiteral("small"), palette));
    body->addWidget(palette);

    AZ::Debug::TraceMessageBus::Handler::BusConnect();
    QTimer::singleShot(0, this, &HearthScriptBlocksPane::ListScripts);
}

HearthScriptBlocksPane::~HearthScriptBlocksPane()
{
    AZ::Debug::TraceMessageBus::Handler::BusDisconnect();
}

bool HearthScriptBlocksPane::OnPrintf(const char* window, const char* message)
{
    // Can come from any thread: hand it to the window's thread.
    if (window && QLatin1String(window) == QLatin1String("HearthBlocks") && message)
    {
        const QString text = QString::fromUtf8(message).trimmed();
        const QRegularExpressionMatch match = QRegularExpression(QStringLiteral("^ran (.*) (\\d+)$")).match(text);
        if (match.hasMatch())
        {
            const QString script = match.captured(1);
            const int id = match.captured(2).toInt();
            QMetaObject::invokeMethod(this, [this, script, id]() { Ran(script, id); }, Qt::QueuedConnection);
        }
    }
    return false;
}

void HearthScriptBlocksPane::Ran(const QString& script, int id)
{
    if (script != m_name)
    {
        return;
    }
    m_lit = id;
    m_running->setText(tr("Running in Play: the highlighted block just ran"));
    Rebuild();
    QTimer::singleShot(1500, this, [this, id]()
    {
        if (m_lit == id)
        {
            m_lit = -1;
            Rebuild();
        }
    });
}

QStringList HearthScriptBlocksPane::EntityNames(const char* componentName) const
{
    QStringList names;
    const AZ::Uuid type = componentName ? BlocksComponentType(componentName) : AZ::Uuid::CreateNull();
    AZ::ComponentApplicationBus::Broadcast(&AZ::ComponentApplicationRequests::EnumerateEntities, [&names, &type, componentName](AZ::Entity* entity)
    {
        bool editorEntity = false;
        AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(editorEntity, &AzToolsFramework::EditorEntityContextRequests::IsEditorEntity, entity->GetId());
        if (!editorEntity)
        {
            return;
        }
        bool has = componentName == nullptr;
        if (!has && !type.IsNull())
        {
            AzToolsFramework::EditorComponentAPIBus::BroadcastResult(has, &Components::HasComponentOfType, entity->GetId(), type);
        }
        if (has)
        {
            names << QString::fromUtf8(entity->GetName().c_str());
        }
    });
    names.removeDuplicates();
    names.sort(Qt::CaseInsensitive);
    return names;
}

void HearthScriptBlocksPane::ListScripts()
{
    m_scripts->clear();
    const AZ::Uuid type = BlocksComponentType(BlocksComponentName);
    AZ::EntityId first;
    AZ::ComponentApplicationBus::Broadcast(&AZ::ComponentApplicationRequests::EnumerateEntities, [this, &type, &first](AZ::Entity* entity)
    {
        bool editorEntity = false;
        AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(editorEntity, &AzToolsFramework::EditorEntityContextRequests::IsEditorEntity, entity->GetId());
        bool has = false;
        if (editorEntity && !type.IsNull())
        {
            AzToolsFramework::EditorComponentAPIBus::BroadcastResult(has, &Components::HasComponentOfType, entity->GetId(), type);
        }
        if (has)
        {
            m_scripts->addItem(QString::fromUtf8(entity->GetName().c_str()), QString::number(static_cast<AZ::u64>(entity->GetId())));
            first = first.IsValid() ? first : entity->GetId();
        }
    });
    m_scripts->addItem(tr("+ New script…"), QString());
    if (type.IsNull())
    {
        m_running->setText(tr("The game's Hearth Blocks component isn't loaded: build Noclip Protocol with this engine first."));
    }
    if (m_entity.IsValid() && m_scripts->findData(QString::number(static_cast<AZ::u64>(m_entity))) >= 0)
    {
        LoadScript(m_entity);
    }
    else if (first.IsValid())
    {
        LoadScript(first);
    }
    else
    {
        m_graph = QJsonObject{ { QStringLiteral("blocks"), QJsonArray() } };
        Rebuild();
    }
}

void HearthScriptBlocksPane::LoadScript(AZ::EntityId entity)
{
    m_entity = entity;
    m_scripts->setCurrentIndex(qMax(0, m_scripts->findData(QString::number(static_cast<AZ::u64>(entity)))));
    m_graph = QJsonObject{ { QStringLiteral("blocks"), QJsonArray() } };
    m_name.clear();
    Components::GetComponentOutcome component = AZ::Failure(AZStd::string());
    AzToolsFramework::EditorComponentAPIBus::BroadcastResult(component, &Components::GetComponentOfType, entity, BlocksComponentType(BlocksComponentName));
    if (component.IsSuccess())
    {
        auto read = [&component](const char* path)
        {
            Components::PropertyOutcome value = AZ::Failure(AZStd::string());
            AzToolsFramework::EditorComponentAPIBus::BroadcastResult(value, &Components::GetComponentProperty, component.GetValue(), AZStd::string_view(path));
            const AZStd::string* text = value.IsSuccess() ? AZStd::any_cast<AZStd::string>(&value.GetValue()) : nullptr;
            return text ? QString::fromUtf8(text->c_str()) : QString();
        };
        m_name = read("Name");
        const QJsonObject graph = QJsonDocument::fromJson(read("Blocks").toUtf8()).object();
        if (graph.contains(QStringLiteral("blocks")))
        {
            m_graph = graph;
        }
    }
    const QString level = GetIEditor() ? GetIEditor()->GetLevelName() : QString();
    if (m_subtitle)
    {
        m_subtitle->setText(QStringLiteral("%1 · %2").arg(m_name.isEmpty() ? m_scripts->currentText() : m_name, level));
    }
    m_selected = -1;
    Rebuild();
}

void HearthScriptBlocksPane::SaveScript()
{
    if (!m_entity.IsValid())
    {
        return;
    }
    Components::GetComponentOutcome component = AZ::Failure(AZStd::string());
    AzToolsFramework::EditorComponentAPIBus::BroadcastResult(component, &Components::GetComponentOfType, m_entity, BlocksComponentType(BlocksComponentName));
    if (!component.IsSuccess())
    {
        return;
    }
    AzToolsFramework::ScopedUndoBatch undo("Change Hearth Blocks");
    const AZStd::string blocks(QJsonDocument(m_graph).toJson(QJsonDocument::Compact).constData());
    const AZStd::string name(m_name.toUtf8().constData());
    Components::PropertyOutcome set = AZ::Failure(AZStd::string());
    AzToolsFramework::EditorComponentAPIBus::BroadcastResult(set, &Components::SetComponentProperty, component.GetValue(), AZStd::string_view("Blocks"), AZStd::any(blocks));
    AzToolsFramework::EditorComponentAPIBus::BroadcastResult(set, &Components::SetComponentProperty, component.GetValue(), AZStd::string_view("Name"), AZStd::any(name));
    AzToolsFramework::ToolsApplicationRequestBus::Broadcast(&AzToolsFramework::ToolsApplicationRequests::AddDirtyEntity, m_entity);
    // A readable copy for Claude and for diffs (the component is what the game runs).
    const auto project = AZ::Utils::GetProjectPath();
    QDir folder(QDir(QString::fromUtf8(project.c_str())).filePath(QStringLiteral("Scripts/Blocks")));
    folder.mkpath(QStringLiteral("."));
    QString file = m_name.toLower();
    file.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    QFile copy(folder.filePath((file.isEmpty() ? QStringLiteral("script") : file) + QStringLiteral(".json")));
    if (copy.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        QJsonObject withName = m_graph;
        withName.insert(QStringLiteral("name"), m_name);
        withName.insert(QStringLiteral("entity"), m_scripts->currentText());
        copy.write(QJsonDocument(withName).toJson(QJsonDocument::Indented));
    }
}

void HearthScriptBlocksPane::NewScript()
{
    if (!GetIEditor() || GetIEditor()->GetLevelName().isEmpty())
    {
        m_running->setText(tr("Open a level first."));
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New script"), tr("What is it for?"), QLineEdit::Normal, tr("Exit door"), &ok).trimmed();
    if (!ok || name.isEmpty())
    {
        ListScripts();
        return;
    }
    // On the picked entity if there is one, otherwise on a new entity named after the script.
    AzToolsFramework::EntityIdList selected;
    AzToolsFramework::ToolsApplicationRequestBus::BroadcastResult(selected, &AzToolsFramework::ToolsApplicationRequests::GetSelectedEntities);
    AzToolsFramework::ScopedUndoBatch undo("New Hearth Blocks script");
    AZ::EntityId entity = selected.size() == 1 ? selected.front() : AZ::EntityId();
    if (!entity.IsValid())
    {
        AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(entity, &AzToolsFramework::EditorEntityContextRequests::CreateNewEditorEntity,
                                                                         (name + QStringLiteral(" script")).toUtf8().constData());
    }
    const AZ::Uuid type = BlocksComponentType(BlocksComponentName);
    if (!entity.IsValid() || type.IsNull())
    {
        m_running->setText(tr("Couldn't add Hearth Blocks: is Noclip Protocol built with this engine?"));
        return;
    }
    Components::AddComponentsOutcome added = AZ::Failure(AZStd::string());
    AzToolsFramework::EditorComponentAPIBus::BroadcastResult(added, &Components::AddComponentOfType, entity, type);
    m_entity = entity;
    m_name = name;
    m_graph = QJsonObject{ { QStringLiteral("blocks"), QJsonArray() } };
    SaveScript();
    ListScripts();
}

QJsonObject HearthScriptBlocksPane::Block(int id) const
{
    for (const QJsonValue& value : m_graph.value(QStringLiteral("blocks")).toArray())
    {
        if (value.toObject().value(QStringLiteral("id")).toInt() == id)
        {
            return value.toObject();
        }
    }
    return {};
}

void HearthScriptBlocksPane::SetBlock(const QJsonObject& block)
{
    QJsonArray blocks = m_graph.value(QStringLiteral("blocks")).toArray();
    for (int i = 0; i < blocks.size(); ++i)
    {
        if (blocks[i].toObject().value(QStringLiteral("id")).toInt() == block.value(QStringLiteral("id")).toInt())
        {
            blocks[i] = block;
            m_graph.insert(QStringLiteral("blocks"), blocks);
            return;
        }
    }
    blocks.append(block);
    m_graph.insert(QStringLiteral("blocks"), blocks);
}

int HearthScriptBlocksPane::NextId() const
{
    int most = 0;
    for (const QJsonValue& value : m_graph.value(QStringLiteral("blocks")).toArray())
    {
        most = qMax(most, value.toObject().value(QStringLiteral("id")).toInt());
    }
    return most + 1;
}

void HearthScriptBlocksPane::AddBlock(const QString& kind, int after, bool onNo)
{
    if (!m_entity.IsValid())
    {
        NewScript();
        if (!m_entity.IsValid())
        {
            return;
        }
    }
    QJsonObject block{ { QStringLiteral("id"), NextId() }, { QStringLiteral("kind"), kind }, { QStringLiteral("next"), QJsonArray() } };
    if (kind == QLatin1String("when.enter"))
    {
        block.insert(QStringLiteral("who"), QStringLiteral("player"));
    }
    else if (kind == QLatin1String("check.item"))
    {
        block.insert(QStringLiteral("no"), QJsonArray());
    }
    else if (kind == QLatin1String("do.move"))
    {
        block.insert(QStringLiteral("mode"), QStringLiteral("open"));
        block.insert(QStringLiteral("seconds"), 1.5);
        block.insert(QStringLiteral("z"), 3.0);
    }
    else if (kind == QLatin1String("when.seen"))
    {
        block.insert(QStringLiteral("distance"), 12.0);
    }
    SetBlock(block);
    const QJsonObject parent = Block(after);
    if (!parent.isEmpty() && !kind.startsWith(QLatin1String("when")))
    {
        QJsonObject changed = parent;
        const QString key = onNo ? QStringLiteral("no") : QStringLiteral("next");
        QJsonArray ids = changed.value(key).toArray();
        ids.append(block.value(QStringLiteral("id")));
        changed.insert(key, ids);
        SetBlock(changed);
    }
    m_selected = block.value(QStringLiteral("id")).toInt();
    if (kind != QLatin1String("do.shift"))
    {
        EditBlock(m_selected);
    }
    SaveScript();
    Rebuild();
}

void HearthScriptBlocksPane::RemoveBlock(int id)
{
    QJsonArray blocks = m_graph.value(QStringLiteral("blocks")).toArray();
    QJsonArray kept;
    for (const QJsonValue& value : blocks)
    {
        QJsonObject block = value.toObject();
        if (block.value(QStringLiteral("id")).toInt() == id)
        {
            continue;
        }
        for (const char* key : { "next", "no" })
        {
            QJsonArray ids = BlocksIds(block, key);
            QJsonArray left;
            for (const QJsonValue& n : ids)
            {
                if (n.toInt() != id)
                {
                    left.append(n);
                }
            }
            // What came after the removed block now follows its parent.
            if (left.size() != ids.size())
            {
                for (const QJsonValue& n : BlocksIds(Block(id), "next"))
                {
                    left.append(n);
                }
            }
            if (block.contains(QLatin1String(key)))
            {
                block.insert(QLatin1String(key), left);
            }
        }
        kept.append(block);
    }
    m_graph.insert(QStringLiteral("blocks"), kept);
    m_selected = -1;
    SaveScript();
    Rebuild();
}

QString HearthScriptBlocksPane::EnsureLineEntity(const QJsonObject& line)
{
    // A Voice Studio line plays from an entity with a MiniAudio Playback component (made here, saved with the level).
    QString label = line.value(QStringLiteral("who")).toString() + QStringLiteral("_") + line.value(QStringLiteral("id")).toString();
    label.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9]+")), QStringLiteral("_"));
    const QString entity = QStringLiteral("Line_") + label;
    QString source = QStringLiteral("Voice/") + line.value(QStringLiteral("made")).toObject().value(QStringLiteral("file")).toString();
    source.replace(QLatin1Char('\''), QStringLiteral("\\'"));
    const QString code = QStringLiteral(
        "import azlmbr.bus as b, azlmbr.editor as ed, azlmbr.entity as ent, azlmbr.asset as asset, azlmbr.math as m\n"
        "name, source = '%1', '%2'\n"
        "tid = ed.EditorComponentAPIBus(b.Broadcast, 'FindComponentTypeIdsByEntityType', ['MiniAudio Playback'], ent.EntityType().Game)\n"
        "e = None\n"
        "for x in ed.ToolsApplicationRequestBus(b.Broadcast, 'GetAllEntities') or []:\n"
        "    if ed.EditorEntityInfoRequestBus(b.Event, 'GetName', x) == name:\n"
        "        e = x\n"
        "if e is None:\n"
        "    e = ed.ToolsApplicationRequestBus(b.Broadcast, 'CreateNewEntity', ent.EntityId())\n"
        "    ed.EditorEntityAPIBus(b.Event, 'SetName', e, name)\n"
        "if tid:\n"
        "    got = ed.EditorComponentAPIBus(b.Broadcast, 'GetComponentOfType', e, tid[0])\n"
        "    comp = got.GetValue() if got.IsSuccess() else ed.EditorComponentAPIBus(b.Broadcast, 'AddComponentsOfType', e, [tid[0]]).GetValue()[0]\n"
        "    aid = asset.AssetCatalogRequestBus(b.Broadcast, 'GetAssetIdByPath', source.lower() + '.miniaudio', m.Uuid(), False)\n"
        "    for p in ed.EditorComponentAPIBus(b.Broadcast, 'BuildComponentPropertyList', comp):\n"
        "        if p.endswith('Sound Asset') and aid and aid.is_valid():\n"
        "            ed.EditorComponentAPIBus(b.Broadcast, 'SetComponentProperty', comp, p, aid)\n"
        "        elif p.endswith('Autoplay'):\n"
        "            ed.EditorComponentAPIBus(b.Broadcast, 'SetComponentProperty', comp, p, False)\n")
        .arg(entity, source);
    AzToolsFramework::EditorPythonRunnerRequestBus::Broadcast(&AzToolsFramework::EditorPythonRunnerRequests::ExecuteByString,
                                                              AZStd::string_view(code.toUtf8().constData()), false);
    return entity;
}

void HearthScriptBlocksPane::EditBlock(int id)
{
    QJsonObject block = Block(id);
    if (block.isEmpty())
    {
        return;
    }
    const QString kind = block.value(QStringLiteral("kind")).toString();
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Change the block"));
    auto* form = new QFormLayout(&dialog);
    auto entityBox = [&dialog](const QStringList& names, const QString& current)
    {
        auto* box = new QComboBox(&dialog);
        box->setEditable(true);
        box->addItems(names);
        box->setCurrentText(current);
        return box;
    };
    auto number = [&dialog](double value, double low, double high, const QString& suffix)
    {
        auto* spin = new QDoubleSpinBox(&dialog);
        spin->setRange(low, high);
        spin->setDecimals(2);
        spin->setValue(value);
        spin->setSuffix(suffix);
        return spin;
    };
    QComboBox* target = nullptr;
    QComboBox* who = nullptr;
    QComboBox* mode = nullptr;
    QComboBox* sound = nullptr;
    QComboBox* line = nullptr;
    QLineEdit* text = nullptr;
    QLineEdit* title = new QLineEdit(block.value(QStringLiteral("title")).toString(), &dialog);
    title->setPlaceholderText(tr("Leave empty to say it in plain words"));
    QDoubleSpinBox* seconds = nullptr;
    QDoubleSpinBox* distance = nullptr;
    QDoubleSpinBox* x = nullptr;
    QDoubleSpinBox* y = nullptr;
    QDoubleSpinBox* z = nullptr;
    const QString current = block.value(QStringLiteral("target")).toString();
    QJsonArray voiceLines;
    if (kind == QLatin1String("when.enter"))
    {
        target = entityBox(EntityNames("PhysX Primitive Collider") + EntityNames("PhysX Mesh Collider"), current);
        form->addRow(tr("Trigger area"), target);
        who = new QComboBox(&dialog);
        who->addItems({ tr("player"), tr("anyone") });
        who->setCurrentIndex(block.value(QStringLiteral("who")).toString() == QLatin1String("anyone") ? 1 : 0);
        form->addRow(tr("Who"), who);
    }
    else if (kind == QLatin1String("when.lightDies"))
    {
        target = entityBox(EntityNames("Light") + EntityNames("Flicker Light"), current);
        form->addRow(tr("Light"), target);
    }
    else if (kind == QLatin1String("when.seen"))
    {
        target = entityBox(EntityNames(), current);
        form->addRow(tr("Entity"), target);
        distance = number(block.value(QStringLiteral("distance")).toDouble(12.0), 1.0, 100.0, tr(" m"));
        form->addRow(tr("Sees as far as"), distance);
    }
    else if (kind == QLatin1String("check.item"))
    {
        text = new QLineEdit(current, &dialog);
        text->setPlaceholderText(tr("Level 2 keycard"));
        form->addRow(tr("Item"), text);
    }
    else if (kind == QLatin1String("do.play"))
    {
        target = entityBox(EntityNames("MiniAudio Playback"), current);
        form->addRow(tr("Sound (entity)"), target);
        line = new QComboBox(&dialog);
        line->addItem(tr("Or a line from Voice Studio…"), QString());
        const auto project = AZ::Utils::GetProjectPath();
        QFile file(QDir(QString::fromUtf8(project.c_str())).filePath(QStringLiteral("Voice/voice_lines.json")));
        if (file.open(QIODevice::ReadOnly))
        {
            for (const QJsonValue& scene : QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("scenes")).toArray())
            {
                for (const QJsonValue& value : scene.toObject().value(QStringLiteral("lines")).toArray())
                {
                    const QJsonObject l = value.toObject();
                    if (!l.value(QStringLiteral("id")).toString().isEmpty())
                    {
                        voiceLines.append(l);
                        line->addItem(QStringLiteral("%1: \"%2\"").arg(l.value(QStringLiteral("who")).toString(), l.value(QStringLiteral("text")).toString().left(60)),
                                      l.value(QStringLiteral("id")).toString());
                    }
                }
            }
        }
        line->setCurrentIndex(qMax(0, line->findData(block.value(QStringLiteral("line")).toString())));
        form->addRow(tr("Line"), line);
    }
    else if (kind == QLatin1String("do.move"))
    {
        target = entityBox(EntityNames(), current);
        form->addRow(tr("What"), target);
        mode = new QComboBox(&dialog);
        mode->addItems({ QStringLiteral("open"), QStringLiteral("close"), QStringLiteral("move") });
        mode->setCurrentText(block.value(QStringLiteral("mode")).toString(QStringLiteral("open")));
        form->addRow(tr("Do"), mode);
        x = number(block.value(QStringLiteral("x")).toDouble(), -100.0, 100.0, tr(" m"));
        y = number(block.value(QStringLiteral("y")).toDouble(), -100.0, 100.0, tr(" m"));
        z = number(block.value(QStringLiteral("z")).toDouble(3.0), -100.0, 100.0, tr(" m"));
        auto* offset = new QHBoxLayout();
        offset->addWidget(x);
        offset->addWidget(y);
        offset->addWidget(z);
        form->addRow(tr("By (x, y, z)"), offset);
        seconds = number(block.value(QStringLiteral("seconds")).toDouble(1.5), 0.05, 60.0, tr(" s"));
        form->addRow(tr("Takes"), seconds);
        sound = entityBox(QStringList{ QString() } + EntityNames("MiniAudio Playback"), block.value(QStringLiteral("sound")).toString());
        form->addRow(tr("Sound while it moves"), sound);
    }
    else if (kind == QLatin1String("do.level"))
    {
        target = new QComboBox(&dialog);
        target->setEditable(true);
        const auto project = AZ::Utils::GetProjectPath();
        target->addItems(QDir(QDir(QString::fromUtf8(project.c_str())).filePath(QStringLiteral("Levels"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot));
        target->setCurrentText(current);
        form->addRow(tr("Level"), target);
    }
    form->addRow(tr("Title"), title);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    if (target)
    {
        block.insert(QStringLiteral("target"), target->currentText().trimmed());
    }
    if (text)
    {
        block.insert(QStringLiteral("target"), text->text().trimmed());
    }
    if (who)
    {
        block.insert(QStringLiteral("who"), who->currentIndex() == 1 ? QStringLiteral("anyone") : QStringLiteral("player"));
    }
    if (mode)
    {
        block.insert(QStringLiteral("mode"), mode->currentText());
    }
    if (sound)
    {
        block.insert(QStringLiteral("sound"), sound->currentText().trimmed());
    }
    if (seconds)
    {
        block.insert(QStringLiteral("seconds"), seconds->value());
    }
    if (distance)
    {
        block.insert(QStringLiteral("distance"), distance->value());
    }
    if (x)
    {
        block.insert(QStringLiteral("x"), x->value());
        block.insert(QStringLiteral("y"), y->value());
        block.insert(QStringLiteral("z"), z->value());
    }
    if (line && !line->currentData().toString().isEmpty())
    {
        for (const QJsonValue& value : voiceLines)
        {
            if (value.toObject().value(QStringLiteral("id")).toString() == line->currentData().toString())
            {
                const QJsonObject l = value.toObject();
                block.insert(QStringLiteral("line"), l.value(QStringLiteral("id")).toString());
                block.insert(QStringLiteral("say"), QStringLiteral("%1: \"%2\"").arg(l.value(QStringLiteral("who")).toString(), l.value(QStringLiteral("text")).toString()));
                block.insert(QStringLiteral("target"), EnsureLineEntity(l));
            }
        }
    }
    else if (line)
    {
        block.remove(QStringLiteral("line"));
        block.remove(QStringLiteral("say"));
    }
    if (title->text().trimmed().isEmpty())
    {
        block.remove(QStringLiteral("title"));
    }
    else
    {
        block.insert(QStringLiteral("title"), title->text().trimmed());
    }
    SetBlock(block);
    SaveScript();
    Rebuild();
}

QWidget* HearthScriptBlocksPane::BuildCard(const QJsonObject& block, QWidget* parent)
{
    const int id = block.value(QStringLiteral("id")).toInt();
    const QString kind = block.value(QStringLiteral("kind")).toString();
    const QString target = block.value(QStringLiteral("target")).toString();
    const QString unset = tr("(pick one)");
    QString title;
    QString detail;
    QString tone = QStringLiteral("#E8743B");
    if (kind == QLatin1String("when.enter"))
    {
        title = block.value(QStringLiteral("who")).toString() == QLatin1String("anyone") ? tr("When anyone enters") : tr("When the player enters");
        detail = tr("Trigger: %1 · Who: %2").arg(target.isEmpty() ? unset : target, block.value(QStringLiteral("who")).toString(QStringLiteral("player")));
    }
    else if (kind == QLatin1String("when.lightDies"))
    {
        title = tr("When a light dies");
        detail = tr("Light: %1").arg(target.isEmpty() ? unset : target);
    }
    else if (kind == QLatin1String("when.seen"))
    {
        title = tr("When %1 sees the player").arg(target.isEmpty() ? tr("an entity") : target);
        detail = tr("Within %1 m, in front of it").arg(block.value(QStringLiteral("distance")).toDouble(12.0), 0, 'f', 0);
    }
    else if (kind == QLatin1String("check.item"))
    {
        tone = QStringLiteral("#F0D17A");
        title = tr("Has the %1?").arg(target.isEmpty() ? tr("item") : target.section(QLatin1Char(' '), -1));
        detail = tr("Item: %1").arg(target.isEmpty() ? unset : target);
    }
    else if (kind == QLatin1String("do.play"))
    {
        tone = QStringLiteral("#7FE0A0");
        const bool isLine = block.contains(QStringLiteral("line"));
        title = isLine ? tr("Say a line") : tr("Play a sound");
        detail = isLine ? block.value(QStringLiteral("say")).toString() : tr("Play: %1").arg(target.isEmpty() ? unset : target);
    }
    else if (kind == QLatin1String("do.move"))
    {
        tone = QStringLiteral("#7FE0A0");
        const QString mode = block.value(QStringLiteral("mode")).toString(QStringLiteral("open"));
        const QString what = target.isEmpty() ? tr("something") : target;
        title = mode == QLatin1String("close") ? tr("Close %1").arg(what) : (mode == QLatin1String("move") ? tr("Move %1").arg(what) : tr("Open %1").arg(what));
        const QString sound = block.value(QStringLiteral("sound")).toString();
        detail = sound.isEmpty() ? tr("%1 s").arg(block.value(QStringLiteral("seconds")).toDouble(1.5), 0, 'f', 1)
                                 : tr("Play: %1 · %2 s").arg(sound).arg(block.value(QStringLiteral("seconds")).toDouble(1.5), 0, 'f', 1);
    }
    else if (kind == QLatin1String("do.level"))
    {
        tone = QStringLiteral("#7FE0A0");
        title = tr("Send to another level");
        detail = tr("Level: %1").arg(target.isEmpty() ? unset : target);
    }
    else if (kind == QLatin1String("do.shift"))
    {
        tone = QStringLiteral("#7FE0A0");
        title = tr("Shift the rooms");
        detail = tr("Every room the player isn't standing in changes");
    }
    if (block.contains(QStringLiteral("title")))
    {
        title = block.value(QStringLiteral("title")).toString();
    }

    auto* card = new BlocksCard(parent);
    card->setObjectName(QStringLiteral("BlocksCard"));
    const bool lit = id == m_lit;
    const bool picked = id == m_selected;
    card->setStyleSheet(QStringLiteral("QFrame#BlocksCard { background-color: %1; border: %2; border-left: 4px solid %3; border-radius: 12px; }"
                                       "QFrame#BlocksCard QLabel { background: transparent; }")
                            .arg(lit ? QStringLiteral("rgba(232, 116, 59, 70)") : QStringLiteral("#1B1815"),
                                 picked || lit ? QStringLiteral("2px solid #E8743B") : QStringLiteral("1px solid rgba(255, 255, 255, 20)"), tone));
    card->setMinimumWidth(240);
    card->setMaximumWidth(340);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(2);
    layout->addWidget(Hearth::MakeLabel(title, QStringLiteral("title"), card));
    QLabel* text = Hearth::MakeLabel(detail, QStringLiteral("small"), card);
    text->setWordWrap(true);
    layout->addWidget(text);
    card->m_onClick = [this, id]()
    {
        m_selected = id;
        Rebuild();
    };
    card->m_onEdit = [this, id]() { EditBlock(id); };
    card->m_onMenu = [this, id, kind](const QPoint& at)
    {
        QMenu menu;
        QMenu* after = menu.addMenu(kind == QLatin1String("check.item") ? tr("Add after (Yes)") : tr("Add after"));
        for (const BlocksKind& k : BlocksKinds)
        {
            if (QLatin1String(k.group) != QLatin1String("When"))
            {
                const QString name = QLatin1String(k.kind);
                after->addAction(tr(k.palette), [this, name, id]() { AddBlock(name, id); });
            }
        }
        if (kind == QLatin1String("check.item"))
        {
            QMenu* no = menu.addMenu(tr("Add after (No)"));
            for (const BlocksKind& k : BlocksKinds)
            {
                if (QLatin1String(k.group) != QLatin1String("When"))
                {
                    const QString name = QLatin1String(k.kind);
                    no->addAction(tr(k.palette), [this, name, id]() { AddBlock(name, id, true); });
                }
            }
        }
        menu.addAction(tr("Change…"), [this, id]() { EditBlock(id); });
        menu.addAction(tr("Remove"), [this, id]() { RemoveBlock(id); });
        menu.exec(at);
    };
    return card;
}

QWidget* HearthScriptBlocksPane::BuildBranch(const QJsonArray& ids, QWidget* parent, int depth)
{
    auto* column = new QWidget(parent);
    auto* layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    for (const QJsonValue& value : ids)
    {
        const QJsonObject block = Block(value.toInt());
        if (block.isEmpty() || depth > 24)
        {
            continue;
        }
        layout->addWidget(BuildCard(block, column), 0, Qt::AlignHCenter);
        const QJsonArray next = BlocksIds(block, "next");
        const bool check = block.value(QStringLiteral("kind")).toString() == QLatin1String("check.item");
        if (next.isEmpty() && (!check || BlocksIds(block, "no").isEmpty()))
        {
            layout->addSpacing(12);
            continue;
        }
        auto* line = new QFrame(column);
        line->setFixedSize(2, 18);
        line->setStyleSheet(QStringLiteral("background-color: rgba(232, 116, 59, 140);"));
        layout->addWidget(line, 0, Qt::AlignHCenter);
        if (check)
        {
            auto* branches = new QHBoxLayout();
            branches->setSpacing(24);
            for (int side = 0; side < 2; ++side)
            {
                auto* holder = new QVBoxLayout();
                holder->setSpacing(4);
                holder->addWidget(Hearth::MakeChip(side == 0 ? tr("Yes") : tr("No"), side == 0 ? QStringLiteral("good") : QStringLiteral("bad"), column), 0, Qt::AlignHCenter);
                holder->addWidget(BuildBranch(BlocksIds(block, side == 0 ? "next" : "no"), column, depth + 1));
                holder->addStretch(1);
                branches->addLayout(holder);
            }
            layout->addLayout(branches);
        }
        else
        {
            layout->addWidget(BuildBranch(next, column, depth + 1));
        }
    }
    return column;
}

void HearthScriptBlocksPane::Rebuild()
{
    auto* page = new QWidget();
    auto* layout = new QHBoxLayout(page);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(32);
    layout->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    // Each When starts its own chain.
    QJsonArray roots;
    for (const QJsonValue& value : m_graph.value(QStringLiteral("blocks")).toArray())
    {
        if (value.toObject().value(QStringLiteral("kind")).toString().startsWith(QLatin1String("when")))
        {
            roots.append(value.toObject().value(QStringLiteral("id")));
        }
    }
    for (const QJsonValue& root : roots)
    {
        layout->addWidget(BuildBranch(QJsonArray{ root }, page, 0), 0, Qt::AlignTop);
    }
    if (roots.isEmpty())
    {
        QLabel* empty = Hearth::MakeLabel(m_entity.IsValid() ? tr("Start with a When block from the list on the right.")
                                                             : tr("Pick a script above, or make a new one."),
                                          QStringLiteral("dim"), page);
        layout->addWidget(empty, 0, Qt::AlignCenter);
    }
    m_scroll->setWidget(page);
}
