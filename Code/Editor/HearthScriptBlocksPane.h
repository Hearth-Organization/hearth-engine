/*
 * Hearth Engine "Script Canvas" (approved mockup "Script Canvas", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * Scripts as plain blocks: When (the player enters an area, a light dies, an entity sees the player), a check
 * (does the player have an item, with Yes and No), and Do (play a sound or line, open/close/move, send to another
 * level, shift the rooms). The blocks are kept in a "Hearth Blocks" component on the script's entity, which the game
 * (Noclip Protocol) runs; while playing, the block that just ran lights up. "Describe it, Claude builds it" hands the
 * script and the description to Claude. The node-graph editor is still there as "Script Canvas graphs".
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <AzCore/Component/EntityId.h>
#include <AzCore/Debug/TraceMessageBus.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QComboBox)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)
QT_FORWARD_DECLARE_CLASS(QScrollArea)

class HearthScriptBlocksPane
    : public QWidget
    , private AZ::Debug::TraceMessageBus::Handler
{
    Q_OBJECT

public:
    explicit HearthScriptBlocksPane(QWidget* parent = nullptr);
    ~HearthScriptBlocksPane() override;
    static void RegisterViewClass();

private:
    // AZ::Debug::TraceMessageBus: "HearthBlocks: ran <script> <block>" from the game while it plays
    bool OnPrintf(const char* window, const char* message) override;

    void ListScripts();
    void LoadScript(AZ::EntityId entity);
    void SaveScript();
    void NewScript();
    void Rebuild();
    QWidget* BuildBranch(const QJsonArray& ids, QWidget* parent, int depth);
    QWidget* BuildCard(const QJsonObject& block, QWidget* parent);
    void AddBlock(const QString& kind, int after, bool onNo = false);
    void EditBlock(int id);
    void RemoveBlock(int id);
    QJsonObject Block(int id) const;
    void SetBlock(const QJsonObject& block);
    int NextId() const;
    QStringList EntityNames(const char* componentName = nullptr) const;
    QString EnsureLineEntity(const QJsonObject& line);
    void Ran(const QString& script, int id);

    AZ::EntityId m_entity;
    QJsonObject m_graph;      //!< { "blocks": [ { id, kind, target, ... next: [], no: [] } ] }
    QString m_name;
    int m_selected = -1;
    int m_lit = -1;

    QLabel* m_subtitle = nullptr;
    QComboBox* m_scripts = nullptr;
    QScrollArea* m_scroll = nullptr;
    QLabel* m_running = nullptr;
};
