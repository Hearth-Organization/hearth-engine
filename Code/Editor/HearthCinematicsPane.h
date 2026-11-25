/*
 * Hearth Engine "Cinematics" (approved mockup "Cinematics", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * A cutscene as four tracks (Cameras, Voices, Sound, Lights) on one timeline, a preview with the line being said,
 * and the picked shot's camera, move, lens, length and line. Voice lines come from Voice Studio and a shot with a
 * line is as long as the line. Everything is kept in <project>/Cinematics/<name>.json and written into a real
 * Track View sequence (camera shots as camera keys, the rest as hearth_cine console keys), so it plays in the game.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QComboBox)
QT_FORWARD_DECLARE_CLASS(QDoubleSpinBox)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QTimer)

class HearthCineTimeline;
class HearthCinePreview;

class HearthCinematicsPane : public QWidget
{
    Q_OBJECT

public:
    explicit HearthCinematicsPane(QWidget* parent = nullptr);
    ~HearthCinematicsPane() override;
    static void RegisterViewClass();

    static const char* const TrackNames[4]; //!< Cameras, Voices, Sound, Lights

protected:
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QString Folder() const;
    void ListCinematics();
    void Load(const QString& name);
    void Save();
    void NewCinematic();
    double Length() const;
    void FollowVoiceLines();   //!< voice clips (and the shots that say them) take the length of their audio
    void SyncTrackView();      //!< writes the Track View sequence from the JSON
    void AddCameraShot();
    void AddClip(int track, double seconds);
    void WatchFullScreen();
    void StopFullScreen();
    void SetTime(double seconds, bool fromPlayback = false);
    void Tick();
    void Pick(int track, int clip);
    void ShowPicked();
    void ChangePicked(const QString& key, const QJsonValue& value);
    void Capture();
    QJsonArray VoiceLines() const;   //!< every line in Voice/voice_lines.json, with "scene"
    void PlayAudio(const QString& path);

    QJsonObject m_data;
    QString m_name;
    double m_time = 0.0;
    bool m_playing = false;
    bool m_fullScreen = false;
    QByteArray m_windowState;
    int m_track = 0;
    int m_clip = -1;
    QTimer* m_clock = nullptr;
    qint64 m_lastTick = 0;
    bool m_capturing = false;

    QLabel* m_subtitle = nullptr;
    QComboBox* m_pick = nullptr;
    HearthCinePreview* m_preview = nullptr;
    HearthCineTimeline* m_timeline = nullptr;
    QPushButton* m_play = nullptr;
    QLabel* m_timeText = nullptr;
    QLabel* m_shotTitle = nullptr;
    QComboBox* m_camera = nullptr;
    QLineEdit* m_move = nullptr;
    QDoubleSpinBox* m_lens = nullptr;
    QDoubleSpinBox* m_length = nullptr;
    QComboBox* m_line = nullptr;
    QLabel* m_note = nullptr;
    QWidget* m_shotCard = nullptr;
    QLabel* m_status = nullptr;
};
