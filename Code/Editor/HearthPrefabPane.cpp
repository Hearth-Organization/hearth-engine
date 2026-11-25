/*
 * Hearth Engine Prefab editing. Built on O3DE (Apache-2.0 OR MIT). See HearthPrefabPane.h.
 */

#include "EditorDefs.h"

#include "HearthPrefabPane.h"
#include "QtViewPaneManager.h"

#include <AzCore/Console/IConsole.h>
#include <AzCore/Interface/Interface.h>
#include <AzCore/JSON/stringbuffer.h>
#include <AzCore/JSON/writer.h>
#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>
#include <AzToolsFramework/Component/EditorComponentAPIBus.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>
#include <AzToolsFramework/Entity/EditorEntityInfoBus.h>
#include <AzToolsFramework/Prefab/Instance/Instance.h>
#include <AzToolsFramework/Prefab/Instance/TemplateInstanceMapperInterface.h>
#include <AzToolsFramework/Prefab/PrefabFocusInterface.h>
#include <AzToolsFramework/Prefab/PrefabFocusPublicInterface.h>
#include <AzToolsFramework/Prefab/PrefabLoaderInterface.h>
#include <AzToolsFramework/Prefab/PrefabSystemComponentInterface.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/Entity.h>

#include <QColorDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMap>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>
#include <memory>
#include <QSet>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;
    namespace Prefab = AzToolsFramework::Prefab;
    using ComponentApi = AzToolsFramework::EditorComponentAPIBus;
    using ComponentRequests = AzToolsFramework::EditorComponentAPIRequests;

    AzFramework::EntityContextId PrefabContext()
    {
        AzFramework::EntityContextId context = AzFramework::EntityContextId::CreateNull();
        AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(context, &AzToolsFramework::EditorEntityContextRequests::GetEditorEntityContextId);
        return context;
    }

    QJsonObject PrefabDomToJson(const rapidjson::Value& dom)
    {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        dom.Accept(writer);
        return QJsonDocument::fromJson(QByteArray(buffer.GetString(), int(buffer.GetSize()))).object();
    }

    QJsonObject PrefabCurrentDom(Prefab::TemplateId id)
    {
        auto* system = AZ::Interface<Prefab::PrefabSystemComponentInterface>::Get();
        if (!system || id == Prefab::InvalidTemplateId)
        {
            return {};
        }
        return PrefabDomToJson(system->FindTemplateDom(id));
    }

    //! "L0_FlickerBulb" in "L0_LightFlicker" -> "Bulb".
    QString PrefabNiceName(QString name, const QString& prefabName)
    {
        QString prefix = prefabName.section(QLatin1Char('_'), 0, 0) + QLatin1Char('_');
        if (name.startsWith(prefix) && name.size() > prefix.size())
        {
            name = name.mid(prefix.size());
        }
        const QString word = prefabName.section(QLatin1Char('_'), 1).remove(QRegularExpression(QStringLiteral("^Light")));
        if (!word.isEmpty() && name.startsWith(word) && name.size() > word.size())
        {
            name = name.mid(word.size());
        }
        name.replace(QRegularExpression(QStringLiteral("([a-z])([A-Z])")), QStringLiteral("\\1 \\2"));
        return name;
    }

    QString PrefabSoundName(const QString& hint)
    {
        // "assets/sounds/level0/level0_hum.wav.miniaudio" -> "Hum sound"
        QString stem = QFileInfo(hint).fileName().section(QLatin1Char('.'), 0, 0);
        stem = stem.section(QLatin1Char('_'), -1);
        if (!stem.isEmpty())
        {
            stem[0] = stem[0].toUpper();
        }
        return QObject::tr("%1 sound").arg(stem);
    }

    QString PrefabLightType(int type)
    {
        switch (type)
        {
        case 1: return QObject::tr("Sphere");
        case 2: case 7: return QObject::tr("Spot");
        case 3: return QObject::tr("Tube");
        case 4: return QObject::tr("Panel");
        case 5: return QObject::tr("Polygon");
        case 6: return QObject::tr("Point");
        default: return QObject::tr("Light");
        }
    }

    QString PrefabColorName(const QColor& c)
    {
        const double r = c.redF();
        const double g = c.greenF();
        const double b = c.blueF();
        if (qMin(r, qMin(g, b)) > 0.7)
        {
            if (r - b > 0.08)
            {
                return QObject::tr("Warm white");
            }
            if (b - r > 0.08)
            {
                return QObject::tr("Cool white");
            }
            return QObject::tr("White");
        }
        return c.name().toUpper();
    }

    //! Every leaf of a prefab's entities: "<entity alias>/<component>/<path>" -> value.
    void PrefabFlatten(const QJsonValue& value, const QString& path, QMap<QString, QJsonValue>& out)
    {
        if (value.isObject())
        {
            const QJsonObject object = value.toObject();
            for (auto it = object.begin(); it != object.end(); ++it)
            {
                if (it.key() != QLatin1String("Id") && it.key() != QLatin1String("$type"))
                {
                    PrefabFlatten(it.value(), path.isEmpty() ? it.key() : path + QLatin1Char('/') + it.key(), out);
                }
            }
        }
        else if (value.isArray() && !value.toArray().isEmpty() && value.toArray().first().isDouble())
        {
            out.insert(path, value);
        }
        else if (value.isArray())
        {
            const QJsonArray array = value.toArray();
            for (int i = 0; i < array.size(); ++i)
            {
                PrefabFlatten(array[i], path + QStringLiteral("/%1").arg(i), out);
            }
        }
        else
        {
            out.insert(path, value);
        }
    }

    QString PrefabValueText(const QString& key, const QJsonValue& value)
    {
        if (value.isDouble())
        {
            const double v = value.toDouble();
            if (key.endsWith(QLatin1String("Volume")))
            {
                return QStringLiteral("%1%").arg(qRound(v));
            }
            if (key.endsWith(QLatin1String("Chance")))
            {
                return QStringLiteral("%1%").arg(qRound(v * 100));
            }
            return QString::number(v, 'g', 4);
        }
        if (value.isBool())
        {
            return value.toBool() ? QObject::tr("on") : QObject::tr("off");
        }
        if (value.isArray())
        {
            const QJsonArray a = value.toArray();
            if (a.size() >= 3 && key.endsWith(QLatin1String("Color")))
            {
                return PrefabColorName(QColor::fromRgbF(qBound(0.0, a[0].toDouble(), 1.0), qBound(0.0, a[1].toDouble(), 1.0), qBound(0.0, a[2].toDouble(), 1.0)));
            }
            QStringList parts;
            for (const QJsonValue& v : a)
            {
                parts << QString::number(v.toDouble(), 'g', 3);
            }
            return parts.join(QStringLiteral(", "));
        }
        return value.toString().section(QLatin1Char('/'), -1);
    }

    //! "Volume" in "Controller/Config/Volume" -> "volume"; "AttenuationRadius" -> "range".
    QString PrefabKeyText(const QString& key)
    {
        if (key == QLatin1String("AttenuationRadius")) return QObject::tr("range");
        if (key == QLatin1String("CrackleChance")) return QObject::tr("crackle");
        if (key == QLatin1String("BlackoutChance")) return QObject::tr("blackouts");
        QString text = key;
        text.replace(QRegularExpression(QStringLiteral("([a-z])([A-Z])")), QStringLiteral("\\1 \\2"));
        return text.toLower();
    }

    //! A property path of a live component whose last part contains the word.
    AZStd::string PrefabPropertyPath(const AZ::EntityComponentIdPair& component, const QString& word, const QString& not_ = QString())
    {
        AZStd::vector<AZStd::string> paths;
        ComponentApi::BroadcastResult(paths, &ComponentRequests::BuildComponentPropertyList, component);
        for (const AZStd::string& path : paths)
        {
            const QString full = QString::fromUtf8(path.c_str());
            if (full.section(QLatin1Char('|'), -1).contains(word, Qt::CaseInsensitive) && (not_.isEmpty() || !full.contains(not_, Qt::CaseInsensitive)))
            {
                return path;
            }
        }
        return {};
    }

    AZ::EntityComponentIdPair PrefabComponent(AZ::EntityId entity, const char* componentName)
    {
        AZStd::vector<AZ::Uuid> types;
        ComponentApi::BroadcastResult(types, &ComponentRequests::FindComponentTypeIdsByEntityType, AZStd::vector<AZStd::string>{ componentName },
                                      ComponentRequests::EntityType::Game);
        if (types.empty() || types.front().IsNull())
        {
            return {};
        }
        ComponentRequests::GetComponentOutcome got = AZ::Failure(AZStd::string());
        ComponentApi::BroadcastResult(got, &ComponentRequests::GetComponentOfType, entity, types.front());
        return got.IsSuccess() ? got.GetValue() : AZ::EntityComponentIdPair();
    }

    //! Opens the Prefab editing window whenever you go into a prefab (it's made the first time).
    class PrefabFocusOpener : public Prefab::PrefabFocusNotificationBus::Handler
    {
    public:
        void Connect()
        {
            Prefab::PrefabFocusNotificationBus::Handler::BusConnect(PrefabContext());
        }
        void OnPrefabFocusChanged(AZ::EntityId, AZ::EntityId) override
        {
            auto* focus = AZ::Interface<Prefab::PrefabFocusPublicInterface>::Get();
            if (focus && focus->GetPrefabFocusPathLength(PrefabContext()) > 1)
            {
                QTimer::singleShot(0, []() { QtViewPaneManager::instance()->OpenPane(QStringLiteral("Prefab editing")); });
            }
        }
    };
} // namespace

void HearthPrefabPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::RightDockWidgetArea;
    options.paneRect = QRect(200, 120, 420, 760);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthPrefabPane>("Prefab editing", "Tools", options);
    static PrefabFocusOpener opener;
    QTimer::singleShot(0, []() { opener.Connect(); });
}

HearthPrefabPane::HearthPrefabPane(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthPrefab"));
    setStyleSheet(QStringLiteral("QWidget#HearthPrefab { background-color: #0D0C0B; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    QVBoxLayout* top = nullptr;
    QFrame* topCard = Hearth::MakeCard(this, &top);
    m_path = Hearth::MakeLabel(QString(), QStringLiteral("title"), topCard);
    m_path->setTextFormat(Qt::RichText);
    top->addWidget(m_path);
    m_subtitle = Hearth::MakeLabel(QString(), QStringLiteral("small"), topCard);
    top->addWidget(m_subtitle);
    auto* buttons = new QHBoxLayout();
    m_back = Hearth::MakeButton(tr("Back to the level"), false, topCard);
    connect(m_back, &QPushButton::clicked, this, []()
    {
        if (auto* focus = AZ::Interface<Prefab::PrefabFocusPublicInterface>::Get())
        {
            focus->FocusOnPathIndex(PrefabContext(), 0);
        }
    });
    m_save = Hearth::MakeButton(tr("Save to all"), true, topCard);
    connect(m_save, &QPushButton::clicked, this, [this]()
    {
        auto* prefabFocus = AZ::Interface<Prefab::PrefabFocusInterface>::Get();
        auto* loader = AZ::Interface<Prefab::PrefabLoaderInterface>::Get();
        if (!prefabFocus || !loader)
        {
            return;
        }
        const Prefab::TemplateId id = prefabFocus->GetFocusedPrefabTemplateId(PrefabContext());
        if (loader->SaveTemplate(id))
        {
            auto* system = AZ::Interface<Prefab::PrefabSystemComponentInterface>::Get();
            m_saved.CopyFrom(system->FindTemplateDom(id), m_saved.GetAllocator());
            FillChanges();
        }
    });
    buttons->addWidget(m_back);
    buttons->addWidget(m_save);
    top->addLayout(buttons);
    root->addWidget(topCard);

    QVBoxLayout* contents = nullptr;
    QFrame* contentsCard = Hearth::MakeCard(this, &contents);
    contents->addWidget(Hearth::MakeLabel(tr("In this prefab"), QStringLiteral("title"), contentsCard));
    m_contents = new QListWidget(contentsCard);
    m_contents->setFrameShape(QFrame::NoFrame);
    m_contents->setMaximumHeight(170);
    m_contents->setStyleSheet(QStringLiteral("QListWidget { background: transparent; outline: none; } QListWidget::item { padding: 5px 6px; border-radius: 7px; }"
                                             "QListWidget::item:selected { background-color: rgba(232, 116, 59, 41); color: #FFCFB4; }"));
    connect(m_contents, &QListWidget::itemClicked, this, [](QListWidgetItem* item)
    {
        const AZ::EntityId id(item->data(Qt::UserRole).toULongLong());
        if (id.IsValid())
        {
            AzToolsFramework::ToolsApplicationRequestBus::Broadcast(&AzToolsFramework::ToolsApplicationRequests::SetSelectedEntities, AzToolsFramework::EntityIdList{ id });
        }
    });
    contents->addWidget(m_contents);
    contents->addWidget(Hearth::MakeLabel(tr("Everything else in the level is hidden while you edit"), QStringLiteral("small"), contentsCard));
    auto* viewButtons = new QHBoxLayout();
    QPushButton* flicker = Hearth::MakeButton(tr("Flicker once"), false, contentsCard);
    connect(flicker, &QPushButton::clicked, this, &HearthPrefabPane::FlickerOnce);
    m_around = Hearth::MakeButton(tr("Show around it"), false, contentsCard);
    m_around->setCheckable(true);
    connect(m_around, &QPushButton::toggled, this, &HearthPrefabPane::SetShowAround);
    viewButtons->addWidget(flicker);
    viewButtons->addWidget(m_around);
    contents->addLayout(viewButtons);
    root->addWidget(contentsCard);

    QVBoxLayout* details = nullptr;
    QFrame* detailsCard = Hearth::MakeCard(this, &details);
    auto* detailsHolder = new QWidget(detailsCard);
    m_detailsLayout = new QVBoxLayout(detailsHolder);
    m_detailsLayout->setContentsMargins(0, 0, 0, 0);
    details->addWidget(detailsHolder);
    root->addWidget(detailsCard);

    QVBoxLayout* changes = nullptr;
    QFrame* changesCard = Hearth::MakeCard(this, &changes);
    changes->addWidget(Hearth::MakeLabel(tr("Changed here"), QStringLiteral("title"), changesCard));
    auto* changesHolder = new QWidget(changesCard);
    m_changesLayout = new QVBoxLayout(changesHolder);
    m_changesLayout->setContentsMargins(0, 0, 0, 0);
    changes->addWidget(changesHolder);
    root->addWidget(changesCard);
    m_footer = Hearth::MakeLabel(QString(), QStringLiteral("small"), this);
    m_footer->setWordWrap(true);
    root->addWidget(m_footer);
    root->addStretch(1);

    m_changesTimer = new QTimer(this);
    m_changesTimer->setInterval(1500);
    connect(m_changesTimer, &QTimer::timeout, this, &HearthPrefabPane::FillChanges);

    Prefab::PrefabFocusNotificationBus::Handler::BusConnect(PrefabContext());
}

HearthPrefabPane::~HearthPrefabPane()
{
    Prefab::PrefabFocusNotificationBus::Handler::BusDisconnect();
    SetShowAround(true);
}

void HearthPrefabPane::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    Refresh();
}

void HearthPrefabPane::OnPrefabFocusChanged(AZ::EntityId, AZ::EntityId)
{
    Refresh();
}

bool HearthPrefabPane::InPrefab() const
{
    auto* focus = AZ::Interface<Prefab::PrefabFocusPublicInterface>::Get();
    return focus && focus->GetPrefabFocusPathLength(PrefabContext()) > 1;
}

void HearthPrefabPane::SetShowAround(bool show)
{
    // While editing, the rest of the level is tinted to the background (the editor's focus-mode feedback);
    // Show around it brings it back desaturated.
    m_showAround = show;
    if (auto* console = AZ::Interface<AZ::IConsole>::Get())
    {
        if (show || !InPrefab())
        {
            console->PerformCommand("cl_editorModeTintPass_TintAmount 0");
        }
        else
        {
            console->PerformCommand("cl_editorModeTintPass_TintColor 0.051 0.047 0.043 1");
            console->PerformCommand("cl_editorModeTintPass_TintAmount 1");
        }
    }
}

void HearthPrefabPane::Refresh()
{
    auto* prefabFocus = AZ::Interface<Prefab::PrefabFocusInterface>::Get();
    auto* focus = AZ::Interface<Prefab::PrefabFocusPublicInterface>::Get();
    if (!InPrefab() || !prefabFocus)
    {
        m_path->setText(tr("Not in a prefab"));
        m_subtitle->setText(tr("Double-click a prefab in the level (or pick it and press Edit prefab) to change every copy at once."));
        m_back->setEnabled(false);
        m_save->setEnabled(false);
        m_contents->clear();
        m_changesTimer->stop();
        SetShowAround(true);
        return;
    }
    const Prefab::TemplateId id = prefabFocus->GetFocusedPrefabTemplateId(PrefabContext());
    auto* system = AZ::Interface<Prefab::PrefabSystemComponentInterface>::Get();
    if (system && (m_savedTemplate != AZ::u64(id) || m_saved.IsNull()))
    {
        m_saved.CopyFrom(system->FindTemplateDom(id), m_saved.GetAllocator());
        m_savedTemplate = AZ::u64(id);
    }
    // Where you are: Level 0 / L0_LightFlicker
    const QString focusPath = QString::fromUtf8(focus->GetPrefabFocusPath(PrefabContext()).c_str());
    QStringList crumbs = focusPath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (QString& crumb : crumbs)
    {
        crumb = crumb.toHtmlEscaped();
    }
    if (!crumbs.isEmpty())
    {
        crumbs.last() = QStringLiteral("<span style='color:#FFCFB4'>%1</span>").arg(crumbs.last());
    }
    m_path->setText(crumbs.join(QStringLiteral(" <span style='color:#8D857B'>/</span> ")));

    // How many copies are in the level.
    m_uses = 0;
    if (auto* mapper = AZ::Interface<Prefab::TemplateInstanceMapperInterface>::Get())
    {
        auto instances = mapper->FindInstancesOwnedByTemplate(id);
        if (instances.has_value())
        {
            m_uses = int(instances->get().size());
        }
    }
    const QString level = GetIEditor() ? GetIEditor()->GetLevelName() : QString();
    const QJsonObject dom = PrefabCurrentDom(id);
    const bool isLight = QJsonDocument(dom).toJson().contains("AreaLightComponent");
    const QString noun = isLight ? tr("lights") : tr("copies");
    m_subtitle->setText(tr("Editing a prefab · used %n time(s) in %1", nullptr, m_uses).arg(level.isEmpty() ? tr("the level") : level));
    m_save->setText(m_uses > 1 ? tr("Save to all %1").arg(m_uses) : tr("Save"));
    m_footer->setText(tr("Saving changes all %1 %2 in %3. %4 you changed one by one in the level keep their own values.")
                          .arg(m_uses).arg(noun, level.isEmpty() ? tr("the level") : level, isLight ? tr("Lights") : tr("Copies")));
    m_back->setEnabled(true);
    m_save->setEnabled(true);
    SetShowAround(m_around->isChecked());
    FillContents();
    FillDetails();
    FillChanges();
    m_changesTimer->start();
}

void HearthPrefabPane::FillContents()
{
    m_contents->clear();
    auto* prefabFocus = AZ::Interface<Prefab::PrefabFocusInterface>::Get();
    if (!prefabFocus)
    {
        return;
    }
    auto instance = prefabFocus->GetFocusedPrefabInstance(PrefabContext());
    const QJsonObject dom = PrefabCurrentDom(prefabFocus->GetFocusedPrefabTemplateId(PrefabContext()));
    const QString prefabName = dom.value(QStringLiteral("ContainerEntity")).toObject().value(QStringLiteral("Name")).toString();
    const QJsonObject entities = dom.value(QStringLiteral("Entities")).toObject();
    for (auto it = entities.begin(); it != entities.end(); ++it)
    {
        const QJsonObject entity = it.value().toObject();
        AZ::EntityId live;
        if (instance.has_value())
        {
            live = instance->get().GetEntityId(it.key().toUtf8().constData());
        }
        auto* item = new QListWidgetItem(PrefabNiceName(entity.value(QStringLiteral("Name")).toString(), prefabName), m_contents);
        item->setData(Qt::UserRole, qulonglong(AZ::u64(live)));
        // Its sounds too, as their own rows.
        const QJsonObject components = entity.value(QStringLiteral("Components")).toObject();
        for (auto c = components.begin(); c != components.end(); ++c)
        {
            if (c.key().contains(QLatin1String("MiniAudioPlayback")))
            {
                const QString hint = c.value().toObject().value(QStringLiteral("Controller")).toObject().value(QStringLiteral("Config")).toObject()
                                         .value(QStringLiteral("Sound")).toObject().value(QStringLiteral("assetHint")).toString();
                auto* sound = new QListWidgetItem(QStringLiteral("    ") + PrefabSoundName(hint), m_contents);
                sound->setData(Qt::UserRole, qulonglong(AZ::u64(live)));
                sound->setForeground(Hearth::Dim());
            }
        }
    }
}

void HearthPrefabPane::FillDetails()
{
    while (QLayoutItem* item = m_detailsLayout->takeAt(0))
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
    auto* prefabFocus = AZ::Interface<Prefab::PrefabFocusInterface>::Get();
    if (!prefabFocus)
    {
        return;
    }
    auto instance = prefabFocus->GetFocusedPrefabInstance(PrefabContext());
    const QJsonObject dom = PrefabCurrentDom(prefabFocus->GetFocusedPrefabTemplateId(PrefabContext()));
    const QJsonObject entities = dom.value(QStringLiteral("Entities")).toObject();
    QWidget* parent = m_detailsLayout->parentWidget();
    auto row = [this, parent](const QString& name, QWidget* control)
    {
        auto* line = new QHBoxLayout();
        QLabel* label = Hearth::MakeLabel(name, QStringLiteral("dim"), parent);
        label->setFixedWidth(90);
        line->addWidget(label);
        line->addWidget(control, 1);
        m_detailsLayout->addLayout(line);
    };
    bool any = false;
    for (auto it = entities.begin(); it != entities.end(); ++it)
    {
        const QJsonObject components = it.value().toObject().value(QStringLiteral("Components")).toObject();
        AZ::EntityId live = instance.has_value() ? instance->get().GetEntityId(it.key().toUtf8().constData()) : AZ::EntityId();
        for (auto c = components.begin(); c != components.end(); ++c)
        {
            if (c.key().contains(QLatin1String("AreaLightComponent")))
            {
                any = true;
                const QJsonObject config = c.value().toObject().value(QStringLiteral("Controller")).toObject().value(QStringLiteral("Configuration")).toObject();
                const AZ::EntityComponentIdPair light = PrefabComponent(live, "Light");
                // Light: Point · 7 m (range editable)
                auto* lightRow = new QWidget(parent);
                auto* lightLayout = new QHBoxLayout(lightRow);
                lightLayout->setContentsMargins(0, 0, 0, 0);
                lightLayout->addWidget(Hearth::MakeLabel(PrefabLightType(config.value(QStringLiteral("LightType")).toInt(1)) + QStringLiteral(" ·"), QStringLiteral("body"), lightRow));
                auto* range = new QDoubleSpinBox(lightRow);
                range->setRange(0.1, 200.0);
                range->setDecimals(1);
                range->setSuffix(tr(" m"));
                range->setValue(config.value(QStringLiteral("AttenuationRadius")).toDouble(7.0));
                connect(range, &QDoubleSpinBox::editingFinished, this, [light, range]()
                {
                    const AZStd::string path = PrefabPropertyPath(light, QStringLiteral("Radius"), QStringLiteral("mode"));
                    ComponentRequests::PropertyOutcome done = AZ::Failure(AZStd::string());
                    ComponentApi::BroadcastResult(done, &ComponentRequests::SetComponentProperty, light, AZStd::string_view(path), AZStd::any(float(range->value())));
                });
                lightLayout->addWidget(range);
                lightLayout->addStretch(1);
                row(tr("Light"), lightRow);
                // Color: Warm white (click to change)
                const QJsonArray rgb = config.value(QStringLiteral("Color")).toArray();
                const QColor colour = rgb.size() >= 3 ? QColor::fromRgbF(qBound(0.0, rgb[0].toDouble(), 1.0), qBound(0.0, rgb[1].toDouble(), 1.0), qBound(0.0, rgb[2].toDouble(), 1.0))
                                                      : QColor(Qt::white);
                auto* colourButton = new QPushButton(PrefabColorName(colour), parent);
                colourButton->setStyleSheet(QStringLiteral("QPushButton { text-align: left; border: none; background: transparent; color: #ECE6DE; padding-left: 26px; }"));
                QPixmap swatch(16, 16);
                swatch.fill(colour);
                colourButton->setIcon(QIcon(swatch));
                connect(colourButton, &QPushButton::clicked, this, [this, light, colour]()
                {
                    const QColor picked = QColorDialog::getColor(colour, this, tr("Light color"));
                    if (!picked.isValid())
                    {
                        return;
                    }
                    const AZStd::string path = PrefabPropertyPath(light, QStringLiteral("Color"));
                    ComponentRequests::PropertyOutcome done = AZ::Failure(AZStd::string());
                    ComponentApi::BroadcastResult(done, &ComponentRequests::SetComponentProperty, light, AZStd::string_view(path),
                                                  AZStd::any(AZ::Color(float(picked.redF()), float(picked.greenF()), float(picked.blueF()), 1.0f)));
                    QTimer::singleShot(200, this, &HearthPrefabPane::FillDetails);
                });
                row(tr("Color"), colourButton);
            }
            if (c.key().contains(QLatin1String("FlickerLight")))
            {
                any = true;
                const QJsonObject values = c.value().toObject().value(QStringLiteral("m_template")).toObject();
                const AZ::EntityComponentIdPair flicker = PrefabComponent(live, "Flicker Light");
                auto* flickerRow = new QWidget(parent);
                auto* flickerLayout = new QHBoxLayout(flickerRow);
                flickerLayout->setContentsMargins(0, 0, 0, 0);
                flickerLayout->addWidget(Hearth::MakeLabel(values.value(QStringLiteral("DiesWhenNear")).toBool() ? tr("Dies when near, crackle") : tr("Random, crackle"),
                                                          QStringLiteral("body"), flickerRow));
                auto* crackle = new QSpinBox(flickerRow);
                crackle->setRange(0, 100);
                crackle->setSuffix(QStringLiteral("%"));
                crackle->setValue(qRound(values.value(QStringLiteral("CrackleChance")).toDouble(0.6) * 100));
                connect(crackle, &QSpinBox::editingFinished, this, [flicker, crackle]()
                {
                    const AZStd::string path = PrefabPropertyPath(flicker, QStringLiteral("Crackle"));
                    ComponentRequests::PropertyOutcome done = AZ::Failure(AZStd::string());
                    ComponentApi::BroadcastResult(done, &ComponentRequests::SetComponentProperty, flicker, AZStd::string_view(path), AZStd::any(float(crackle->value() / 100.0)));
                });
                flickerLayout->addWidget(crackle);
                flickerLayout->addStretch(1);
                row(tr("Flicker"), flickerRow);
            }
        }
    }
    if (!any)
    {
        m_detailsLayout->addWidget(Hearth::MakeLabel(tr("Pick something in the prefab to change it in the Inspector."), QStringLiteral("dim"), parent));
    }
}

void HearthPrefabPane::FillChanges()
{
    while (QLayoutItem* item = m_changesLayout->takeAt(0))
    {
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
    auto* prefabFocus = AZ::Interface<Prefab::PrefabFocusInterface>::Get();
    if (!prefabFocus || !InPrefab() || m_saved.IsNull())
    {
        return;
    }
    const QJsonObject now = PrefabCurrentDom(prefabFocus->GetFocusedPrefabTemplateId(PrefabContext()));
    const QJsonObject before = PrefabDomToJson(m_saved);
    QMap<QString, QJsonValue> a;
    QMap<QString, QJsonValue> b;
    PrefabFlatten(before.value(QStringLiteral("Entities")), QString(), a);
    PrefabFlatten(now.value(QStringLiteral("Entities")), QString(), b);
    const QString prefabName = now.value(QStringLiteral("ContainerEntity")).toObject().value(QStringLiteral("Name")).toString();
    const QJsonObject entities = now.value(QStringLiteral("Entities")).toObject();
    QSet<QString> keys;
    for (auto it = a.begin(); it != a.end(); ++it)
    {
        keys.insert(it.key());
    }
    for (auto it = b.begin(); it != b.end(); ++it)
    {
        keys.insert(it.key());
    }
    QStringList sorted = keys.values();
    sorted.sort();
    int shown = 0;
    for (const QString& key : sorted)
    {
        const QJsonValue was = a.value(key);
        const QJsonValue is = b.value(key);
        if (was == is)
        {
            continue;
        }
        // "<alias>/Components/<component>/.../<field>" -> "Hum sound volume" or "Bulb range".
        const QString alias = key.section(QLatin1Char('/'), 0, 0);
        const QString component = key.section(QLatin1Char('/'), 2, 2);
        const QString field = key.section(QLatin1Char('/'), -1);
        const QJsonObject entity = entities.value(alias).toObject();
        QString owner = PrefabNiceName(entity.value(QStringLiteral("Name")).toString(), prefabName);
        if (component.contains(QLatin1String("MiniAudio")))
        {
            owner = PrefabSoundName(entity.value(QStringLiteral("Components")).toObject().value(component).toObject().value(QStringLiteral("Controller")).toObject()
                                        .value(QStringLiteral("Config")).toObject().value(QStringLiteral("Sound")).toObject().value(QStringLiteral("assetHint")).toString())
                        .section(QLatin1Char(' '), 0, 0);
        }
        if (field == QLatin1String("Name") || key.contains(QLatin1String("Child Entity Order")))
        {
            continue;
        }
        auto* line = new QWidget(m_changesLayout->parentWidget());
        auto* layout = new QHBoxLayout(line);
        layout->setContentsMargins(0, 2, 0, 2);
        QString label = QStringLiteral("%1 %2").arg(owner, PrefabKeyText(field)).trimmed();
        if (!label.isEmpty())
        {
            label[0] = label[0].toUpper();
        }
        layout->addWidget(Hearth::MakeLabel(label, QStringLiteral("body"), line), 1);
        const QString arrow = QStringLiteral("%1 → %2").arg(was.isUndefined() ? tr("none") : PrefabValueText(field, was),
                                                             is.isUndefined() ? tr("none") : PrefabValueText(field, is));
        QLabel* values = Hearth::MakeLabel(arrow, QStringLiteral("mono"), line);
        values->setStyleSheet(QStringLiteral("background: transparent; color: #F0D17A; font-family: \"Geist Mono\";"));
        layout->addWidget(values);
        m_changesLayout->addWidget(line);
        if (++shown >= 12)
        {
            break;
        }
    }
    if (shown == 0)
    {
        m_changesLayout->addWidget(Hearth::MakeLabel(tr("Nothing yet."), QStringLiteral("dim"), m_changesLayout->parentWidget()));
    }
}

void HearthPrefabPane::FlickerOnce()
{
    // The stutter a failing tube does: the light and its glowing panel off and on a few times.
    auto* prefabFocus = AZ::Interface<Prefab::PrefabFocusInterface>::Get();
    if (!prefabFocus)
    {
        return;
    }
    auto instance = prefabFocus->GetFocusedPrefabInstance(PrefabContext());
    if (!instance.has_value())
    {
        return;
    }
    AZStd::vector<AZ::EntityId> targets;
    const QJsonObject dom = PrefabCurrentDom(prefabFocus->GetFocusedPrefabTemplateId(PrefabContext()));
    const QJsonObject entities = dom.value(QStringLiteral("Entities")).toObject();
    for (auto it = entities.begin(); it != entities.end(); ++it)
    {
        const QJsonObject components = it.value().toObject().value(QStringLiteral("Components")).toObject();
        for (auto c = components.begin(); c != components.end(); ++c)
        {
            if (c.key().contains(QLatin1String("AreaLightComponent")) || c.key().contains(QLatin1String("EditorMeshComponent")))
            {
                targets.push_back(instance->get().GetEntityId(it.key().toUtf8().constData()));
                break;
            }
        }
    }
    const int pattern[] = { 70, 50, 90, 40, 160, 60, 300 };
    auto step = std::make_shared<int>(0);
    auto timer = new QTimer(this);
    timer->setSingleShot(true);
    auto tick = std::make_shared<std::function<void()>>();
    *tick = [timer, step, targets, pattern, tick]()
    {
        // Switching the entities off and on for a moment (nothing in the prefab changes).
        auto setOn = [&targets](bool on)
        {
            for (const AZ::EntityId& id : targets)
            {
                AZ::Entity* entity = nullptr;
                AZ::ComponentApplicationBus::BroadcastResult(entity, &AZ::ComponentApplicationRequests::FindEntity, id);
                if (!entity)
                {
                    continue;
                }
                if (on && entity->GetState() == AZ::Entity::State::Init)
                {
                    entity->Activate();
                }
                else if (!on && entity->GetState() == AZ::Entity::State::Active)
                {
                    entity->Deactivate();
                }
            }
        };
        const bool last = *step >= int(sizeof(pattern) / sizeof(pattern[0]));
        setOn(last || (*step % 2) == 1);
        if (last)
        {
            timer->deleteLater();
            return;
        }
        timer->start(pattern[(*step)++]);
    };
    connect(timer, &QTimer::timeout, timer, [tick]() { (*tick)(); });
    (*tick)();
}
