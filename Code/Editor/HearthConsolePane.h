/*
 * Hearth Engine "Console and logs" window (approved mockup "Console", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 *
 * Everything the engine says in one place: the editor live, plus the game, asset processing, build and Hub logs
 * from disk, earlier runs from ~/O3DE/HearthEngine/logs, filters, a console command line, and an error panel with
 * "Let Claude fix it" and "Copy for chat".
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <AzCore/Debug/TraceMessageBus.h>

#include <QMutex>
#include <QString>
#include <QVector>
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QListWidget)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QTimer)

class HearthConsolePane
    : public QWidget
    , private AZ::Debug::TraceMessageBus::Handler
{
    Q_OBJECT

public:
    explicit HearthConsolePane(QWidget* parent = nullptr);
    ~HearthConsolePane() override;

    static void RegisterViewClass();

    enum class Level
    {
        Info,
        Warning,
        Error
    };

    struct Line
    {
        QString time;
        Level level = Level::Info;
        QString source;
        QString message;
    };

private:
    // TraceMessageBus (any thread)
    bool OnPreError(const char* window, const char* fileName, int line, const char* func, const char* message) override;
    bool OnPreWarning(const char* window, const char* fileName, int line, const char* func, const char* message) override;
    bool OnPrintf(const char* window, const char* message) override;
    void Queue(Level level, const char* window, const char* message);

    void BuildUi();
    void LoadFiles();
    void LoadFile(const QString& path, const QString& source, int maxLines = 1500);
    void FlushQueue();
    void Rebuild();
    void ShowDetails(int row);
    void RunCommand();
    QString CurrentReport() const;

    QVector<Line> m_lines;
    QVector<Line> m_pending;
    QMutex m_pendingMutex;
    QVector<int> m_shown;

    QString m_source;          //!< empty = everything
    int m_levelFilter = -1;    //!< -1 all, else Level
    QListWidget* m_sources = nullptr;
    QListWidget* m_runs = nullptr;
    QListWidget* m_view = nullptr;
    QPushButton* m_allChip = nullptr;
    QPushButton* m_errorChip = nullptr;
    QPushButton* m_warningChip = nullptr;
    QPushButton* m_infoChip = nullptr;
    QLineEdit* m_filter = nullptr;
    QLineEdit* m_command = nullptr;
    QWidget* m_details = nullptr;
    QLabel* m_detailKind = nullptr;
    QLabel* m_detailTitle = nullptr;
    QLabel* m_detailText = nullptr;
    QLabel* m_detailRaw = nullptr;
    QLabel* m_detailClaude = nullptr;
    int m_selected = -1;
    QTimer* m_timer = nullptr;
};
