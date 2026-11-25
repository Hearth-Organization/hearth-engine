/*
 * Hearth Engine Script console (approved mockup "Python and script console", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * Python in the editor, what Claude uses to drive it: every command Claude and you ran with what it printed,
 * a >>> prompt, saved scripts with how often they're used, Pause Claude, and what Claude may do
 * (read the level always; change and save it; delete things asks first; build the game).
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QDateTime>
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QComboBox)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QScrollArea)
QT_FORWARD_DECLARE_CLASS(QTimer)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

class HearthScriptConsole : public QWidget
{
    Q_OBJECT

public:
    explicit HearthScriptConsole(QWidget* parent = nullptr);
    static void RegisterViewClass();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QString BridgeFolder() const;
    void Refresh(bool force = false);
    void FillScripts();
    void RunOwnerCode(const QString& code, const QString& scriptSlug = QString());
    void UpdatePause();

    QDateTime m_lastSeen;
    QStringList m_history;
    int m_historyAt = -1;

    QLabel* m_claudeState = nullptr;
    QPushButton* m_pause = nullptr;
    QScrollArea* m_scroll = nullptr;
    QVBoxLayout* m_entries = nullptr;
    QLineEdit* m_prompt = nullptr;
    QVBoxLayout* m_scriptsLayout = nullptr;
    QTimer* m_timer = nullptr;
};
