/*
 * Hearth Engine Level Generator window. Built on O3DE (Apache-2.0 OR MIT). See HearthLevelGenerator.h.
 */

#include "EditorDefs.h"

#include "HearthLevelGenerator.h"
#include "HearthAgentClient.h"

#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>

#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QPushButton>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    struct SliderSpec
    {
        const char* key;
        const char* label;
        double min;
        double max;
        double step;
        const char* group;
    };

    const SliderSpec Sliders[] = {
        { "openness", "Openness", 0.0, 1.0, 0.05, "Layout" },
        { "roomChance", "Open room chance", 0.0, 1.0, 0.05, "Layout" },
        { "pillarChance", "Pillar chance", 0.0, 1.0, 0.05, "Layout" },
        { "baseSize", "Forward base size", 0.0, 5.0, 1.0, "Layout" },
        { "darkness", "Darkness", 0.0, 1.0, 0.05, "Horror" },
        { "darkZoneSize", "Dark zone size", 1.0, 8.0, 0.5, "Horror" },
        { "stains", "Stains", 0.0, 1.0, 0.05, "Horror" },
    };

    QString LabelFor(const QString& key, double value)
    {
        if (key == QLatin1String("darkness"))
        {
            return QStringLiteral("%1%").arg(qRound(value * 100));
        }
        if (key == QLatin1String("darkZoneSize"))
        {
            return QStringLiteral("%1 cells").arg(value, 0, 'f', 1);
        }
        if (key == QLatin1String("baseSize"))
        {
            const int metres = (2 * static_cast<int>(value) + 1) * 4;
            return QStringLiteral("%1 × %1 m").arg(metres);
        }
        return QString::number(value, 'f', 2);
    }

    const char* SliderStyle =
        "QSlider::groove:horizontal { height: 4px; background: #2C2824; border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: #E8743B; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: #FFCFB4; border: 3px solid #E8743B; width: 10px; height: 10px; margin: -6px 0; border-radius: 8px; }";
} // namespace

//! The top-down map of the generated area (same drawing as the Hub's Level Generator page).
class HearthLevelMap : public QWidget
{
public:
    explicit HearthLevelMap(QWidget* parent)
        : QWidget(parent)
    {
        setMinimumSize(420, 420);
    }

    void SetArea(const QJsonObject& area, const QJsonObject& values)
    {
        m_area = area;
        m_values = values;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath clip;
        clip.addRoundedRect(frame, 14, 14);
        g.setClipPath(clip);
        g.fillRect(rect(), QColor(0x14, 0x12, 0x0F));
        if (m_area.isEmpty())
        {
            g.setPen(Hearth::Dim());
            g.drawText(rect(), Qt::AlignCenter, QObject::tr("Generating…"));
            return;
        }
        const double size = m_area.value(QStringLiteral("chunkSize")).toDouble(24.0);
        const QJsonArray minA = m_area.value(QStringLiteral("min")).toArray();
        const QJsonArray maxA = m_area.value(QStringLiteral("max")).toArray();
        const double minX = minA.at(0).toDouble();
        const double minY = minA.at(1).toDouble();
        const double span = (maxA.at(0).toDouble() - minX + 1) * size;
        const double side = qMin(width(), height());
        const double ox = (width() - side) / 2.0;
        const double oy = (height() - side) / 2.0;
        const double k = side / span;
        auto X = [&](double x) { return ox + (x - minX * size) * k; };
        auto Y = [&](double y) { return oy + side - (y - minY * size) * k; };

        g.fillRect(QRectF(ox, oy, side, side), QColor(0x1D, 0x1A, 0x15));
        g.setPen(QPen(QColor(255, 255, 255, 13), 1));
        for (int i = 1; i < static_cast<int>(span / size); ++i)
        {
            const double p = i * size * k;
            g.drawLine(QPointF(ox + p, oy), QPointF(ox + p, oy + side));
            g.drawLine(QPointF(ox, oy + p), QPointF(ox + side, oy + p));
        }
        g.setPen(Qt::NoPen);
        if (m_values.value(QStringLiteral("stains")).toDouble() > 0)
        {
            g.setBrush(QColor(120, 96, 50, 90));
            for (const QJsonValue& value : m_area.value(QStringLiteral("decals")).toArray())
            {
                const QJsonArray d = value.toArray();
                const double r = qMax(2.0, d.at(2).toDouble() * k * 0.45);
                g.drawEllipse(QPointF(X(d.at(0).toDouble()), Y(d.at(1).toDouble())), r, r);
            }
        }
        const double cell = size / 6.0;
        static const QColor colors[] = { QColor(0xFF, 0xF8, 0xCD), QColor(0xFF, 0xA0, 0x28), QColor(0x5F, 0x5A, 0x50), QColor(0xFF, 0x5F, 0x4B) };
        const bool flicker = m_values.value(QStringLiteral("flicker")).toBool(true);
        const bool dying = m_values.value(QStringLiteral("dying")).toBool(true);
        for (const QJsonValue& value : m_area.value(QStringLiteral("lights")).toArray())
        {
            const QJsonArray l = value.toArray();
            int s = l.at(2).toInt();
            if ((s == 1 && !flicker) || (s == 3 && !dying))
            {
                s = 0;
            }
            const QColor color = colors[qBound(0, s, 3)];
            const QPointF at(X(l.at(0).toDouble()), Y(l.at(1).toDouble()));
            if (s != 2)
            {
                QRadialGradient glow(at, cell * k * 0.7);
                QColor inner = color;
                inner.setAlpha(64);
                QColor outer = color;
                outer.setAlpha(0);
                glow.setColorAt(0, inner);
                glow.setColorAt(1, outer);
                g.setBrush(glow);
                g.drawRect(QRectF(at.x() - cell * k, at.y() - cell * k, cell * k * 2, cell * k * 2));
            }
            const double d = qMax(4.0, cell * k * 0.22);
            g.setBrush(color);
            g.drawRect(QRectF(at.x() - d / 2, at.y() - d / 2, d, d));
        }
        auto drawBoxes = [&](const QString& key, const QColor& color, bool minimum)
        {
            g.setBrush(color);
            for (const QJsonValue& value : m_area.value(key).toArray())
            {
                const QJsonArray w = value.toArray();
                const double x = w.at(0).toDouble();
                const double y = w.at(1).toDouble();
                const double ww = w.at(2).toDouble();
                const double hh = w.at(3).toDouble();
                g.drawRect(QRectF(X(x - ww / 2), Y(y + hh / 2), minimum ? qMax(2.0, ww * k) : ww * k, minimum ? qMax(2.0, hh * k) : hh * k));
            }
        };
        drawBoxes(QStringLiteral("walls"), QColor(0xE6, 0xD4, 0x8C), true);
        drawBoxes(QStringLiteral("pillars"), QColor(0xC9, 0xB4, 0x6A), false);

        const int r = m_values.value(QStringLiteral("baseSize")).toInt(3);
        const double bx0 = X(-r * cell);
        const double by0 = Y((r + 1) * cell);
        const double bw = (2 * r + 1) * cell * k;
        g.setBrush(Qt::NoBrush);
        g.setPen(QPen(QColor(0x7F, 0xE0, 0xA0), 4));
        g.drawRect(QRectF(bx0 - 8, by0 - 8, bw + 16, bw + 16));
        const QJsonArray spawn = m_area.value(QStringLiteral("spawn")).toArray();
        const double sx = spawn.at(0).toDouble();
        const double sy = spawn.at(1).toDouble();
        const double yaw = m_area.value(QStringLiteral("spawnYaw")).toDouble();
        g.drawLine(QPointF(X(sx), Y(sy)), QPointF(X(sx - std::sin(yaw) * 5), Y(sy + std::cos(yaw) * 5)));
        g.setPen(Qt::NoPen);
        g.setBrush(QColor(0x7F, 0xE0, 0xA0));
        g.drawEllipse(QPointF(X(sx), Y(sy)), 9, 9);
    }

private:
    QJsonObject m_area;
    QJsonObject m_values;
};

void HearthLevelGenerator::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(80, 60, 1400, 880);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthLevelGenerator>("Level Generator", "Tools", options);
}

HearthLevelGenerator::HearthLevelGenerator(QWidget* parent)
    : QWidget(parent)
    , m_agent(std::make_unique<HearthAgentClient>(this))
{
    setObjectName(QStringLiteral("HearthLevelGen"));
    setStyleSheet(QStringLiteral("QWidget#HearthLevelGen { background-color: #0D0C0B; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Level Generator"), QString());
    m_levelLine = Hearth::MakeLabel(QString(), QStringLiteral("small"), this);
    if (auto* titles = qobject_cast<QVBoxLayout*>(header->itemAt(1)->layout()))
    {
        titles->addWidget(m_levelLine);
    }
    m_tests = Hearth::MakeButton(tr("Run layout tests"), false, this);
    m_play = Hearth::MakeButton(tr("Walk it in Play"), false, this);
    m_apply = Hearth::MakeButton(tr("Apply to level"), true, this);
    header->addWidget(m_tests);
    header->addWidget(m_play);
    header->addWidget(m_apply);
    root->addLayout(header);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);
    auto* left = new QVBoxLayout();
    left->setSpacing(10);

    QVBoxLayout* mapLayout = nullptr;
    QFrame* mapCard = Hearth::MakeCard(this, &mapLayout);
    auto* legend = new QHBoxLayout();
    legend->setSpacing(16);
    const QVector<QPair<QString, QString>> legendItems = { { tr("Working"), QStringLiteral("#FFF8CD") }, { tr("Flickering"), QStringLiteral("#FFA028") },
                                                           { tr("Dead"), QStringLiteral("#5F5A50") }, { tr("Dying"), QStringLiteral("#FF5F4B") },
                                                           { tr("Forward base"), QStringLiteral("#7FE0A0") } };
    for (const auto& item : legendItems)
    {
        auto* dot = new QLabel(mapCard);
        dot->setFixedSize(8, 8);
        dot->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 2px;").arg(item.second));
        legend->addWidget(dot);
        legend->addWidget(Hearth::MakeLabel(item.first, QStringLiteral("body"), mapCard));
    }
    legend->addStretch(1);
    mapLayout->addLayout(legend);
    m_map = new HearthLevelMap(mapCard);
    mapLayout->addWidget(m_map, 1);
    m_mapNote = Hearth::MakeLabel(QString(), QStringLiteral("small"), mapCard);
    m_mapNote->setStyleSheet(QStringLiteral("background: transparent; color: #8D857B; font-family: \"Geist Mono\"; font-size: 11px;"));
    mapLayout->addWidget(m_mapNote, 0, Qt::AlignRight);
    left->addWidget(mapCard, 1);

    auto* stats = new QHBoxLayout();
    stats->setSpacing(10);
    const QStringList statNames = { tr("Reachable cells"), tr("Walls per chunk"), tr("Lights per chunk"), tr("Clear view at spawn") };
    for (int i = 0; i < 4; ++i)
    {
        QVBoxLayout* statLayout = nullptr;
        QFrame* card = Hearth::MakeCard(this, &statLayout);
        statLayout->setSpacing(2);
        statLayout->addWidget(Hearth::MakeLabel(statNames[i], QStringLiteral("dim"), card));
        m_stat[i] = Hearth::MakeLabel(QStringLiteral("—"), QStringLiteral("title"), card);
        m_stat[i]->setStyleSheet(QStringLiteral("background: transparent; border: none; color: #ECE6DE; font-size: 20px; font-weight: 600;"));
        statLayout->addWidget(m_stat[i]);
        m_statNote[i] = Hearth::MakeLabel(QString(), QStringLiteral("small"), card);
        statLayout->addWidget(m_statNote[i]);
        stats->addWidget(card);
    }
    left->addLayout(stats);
    body->addLayout(left, 1);

    // Right: seed, layout, horror.
    auto* right = new QVBoxLayout();
    right->setSpacing(10);
    QVBoxLayout* seedLayout = nullptr;
    QFrame* seedCard = Hearth::MakeCard(this, &seedLayout);
    seedLayout->addWidget(Hearth::MakeLabel(tr("Seed"), QStringLiteral("title"), seedCard));
    auto* seedRow = new QHBoxLayout();
    m_seed = new QLineEdit(seedCard);
    m_seed->setStyleSheet(QStringLiteral("QLineEdit { font-family: \"Geist Mono\"; }"));
    seedRow->addWidget(m_seed, 1);
    QPushButton* newSeed = Hearth::MakeButton(QStringLiteral("↻"), false, seedCard);
    newSeed->setFixedWidth(36);
    newSeed->setToolTip(tr("New seed"));
    seedRow->addWidget(newSeed);
    seedLayout->addLayout(seedRow);
    auto* eachRun = new Hearth::Toggle(seedCard);
    m_switches.insert(QStringLiteral("newSeedEachRun"), eachRun);
    auto* eachRow = new QHBoxLayout();
    eachRow->addWidget(Hearth::MakeLabel(tr("New seed every run"), QStringLiteral("body"), seedCard), 1);
    eachRow->addWidget(eachRun);
    seedLayout->addLayout(eachRow);
    right->addWidget(seedCard);

    QMap<QString, QVBoxLayout*> groups;
    for (const char* group : { "Layout", "Horror" })
    {
        QVBoxLayout* groupLayout = nullptr;
        QFrame* card = Hearth::MakeCard(this, &groupLayout);
        groupLayout->addWidget(Hearth::MakeLabel(tr(group), QStringLiteral("title"), card));
        groups.insert(QString::fromLatin1(group), groupLayout);
        right->addWidget(card);
    }
    for (const SliderSpec& spec : Sliders)
    {
        QVBoxLayout* groupLayout = groups.value(QString::fromLatin1(spec.group));
        QWidget* card = groupLayout->parentWidget();
        auto* top = new QHBoxLayout();
        top->addWidget(Hearth::MakeLabel(tr(spec.label), QStringLiteral("body"), card), 1);
        auto* value = new QLabel(card);
        value->setStyleSheet(QStringLiteral("background-color: rgba(255, 255, 255, 13); color: #ECE6DE; border-radius: 6px; padding: 1px 7px;"
                                            " font-family: \"Geist Mono\"; font-size: 11px;"));
        top->addWidget(value);
        groupLayout->addLayout(top);
        auto* slider = new QSlider(Qt::Horizontal, card);
        slider->setRange(0, qRound((spec.max - spec.min) / spec.step));
        slider->setStyleSheet(QLatin1String(SliderStyle));
        groupLayout->addWidget(slider);
        const QString key = QString::fromLatin1(spec.key);
        m_sliders.insert(key, slider);
        m_sliderValues.insert(key, value);
        const double min = spec.min;
        const double step = spec.step;
        connect(slider, &QSlider::valueChanged, this, [this, key, min, step](int position)
        {
            const double v = min + position * step;
            if (key == QLatin1String("baseSize"))
            {
                m_values.insert(key, static_cast<int>(v));
            }
            else
            {
                m_values.insert(key, v);
            }
            Changed();
        });
    }
    QVBoxLayout* horror = groups.value(QStringLiteral("Horror"));
    for (const auto& item : QVector<QPair<QString, QString>>{ { QStringLiteral("flicker"), tr("Flickering lights") },
                                                               { QStringLiteral("dying"), tr("Dying lights") },
                                                               { QStringLiteral("roomShifting"), tr("Room shifting (soon)") } })
    {
        auto* toggle = new Hearth::Toggle(horror->parentWidget());
        auto* row = new QHBoxLayout();
        row->addWidget(Hearth::MakeLabel(item.second, QStringLiteral("body"), horror->parentWidget()), 1);
        row->addWidget(toggle);
        horror->addLayout(row);
        m_switches.insert(item.first, toggle);
    }
    m_switches.value(QStringLiteral("roomShifting"))->setEnabled(false);
    right->addStretch(1);
    auto* rightHolder = new QWidget(this);
    rightHolder->setFixedWidth(300);
    rightHolder->setLayout(right);
    body->addWidget(rightHolder);
    root->addLayout(body, 1);

    for (auto it = m_switches.begin(); it != m_switches.end(); ++it)
    {
        const QString key = it.key();
        connect(it.value(), &QAbstractButton::clicked, this, [this, key](bool on)
        {
            m_values.insert(key, on);
            Changed();
        });
    }
    connect(m_seed, &QLineEdit::editingFinished, this, [this]()
    {
        QString digits = m_seed->text();
        digits.remove(QRegularExpression(QStringLiteral("\\D")));
        if (digits.isEmpty())
        {
            m_seed->setText(m_values.value(QStringLiteral("seed")).toString());
            return;
        }
        bool ok = false;
        const quint64 seed = digits.left(20).toULongLong(&ok);
        m_values.insert(QStringLiteral("seed"), QString::number(ok ? seed : 1));
        Changed();
    });
    connect(newSeed, &QPushButton::clicked, this, [this]()
    {
        m_values.insert(QStringLiteral("seed"), QString::number(QRandomGenerator::global()->generate64()));
        Changed();
    });

    connect(m_tests, &QPushButton::clicked, this, [this]()
    {
        m_tests->setEnabled(false);
        m_tests->setText(tr("Testing 50 seeds…"));
        m_agent->Post(QStringLiteral("levelgen_tests"), QJsonObject{ { QStringLiteral("values"), m_values }, { QStringLiteral("seeds"), 50 } },
                      [this](const QJsonObject& answer, bool)
                      {
                          m_tests->setEnabled(true);
                          m_tests->setText(tr("Run layout tests"));
                          if (answer.value(QStringLiteral("ok")).toBool(true) == false)
                          {
                              Toast(tr("Tests failed to run: %1").arg(answer.value(QStringLiteral("error")).toString()));
                              return;
                          }
                          const QJsonObject t = answer.value(QStringLiteral("data")).toObject();
                          ShowStats(t, tr("over %1 seeds").arg(t.value(QStringLiteral("seeds")).toInt()));
                          const bool good = t.value(QStringLiteral("reachable")).toInt() == t.value(QStringLiteral("total")).toInt() &&
                                            t.value(QStringLiteral("deterministic")).toBool();
                          Toast(good ? tr("Layout tests passed: %1 seeds, every cell reachable, same rooms every time.").arg(t.value(QStringLiteral("seeds")).toInt())
                                     : tr("Layout tests found problems: %1 cells cut off.")
                                           .arg(t.value(QStringLiteral("total")).toInt() - t.value(QStringLiteral("reachable")).toInt()));
                      },
                      180000);
    });
    connect(m_play, &QPushButton::clicked, this, [this]()
    {
        m_play->setEnabled(false);
        m_play->setText(tr("Starting…"));
        m_agent->Post(QStringLiteral("levelgen_play"), QJsonObject{ { QStringLiteral("values"), m_values }, { QStringLiteral("fullscreen"), true } },
                      [this](const QJsonObject& answer, bool)
                      {
                          m_play->setEnabled(true);
                          m_play->setText(tr("Walk it in Play"));
                          Toast(answer.value(QStringLiteral("ok")).toBool(true) ? tr("Playing this exact map full screen. Close the game to come back.")
                                                                                 : tr("Couldn't start the game: %1").arg(answer.value(QStringLiteral("error")).toString()));
                      },
                      120000);
    });
    connect(m_apply, &QPushButton::clicked, this, [this]()
    {
        m_apply->setEnabled(false);
        m_apply->setText(tr("Applying…"));
        m_agent->Post(QStringLiteral("levelgen_apply"), QJsonObject{ { QStringLiteral("values"), m_values } },
                      [this](const QJsonObject& answer, bool)
                      {
                          if (answer.value(QStringLiteral("ok")).toBool(true) == false)
                          {
                              Toast(tr("Couldn't apply: %1").arg(answer.value(QStringLiteral("error")).toString()));
                          }
                          else
                          {
                              m_values = answer.value(QStringLiteral("values")).toObject();
                              m_saved = m_values;
                              Toast(answer.value(QStringLiteral("via")).toString() == QLatin1String("editor") ? tr("Saved into the level in the open editor.")
                                                                                                             : tr("Saved into the level."));
                          }
                          Show();
                      },
                      60000);
    });

    m_toast = new QLabel(this);
    m_toast->setStyleSheet(QStringLiteral("background-color: #1D1A17; color: #ECE6DE; border: 1px solid rgba(255, 255, 255, 26); border-radius: 12px; padding: 10px 16px;"));
    m_toast->hide();
    root->addWidget(m_toast, 0, Qt::AlignHCenter);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(120);
    connect(m_debounce, &QTimer::timeout, this, &HearthLevelGenerator::Regenerate);

    Load();
}

HearthLevelGenerator::~HearthLevelGenerator() = default;

void HearthLevelGenerator::Load()
{
    m_agent->Post(QStringLiteral("levelgen_get"), {}, [this](const QJsonObject& answer, bool reached)
    {
        if (!reached || answer.value(QStringLiteral("ok")).toBool(true) == false)
        {
            Toast(tr("Can't reach the Hearth Engine agent: %1").arg(answer.value(QStringLiteral("error")).toString()));
            return;
        }
        m_values = answer.value(QStringLiteral("values")).toObject();
        // Seeds are 64-bit: keep them as text so nothing rounds them.
        const QJsonValue seed = m_values.value(QStringLiteral("seed"));
        m_values.insert(QStringLiteral("seed"), seed.isString() ? seed.toString() : QString::number(static_cast<qulonglong>(seed.toDouble())));
        m_saved = m_values;
        m_levelLine->setText(QStringLiteral("%1 · %2").arg(answer.value(QStringLiteral("level")).toString(), answer.value(QStringLiteral("name")).toString()));
        Show();
        Regenerate();
    });
}

void HearthLevelGenerator::Show()
{
    m_seed->setText(m_values.value(QStringLiteral("seed")).toString());
    for (const SliderSpec& spec : Sliders)
    {
        const QString key = QString::fromLatin1(spec.key);
        const double value = m_values.value(key).toDouble(spec.min);
        QSlider* slider = m_sliders.value(key);
        const QSignalBlocker blocker(slider);
        slider->setValue(qRound((value - spec.min) / spec.step));
        m_sliderValues.value(key)->setText(LabelFor(key, value));
    }
    for (auto it = m_switches.begin(); it != m_switches.end(); ++it)
    {
        it.value()->setChecked(m_values.value(it.key()).toBool());
    }
    const bool changed = m_values != m_saved;
    m_apply->setText(changed ? tr("Apply to level") : tr("Applied"));
    m_apply->setEnabled(changed);
}

void HearthLevelGenerator::Changed()
{
    Show();
    m_debounce->start();
}

void HearthLevelGenerator::Regenerate()
{
    const int mine = ++m_pending;
    m_agent->Post(QStringLiteral("levelgen_area"), QJsonObject{ { QStringLiteral("values"), m_values } }, [this, mine](const QJsonObject& answer, bool)
    {
        if (mine != m_pending)
        {
            return;
        }
        if (answer.value(QStringLiteral("ok")).toBool(true) == false)
        {
            Toast(tr("Couldn't generate: %1").arg(answer.value(QStringLiteral("error")).toString()));
            return;
        }
        const QJsonObject area = answer.value(QStringLiteral("data")).toObject();
        m_map->SetArea(area, m_values);
        const QJsonArray minA = area.value(QStringLiteral("min")).toArray();
        const QJsonArray maxA = area.value(QStringLiteral("max")).toArray();
        const int chunks = (maxA.at(0).toInt() - minA.at(0).toInt() + 1);
        const int metres = qRound(chunks * area.value(QStringLiteral("chunkSize")).toDouble(24.0));
        m_mapNote->setText(tr("%1 chunks · %2 × %2 m").arg(chunks * chunks).arg(metres));
    });
    m_agent->Post(QStringLiteral("levelgen_tests"), QJsonObject{ { QStringLiteral("values"), m_values }, { QStringLiteral("seeds"), 1 } },
                  [this, mine](const QJsonObject& answer, bool)
                  {
                      if (mine == m_pending && answer.value(QStringLiteral("ok")).toBool(true))
                      {
                          ShowStats(answer.value(QStringLiteral("data")).toObject(), QString());
                      }
                  });
}

void HearthLevelGenerator::ShowStats(const QJsonObject& t, const QString& note)
{
    const int reachable = t.value(QStringLiteral("reachable")).toInt();
    const int total = t.value(QStringLiteral("total")).toInt();
    m_stat[0]->setText(QStringLiteral("%1 / %2").arg(reachable).arg(total));
    m_stat[1]->setText(QString::number(t.value(QStringLiteral("wallsPerChunk")).toDouble(), 'f', 1));
    m_stat[2]->setText(QString::number(t.value(QStringLiteral("lightsPerChunk")).toDouble(), 'f', 1));
    m_stat[3]->setText(tr("%1 m min").arg(qRound(t.value(QStringLiteral("minView")).toDouble())));
    m_statNote[0]->setText(reachable == total ? (note.isEmpty() ? tr("every cell connected") : note) : tr("some cells are cut off"));
    m_statNote[0]->setStyleSheet(reachable == total ? QStringLiteral("background: transparent; color: #8D857B; font-size: 11px;")
                                                    : QStringLiteral("background: transparent; color: #FF9F8F; font-size: 11px;"));
    m_statNote[1]->setText(note);
    m_statNote[2]->setText(note);
    m_statNote[3]->setText(note.isEmpty() ? tr("%1 m along the way you face").arg(qRound(t.value(QStringLiteral("avgView")).toDouble())) : note);
}

void HearthLevelGenerator::Toast(const QString& text)
{
    m_toast->setText(text);
    m_toast->show();
    QTimer::singleShot(7000, m_toast, &QWidget::hide);
}
