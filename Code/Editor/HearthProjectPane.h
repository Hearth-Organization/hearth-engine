/*
 * Hearth Engine Project settings and engine parts (approved mockup "Project settings", Nov 2026).
 * Built on O3DE (Apache-2.0 OR MIT). The game's name, engine, first level, players, icon and repo, and which engine
 * parts it uses (through the Hearth Engine agent, which keeps project.json and knows what was last built).
 * Rebuild on GitHub builds the game's code on hearth-32 and brings it home.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QJsonObject>
#include <QWidget>
#include <memory>
#endif

QT_FORWARD_DECLARE_CLASS(QComboBox)
QT_FORWARD_DECLARE_CLASS(QGridLayout)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QTimer)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

class HearthAgentClient;

class HearthProjectPane : public QWidget
{
    Q_OBJECT

public:
    explicit HearthProjectPane(QWidget* parent = nullptr);
    ~HearthProjectPane() override;
    static void RegisterViewClass();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void LoadGame();
    void LoadParts();
    void FillParts(const QJsonObject& answer);
    void Rebuild(bool commit);
    void WatchCloud();
    QJsonObject ProjectJson() const;
    void SaveProjectJson(const QJsonObject& json);
    void SetProjectSetting(const QString& key, const QJsonValue& value);
    QJsonObject ProjectSettings() const;

    std::unique_ptr<HearthAgentClient> m_agent;
    QLabel* m_subtitle = nullptr;
    QLabel* m_changedChip = nullptr;
    QPushButton* m_rebuild = nullptr;
    QLineEdit* m_name = nullptr;
    QLabel* m_engine = nullptr;
    QComboBox* m_firstLevel = nullptr;
    QComboBox* m_players = nullptr;
    QLabel* m_icon = nullptr;
    QPushButton* m_repo = nullptr;
    QLabel* m_partsSummary = nullptr;
    QGridLayout* m_partsGrid = nullptr;
    QVBoxLayout* m_allLayout = nullptr;
    QWidget* m_allBody = nullptr;
    QPushButton* m_allToggle = nullptr;
    QTimer* m_cloudTimer = nullptr;
};
