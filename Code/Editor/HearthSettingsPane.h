/*
 * Hearth Engine Settings (approved mockup "Settings", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * Pages: Performance, Editor, Viewport and play, Controls and keys, Claude, Logs, Engine parts, Updates, About.
 * Saved as you change them (in ~/.config/hearth-engine/settings.ini, and the editor's own settings for backups).
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QListWidget)
QT_FORWARD_DECLARE_CLASS(QStackedWidget)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

class HearthSettingsPane : public QWidget
{
    Q_OBJECT

public:
    explicit HearthSettingsPane(QWidget* parent = nullptr);
    static void RegisterViewClass();

private:
    QWidget* Page(const QString& title, const QString& hint);
    QWidget* BuildPerformance();
    QWidget* BuildEditor();
    QWidget* BuildViewport();
    QWidget* BuildKeys();
    QWidget* BuildClaude();
    QWidget* BuildLogs();
    QWidget* BuildParts();
    QWidget* BuildUpdates();
    QWidget* BuildAbout();
    QWidget* EngineCard();
    QWidget* ClaudeCard();

    QListWidget* m_nav = nullptr;
    QStackedWidget* m_pages = nullptr;
};
