/*
 * Hearth Engine first-run setup (approved mockup "FirstRun", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 *
 * Six steps: Welcome, Engine and folders, Your laptop, Voice Studio, Claude and logs, Ready. Everything chosen
 * is saved in ~/.config/hearth-engine/settings.ini and is only ever changed by the owner (Settings page or
 * running setup again).
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QDialog>
#include <QString>
#include <QVector>
#endif

QT_FORWARD_DECLARE_CLASS(QButtonGroup)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QStackedWidget)
QT_FORWARD_DECLARE_CLASS(QAbstractButton)

namespace O3DE::ProjectManager
{
    class FirstRunDialog : public QDialog
    {
        Q_OBJECT

    public:
        explicit FirstRunDialog(QWidget* parent = nullptr);

        //! True until setup has been finished or skipped once.
        static bool ShouldShow();

        struct Hardware
        {
            QString cpuName;
            int cores = 1;
            QString gpuName;
            QString gpuApi;
            double memoryGb = 0.0;
            double swapGb = 0.0;
            double freeDiskGb = 0.0;
            QString diskPath;
        };
        static Hardware DetectHardware();

    private:
        enum class Preset
        {
            Light,
            Balanced,
            Full
        };

        QWidget* BuildWelcome();
        QWidget* BuildFolders();
        QWidget* BuildLaptop();
        QWidget* BuildVoice();
        QWidget* BuildClaude();
        QWidget* BuildReady();
        QWidget* Page(const QString& title, const QString& description, QWidget* content);

        void GoTo(int step);
        void SelectPreset(Preset preset);
        int JobsFor(Preset preset) const;
        void Save(bool skipped);
        void UpdateReadySummary();

        Hardware m_hardware;
        Preset m_preset = Preset::Light;
        int m_step = 0;

        QStackedWidget* m_pages = nullptr;
        QVector<QLabel*> m_stepNumbers;
        QVector<QLabel*> m_stepNames;
        QVector<QWidget*> m_stepRows;
        QLabel* m_stepCounter = nullptr;
        QPushButton* m_back = nullptr;
        QPushButton* m_skip = nullptr;
        QPushButton* m_next = nullptr;

        QLineEdit* m_projectsFolder = nullptr;
        QVector<QWidget*> m_presetCards;
        QLabel* m_jobsLabel = nullptr;
        QAbstractButton* m_pauseAssets = nullptr;
        QLineEdit* m_voiceServer = nullptr;
        QLabel* m_voiceStatus = nullptr;
        QAbstractButton* m_claudeHelp = nullptr;
        QLabel* m_readySummary = nullptr;
    };
} // namespace O3DE::ProjectManager
