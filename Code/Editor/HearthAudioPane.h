/*
 * Hearth Engine Audio and mixing (approved mockup "Audio and mixing", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * Every sound in the level and how loud it plays: a mixer (Master, Ambience, Lights, Player, Entities, Music), the
 * sounds with their bus, volume and whether they're playing, waiting or silent, and for a picked sound its length,
 * where it plays from, 3D position and how often it was asked to play against how often it was heard.
 * The MiniAudio gem reports this (hearth_audioStats) and applies the buses (/Hearth/Audio in the settings registry).
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QJsonArray>
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QListWidget)
QT_FORWARD_DECLARE_CLASS(QProgressBar)
QT_FORWARD_DECLARE_CLASS(QSlider)
QT_FORWARD_DECLARE_CLASS(QTimer)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

class HearthAudioPane : public QWidget
{
    Q_OBJECT

public:
    explicit HearthAudioPane(QWidget* parent = nullptr);
    static void RegisterViewClass();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct Group
    {
        QString name;      //!< "step_carpet_1 to 4"
        QStringList stems; //!< step_carpet_1, step_carpet_2, ...
        QJsonArray sounds;
    };

    void Refresh();
    void FillSounds();
    void ShowSound(int index);
    void SetBus(const QString& bus, double decibels);
    void Assign(const QStringList& stems, const QString& bus);
    void WriteAudioRegistry(const QStringList& keys, const QJsonValue& value);

    QJsonArray m_sounds;
    QVector<Group> m_groups;
    int m_picked = 0;
    QString m_signature;

    QLabel* m_state = nullptr;
    QLabel* m_subtitle = nullptr;
    struct BusWidgets
    {
        QString name;
        QProgressBar* meter = nullptr;
        QSlider* fader = nullptr;
        QLabel* db = nullptr;
    };
    QVector<BusWidgets> m_buses;
    QListWidget* m_list = nullptr;
    QWidget* m_detail = nullptr;
    QVBoxLayout* m_detailLayout = nullptr;
    QTimer* m_timer = nullptr;
};
