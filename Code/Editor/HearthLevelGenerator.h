/*
 * Hearth Engine Level Generator window (approved mockup "LevelGen", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 *
 * Live top-down map of the generated level around the spawn, seed, layout and horror settings, layout stats,
 * Run layout tests (50 seeds), Walk it in Play, Apply to level. The work is done by the Hearth Engine agent's
 * levelgen_* operations (the project's own generator), so this window works for any project that has them.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QJsonObject>
#include <QMap>
#include <QWidget>

#include <memory>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QSlider)
QT_FORWARD_DECLARE_CLASS(QTimer)

class HearthAgentClient;
class HearthLevelMap;
namespace AzQtComponents { namespace Hearth { class Toggle; } }

class HearthLevelGenerator : public QWidget
{
    Q_OBJECT

public:
    explicit HearthLevelGenerator(QWidget* parent = nullptr);
    ~HearthLevelGenerator() override;
    static void RegisterViewClass();

private:
    void Load();
    void Show();
    void Changed();
    void Regenerate();
    void ShowStats(const QJsonObject& tests, const QString& note);
    void Toast(const QString& text);

    std::unique_ptr<HearthAgentClient> m_agent;
    QJsonObject m_values;
    QJsonObject m_saved;
    int m_pending = 0;

    QLabel* m_levelLine = nullptr;
    HearthLevelMap* m_map = nullptr;
    QLabel* m_mapNote = nullptr;
    QLineEdit* m_seed = nullptr;
    QMap<QString, QSlider*> m_sliders;
    QMap<QString, QLabel*> m_sliderValues;
    QMap<QString, AzQtComponents::Hearth::Toggle*> m_switches;
    QPushButton* m_tests = nullptr;
    QPushButton* m_play = nullptr;
    QPushButton* m_apply = nullptr;
    QLabel* m_stat[4] = {};
    QLabel* m_statNote[4] = {};
    QLabel* m_toast = nullptr;
    QTimer* m_debounce = nullptr;
};
