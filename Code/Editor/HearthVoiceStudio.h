/*
 * Hearth Engine Voice Studio (approved mockup "VoiceStudio", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 *
 * The Hearth Server's Voice Studio, baked into the editor: the project's voices (designed from a description or
 * cloned from a recording you have the rights to), its spoken lines grouped by scene, and for each line the mood,
 * speed and room; lines are made on the Hearth server (OpenAI-style speech API with the server's API key) and saved
 * in the project with subtitles and lip sync. The lines live in <project>/Voice/voice_lines.json.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QWidget>

#include <functional>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QListWidget)
QT_FORWARD_DECLARE_CLASS(QNetworkAccessManager)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QSlider)
QT_FORWARD_DECLARE_CLASS(QHBoxLayout)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

class HearthWaveform;

class HearthVoiceStudio : public QWidget
{
    Q_OBJECT

public:
    explicit HearthVoiceStudio(QWidget* parent = nullptr);
    static void RegisterViewClass();

private:
    // Server
    QString Server() const;
    QString ApiKey() const;
    void CheckServer();
    void Connect();
    void Request(const QString& method, const QString& path, const QByteArray& body, const QString& contentType,
                 std::function<void(int status, const QByteArray& data)> done, int timeoutMs = 120000);

    // Project data
    QString VoiceFolder() const;
    void Load();
    void Save();
    QJsonObject GetLine(int scene, int line) const;
    void UpdateLine(int scene, int line, const std::function<void(QJsonObject&)>& change);
    static QString LineHash(const QJsonObject& line);
    QString LineState(const QJsonObject& line) const;

    // Screen
    void RenderVoices();
    void RenderScenes();
    void RenderLines();
    void RenderDetails();
    void Select(int line);

    // Making lines
    void Generate(int scene, int line, int takes, std::function<void(bool)> done = {});
    void GenerateChanged();
    void Play(const QString& file);
    void NewVoice();
    void AddLine();
    void Toast(const QString& text);

    QNetworkAccessManager* m_network = nullptr;
    QJsonObject m_data;          //!< {"scenes": [{"name", "lines": [...]}]}
    QJsonArray m_voices;         //!< from the server
    int m_scene = 0;
    int m_line = -1;
    bool m_serverOk = false;

    QLabel* m_subtitle = nullptr;
    QLabel* m_serverChip = nullptr;
    QPushButton* m_generateChanged = nullptr;
    QVBoxLayout* m_voiceList = nullptr;
    QHBoxLayout* m_sceneTabs = nullptr;
    QListWidget* m_lines = nullptr;
    QWidget* m_details = nullptr;
    QLabel* m_detailTitle = nullptr;
    HearthWaveform* m_wave = nullptr;
    QHBoxLayout* m_moods = nullptr;
    QSlider* m_speed = nullptr;
    QLabel* m_speedValue = nullptr;
    QSlider* m_room = nullptr;
    QLabel* m_roomValue = nullptr;
    QLabel* m_subtitles = nullptr;
    QLabel* m_lipSync = nullptr;
    QLabel* m_usedIn = nullptr;
    QVBoxLayout* m_takes = nullptr;
    QLabel* m_toast = nullptr;
};
