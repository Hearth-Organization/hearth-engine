/*
 * Hearth Engine "Claude" panel (approved mockup "Editor", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 *
 * Shows whether Claude is connected to the editor (through the project's Claude bridge), the conversation about
 * this project (JSON files in <project>/user/ClaudeBridge/chat, written by the owner here and by Claude), what Claude did
 * lately, and a box to describe a change. Sending also opens Claude with the message, so it reaches a session.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QString>
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QTimer)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

class HearthClaudePane : public QWidget
{
    Q_OBJECT

public:
    explicit HearthClaudePane(QWidget* parent = nullptr);
    static void RegisterViewClass();

private:
    void Refresh();
    void Send();
    QString BridgeFolder() const;

    QLabel* m_status = nullptr;
    QVBoxLayout* m_messages = nullptr;
    QLabel* m_recent = nullptr;
    QWidget* m_abilities = nullptr;
    QLineEdit* m_input = nullptr;
    QTimer* m_timer = nullptr;
    QString m_lastSignature;
};
