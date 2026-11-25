/*
 * Hearth Engine Asset processing (approved mockup "Asset processing", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * What the Asset Processor is doing: how many at a time, done / left to do (and about how long) / failed / memory,
 * the queue (all, working, waiting), and each failed file: what stopped, how often it was tried, what needs it,
 * Let Claude fix it, its log, Try again. Pause and Process changed files now go to the Asset Processor through
 * <project>/user/AssetProcessor/hearth-control.json (it writes hearth-status.json every second).
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <AzCore/base.h>
#include <QJsonObject>
#include <QVector>
#include <QWidget>
#include <memory>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QListWidget)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QTimer)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

namespace AzToolsFramework
{
    namespace AssetDatabase
    {
        class AssetDatabaseConnection;
    }
}

struct HearthFailedAsset
{
    QString path;      //!< absolute source path
    QString relative;  //!< as the Asset Processor names it
    QString kind;
    AZ::u64 jobRunKey = 0;
    qint64 sourceId = -1;
    QString logFile;
};

class HearthProcessingPane : public QWidget
{
    Q_OBJECT

public:
    explicit HearthProcessingPane(QWidget* parent = nullptr);
    ~HearthProcessingPane() override;
    static void RegisterViewClass();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void Refresh();
    void RefreshDatabase();
    void FillQueue();
    void FillFailed();
    void Send(const QJsonObject& request);
    QString JobLog(const HearthFailedAsset& failed) const;
    QStringList NeededBy(const HearthFailedAsset& failed) const;

    std::unique_ptr<AzToolsFramework::AssetDatabase::AssetDatabaseConnection> m_database;
    QJsonObject m_status;
    bool m_running = false;
    int m_done = 0;
    QVector<HearthFailedAsset> m_failed;
    int m_failedPick = 0;
    QString m_queueFilter; //!< "", "working", "waiting"
    QVector<QPair<qint64, int>> m_leftHistory; //!< (time, left to do) for the time estimate
    int m_ticks = 0;

    QTimer* m_timer = nullptr;
    QLabel* m_state = nullptr;
    QPushButton* m_pause = nullptr;
    QLabel* m_doneValue = nullptr;
    QLabel* m_leftValue = nullptr;
    QLabel* m_leftHint = nullptr;
    QLabel* m_failedValue = nullptr;
    QLabel* m_memoryValue = nullptr;
    QLabel* m_memoryHint = nullptr;
    QList<QPushButton*> m_queueChips;
    QListWidget* m_queue = nullptr;
    QWidget* m_failedCard = nullptr;
    QVBoxLayout* m_failedLayout = nullptr;
};
