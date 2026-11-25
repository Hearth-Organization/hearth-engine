/*
 * Hearth Engine "Monster pathing" (approved mockup "Navigation", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * Where entities can walk in the endless level and how they hunt: a top view of the rooms the game's own generator
 * builds (walk area, walls), each monster's hearing ring and sight, and the path it takes. "Test with a monster"
 * runs the hunt here: drag the player around, footsteps are heard within the hearing distance, a monster that sees
 * or hears you walks the shortest path to you. Monsters and their senses are kept in <project>/Monsters/monsters.json
 * (read by the game's monster code). While the game plays, monsters it reports under /NoclipProtocol/Monsters show live.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QSet>
#include <QVector>
#include <QWidget>

#include <memory>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)
QT_FORWARD_DECLARE_CLASS(QTimer)

class HearthAgentClient;
class HearthPathingView;

class HearthPathingPane : public QWidget
{
    Q_OBJECT

public:
    explicit HearthPathingPane(QWidget* parent = nullptr);
    ~HearthPathingPane() override;
    static void RegisterViewClass();

    struct Monster
    {
        QString name;
        QPointF at;              //!< metres
        double hearing = 7.0;    //!< metres
        double sight = 12.0;     //!< metres
        double speed = 2.5;      //!< m/s while hunting
        QString state = QStringLiteral("Wandering");
        QVector<QPoint> path;    //!< cells
        QPointF heardAt;
        double heardTime = -100.0;
        QPoint wanderTarget = QPoint(INT_MIN, INT_MIN);
        bool live = false;       //!< position comes from the running game
    };

    // Used by the view.
    bool Open(int cellX, int cellY, int dx, int dy) const;   //!< can you step from a cell to its neighbour?
    bool LineOfSight(const QPointF& a, const QPointF& b) const;
    QVector<QPoint> FindPath(const QPoint& from, const QPoint& to) const;
    QPoint Cell(const QPointF& metres) const;
    QPointF Centre(const QPoint& cell) const;

    QJsonObject m_area;
    double m_cell = 4.0;
    QRect m_cells;              //!< in cells
    QSet<qint64> m_blocked;     //!< wall segments, see EdgeKey
    QVector<Monster> m_monsters;
    QPointF m_player;
    QString m_playerName;
    bool m_layers[4] = { true, true, true, true }; //!< Walk area, Paths, Hearing, Sight
    bool m_testing = false;
    double m_time = 0.0;

private:
    static qint64 EdgeKey(int vertical, int line, int cell);
    void Rebuild();
    void LoadMonsters();
    void SaveMonsters();
    void ShowMonsters();
    void Step(double seconds);
    void ReadLive();

    std::unique_ptr<HearthAgentClient> m_agent;
    HearthPathingView* m_view = nullptr;
    QLabel* m_rooms = nullptr;
    QLabel* m_built = nullptr;
    QVBoxLayout* m_list = nullptr;
    QPushButton* m_test = nullptr;
    QTimer* m_clock = nullptr;
    QPointF m_lastStep;
    QLabel* m_status = nullptr;
};
