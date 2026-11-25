/*
 * Hearth Engine Package the game (approved mockup "Package the game", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * Where it's for (Hearth OS / Steam (Linux) / test build), what goes in (levels, first level, the owner's version,
 * leave out editor-only files, build on GitHub), checks before packaging, an estimate and the last package.
 * The Hearth Engine agent does the packaging (on GitHub with hearth-package.yml, or on the laptop).
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QWidget>
#include <memory>
#endif

QT_FORWARD_DECLARE_CLASS(QComboBox)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QListWidget)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QTimer)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

class HearthAgentClient;

namespace AzQtComponents
{
    namespace Hearth
    {
        class Toggle;
    }
}

class HearthPackagePane : public QWidget
{
    Q_OBJECT

public:
    explicit HearthPackagePane(QWidget* parent = nullptr);
    ~HearthPackagePane() override;
    static void RegisterViewClass();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void LoadLevels();
    void RunChecks();
    void Package();
    void Watch();
    QStringList PickedLevels() const;

    std::unique_ptr<HearthAgentClient> m_agent;
    QString m_target = QStringLiteral("hearthos");
    QList<QPushButton*> m_targets;
    QListWidget* m_levels = nullptr;
    QComboBox* m_startsIn = nullptr;
    QLineEdit* m_version = nullptr;
    AzQtComponents::Hearth::Toggle* m_leaveOut = nullptr;
    AzQtComponents::Hearth::Toggle* m_cloud = nullptr;
    QPushButton* m_package = nullptr;
    QLabel* m_status = nullptr;
    QVBoxLayout* m_checksLayout = nullptr;
    QLabel* m_size = nullptr;
    QLabel* m_minutes = nullptr;
    QLabel* m_cost = nullptr;
    QLabel* m_last = nullptr;
    QTimer* m_watch = nullptr;
};
