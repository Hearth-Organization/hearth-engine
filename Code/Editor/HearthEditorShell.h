/*
 * Hearth Engine editor shell (approved mockups "Editor" and "Menus", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 *
 * Puts the Hearth header around the editor's menu bar (flame tile, project and level, Ctrl+K search, build chip,
 * Build, and the Play split button), opens the command palette, and applies the laptop settings chosen in the
 * Hub's first-run setup (viewport scale, shadows).
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QMainWindow)
QT_FORWARD_DECLARE_CLASS(QNetworkAccessManager)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QTimer)

class HearthCommandPalette;
class HearthPlaytestOverlay;

class HearthEditorShell : public QObject
{
    Q_OBJECT

public:
    //! Call once, after the main window's menus exist.
    static void Install(QMainWindow* mainWindow);
    static HearthEditorShell* Instance();

    void OpenCommandPalette(const QString& prefix = {});
    //! Starts the agent's build of the game (the editor closes for it and reopens after).
    void RequestBuild();
    void PlayInViewport();
    void PlayFullScreen();
    void StartStandaloneGame();
    void OpenCoopTest();
    bool TriggerAction(const char* identifier);

private:
    explicit HearthEditorShell(QMainWindow* mainWindow);

    void BuildHeader();
    void ApplyLaptopSettings();
    void UpdateTitle();
    void UpdateDockTitles(); //!< mockup "Editor": "Scene  7", "Console" as the dock headers
    void UpdateBuildChip();
    void WatchFullScreen();
    void PostToAgent(const QString& op, const QByteArray& json, std::function<void(const QByteArray&)> done = {});

    QMainWindow* m_mainWindow = nullptr;
    QLabel* m_projectLabel = nullptr;
    QLabel* m_levelLabel = nullptr;
    QLabel* m_buildChip = nullptr;
    QPushButton* m_buildButton = nullptr;
    QPushButton* m_playButton = nullptr;
    QTimer* m_timer = nullptr;
    QNetworkAccessManager* m_network = nullptr;
    QPointer<HearthCommandPalette> m_palette;
    bool m_fullScreenPlay = false;
    bool m_wasMaximized = false;
    bool m_sawGameMode = false;
    bool m_lastInGame = false;
    QPointer<HearthPlaytestOverlay> m_playtest;
    QString m_agentToken;
    int m_agentPort = 47830;
};
