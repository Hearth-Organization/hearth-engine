/*
 * Hearth Engine "Monster pathing". Built on O3DE (Apache-2.0 OR MIT). See HearthPathingPane.h.
 */

#include "EditorDefs.h"

#include "HearthPathingPane.h"
#include "HearthAgentClient.h"

#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>
#include <AzToolsFramework/ActionManager/Action/ActionManagerInterface.h>

#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRandomGenerator>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <climits>
#include <functional>
#include <cmath>
#include <queue>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;
    constexpr double PathingWallThickness = 0.2;
}

//! The top view: cells, walls, monsters (hearing ring, sight cone, path), the player. Drag the player or a monster.
class HearthPathingView : public QWidget
{
public:
    explicit HearthPathingView(HearthPathingPane* pane)
        : QWidget(pane)
        , m_pane(pane)
    {
        setMinimumSize(520, 420);
        setCursor(Qt::OpenHandCursor);
        setToolTip(QObject::tr("Drag the player (white) or a monster (red). Scroll to zoom."));
    }

    double m_zoom = 1.0;

protected:
    QRectF World() const
    {
        const QRect& c = m_pane->m_cells;
        return QRectF(c.left() * m_pane->m_cell, c.top() * m_pane->m_cell, c.width() * m_pane->m_cell, c.height() * m_pane->m_cell);
    }
    double Scale() const
    {
        const QRectF w = World();
        return w.width() > 0 ? qMin(width() / w.width(), height() / w.height()) * m_zoom : 1.0;
    }
    QPointF ToView(const QPointF& metres) const
    {
        const QRectF w = World();
        const double s = Scale();
        const QPointF origin((width() - w.width() * s) / 2.0, (height() - w.height() * s) / 2.0);
        // +Y (forward in the game) is up on screen.
        return QPointF(origin.x() + (metres.x() - w.left()) * s, origin.y() + (w.bottom() - metres.y()) * s);
    }
    QPointF ToWorld(const QPointF& view) const
    {
        const QRectF w = World();
        const double s = Scale();
        const QPointF origin((width() - w.width() * s) / 2.0, (height() - w.height() * s) / 2.0);
        return QPointF(w.left() + (view.x() - origin.x()) / s, w.bottom() - (view.y() - origin.y()) / s);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()), 12, 12);
        p.setClipPath(clip);
        p.fillRect(rect(), QColor(0x0D, 0x0C, 0x0B));
        if (m_pane->m_cells.isEmpty())
        {
            p.setPen(Hearth::Dim());
            p.drawText(rect(), Qt::AlignCenter, QObject::tr("Press Rebuild to make the walk area from the level's generator."));
            return;
        }
        const double s = Scale();
        const double cell = m_pane->m_cell;
        // Walk area: every cell a monster can stand in (all of them in Level 0's grid), blue.
        if (m_pane->m_layers[0])
        {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x5B, 0x8D, 0xE8, 40));
            for (int x = m_pane->m_cells.left(); x <= m_pane->m_cells.right(); ++x)
            {
                for (int y = m_pane->m_cells.top(); y <= m_pane->m_cells.bottom(); ++y)
                {
                    const QPointF a = ToView(QPointF(x * cell + 0.25, (y + 1) * cell - 0.25));
                    p.drawRoundedRect(QRectF(a, QSizeF(cell * s - 0.5 * s, cell * s - 0.5 * s)), 3, 3);
                }
            }
        }
        // Walls and pillars.
        p.setBrush(QColor(0xE6, 0xD4, 0x8C));
        for (const char* kind : { "walls", "pillars" })
        {
            for (const QJsonValue value : m_pane->m_area.value(QLatin1String(kind)).toArray())
            {
                const QJsonArray b = value.toArray();
                const QPointF topLeft = ToView(QPointF(b[0].toDouble() - b[2].toDouble() / 2.0, b[1].toDouble() + b[3].toDouble() / 2.0));
                p.drawRect(QRectF(topLeft, QSizeF(b[2].toDouble() * s, b[3].toDouble() * s)));
            }
        }
        QFont font = p.font();
        font.setPixelSize(12);
        p.setFont(font);
        for (const HearthPathingPane::Monster& monster : m_pane->m_monsters)
        {
            const QPointF at = ToView(monster.at);
            // Sight: a 120° fan, cut where walls block it.
            if (m_pane->m_layers[3])
            {
                const QPointF toPlayer = m_pane->m_player - monster.at;
                const double facing = std::atan2(toPlayer.y(), toPlayer.x());
                QPainterPath fan;
                fan.moveTo(at);
                for (int i = 0; i <= 24; ++i)
                {
                    const double a = facing - 1.047 + 2.094 * i / 24.0;
                    double reach = monster.sight;
                    for (double d = 0.5; d <= monster.sight; d += 0.5)
                    {
                        if (!m_pane->LineOfSight(monster.at, monster.at + QPointF(std::cos(a) * d, std::sin(a) * d)))
                        {
                            reach = d - 0.5;
                            break;
                        }
                    }
                    fan.lineTo(ToView(monster.at + QPointF(std::cos(a) * reach, std::sin(a) * reach)));
                }
                fan.closeSubpath();
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0xF0, 0xD1, 0x7A, 30));
                p.drawPath(fan);
            }
            // Hearing: a ring of the hearing distance; red when it just heard something.
            if (m_pane->m_layers[2])
            {
                const bool heard = m_pane->m_time - monster.heardTime < 2.0;
                p.setPen(QPen(heard ? QColor(0xFF, 0x7A, 0x6A) : QColor(0xFF, 0x7A, 0x6A, 90), heard ? 2.0 : 1.0, Qt::SolidLine));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(at, monster.hearing * s, monster.hearing * s);
                if (heard)
                {
                    p.drawEllipse(ToView(monster.heardAt), 0.8 * s, 0.8 * s);
                }
            }
            // Path: orange dashes.
            if (m_pane->m_layers[1] && !monster.path.isEmpty())
            {
                QPen dashes(QColor(0xE8, 0x74, 0x3B), 2.0, Qt::DashLine);
                p.setPen(dashes);
                QPointF last = at;
                for (const QPoint& step : monster.path)
                {
                    const QPointF next = ToView(m_pane->Centre(step));
                    p.drawLine(last, next);
                    last = next;
                }
            }
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0xFF, 0x7A, 0x6A));
            p.drawEllipse(at, 7, 7);
            p.setPen(Hearth::Text());
            p.drawText(at + QPointF(10, -8), monster.name);
            if (monster.state.startsWith(QLatin1String("Hears")) || monster.state.startsWith(QLatin1String("Hunting")))
            {
                p.setPen(QColor(0xFF, 0x9F, 0x8F));
                p.drawText(at + QPointF(10, 8), monster.state);
            }
        }
        const QPointF player = ToView(m_pane->m_player);
        p.setPen(Qt::NoPen);
        p.setBrush(Hearth::Text());
        p.drawEllipse(player, 7, 7);
        p.setPen(Hearth::Text());
        p.drawText(player + QPointF(10, -8), m_pane->m_playerName);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        const QPointF at = ToWorld(event->pos());
        m_dragging = -2;
        if (QLineF(event->pos(), ToView(m_pane->m_player)).length() < 14)
        {
            m_dragging = -1;
        }
        for (int i = 0; i < m_pane->m_monsters.size(); ++i)
        {
            if (QLineF(event->pos(), ToView(m_pane->m_monsters[i].at)).length() < 14)
            {
                m_dragging = i;
            }
        }
        Q_UNUSED(at);
        setCursor(m_dragging >= -1 ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!(event->buttons() & Qt::LeftButton) || m_dragging < -1)
        {
            return;
        }
        const QPointF at = ToWorld(event->pos());
        if (m_dragging == -1)
        {
            m_pane->m_player = at;
        }
        else if (m_dragging < m_pane->m_monsters.size())
        {
            m_pane->m_monsters[m_dragging].at = at;
            m_pane->m_monsters[m_dragging].path.clear();
        }
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        m_dragging = -2;
        setCursor(Qt::OpenHandCursor);
        if (m_onMoved)
        {
            m_onMoved();
        }
    }
    void wheelEvent(QWheelEvent* event) override
    {
        m_zoom = qBound(0.5, m_zoom * (event->angleDelta().y() > 0 ? 1.15 : 0.87), 6.0);
        update();
    }

public:
    std::function<void()> m_onMoved;

private:
    HearthPathingPane* m_pane;
    int m_dragging = -2;
};

void HearthPathingPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(140, 80, 1240, 800);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthPathingPane>("Monster pathing", "Tools", options);
}

HearthPathingPane::HearthPathingPane(QWidget* parent)
    : QWidget(parent)
    , m_agent(std::make_unique<HearthAgentClient>(this))
{
    setObjectName(QStringLiteral("HearthPathing"));
    setStyleSheet(QStringLiteral("QWidget#HearthPathing { background-color: #0D0C0B; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);
    const QString level = GetIEditor() ? GetIEditor()->GetLevelName() : QString();
    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Monster pathing"), tr("%1 · where entities can walk and how they hunt").arg(level.isEmpty() ? tr("Level0") : level));
    const QStringList layers = { tr("Walk area"), tr("Paths"), tr("Hearing"), tr("Sight") };
    for (int i = 0; i < 4; ++i)
    {
        QPushButton* chip = Hearth::MakeButton(layers[i], false, this);
        chip->setCheckable(true);
        chip->setChecked(true);
        connect(chip, &QPushButton::toggled, this, [this, i](bool on)
        {
            m_layers[i] = on;
            m_view->update();
        });
        header->addWidget(chip);
    }
    QPushButton* rebuild = Hearth::MakeButton(tr("Rebuild"), false, this);
    m_test = Hearth::MakeButton(tr("Test with a monster"), true, this);
    m_test->setCheckable(true);
    header->addWidget(rebuild);
    header->addWidget(m_test);
    root->addLayout(header);
    connect(rebuild, &QPushButton::clicked, this, &HearthPathingPane::Rebuild);
    connect(m_test, &QPushButton::toggled, this, [this](bool on)
    {
        m_testing = on;
        m_test->setText(on ? tr("Stop the test") : tr("Test with a monster"));
        if (on && m_monsters.isEmpty())
        {
            m_monsters.push_back({ tr("Smiler"), m_player + QPointF(10.0, 6.0) });
            SaveMonsters();
            ShowMonsters();
        }
        m_lastStep = m_player;
    });

    auto* body = new QHBoxLayout();
    body->setSpacing(10);
    root->addLayout(body, 1);
    QVBoxLayout* viewLayout = nullptr;
    QFrame* viewCard = Hearth::MakeCard(this, &viewLayout);
    auto* viewTop = new QHBoxLayout();
    viewTop->addWidget(Hearth::MakeLabel(tr("Top view"), QStringLiteral("title"), viewCard), 1);
    m_rooms = Hearth::MakeChip(tr("Rooms loaded: 0"), QStringLiteral("plain"), viewCard);
    m_built = Hearth::MakeChip(tr("Not built"), QStringLiteral("plain"), viewCard);
    viewTop->addWidget(m_rooms);
    viewTop->addWidget(m_built);
    viewLayout->addLayout(viewTop);
    m_view = new HearthPathingView(this);
    m_view->m_onMoved = [this]() { SaveMonsters(); };
    viewLayout->addWidget(m_view, 1);
    auto* legend = new QHBoxLayout();
    legend->addWidget(Hearth::MakeLabel(tr("Blue: monsters can walk"), QStringLiteral("small"), viewCard));
    legend->addWidget(Hearth::MakeLabel(tr("Orange dashes: current path"), QStringLiteral("small"), viewCard));
    legend->addWidget(Hearth::MakeLabel(tr("Red ring: heard a sound"), QStringLiteral("small"), viewCard));
    legend->addStretch(1);
    viewLayout->addLayout(legend);
    body->addWidget(viewCard, 1);

    QVBoxLayout* sideLayout = nullptr;
    QFrame* side = Hearth::MakeCard(this, &sideLayout);
    side->setFixedWidth(320);
    sideLayout->addWidget(Hearth::MakeLabel(tr("Monsters in the test"), QStringLiteral("title"), side));
    m_list = new QVBoxLayout();
    m_list->setSpacing(8);
    sideLayout->addLayout(m_list);
    QPushButton* add = Hearth::MakeButton(tr("+ Add a monster"), false, side);
    connect(add, &QPushButton::clicked, this, [this]()
    {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Add a monster"), tr("Name"), QLineEdit::Normal, tr("Hound"), &ok).trimmed();
        if (ok && !name.isEmpty())
        {
            Monster monster;
            monster.name = name;
            monster.at = m_player + QPointF(-8.0, 8.0);
            m_monsters.push_back(monster);
            SaveMonsters();
            ShowMonsters();
            m_view->update();
        }
    });
    sideLayout->addWidget(add);
    QPushButton* play = Hearth::MakeButton(tr("Walk it in Play"), false, side);
    play->setToolTip(tr("Play the level; the game's monsters show here live"));
    connect(play, &QPushButton::clicked, this, []()
    {
        if (auto* actions = AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get())
        {
            actions->TriggerAction("o3de.action.game.play");
        }
    });
    sideLayout->addWidget(play);
    sideLayout->addStretch(1);
    m_status = Hearth::MakeLabel(QString(), QStringLiteral("small"), side);
    m_status->setWordWrap(true);
    sideLayout->addWidget(m_status);
    body->addWidget(side);

    m_clock = new QTimer(this);
    m_clock->setInterval(100);
    connect(m_clock, &QTimer::timeout, this, [this]()
    {
        m_time += 0.1;
        ReadLive();
        if (m_testing)
        {
            Step(0.1);
        }
        if (m_testing || std::any_of(m_monsters.begin(), m_monsters.end(), [](const Monster& m) { return m.live; }))
        {
            m_view->update();
            ShowMonsters();
        }
    });
    m_clock->start();

    m_playerName = tr("You");
    LoadMonsters();
    QTimer::singleShot(0, this, &HearthPathingPane::Rebuild);
}

HearthPathingPane::~HearthPathingPane() = default;

qint64 HearthPathingPane::EdgeKey(int vertical, int line, int cell)
{
    return (static_cast<qint64>(vertical) << 62) ^ (static_cast<qint64>(line + 0x100000) << 31) ^ static_cast<qint64>(cell + 0x100000);
}

QPoint HearthPathingPane::Cell(const QPointF& metres) const
{
    return QPoint(static_cast<int>(std::floor(metres.x() / m_cell)), static_cast<int>(std::floor(metres.y() / m_cell)));
}

QPointF HearthPathingPane::Centre(const QPoint& cell) const
{
    return QPointF((cell.x() + 0.5) * m_cell, (cell.y() + 0.5) * m_cell);
}

bool HearthPathingPane::Open(int x, int y, int dx, int dy) const
{
    const QPoint next(x + dx, y + dy);
    if (!m_cells.contains(next) || !m_cells.contains(QPoint(x, y)))
    {
        return false;
    }
    if (dx == 1) return !m_blocked.contains(EdgeKey(1, x + 1, y));
    if (dx == -1) return !m_blocked.contains(EdgeKey(1, x, y));
    if (dy == 1) return !m_blocked.contains(EdgeKey(0, y + 1, x));
    if (dy == -1) return !m_blocked.contains(EdgeKey(0, y, x));
    return true;
}

bool HearthPathingPane::LineOfSight(const QPointF& a, const QPointF& b) const
{
    // Walk the segment a little at a time; every time it crosses into another cell, that edge must be open.
    const double length = QLineF(a, b).length();
    QPoint cell = Cell(a);
    const int steps = qMax(1, static_cast<int>(length / 0.2));
    for (int i = 1; i <= steps; ++i)
    {
        const QPoint next = Cell(a + (b - a) * (static_cast<double>(i) / steps));
        if (next == cell)
        {
            continue;
        }
        const int dx = next.x() - cell.x();
        const int dy = next.y() - cell.y();
        if (qAbs(dx) + qAbs(dy) == 1)
        {
            if (!Open(cell.x(), cell.y(), dx, dy))
            {
                return false;
            }
        }
        else
        {
            // Through a corner: either way round must be open.
            const bool viaX = Open(cell.x(), cell.y(), dx, 0) && Open(cell.x() + dx, cell.y(), 0, dy);
            const bool viaY = Open(cell.x(), cell.y(), 0, dy) && Open(cell.x(), cell.y() + dy, dx, 0);
            if (!viaX && !viaY)
            {
                return false;
            }
        }
        cell = next;
    }
    return true;
}

QVector<QPoint> HearthPathingPane::FindPath(const QPoint& from, const QPoint& to) const
{
    // A* over the cells (4 neighbours), shortest walk through the doorways.
    auto key = [](const QPoint& p) { return (static_cast<qint64>(p.x()) << 32) ^ static_cast<quint32>(p.y()); };
    using Node = std::pair<int, qint64>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    QHash<qint64, QPoint> cameFrom;
    QHash<qint64, int> cost;
    QHash<qint64, QPoint> points;
    open.push({ 0, key(from) });
    cost.insert(key(from), 0);
    points.insert(key(from), from);
    int visited = 0;
    while (!open.empty() && visited++ < 20000)
    {
        const QPoint current = points.value(open.top().second);
        open.pop();
        if (current == to)
        {
            QVector<QPoint> path;
            QPoint at = to;
            while (at != from)
            {
                path.prepend(at);
                at = cameFrom.value(key(at));
            }
            return path;
        }
        const QPoint directions[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (const QPoint& d : directions)
        {
            if (!Open(current.x(), current.y(), d.x(), d.y()))
            {
                continue;
            }
            const QPoint next = current + d;
            const int newCost = cost.value(key(current)) + 1;
            if (!cost.contains(key(next)) || newCost < cost.value(key(next)))
            {
                cost.insert(key(next), newCost);
                points.insert(key(next), next);
                cameFrom.insert(key(next), current);
                open.push({ newCost + qAbs(to.x() - next.x()) + qAbs(to.y() - next.y()), key(next) });
            }
        }
    }
    return {};
}

void HearthPathingPane::Rebuild()
{
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    m_built->setText(tr("Building…"));
    m_agent->Post(QStringLiteral("levelgen_get"), {}, [this, clock](const QJsonObject& level, bool reached)
    {
        if (!reached || !level.value(QStringLiteral("ok")).toBool(false))
        {
            m_built->setText(tr("Can't reach the agent"));
            m_status->setText(level.value(QStringLiteral("error")).toString());
            return;
        }
        const QJsonObject values = level.value(QStringLiteral("values")).toObject();
        // 6 × 6 chunks around the spawn.
        const QJsonObject args{ { QStringLiteral("values"), values }, { QStringLiteral("minX"), -3 }, { QStringLiteral("minY"), -3 },
                                { QStringLiteral("maxX"), 2 }, { QStringLiteral("maxY"), 2 } };
        m_agent->Post(QStringLiteral("levelgen_area"), args, [this, clock, values](const QJsonObject& answer, bool ok)
        {
            if (!ok || !answer.value(QStringLiteral("ok")).toBool(false))
            {
                m_built->setText(tr("Couldn't build"));
                m_status->setText(answer.value(QStringLiteral("error")).toString());
                return;
            }
            m_area = answer.value(QStringLiteral("data")).toObject();
            const int chunkCells = values.value(QStringLiteral("chunkCells")).toInt(6);
            const double chunkSize = m_area.value(QStringLiteral("chunkSize")).toDouble(24.0);
            m_cell = chunkSize / qMax(1, chunkCells);
            const QJsonArray minA = m_area.value(QStringLiteral("min")).toArray();
            const QJsonArray maxA = m_area.value(QStringLiteral("max")).toArray();
            const int chunks = (maxA[0].toInt() - minA[0].toInt() + 1) * (maxA[1].toInt() - minA[1].toInt() + 1);
            m_cells = QRect(QPoint(minA[0].toInt() * chunkCells, minA[1].toInt() * chunkCells),
                            QPoint((maxA[0].toInt() + 1) * chunkCells - 1, (maxA[1].toInt() + 1) * chunkCells - 1));
            // Walls as blocked edges between cells (same rule as the layout tests).
            m_blocked.clear();
            for (const QJsonValue value : m_area.value(QStringLiteral("walls")).toArray())
            {
                const QJsonArray w = value.toArray();
                const double cx = w[0].toDouble(), cy = w[1].toDouble(), sx = w[2].toDouble(), sy = w[3].toDouble();
                const bool horizontal = sy < sx;
                const double length = (horizontal ? sx : sy) - PathingWallThickness;
                const int count = static_cast<int>(std::lround(length / m_cell));
                const int line = static_cast<int>(std::lround((horizontal ? cy : cx) / m_cell));
                const int first = static_cast<int>(std::lround(((horizontal ? cx : cy) - length * 0.5) / m_cell));
                for (int i = 0; i < count; ++i)
                {
                    m_blocked.insert(EdgeKey(horizontal ? 0 : 1, line, first + i));
                }
            }
            const QJsonArray spawn = m_area.value(QStringLiteral("spawn")).toArray();
            if (m_player.isNull())
            {
                m_player = QPointF(spawn[0].toDouble(), spawn[1].toDouble());
            }
            m_rooms->setText(tr("Rooms loaded: %1").arg(chunks));
            m_built->setText(tr("Built in %1 s").arg(clock->elapsed() / 1000.0, 0, 'f', 1));
            m_view->update();
            ShowMonsters();
        });
    });
}

void HearthPathingPane::LoadMonsters()
{
    m_monsters.clear();
    QFile file(QDir(QString::fromUtf8(AZ::Utils::GetProjectPath().c_str())).filePath(QStringLiteral("Monsters/monsters.json")));
    if (file.open(QIODevice::ReadOnly))
    {
        for (const QJsonValue value : QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("monsters")).toArray())
        {
            const QJsonObject m = value.toObject();
            Monster monster;
            monster.name = m.value(QStringLiteral("name")).toString();
            monster.hearing = m.value(QStringLiteral("hearing")).toDouble(7.0);
            monster.sight = m.value(QStringLiteral("sight")).toDouble(12.0);
            monster.speed = m.value(QStringLiteral("speed")).toDouble(2.5);
            const QJsonArray at = m.value(QStringLiteral("testAt")).toArray();
            monster.at = QPointF(at.at(0).toDouble(10.0), at.at(1).toDouble(10.0));
            m_monsters.push_back(monster);
        }
    }
    ShowMonsters();
}

void HearthPathingPane::SaveMonsters()
{
    QJsonArray list;
    for (const Monster& monster : m_monsters)
    {
        list.append(QJsonObject{ { QStringLiteral("name"), monster.name }, { QStringLiteral("hearing"), monster.hearing },
                                 { QStringLiteral("sight"), monster.sight }, { QStringLiteral("speed"), monster.speed },
                                 { QStringLiteral("testAt"), QJsonArray{ monster.at.x(), monster.at.y() } } });
    }
    const QString path = QDir(QString::fromUtf8(AZ::Utils::GetProjectPath().c_str())).filePath(QStringLiteral("Monsters/monsters.json"));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        file.write(QJsonDocument(QJsonObject{ { QStringLiteral("monsters"), list } }).toJson(QJsonDocument::Indented));
    }
    m_status->setText(tr("Saved in Monsters/monsters.json for the game's monsters (hearing and sight in metres, speed in m/s)."));
}

void HearthPathingPane::ShowMonsters()
{
    // Rebuild the list only when the set changed; otherwise update the state lines.
    if (m_list->count() != m_monsters.size())
    {
        while (QLayoutItem* item = m_list->takeAt(0))
        {
            delete item->widget();
            delete item;
        }
        for (int i = 0; i < m_monsters.size(); ++i)
        {
            QVBoxLayout* cardLayout = nullptr;
            QFrame* card = Hearth::MakeCard(this, &cardLayout);
            auto* top = new QHBoxLayout();
            top->addWidget(Hearth::MakeLabel(m_monsters[i].name, QStringLiteral("title"), card), 1);
            QPushButton* remove = Hearth::MakeButton(QStringLiteral("✕"), false, card);
            remove->setFixedWidth(30);
            connect(remove, &QPushButton::clicked, this, [this, i]()
            {
                if (i < m_monsters.size())
                {
                    m_monsters.removeAt(i);
                    SaveMonsters();
                    QTimer::singleShot(0, this, &HearthPathingPane::ShowMonsters);
                    m_view->update();
                }
            });
            top->addWidget(remove);
            cardLayout->addLayout(top);
            QLabel* state = Hearth::MakeLabel(m_monsters[i].state, QStringLiteral("small"), card);
            state->setObjectName(QStringLiteral("state"));
            cardLayout->addWidget(state);
            struct Field
            {
                const char* label;
                double Monster::*value;
                double low, high;
                const char* suffix;
            };
            const Field fields[] = { { "Hears", &Monster::hearing, 1.0, 40.0, " m" }, { "Sees", &Monster::sight, 1.0, 60.0, " m" },
                                     { "Hunts at", &Monster::speed, 0.5, 12.0, " m/s" } };
            for (const Field& f : fields)
            {
                auto* spin = new QDoubleSpinBox(card);
                spin->setRange(f.low, f.high);
                spin->setDecimals(1);
                spin->setSuffix(tr(f.suffix));
                spin->setValue(m_monsters[i].*(f.value));
                double Monster::*member = f.value;
                connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, i, member](double v)
                {
                    if (i < m_monsters.size())
                    {
                        m_monsters[i].*member = v;
                        SaveMonsters();
                        m_view->update();
                    }
                });
                cardLayout->addWidget(Hearth::MakeSettingRow(tr(f.label), QString(), spin, card));
            }
            m_list->addWidget(card);
        }
    }
    for (int i = 0; i < m_list->count() && i < m_monsters.size(); ++i)
    {
        if (QWidget* card = m_list->itemAt(i)->widget())
        {
            if (auto* state = card->findChild<QLabel*>(QStringLiteral("state")))
            {
                state->setText(m_monsters[i].live ? tr("%1 (in the game)").arg(m_monsters[i].state) : m_monsters[i].state);
            }
        }
    }
}

void HearthPathingPane::Step(double seconds)
{
    if (m_cells.isEmpty())
    {
        return;
    }
    // A footstep every metre the player moves.
    const bool stepped = QLineF(m_player, m_lastStep).length() >= 1.0;
    if (stepped)
    {
        m_lastStep = m_player;
    }
    for (Monster& monster : m_monsters)
    {
        if (monster.live)
        {
            continue;
        }
        const double distance = QLineF(monster.at, m_player).length();
        const bool sees = distance <= monster.sight && LineOfSight(monster.at, m_player);
        if (stepped && distance <= monster.hearing)
        {
            monster.heardAt = m_player;
            monster.heardTime = m_time;
        }
        QPoint goal;
        if (sees)
        {
            monster.state = tr("Hunting %1").arg(m_playerName);
            goal = Cell(m_player);
        }
        else if (m_time - monster.heardTime < 6.0)
        {
            monster.state = tr("Hears footsteps · %1 m").arg(qRound(QLineF(monster.at, monster.heardAt).length()));
            goal = Cell(monster.heardAt);
        }
        else
        {
            monster.state = tr("Wandering");
            if (monster.wanderTarget.x() == INT_MIN || Cell(monster.at) == monster.wanderTarget)
            {
                const QPoint here = Cell(monster.at);
                monster.wanderTarget = QPoint(qBound(m_cells.left(), here.x() + QRandomGenerator::global()->bounded(-4, 5), m_cells.right()),
                                              qBound(m_cells.top(), here.y() + QRandomGenerator::global()->bounded(-4, 5), m_cells.bottom()));
            }
            goal = monster.wanderTarget;
        }
        monster.path = FindPath(Cell(monster.at), goal);
        // Walk along the path (slower when wandering).
        const double speed = monster.state == tr("Wandering") ? monster.speed * 0.4 : monster.speed;
        double left = speed * seconds;
        while (left > 0.0)
        {
            const QPointF target = monster.path.isEmpty() ? (sees ? m_player : monster.at) : Centre(monster.path.front());
            const QPointF to = target - monster.at;
            const double length = std::hypot(to.x(), to.y());
            if (length < 0.01)
            {
                if (monster.path.isEmpty())
                {
                    break;
                }
                monster.path.removeFirst();
                continue;
            }
            const double move = qMin(left, length);
            monster.at += to / length * move;
            left -= move;
        }
    }
}

void HearthPathingPane::ReadLive()
{
    // Monsters the running game reports: /NoclipProtocol/Monsters/List = "Smiler,Hound", and per monster
    // /NoclipProtocol/Monsters/<name>/X, /Y and /State.
    auto* registry = AZ::SettingsRegistry::Get();
    if (!registry)
    {
        return;
    }
    AZStd::string list;
    if (!registry->Get(list, "/NoclipProtocol/Monsters/List") || list.empty())
    {
        for (Monster& monster : m_monsters)
        {
            monster.live = false;
        }
        return;
    }
    for (const QString& name : QString::fromUtf8(list.c_str()).split(QLatin1Char(','), Qt::SkipEmptyParts))
    {
        double x = 0.0;
        double y = 0.0;
        AZStd::string state;
        const AZStd::string base = AZStd::string::format("/NoclipProtocol/Monsters/%s", name.toUtf8().constData());
        registry->Get(x, base + "/X");
        registry->Get(y, base + "/Y");
        registry->Get(state, base + "/State");
        auto found = std::find_if(m_monsters.begin(), m_monsters.end(), [&name](const Monster& m) { return m.name == name; });
        if (found == m_monsters.end())
        {
            Monster monster;
            monster.name = name;
            m_monsters.push_back(monster);
            found = m_monsters.end() - 1;
        }
        found->live = true;
        found->at = QPointF(x, y);
        found->state = QString::fromUtf8(state.c_str());
    }
    double px = 0.0;
    double py = 0.0;
    if (registry->Get(px, "/NoclipProtocol/Player/X") && registry->Get(py, "/NoclipProtocol/Player/Y"))
    {
        m_player = QPointF(px, py);
    }
}
