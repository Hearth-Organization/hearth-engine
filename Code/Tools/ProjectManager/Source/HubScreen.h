/*
 * Hearth Engine Hub: the home screen of the Hearth Engine (replaces the Project Manager's project list).
 * Built on O3DE (Apache-2.0 OR MIT). Matches the approved mockups "Hub" and "Crash" (Nov 2026).
 *
 * The Hub's work (building, running, measuring memory, engine parts) is done by the Hearth Engine agent
 * (scripts/hearth/agent/hearth_agent.py), which the Hub starts if it isn't running and talks to over its
 * localhost API (127.0.0.1, token from ~/.config/hearth-engine/agent.json).
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <ScreenWidget.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QString>

#include <functional>
#endif

QT_FORWARD_DECLARE_CLASS(QFrame)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QNetworkAccessManager)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QTimer)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

namespace O3DE::ProjectManager
{
    class HubScreen : public ScreenWidget
    {
        Q_OBJECT

    public:
        explicit HubScreen(QWidget* parent = nullptr);
        ~HubScreen() override = default;

        ProjectManagerScreen GetScreenEnum() override;
        void NotifyCurrentScreen() override;

    private:
        using Callback = std::function<void(const QJsonObject&)>;

        void BuildUi();
        QWidget* BuildProjectCard();
        QWidget* BuildStats();
        QWidget* BuildParts();
        QWidget* BuildCrashCard();
        QWidget* BuildToastColumn();

        // Agent
        bool ReadAgentFile();
        void EnsureAgent();
        void StartAgent(const QString& projectPath);
        QString DefaultProjectPath() const;
        void Call(const QString& op, const QJsonObject& args, Callback done, int timeoutMs = 15000);

        // Updating the screen
        void Refresh();
        void Render();
        void RenderParts(const QJsonArray& parts);
        void LoadCover();
        void CheckLastExit();
        void CheckMemory();
        void CheckClaudeResults();
        void AddNotice(const QString& kind, const QString& title, const QString& body, const QString& primaryText,
                       std::function<void()> primary, const QString& secondaryText = {}, std::function<void()> secondary = {});
        void Toast(const QString& text, int ms = 5000);

        // Buttons
        void HandleOpenProject();
        void HandleNewProject();
        void HandleBuild();
        void HandlePlay();
        void HandleEditor(const QString& projectPath = {});
        void HandleSetPart(const QString& name, bool on);
        void AskClaude(const QString& title, const QString& details);

        QNetworkAccessManager* m_network = nullptr;
        QTimer* m_refreshTimer = nullptr;
        QString m_token;
        int m_port = 47830;
        QString m_agentProject;
        qint64 m_agentStartTried = 0;
        QJsonObject m_info;
        bool m_agentReachable = false;
        bool m_busyBuild = false;
        bool m_busyPlay = false;
        bool m_busyEditor = false;
        qint64 m_lastCrashShown = 0;
        bool m_lowMemoryShown = false;

        QLabel* m_engineLine = nullptr;
        QLabel* m_cover = nullptr;
        QLabel* m_buildChip = nullptr;
        QLabel* m_projectName = nullptr;
        QLabel* m_projectLine = nullptr;
        QPushButton* m_buildButton = nullptr;
        QPushButton* m_playButton = nullptr;
        QPushButton* m_editorButton = nullptr;
        QLabel* m_gameMemory = nullptr;
        QLabel* m_gameMemoryNote = nullptr;
        QLabel* m_editorMemory = nullptr;
        QLabel* m_editorMemoryNote = nullptr;
        QLabel* m_partsCount = nullptr;
        QVBoxLayout* m_partsLayout = nullptr;
        QPushButton* m_rebuildButton = nullptr;
        QFrame* m_crashCard = nullptr;
        QLabel* m_crashTitle = nullptr;
        QLabel* m_crashWhen = nullptr;
        QLabel* m_crashSafe = nullptr;
        QLabel* m_crashSummary = nullptr;
        QLabel* m_crashLog = nullptr;
        QPushButton* m_crashLogToggle = nullptr;
        QString m_crashLogPath;
        QVBoxLayout* m_noticeLayout = nullptr;
        QLabel* m_toast = nullptr;
        QTimer* m_toastTimer = nullptr;
    };
} // namespace O3DE::ProjectManager
