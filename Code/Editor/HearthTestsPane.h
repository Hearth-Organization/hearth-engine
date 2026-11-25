/*
 * Hearth Engine "Tests" (approved mockup "Editor", bottom dock next to Console). Built on O3DE (Apache-2.0 OR MIT).
 * Runs the level's layout tests through the Hearth Engine agent (the game's own generator code, many seeds) and
 * shows each check in words: same layout every time, every room reachable, how far you can see, how full chunks are.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QJsonObject>
#include <QWidget>
#endif

#include <memory>

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)
QT_FORWARD_DECLARE_CLASS(QComboBox)

class HearthAgentClient;

class HearthTestsPane : public QWidget
{
    Q_OBJECT

public:
    explicit HearthTestsPane(QWidget* parent = nullptr);
    ~HearthTestsPane() override;
    static void RegisterViewClass();

private:
    void Run();
    void ShowResult(const QJsonObject& data, qint64 tookMs);
    void ShowHistory();
    void AddCheck(const QString& tone, const QString& title, const QString& detail);

    std::unique_ptr<HearthAgentClient> m_agent;
    QLabel* m_summary = nullptr;
    QComboBox* m_seeds = nullptr;
    QPushButton* m_run = nullptr;
    QWidget* m_checks = nullptr;
    QVBoxLayout* m_checksLayout = nullptr;
    QLabel* m_history = nullptr;
    QJsonObject m_lastFailed;
};
