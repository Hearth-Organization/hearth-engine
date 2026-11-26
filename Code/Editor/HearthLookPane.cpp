/*
 * Hearth Engine "Lighting and look" window. Built on O3DE (Apache-2.0 OR MIT). See HearthLookPane.h.
 */

#include "EditorDefs.h"

#include "HearthLookPane.h"
#include "HearthAgentClient.h"

#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/Entity.h>
#include <AzCore/Console/IConsole.h>
#include <AzCore/Interface/Interface.h>
#include <AzCore/std/any.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>
#include <AzToolsFramework/Component/EditorComponentAPIBus.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>

#include <Atom/Feature/Utils/FrameCaptureBus.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>
#include <QTimer>
#include <QScrollArea>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QVBoxLayout>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;
    using ComponentApi = AzToolsFramework::EditorComponentAPIBus;
    using ComponentRequests = AzToolsFramework::EditorComponentAPIRequests;

    const char* LookEntityName = "Hearth Look";

    struct LookSlider
    {
        const char* key;
        const char* label;
        const char* hint;
        double min;
        double max;
        double step;
        const char* component;  //!< component name (as the Add Component list shows it)
        const char* enable;     //!< property path that switches the effect on
        const char* property;   //!< property path for the value
    };

    const LookSlider LookSliders[] = {
        { "exposure", "Exposure", "Overall brightness", -3.0, 3.0, 0.1, "Exposure Control", "Controller|Configuration|Enable", "Controller|Configuration|Manual Compensation" },
        { "temperature", "Colour temperature", "Level 0 yellow", 1500.0, 10000.0, 100.0, "White Balance", "Controller|Configuration|Enable White Balance", "Controller|Configuration|White Balance|Temperature" },
        { "fog", "Fog", "How far you can see", 5.0, 200.0, 1.0, "Deferred Fog", "Controller|Configuration|Enable Deferred Fog", "Controller|Configuration|Distance|Fog End Distance" },
        { "grain", "Film grain", "", 0.0, 1.0, 0.05, "Film Grain", "Controller|Configuration|Enable Film Grain", "Controller|Configuration|Intensity" },
        { "vignette", "Vignette", "", 0.0, 1.0, 0.05, "Vignette", "Controller|Configuration|Enable Vignette", "Controller|Configuration|Intensity" },
        { "bloom", "Bloom on lights", "", 0.0, 1.0, 0.05, "Bloom", "Controller|Configuration|Enable Bloom", "Controller|Configuration|Intensity" },
    };

    QString LookLabel(const QString& key, double v)
    {
        if (key == QLatin1String("exposure"))
        {
            return (v < 0 ? QStringLiteral("−") : QString()) + QString::number(qAbs(v), 'f', 1);
        }
        if (key == QLatin1String("temperature"))
        {
            return QStringLiteral("%1K").arg(qRound(v));
        }
        if (key == QLatin1String("fog"))
        {
            return QStringLiteral("%1 m").arg(qRound(v));
        }
        return QString::number(v, 'f', 2);
    }

    struct Look
    {
        QString name;
        QString about;
        QString top;
        QString bottom;
        QMap<QString, double> values;
    };

    QVector<Look> BuiltInLooks()
    {
        return {
            { QObject::tr("Level 0"), QObject::tr("Humming yellow"), QStringLiteral("#5C5230"), QStringLiteral("#2C2816"),
              { { "exposure", -0.4 }, { "temperature", 3600 }, { "fog", 38 }, { "grain", 0.35 }, { "vignette", 0.40 }, { "bloom", 0.55 }, { "ao", 1 } } },
            { QObject::tr("Blackout"), QObject::tr("Lights dying"), QStringLiteral("#1D1B14"), QStringLiteral("#0B0A08"),
              { { "exposure", -2.2 }, { "temperature", 3200 }, { "fog", 18 }, { "grain", 0.5 }, { "vignette", 0.7 }, { "bloom", 0.2 }, { "ao", 1 } } },
            { QObject::tr("Foundation site"), QObject::tr("Cold white"), QStringLiteral("#9AA6AD"), QStringLiteral("#3A4046"),
              { { "exposure", 0.3 }, { "temperature", 6800 }, { "fog", 90 }, { "grain", 0.1 }, { "vignette", 0.2 }, { "bloom", 0.3 }, { "ao", 1 } } },
            { QObject::tr("Containment breach"), QObject::tr("Red alarm"), QStringLiteral("#6B1D14"), QStringLiteral("#200806"),
              { { "exposure", -0.8 }, { "temperature", 1700 }, { "fog", 30 }, { "grain", 0.4 }, { "vignette", 0.55 }, { "bloom", 0.8 }, { "ao", 1 } } },
        };
    }

    const char* LookSliderStyle =
        "QSlider::groove:horizontal { height: 4px; background: #2C2824; border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: #E8743B; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: #ECE6DE; width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; }";
} // namespace

//! Before on the left of the line, after on the right; drag the line.
class HearthLookPreview : public QWidget
{
public:
    explicit HearthLookPreview(QWidget* parent)
        : QWidget(parent)
    {
        setMinimumSize(320, 200);
        setCursor(Qt::SplitHCursor);
    }
    void SetBefore(const QImage& image) { m_before = image; update(); }
    void SetAfter(const QImage& image) { m_after = image; update(); }

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
        const QImage& any = m_after.isNull() ? m_before : m_after;
        if (any.isNull())
        {
            p.setPen(QColor(0x8D, 0x85, 0x7B));
            p.drawText(rect(), Qt::AlignCenter, QObject::tr("Open a level to see it here"));
            return;
        }
        // Fill the box, keeping the picture's shape (crop the edges).
        const QSizeF size = QSizeF(any.size()).scaled(QSizeF(size_()), Qt::KeepAspectRatioByExpanding);
        const QRectF target(QPointF((width() - size.width()) / 2.0, (height() - size.height()) / 2.0), size);
        const int split = qRound(width() * m_split);
        if (!m_before.isNull())
        {
            p.save();
            p.setClipRect(QRect(0, 0, split, height()), Qt::IntersectClip);
            p.drawImage(target, m_before);
            p.restore();
        }
        if (!m_after.isNull())
        {
            p.save();
            p.setClipRect(QRect(split, 0, width() - split, height()), Qt::IntersectClip);
            p.drawImage(target, m_after);
            p.restore();
        }
        p.setPen(QPen(QColor(0xE8, 0x74, 0x3B), 2));
        p.drawLine(split, 0, split, height());
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xE8, 0x74, 0x3B));
        p.drawEllipse(QPointF(split, height() / 2.0), 9, 9);
        auto tag = [&p](const QRect& box, const QString& text)
        {
            p.setBrush(QColor(13, 12, 11, 210));
            p.drawRoundedRect(box, 10, 10);
            p.setPen(QColor(0xEC, 0xE6, 0xDE));
            p.drawText(box, Qt::AlignCenter, text);
            p.setPen(Qt::NoPen);
        };
        tag(QRect(10, height() - 32, 64, 22), QObject::tr("Before"));
        tag(QRect(width() - 74, height() - 32, 64, 22), QObject::tr("After"));
    }
    void mousePressEvent(QMouseEvent* event) override { Drag(event); }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (event->buttons() & Qt::LeftButton)
        {
            Drag(event);
        }
    }

private:
    QSize size_() const { return size(); }
    void Drag(QMouseEvent* event)
    {
        m_split = qBound(0.05, event->pos().x() / static_cast<double>(qMax(1, width())), 0.95);
        update();
    }
    QImage m_before;
    QImage m_after;
    double m_split = 0.5;
};

void HearthLookPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::RightDockWidgetArea;
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthLookPane>("Lighting and look", "Tools", options);
}

HearthLookPane::HearthLookPane(QWidget* parent)
    : QWidget(parent)
    , m_agent(std::make_unique<HearthAgentClient>(this))
{
    setObjectName(QStringLiteral("HearthLook"));
    setStyleSheet(QStringLiteral("QWidget#HearthLook { background-color: #0D0C0B; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    const QString level = GetIEditor() ? GetIEditor()->GetLevelName() : QString();
    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Lighting and look"), tr("%1 · applies to the whole level").arg(level));
    m_beforeAfter = Hearth::MakeButton(tr("Before / after"), false, this);
    m_beforeAfter->setCheckable(true);
    QPushButton* save = Hearth::MakeButton(tr("Save as preset"), false, this);
    QPushButton* apply = Hearth::MakeButton(tr("Apply"), true, this);
    header->addWidget(m_beforeAfter);
    header->addWidget(save);
    header->addWidget(apply);
    root->addLayout(header);
    connect(m_beforeAfter, &QPushButton::toggled, this, [this](bool before)
    {
        // Before: the level without the look (also in the main viewport); After: with it.
        m_beforeAfter->setText(before ? tr("Showing before") : tr("Before / after"));
        Apply(before);
    });

    // Left: this window's view of the level, before and after. Right: looks, settings and lights.
    auto* body = new QHBoxLayout();
    body->setSpacing(10);
    root->addLayout(body, 1);
    QVBoxLayout* viewLayout = nullptr;
    QFrame* view = Hearth::MakeCard(this, &viewLayout);
    auto* viewTop = new QHBoxLayout();
    viewTop->addWidget(Hearth::MakeLabel(tr("Viewport"), QStringLiteral("title"), view), 1);
    m_live = Hearth::MakeButton(tr("● Live"), false, view);
    m_live->setCheckable(true);
    m_live->setChecked(true);
    m_live->setToolTip(tr("Keep the after side up to date"));
    viewTop->addWidget(m_live);
    QPushButton* refresh = Hearth::MakeButton(tr("Retake before"), false, view);
    viewTop->addWidget(refresh);
    viewLayout->addLayout(viewTop);
    m_preview = new HearthLookPreview(view);
    viewLayout->addWidget(m_preview, 1);
    body->addWidget(view, 3);
    // The right side scrolls (its looks, settings and lights are taller than a laptop screen), and the looks scroll
    // sideways: together they needed ~1100 x 1200 px, so docking this window pushed the editor past the screen.
    auto* columnScroll = new QScrollArea(this);
    columnScroll->setWidgetResizable(true);
    columnScroll->setFrameShape(QFrame::NoFrame);
    columnScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    columnScroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
    auto* columnHolder = new QWidget(columnScroll);
    columnHolder->setAttribute(Qt::WA_TranslucentBackground);
    auto* column = new QVBoxLayout(columnHolder);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(10);
    columnScroll->setWidget(columnHolder);
    columnScroll->setMinimumWidth(320);
    body->addWidget(columnScroll, 2);
    root = column;
    connect(refresh, &QPushButton::clicked, this, &HearthLookPane::CaptureBefore);
    auto* liveTimer = new QTimer(this);
    liveTimer->setInterval(2000);
    connect(liveTimer, &QTimer::timeout, this, [this]()
    {
        if (m_live->isChecked() && isVisible() && !m_beforeAfter->isChecked())
        {
            CaptureAfter();
        }
    });
    liveTimer->start();
    QTimer::singleShot(1200, this, &HearthLookPane::CaptureBefore);
    connect(save, &QPushButton::clicked, this, &HearthLookPane::SavePreset);
    connect(apply, &QPushButton::clicked, this, [this]() { Apply(false); });

    QVBoxLayout* looksLayout = nullptr;
    QFrame* looks = Hearth::MakeCard(this, &looksLayout);
    looksLayout->addWidget(Hearth::MakeLabel(tr("Looks"), QStringLiteral("title"), looks));
    auto* presetsScroll = new QScrollArea(looks);
    presetsScroll->setWidgetResizable(true);
    presetsScroll->setFrameShape(QFrame::NoFrame);
    presetsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    presetsScroll->setFixedHeight(126);
    presetsScroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
    auto* presetsHolder = new QWidget(presetsScroll);
    presetsHolder->setAttribute(Qt::WA_TranslucentBackground);
    m_presets = new QHBoxLayout(presetsHolder);
    m_presets->setContentsMargins(0, 0, 0, 0);
    m_presets->setSpacing(8);
    presetsScroll->setWidget(presetsHolder);
    looksLayout->addWidget(presetsScroll);
    root->addWidget(looks);

    QVBoxLayout* settingsLayout = nullptr;
    QFrame* settings = Hearth::MakeCard(this, &settingsLayout);
    auto* settingsTop = new QHBoxLayout();
    settingsTop->addWidget(Hearth::MakeLabel(tr("Look settings"), QStringLiteral("title"), settings), 1);
    {
        QScopedPointer<QSettings> hearth(Hearth::NewSettings());
        const QString preset = hearth->value(QStringLiteral("laptop/preset"), QStringLiteral("light")).toString();
        settingsTop->addWidget(Hearth::MakeLabel(tr("%1 preset").arg(preset == QLatin1String("light") ? tr("Laptop") : preset), QStringLiteral("dim"), settings));
    }
    settingsLayout->addLayout(settingsTop);
    for (const LookSlider& spec : LookSliders)
    {
        const QString key = QString::fromLatin1(spec.key);
        auto* holder = new QWidget(settings);
        auto* layout = new QHBoxLayout(holder);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* slider = new QSlider(Qt::Horizontal, holder);
        slider->setRange(0, qRound((spec.max - spec.min) / spec.step));
        slider->setMinimumWidth(100);
        slider->setMaximumWidth(160);
        slider->setStyleSheet(QLatin1String(LookSliderStyle));
        auto* value = Hearth::MakeLabel(QString(), QStringLiteral("mono"), holder);
        value->setFixedWidth(52);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(slider);
        layout->addWidget(value);
        const double min = spec.min;
        const double step = spec.step;
        connect(slider, &QSlider::valueChanged, value, [value, key, min, step](int position) { value->setText(LookLabel(key, min + position * step)); });
        settingsLayout->addWidget(Hearth::MakeSettingRow(tr(spec.label), tr(spec.hint), holder, settings));
        m_sliders.insert(key, slider);
        m_values.insert(key, value);
    }
    m_ao = new Hearth::Toggle(settings);
    settingsLayout->addWidget(Hearth::MakeSettingRow(tr("Ambient occlusion"), tr("Dark corners"), m_ao, settings));
    m_shadows = new Hearth::Toggle(settings);
    {
        QScopedPointer<QSettings> hearth(Hearth::NewSettings());
        m_shadows->setChecked(hearth->value(QStringLiteral("laptop/shadows"), true).toBool());
    }
    settingsLayout->addWidget(Hearth::MakeSettingRow(tr("Shadows"), tr("Off on the laptop preset"), m_shadows, settings));
    connect(m_shadows, &QAbstractButton::toggled, this, [](bool on)
    {
        QScopedPointer<QSettings> hearth(Hearth::NewSettings());
        hearth->setValue(QStringLiteral("laptop/shadows"), on);
        if (auto* console = AZ::Interface<AZ::IConsole>::Get())
        {
            console->PerformCommand(on ? "r_directionalShadowFilteringMethod -1" : "r_directionalShadowFilteringMethod 0");
        }
    });

    settingsLayout->addWidget(Hearth::MakeLabel(tr("Lights in this level"), QStringLiteral("title"), settings));
    auto* chips = new QHBoxLayout();
    m_working = Hearth::MakeChip(tr("… working"), QStringLiteral("good"), settings);
    m_flickering = Hearth::MakeChip(tr("… flickering"), QStringLiteral("warn"), settings);
    m_dead = Hearth::MakeChip(tr("… dead"), QStringLiteral("bad"), settings);
    chips->addWidget(m_working);
    chips->addWidget(m_flickering);
    chips->addWidget(m_dead);
    chips->addStretch(1);
    settingsLayout->addLayout(chips);
    settingsLayout->addWidget(Hearth::MakeLabel(tr("Dead and flickering lights follow the Level Generator settings."), QStringLiteral("small"), settings));
    root->addWidget(settings, 1);

    m_status = Hearth::MakeLabel(QString(), QStringLiteral("small"), this);
    root->addWidget(m_status);

    RebuildPresets();
    ApplyPreset(BuiltInLooks().first().values);
    UpdateLights();
}

HearthLookPane::~HearthLookPane() = default;

void HearthLookPane::Capture(const QString& path, std::function<void()> then)
{
    if (m_capturing || !GetIEditor() || GetIEditor()->GetLevelName().isEmpty())
    {
        return;
    }
    bool canCapture = false;
    AZ::Render::FrameCaptureRequestBus::BroadcastResult(canCapture, &AZ::Render::FrameCaptureRequestBus::Events::CanCapture);
    if (!canCapture)
    {
        return;
    }
    QFile::remove(path);
    AZ::Render::FrameCaptureOutcome outcome;
    AZ::Render::FrameCaptureRequestBus::BroadcastResult(
        outcome, &AZ::Render::FrameCaptureRequestBus::Events::CaptureScreenshot, AZStd::string(path.toUtf8().constData()));
    if (!outcome.IsSuccess())
    {
        return;
    }
    m_capturing = true;
    // The capture is written a frame or two later: wait for the file (at most 3 s).
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto* poll = new QTimer(this);
    poll->setInterval(60);
    connect(poll, &QTimer::timeout, this, [this, poll, path, clock, then]()
    {
        const QFileInfo file(path);
        if ((file.exists() && file.size() > 0) || clock->elapsed() > 3000)
        {
            poll->stop();
            poll->deleteLater();
            m_capturing = false;
            if (then)
            {
                then();
            }
        }
    });
    poll->start();
}

void HearthLookPane::CaptureBefore()
{
    // Switch the look entity off for a moment (deactivate only: nothing in the level changes or needs saving),
    // take the picture, switch it back on.
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/hearth-look");
    QDir().mkpath(folder);
    AZ::Entity* look = nullptr;
    AZ::ComponentApplicationBus::Broadcast(&AZ::ComponentApplicationRequests::EnumerateEntities, [&look](AZ::Entity* entity)
    {
        if (!look && entity && entity->GetName() == LookEntityName && entity->GetState() == AZ::Entity::State::Active)
        {
            bool editorEntity = false;
            AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(
                editorEntity, &AzToolsFramework::EditorEntityContextRequests::IsEditorEntity, entity->GetId());
            look = editorEntity ? entity : nullptr;
        }
    });
    const AZ::EntityId lookId = look ? look->GetId() : AZ::EntityId();
    if (look)
    {
        look->Deactivate();
    }
    QTimer::singleShot(150, this, [this, folder, lookId]()
    {
        auto restore = [lookId]()
        {
            AZ::Entity* entity = nullptr;
            AZ::ComponentApplicationBus::BroadcastResult(entity, &AZ::ComponentApplicationRequests::FindEntity, lookId);
            if (entity && entity->GetState() == AZ::Entity::State::Init)
            {
                entity->Activate();
            }
        };
        if (m_capturing)
        {
            restore();
            return;
        }
        Capture(folder + QStringLiteral("/before.png"), [this, folder, restore]()
        {
            restore();
            m_preview->SetBefore(QImage(folder + QStringLiteral("/before.png")));
            QTimer::singleShot(150, this, &HearthLookPane::CaptureAfter);
        });
        if (!m_capturing)
        {
            restore(); // the capture didn't start (no level, renderer busy)
        }
    });
}

void HearthLookPane::CaptureAfter()
{
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/hearth-look");
    QDir().mkpath(folder);
    Capture(folder + QStringLiteral("/after.png"), [this, folder]()
    {
        m_preview->SetAfter(QImage(folder + QStringLiteral("/after.png")));
    });
}

void HearthLookPane::RebuildPresets()
{
    while (QLayoutItem* item = m_presets->takeAt(0))
    {
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
    QVector<Look> looks = BuiltInLooks();
    QScopedPointer<QSettings> hearth(Hearth::NewSettings());
    const int saved = hearth->beginReadArray(QStringLiteral("looks"));
    for (int i = 0; i < saved; ++i)
    {
        hearth->setArrayIndex(i);
        Look look{ hearth->value(QStringLiteral("name")).toString(), tr("Saved look"), QStringLiteral("#3A332A"), QStringLiteral("#191612"), {} };
        for (const QString& key : { QStringLiteral("exposure"), QStringLiteral("temperature"), QStringLiteral("fog"), QStringLiteral("grain"),
                                    QStringLiteral("vignette"), QStringLiteral("bloom"), QStringLiteral("ao") })
        {
            look.values.insert(key, hearth->value(key).toDouble());
        }
        looks.push_back(look);
    }
    hearth->endArray();
    for (const Look& look : looks)
    {
        auto* card = new QPushButton(this);
        card->setCursor(Qt::PointingHandCursor);
        card->setFixedSize(130, 110);
        card->setStyleSheet(QStringLiteral("QPushButton { border: 1px solid rgba(255, 255, 255, 15); border-radius: 12px; text-align: left;"
                                           " background: qlineargradient(x1:0, y1:0, x2:0, y2:0.62, stop:0 %1, stop:1 %2); }"
                                           "QPushButton:hover { border-color: rgba(232, 116, 59, 150); }")
                                .arg(look.top, look.bottom));
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->addStretch(1);
        auto* caption = new QWidget(card);
        caption->setAttribute(Qt::WA_TransparentForMouseEvents);
        caption->setStyleSheet(QStringLiteral("background-color: #171513; border-radius: 8px;"));
        auto* captionLayout = new QVBoxLayout(caption);
        captionLayout->setContentsMargins(8, 6, 8, 6);
        captionLayout->setSpacing(0);
        captionLayout->addWidget(Hearth::MakeLabel(look.name, QStringLiteral("body"), caption));
        captionLayout->addWidget(Hearth::MakeLabel(look.about, QStringLiteral("small"), caption));
        layout->addWidget(caption);
        const QMap<QString, double> values = look.values;
        connect(card, &QPushButton::clicked, this, [this, values]()
        {
            ApplyPreset(values);
            Apply(false);
        });
        m_presets->addWidget(card);
    }
}

void HearthLookPane::ApplyPreset(const QMap<QString, double>& values)
{
    for (const LookSlider& spec : LookSliders)
    {
        const QString key = QString::fromLatin1(spec.key);
        if (values.contains(key))
        {
            m_sliders.value(key)->setValue(qRound((values.value(key) - spec.min) / spec.step));
            m_values.value(key)->setText(LookLabel(key, values.value(key)));
        }
    }
    m_ao->setChecked(values.value(QStringLiteral("ao"), 1.0) > 0.5);
}

AZ::EntityId HearthLookPane::FindOrCreateLookEntity()
{
    AZ::EntityId found;
    AZ::ComponentApplicationBus::Broadcast(&AZ::ComponentApplicationRequests::EnumerateEntities,
        [&found](AZ::Entity* entity)
        {
            if (!found.IsValid() && entity && entity->GetName() == LookEntityName)
            {
                bool editorEntity = false;
                AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(
                    editorEntity, &AzToolsFramework::EditorEntityContextRequests::IsEditorEntity, entity->GetId());
                if (editorEntity)
                {
                    found = entity->GetId();
                }
            }
        });
    if (!found.IsValid())
    {
        AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(
            found, &AzToolsFramework::EditorEntityContextRequests::CreateNewEditorEntity, LookEntityName);
    }
    return found;
}

void HearthLookPane::Apply(bool showBefore)
{
    if (!GetIEditor() || GetIEditor()->GetLevelName().isEmpty())
    {
        m_status->setText(tr("Open a level first."));
        return;
    }
    AzToolsFramework::ScopedUndoBatch undo("Hearth look");
    const AZ::EntityId entity = FindOrCreateLookEntity();
    if (!entity.IsValid())
    {
        m_status->setText(tr("Couldn't make the Hearth Look entity."));
        return;
    }
    QStringList problems;
    auto ensure = [&](const char* componentName) -> AZ::EntityComponentIdPair
    {
        AZStd::vector<AZ::Uuid> types;
        ComponentApi::BroadcastResult(types, &ComponentRequests::FindComponentTypeIdsByEntityType,
                                      AZStd::vector<AZStd::string>{ componentName }, ComponentRequests::EntityType::Game);
        if (types.empty() || types.front().IsNull())
        {
            problems << tr("%1 isn't available (its engine part is off)").arg(QString::fromLatin1(componentName));
            return {};
        }
        ComponentRequests::GetComponentOutcome existing = AZ::Failure(AZStd::string());
        ComponentApi::BroadcastResult(existing, &ComponentRequests::GetComponentOfType, entity, types.front());
        if (existing.IsSuccess())
        {
            return existing.GetValue();
        }
        ComponentRequests::AddComponentsOutcome added = AZ::Failure(AZStd::string());
        ComponentApi::BroadcastResult(added, &ComponentRequests::AddComponentOfType, entity, types.front());
        if (added.IsSuccess() && !added.GetValue().empty())
        {
            return added.GetValue().front();
        }
        problems << tr("couldn't add %1").arg(QString::fromLatin1(componentName));
        return {};
    };
    auto set = [&](const AZ::EntityComponentIdPair& component, const char* path, const AZStd::any& value)
    {
        if (!component.GetEntityId().IsValid())
        {
            return;
        }
        ComponentRequests::PropertyOutcome outcome = AZ::Failure(AZStd::string());
        ComponentApi::BroadcastResult(outcome, &ComponentRequests::SetComponentProperty, component, AZStd::string_view(path), value);
        if (!outcome.IsSuccess())
        {
            problems << QStringLiteral("%1: %2").arg(QString::fromLatin1(path), QString::fromUtf8(outcome.GetError().c_str()));
        }
    };

    ensure("PostFX Layer"); // post effects need a layer on the same entity
    for (const LookSlider& spec : LookSliders)
    {
        const QString key = QString::fromLatin1(spec.key);
        const AZ::EntityComponentIdPair component = ensure(spec.component);
        const float value = static_cast<float>(spec.min + m_sliders.value(key)->value() * spec.step);
        set(component, spec.enable, AZStd::any(!showBefore));
        set(component, spec.property, AZStd::any(value));
    }
    const AZ::EntityComponentIdPair ao = ensure("Ambient Occlusion");
    set(ao, "Controller|Configuration|Enable AO", AZStd::any(!showBefore && m_ao->isChecked()));

    m_status->setText(problems.isEmpty() ? (showBefore ? tr("Showing the level without the look.") : tr("Look applied to %1.").arg(GetIEditor()->GetLevelName()))
                                         : tr("Applied, with problems: %1").arg(problems.join(QStringLiteral("; "))));
}

void HearthLookPane::SavePreset()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save as preset"), tr("Name for this look"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty())
    {
        return;
    }
    QScopedPointer<QSettings> hearth(Hearth::NewSettings());
    const int count = hearth->beginReadArray(QStringLiteral("looks"));
    QVector<QMap<QString, QVariant>> existing;
    for (int i = 0; i < count; ++i)
    {
        hearth->setArrayIndex(i);
        QMap<QString, QVariant> look;
        for (const QString& key : hearth->childKeys())
        {
            look.insert(key, hearth->value(key));
        }
        existing.push_back(look);
    }
    hearth->endArray();
    QMap<QString, QVariant> look;
    look.insert(QStringLiteral("name"), name);
    for (const LookSlider& spec : LookSliders)
    {
        const QString key = QString::fromLatin1(spec.key);
        look.insert(key, spec.min + m_sliders.value(key)->value() * spec.step);
    }
    look.insert(QStringLiteral("ao"), m_ao->isChecked() ? 1.0 : 0.0);
    existing.push_back(look);
    hearth->beginWriteArray(QStringLiteral("looks"), existing.size());
    for (int i = 0; i < existing.size(); ++i)
    {
        hearth->setArrayIndex(i);
        for (auto it = existing[i].begin(); it != existing[i].end(); ++it)
        {
            hearth->setValue(it.key(), it.value());
        }
    }
    hearth->endArray();
    RebuildPresets();
    m_status->setText(tr("Saved \"%1\".").arg(name));
}

void HearthLookPane::UpdateLights()
{
    // The generated area around the spawn: how many lights work, flicker, are dead (or dying).
    m_agent->Post(QStringLiteral("levelgen_get"), {}, [this](const QJsonObject& got, bool reached)
    {
        if (!reached)
        {
            m_working->setText(tr("agent offline"));
            return;
        }
        const QJsonObject values = got.value(QStringLiteral("values")).toObject();
        m_agent->Post(QStringLiteral("levelgen_area"), QJsonObject{ { QStringLiteral("values"), values } }, [this](const QJsonObject& answer, bool)
        {
            int counts[4] = { 0, 0, 0, 0 };
            for (const QJsonValue light : answer.value(QStringLiteral("data")).toObject().value(QStringLiteral("lights")).toArray())
            {
                counts[qBound(0, light.toArray().at(2).toInt(), 3)]++;
            }
            m_working->setText(tr("%1 working").arg(counts[0]));
            m_flickering->setText(tr("%1 flickering").arg(counts[1] + counts[3]));
            m_dead->setText(tr("%1 dead").arg(counts[2]));
        });
    });
}
