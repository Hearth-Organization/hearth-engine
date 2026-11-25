/*
 * Hearth Engine "Co-op test" window (approved mockup "Coop", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * Players (this computer as host, a second game window, other computers that joined), the join log, and the test
 * settings (level, seed, start together, fake lag, packet loss, monsters, second window). The agent runs the games.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QWidget>

#include <memory>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QSlider)
QT_FORWARD_DECLARE_CLASS(QTimer)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

class HearthAgentClient;
namespace AzQtComponents { namespace Hearth { class Toggle; } }

class HearthCoopPane : public QWidget
{
    Q_OBJECT

public:
    explicit HearthCoopPane(QWidget* parent = nullptr);
    ~HearthCoopPane() override;
    static void RegisterViewClass();

private:
    void Refresh();
    void Start();

    std::unique_ptr<HearthAgentClient> m_agent;
    QVBoxLayout* m_players = nullptr;
    QLabel* m_playerCount = nullptr;
    QLabel* m_log = nullptr;
    QLineEdit* m_seed = nullptr;
    QSlider* m_lag = nullptr;
    QSlider* m_loss = nullptr;
    QLabel* m_lagValue = nullptr;
    QLabel* m_lossValue = nullptr;
    AzQtComponents::Hearth::Toggle* m_together = nullptr;
    AzQtComponents::Hearth::Toggle* m_monsters = nullptr;
    AzQtComponents::Hearth::Toggle* m_secondWindow = nullptr;
    QPushButton* m_start = nullptr;
    QString m_invite;
    QTimer* m_timer = nullptr;
};
