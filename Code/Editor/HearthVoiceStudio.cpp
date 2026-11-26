/*
 * Hearth Engine Voice Studio. Built on O3DE (Apache-2.0 OR MIT). See HearthVoiceStudio.h.
 */

#include "EditorDefs.h"

#include "HearthVoiceStudio.h"

#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>

#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QUrlQuery>
#include <QRegularExpression>
#include <QVBoxLayout>

#include <cmath>
#include <cstring>
#include <memory>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    const char* Moods[] = { "Calm", "Urgent", "Whisper", "Over radio" };
    const char* MoodInstructions[] = { "calm, steady, clear", "urgent, fast, tense", "whispering, quiet, close", "speaking over a radio, clipped" };
    const char* Rooms[] = { "Dry", "Small room", "Briefing room", "Hall", "Huge space" };
    const char* RoomFilters[] = { "", "aecho=0.8:0.6:20:0.25", "aecho=0.8:0.7:40|70:0.3|0.2", "aecho=0.8:0.8:80|140:0.35|0.25",
                                  "aecho=0.8:0.9:200|350:0.4|0.3" };

    const char* VoiceSliderStyle =
        "QSlider::groove:horizontal { height: 4px; background: #2C2824; border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: #E8743B; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: #FFCFB4; border: 3px solid #E8743B; width: 10px; height: 10px; margin: -6px 0; border-radius: 8px; }";

    //! Mono 16-bit samples of a PCM WAV file (first channel), and its rate.
    QVector<qint16> ReadWav(const QString& path, int* rate)
    {
        QVector<qint16> samples;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return samples;
        }
        const QByteArray data = file.readAll();
        if (data.size() < 44 || !data.startsWith("RIFF"))
        {
            return samples;
        }
        int channels = 1;
        int bits = 16;
        int pos = 12;
        while (pos + 8 <= data.size())
        {
            const QByteArray id = data.mid(pos, 4);
            quint32 size = 0;
            std::memcpy(&size, data.constData() + pos + 4, 4);
            if (id == "fmt " && pos + 24 <= data.size())
            {
                quint16 ch = 0;
                quint32 sr = 0;
                quint16 bps = 0;
                std::memcpy(&ch, data.constData() + pos + 10, 2);
                std::memcpy(&sr, data.constData() + pos + 12, 4);
                std::memcpy(&bps, data.constData() + pos + 22, 2);
                channels = qMax<int>(1, ch);
                *rate = static_cast<int>(sr);
                bits = bps;
            }
            else if (id == "data")
            {
                if (bits != 16)
                {
                    return samples;
                }
                const int count = static_cast<int>(qMin<qint64>(size, data.size() - pos - 8) / 2 / channels);
                samples.resize(count);
                for (int i = 0; i < count; ++i)
                {
                    std::memcpy(&samples[i], data.constData() + pos + 8 + i * 2 * channels, 2);
                }
                return samples;
            }
            pos += 8 + static_cast<int>(size) + (size & 1);
        }
        return samples;
    }

    QString SrtTime(double seconds)
    {
        const int ms = qRound(seconds * 1000.0);
        return QStringLiteral("%1:%2:%3,%4")
            .arg(ms / 3600000, 2, 10, QLatin1Char('0'))
            .arg((ms / 60000) % 60, 2, 10, QLatin1Char('0'))
            .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
            .arg(ms % 1000, 3, 10, QLatin1Char('0'));
    }

    //! Subtitles: the line split into sentences, each shown for its share of the audio.
    void WriteSubtitles(const QString& path, const QString& text, double seconds)
    {
        QStringList parts = text.split(QRegularExpression(QStringLiteral("(?<=[.!?])\\s+")), Qt::SkipEmptyParts);
        if (parts.isEmpty())
        {
            parts << text;
        }
        int letters = 0;
        for (const QString& part : parts)
        {
            letters += part.size();
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            return;
        }
        double at = 0.0;
        for (int i = 0; i < parts.size(); ++i)
        {
            const double length = seconds * parts[i].size() / qMax(1, letters);
            file.write(QStringLiteral("%1\n%2 --> %3\n%4\n\n").arg(i + 1).arg(SrtTime(at), SrtTime(at + length), parts[i]).toUtf8());
            at += length;
        }
    }

    //! Lip sync: how open the mouth is, 30 times a second, from the loudness of the voice.
    void WriteLipSync(const QString& path, const QVector<qint16>& samples, int rate)
    {
        const int step = qMax(1, rate / 30);
        QJsonArray mouth;
        double peak = 1.0;
        QVector<double> levels;
        for (int start = 0; start < samples.size(); start += step)
        {
            double sum = 0.0;
            const int end = qMin(samples.size(), start + step);
            for (int i = start; i < end; ++i)
            {
                sum += static_cast<double>(samples[i]) * samples[i];
            }
            const double rms = std::sqrt(sum / qMax(1, end - start));
            levels.push_back(rms);
            peak = qMax(peak, rms);
        }
        for (double level : levels)
        {
            mouth.append(qRound(qBound(0.0, level / peak * 1.3, 1.0) * 100) / 100.0);
        }
        QFile file(path);
        if (file.open(QIODevice::WriteOnly))
        {
            file.write(QJsonDocument(QJsonObject{ { QStringLiteral("fps"), 30 }, { QStringLiteral("mouth"), mouth } }).toJson(QJsonDocument::Compact));
        }
    }

    QPixmap MicTile(int size, qreal ratio)
    {
        QPixmap pixmap(QSize(size, size) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(Hearth::Ember());
        p.drawRoundedRect(QRectF(0, 0, size, size), size * 0.3, size * 0.3);
        const double u = size / 30.0;
        p.setBrush(Hearth::Ink());
        p.drawRoundedRect(QRectF(12 * u, 7 * u, 6 * u, 11 * u), 3 * u, 3 * u);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Hearth::Ink(), 1.8 * u, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(QRectF(9 * u, 10 * u, 12 * u, 11 * u), 180 * 16, 180 * 16);
        p.drawLine(QPointF(15 * u, 21 * u), QPointF(15 * u, 24 * u));
        return pixmap;
    }
} // namespace

//! The selected line's waveform.
class HearthWaveform : public QWidget
{
public:
    explicit HearthWaveform(QWidget* parent)
        : QWidget(parent)
    {
        setFixedHeight(64);
    }
    void SetFile(const QString& path)
    {
        int rate = 24000;
        m_samples = path.isEmpty() ? QVector<qint16>() : ReadWav(path, &rate);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(255, 255, 255, 26), 1));
        p.setBrush(QColor(0x10, 0x0F, 0x0E));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
        if (m_samples.isEmpty())
        {
            p.setPen(Hearth::Dim());
            p.drawText(rect(), Qt::AlignCenter, QObject::tr("Not made yet"));
            return;
        }
        const int bars = qMax(1, width() / 3);
        const int per = qMax(1, m_samples.size() / bars);
        p.setPen(QPen(QColor(0xEC, 0xE6, 0xDE, 200), 1));
        for (int b = 0; b < bars; ++b)
        {
            int peak = 0;
            for (int i = b * per; i < qMin(m_samples.size(), (b + 1) * per); ++i)
            {
                peak = qMax(peak, qAbs(static_cast<int>(m_samples[i])));
            }
            const double h = qMax(2.0, (height() - 12) * peak / 32768.0);
            const double x = b * 3 + 1.5;
            p.drawLine(QPointF(x, height() / 2.0 - h / 2), QPointF(x, height() / 2.0 + h / 2));
        }
    }

private:
    QVector<qint16> m_samples;
};

void HearthVoiceStudio::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(80, 60, 1400, 880);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthVoiceStudio>("Voice Studio", "Tools", options);
}

HearthVoiceStudio::HearthVoiceStudio(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthVoice"));
    setStyleSheet(QStringLiteral("QWidget#HearthVoice { background-color: #0D0C0B; }"));
    m_network = new QNetworkAccessManager(this);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    // Header (Voice Studio has its own mic tile).
    auto* header = new QHBoxLayout();
    header->setContentsMargins(8, 6, 8, 6);
    header->setSpacing(14);
    auto* tile = new QLabel(this);
    tile->setPixmap(MicTile(30, devicePixelRatioF()));
    tile->setFixedSize(30, 30);
    header->addWidget(tile);
    auto* titles = new QVBoxLayout();
    titles->setSpacing(0);
    titles->addWidget(Hearth::MakeLabel(tr("Voice Studio"), QStringLiteral("title"), this));
    m_subtitle = Hearth::MakeLabel(QString(), QStringLiteral("small"), this);
    titles->addWidget(m_subtitle);
    header->addLayout(titles);
    header->addStretch(1);
    m_serverChip = Hearth::MakeChip(tr("Checking the Hearth server…"), QStringLiteral("plain"), this);
    m_serverChip->setCursor(Qt::PointingHandCursor);
    header->addWidget(m_serverChip);
    QPushButton* connectButton = Hearth::MakeButton(tr("Server"), false, this);
    connect(connectButton, &QPushButton::clicked, this, &HearthVoiceStudio::Connect);
    header->addWidget(connectButton);
    QPushButton* newVoice = Hearth::MakeButton(tr("New voice"), false, this);
    connect(newVoice, &QPushButton::clicked, this, &HearthVoiceStudio::NewVoice);
    header->addWidget(newVoice);
    m_generateChanged = Hearth::MakeButton(tr("Generate changed lines"), true, this);
    connect(m_generateChanged, &QPushButton::clicked, this, &HearthVoiceStudio::GenerateChanged);
    header->addWidget(m_generateChanged);
    root->addLayout(header);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);

    // Voices
    QVBoxLayout* voices = nullptr;
    QFrame* voicesCard = Hearth::MakeCard(this, &voices);
    voicesCard->setFixedWidth(256);
    voices->addWidget(Hearth::MakeLabel(tr("Voices"), QStringLiteral("title"), voicesCard));
    m_voiceList = new QVBoxLayout();
    m_voiceList->setSpacing(6);
    voices->addLayout(m_voiceList);
    auto* line = new QFrame(voicesCard);
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background-color: rgba(255, 255, 255, 15); border: none;"));
    voices->addWidget(line);
    voices->addWidget(Hearth::MakeLabel(tr("Voices are designed from a description, or cloned from a recording you have the rights to."),
                                        QStringLiteral("dim"), voicesCard));
    voices->addStretch(1);
    body->addWidget(voicesCard);

    // Lines
    auto* middle = new QVBoxLayout();
    middle->setSpacing(10);
    m_sceneTabs = new QHBoxLayout();
    m_sceneTabs->setSpacing(6);
    middle->addLayout(m_sceneTabs);
    m_lines = new QListWidget(this);
    m_lines->setObjectName(QStringLiteral("VoiceLines"));
    m_lines->setStyleSheet(QStringLiteral(
        "QListWidget#VoiceLines { background-color: #171513; border: 1px solid rgba(255, 255, 255, 15); border-radius: 14px; padding: 8px; outline: none; }"
        "QListWidget#VoiceLines::item { border-radius: 10px; }"
        "QListWidget#VoiceLines::item:selected { background-color: rgba(232, 116, 59, 31); }"));
    connect(m_lines, &QListWidget::currentRowChanged, this, &HearthVoiceStudio::Select);
    connect(m_lines, &QListWidget::itemDoubleClicked, this, [this]()
    {
        if (m_line < 0)
        {
            return;
        }
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(this, tr("Edit line"), tr("What they say"),
                                                            GetLine(m_scene, m_line).value(QStringLiteral("text")).toString(), &ok);
        if (ok)
        {
            UpdateLine(m_scene, m_line, [text](QJsonObject& l) { l.insert(QStringLiteral("text"), text.trimmed()); });
            RenderLines();
        }
    });
    middle->addWidget(m_lines, 1);
    QPushButton* addLine = Hearth::MakeButton(tr("Add a line"), false, this);
    connect(addLine, &QPushButton::clicked, this, &HearthVoiceStudio::AddLine);
    middle->addWidget(addLine, 0, Qt::AlignLeft);
    body->addLayout(middle, 1);

    // Selected line
    QVBoxLayout* details = nullptr;
    QFrame* detailsCard = Hearth::MakeCard(this, &details);
    m_details = detailsCard;
    detailsCard->setFixedWidth(330);
    m_detailTitle = Hearth::MakeLabel(QString(), QStringLiteral("title"), detailsCard);
    m_detailTitle->setStyleSheet(QStringLiteral("background: transparent; border: none; color: #ECE6DE; font-size: 16px; font-weight: 600;"));
    details->addWidget(m_detailTitle);
    m_wave = new HearthWaveform(detailsCard);
    details->addWidget(m_wave);
    m_moods = new QHBoxLayout();
    m_moods->setSpacing(6);
    for (int i = 0; i < 4; ++i)
    {
        auto* chip = new QPushButton(tr(Moods[i]), detailsCard);
        chip->setCheckable(true);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setStyleSheet(QStringLiteral("QPushButton { border: 1px solid rgba(255, 255, 255, 26); border-radius: 13px; padding: 4px 10px; color: #ECE6DE; background: transparent; }"
                                           "QPushButton:checked { background-color: #ECE6DE; color: #1A0F08; }"));
        const QString mood = QString::fromLatin1(Moods[i]);
        connect(chip, &QPushButton::clicked, this, [this, mood]()
        {
            if (m_line >= 0)
            {
                UpdateLine(m_scene, m_line, [mood](QJsonObject& l) { l.insert(QStringLiteral("mood"), mood); });
                RenderLines();
                RenderDetails();
            }
        });
        m_moods->addWidget(chip);
    }
    details->addLayout(m_moods);
    auto sliderRow = [detailsCard, details](const QString& name, QSlider** slider, QLabel** value)
    {
        auto* top = new QHBoxLayout();
        top->addWidget(Hearth::MakeLabel(name, QStringLiteral("body"), detailsCard), 1);
        *value = new QLabel(detailsCard);
        (*value)->setStyleSheet(QStringLiteral("background-color: rgba(255, 255, 255, 13); color: #ECE6DE; border-radius: 6px; padding: 1px 7px;"
                                               " font-family: \"Geist Mono\"; font-size: 11px;"));
        top->addWidget(*value);
        details->addLayout(top);
        *slider = new QSlider(Qt::Horizontal, detailsCard);
        (*slider)->setStyleSheet(QLatin1String(VoiceSliderStyle));
        details->addWidget(*slider);
    };
    sliderRow(tr("Speed"), &m_speed, &m_speedValue);
    m_speed->setRange(50, 150);
    sliderRow(tr("Room"), &m_room, &m_roomValue);
    m_room->setRange(0, 4);
    connect(m_speed, &QSlider::valueChanged, this, [this](int v)
    {
        m_speedValue->setText(QStringLiteral("%1×").arg(v / 100.0, 0, 'f', 2));
        if (m_line >= 0)
        {
            UpdateLine(m_scene, m_line, [v](QJsonObject& l) { l.insert(QStringLiteral("speed"), v / 100.0); });
            RenderLines();
        }
    });
    connect(m_room, &QSlider::valueChanged, this, [this](int v)
    {
        m_roomValue->setText(tr(Rooms[qBound(0, v, 4)]));
        if (m_line >= 0)
        {
            UpdateLine(m_scene, m_line, [v](QJsonObject& l) { l.insert(QStringLiteral("room"), v); });
            RenderLines();
        }
    });
    auto infoRow = [detailsCard, details](const QString& name, QLabel** value)
    {
        auto* row = new QHBoxLayout();
        row->addWidget(Hearth::MakeLabel(name, QStringLiteral("dim"), detailsCard), 1);
        *value = Hearth::MakeLabel(QString(), QStringLiteral("body"), detailsCard);
        row->addWidget(*value);
        details->addLayout(row);
    };
    infoRow(tr("Subtitles"), &m_subtitles);
    infoRow(tr("Lip sync"), &m_lipSync);
    infoRow(tr("Used in"), &m_usedIn);
    m_takes = new QVBoxLayout();
    m_takes->setSpacing(4);
    details->addLayout(m_takes);
    details->addStretch(1);
    QPushButton* again = Hearth::MakeButton(tr("Generate again"), true, detailsCard);
    connect(again, &QPushButton::clicked, this, [this]()
    {
        if (m_line >= 0)
        {
            Generate(m_scene, m_line, 1);
        }
    });
    details->addWidget(again);
    QPushButton* takes = Hearth::MakeButton(tr("Keep 3 takes, pick the best"), false, detailsCard);
    connect(takes, &QPushButton::clicked, this, [this]()
    {
        if (m_line >= 0)
        {
            Generate(m_scene, m_line, 3);
        }
    });
    details->addWidget(takes);
    body->addWidget(detailsCard);
    root->addLayout(body, 1);

    m_toast = new QLabel(this);
    m_toast->setWordWrap(true);
    m_toast->setFixedWidth(560); // a set width so the layout gives a wrapped message its full height (it was cut in half)
    m_toast->setStyleSheet(QStringLiteral("background-color: #1D1A17; color: #ECE6DE; border: 1px solid rgba(255, 255, 255, 26); border-radius: 12px; padding: 10px 16px;"));
    m_toast->hide();
    root->addWidget(m_toast, 0, Qt::AlignHCenter);

    Load();
    CheckServer();
}

// ---------------------------------------------------------------- server

QString HearthVoiceStudio::Server() const
{
    QScopedPointer<QSettings> settings(Hearth::NewSettings());
    QString server = settings->value(QStringLiteral("voice/server")).toString().trimmed();
    if (server.isEmpty())
    {
        return {};
    }
    if (!server.contains(QLatin1String("://")))
    {
        server.prepend(QStringLiteral("http://"));
    }
    if (QUrl(server).port() < 0)
    {
        server += QStringLiteral(":8890"); // Voice Studio's port on the Hearth server
    }
    return server.endsWith(QLatin1Char('/')) ? server.chopped(1) : server;
}

QString HearthVoiceStudio::ApiKey() const
{
    QScopedPointer<QSettings> settings(Hearth::NewSettings());
    return settings->value(QStringLiteral("voice/apiKey")).toString().trimmed();
}

void HearthVoiceStudio::Request(const QString& method, const QString& path, const QByteArray& body, const QString& contentType,
                                std::function<void(int, const QByteArray&)> done, int timeoutMs)
{
    const QString server = Server();
    if (server.isEmpty())
    {
        done(0, QByteArrayLiteral("No Hearth server set: press Server"));
        return;
    }
    QNetworkRequest request(QUrl(server + path));
    if (!contentType.isEmpty())
    {
        request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    }
    const QString key = ApiKey();
    if (!key.isEmpty())
    {
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + key.toUtf8());
    }
    QNetworkReply* reply = method == QLatin1String("GET") ? m_network->get(request) : m_network->post(request, body);
    QPointer<QNetworkReply> guard(reply);
    QTimer::singleShot(timeoutMs, reply, [guard]()
    {
        if (guard && guard->isRunning())
        {
            guard->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [reply, done]()
    {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        done(status, status == 0 ? reply->errorString().toUtf8() : reply->readAll());
    });
}

void HearthVoiceStudio::CheckServer()
{
    Request(QStringLiteral("GET"), QStringLiteral("/health"), {}, {}, [this](int status, const QByteArray& data)
    {
        m_serverOk = status == 200;
        if (!m_serverOk)
        {
            m_serverChip->setText(Server().isEmpty() ? tr("Set up your Hearth server") : tr("Hearth server not answering"));
            m_serverChip->setStyleSheet(QStringLiteral("background-color: rgba(232, 190, 59, 36); color: #F0D17A; border-radius: 10px; padding: 3px 10px; font-size: 11px;"));
            return;
        }
        const QString gpu = QJsonDocument::fromJson(data).object().value(QStringLiteral("engines")).toObject().value(QStringLiteral("gpu")).toString();
        QString shortGpu = gpu;
        shortGpu.remove(QStringLiteral("NVIDIA ")).remove(QStringLiteral("GeForce "));
        m_serverChip->setText(QStringLiteral("● ") + (gpu.isEmpty() ? tr("Voice Studio on your Hearth server") : tr("Voice Studio on your Hearth server · %1").arg(shortGpu)));
        m_serverChip->setStyleSheet(QStringLiteral("background-color: rgba(127, 207, 106, 31); color: #A6DD97; border-radius: 10px; padding: 3px 10px; font-size: 11px;"));
        Request(QStringLiteral("GET"), QStringLiteral("/v1/audio/voices"), {}, {}, [this](int voicesStatus, const QByteArray& voices)
        {
            if (voicesStatus == 401)
            {
                Toast(tr("The Hearth server wants Voice Studio's API key: press Server and paste it (Settings › Home server › Voice Studio on the server)."));
            }
            m_voices = QJsonDocument::fromJson(voices).object().value(QStringLiteral("voices")).toArray();
            RenderVoices();
            RenderLines();
        });
    }, 8000);
}

void HearthVoiceStudio::Connect()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Hearth server"));
    auto* form = new QFormLayout(&dialog);
    auto* server = new QLineEdit(&dialog);
    auto* key = new QLineEdit(&dialog);
    {
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        server->setText(settings->value(QStringLiteral("voice/server")).toString());
        key->setText(settings->value(QStringLiteral("voice/apiKey")).toString());
    }
    server->setPlaceholderText(tr("like 192.168.1.40 or my-pc (Tailscale)"));
    key->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Server"), server);
    form->addRow(tr("API key"), key);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted)
    {
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        settings->setValue(QStringLiteral("voice/server"), server->text().trimmed());
        settings->setValue(QStringLiteral("voice/apiKey"), key->text().trimmed());
        settings->sync();
        CheckServer();
    }
}

// ---------------------------------------------------------------- project data

QString HearthVoiceStudio::VoiceFolder() const
{
    const auto project = AZ::Utils::GetProjectPath();
    return QDir(QString::fromUtf8(project.c_str())).filePath(QStringLiteral("Voice"));
}

void HearthVoiceStudio::Load()
{
    QFile file(QDir(VoiceFolder()).filePath(QStringLiteral("voice_lines.json")));
    if (file.open(QIODevice::ReadOnly))
    {
        m_data = QJsonDocument::fromJson(file.readAll()).object();
    }
    if (m_data.value(QStringLiteral("scenes")).toArray().isEmpty())
    {
        QJsonArray scenes;
        for (const char* name : { "Foundation intro", "PA announcements", "Player radio", "Level 0 whispers" })
        {
            scenes.append(QJsonObject{ { QStringLiteral("name"), QString::fromLatin1(name) }, { QStringLiteral("lines"), QJsonArray() } });
        }
        m_data.insert(QStringLiteral("scenes"), scenes);
    }
    RenderScenes();
    RenderLines();
}

void HearthVoiceStudio::Save()
{
    QDir().mkpath(VoiceFolder());
    QFile file(QDir(VoiceFolder()).filePath(QStringLiteral("voice_lines.json")));
    if (file.open(QIODevice::WriteOnly))
    {
        file.write(QJsonDocument(m_data).toJson(QJsonDocument::Indented));
    }
}

QJsonObject HearthVoiceStudio::GetLine(int scene, int line) const
{
    const QJsonArray scenes = m_data.value(QStringLiteral("scenes")).toArray();
    if (scene < 0 || scene >= scenes.size())
    {
        return {};
    }
    const QJsonArray lines = scenes[scene].toObject().value(QStringLiteral("lines")).toArray();
    return line >= 0 && line < lines.size() ? lines[line].toObject() : QJsonObject();
}

void HearthVoiceStudio::UpdateLine(int scene, int line, const std::function<void(QJsonObject&)>& change)
{
    QJsonArray scenes = m_data.value(QStringLiteral("scenes")).toArray();
    if (scene < 0 || scene >= scenes.size())
    {
        return;
    }
    QJsonObject sceneObject = scenes[scene].toObject();
    QJsonArray lines = sceneObject.value(QStringLiteral("lines")).toArray();
    if (line < 0 || line >= lines.size())
    {
        return;
    }
    QJsonObject object = lines[line].toObject();
    change(object);
    lines[line] = object;
    sceneObject.insert(QStringLiteral("lines"), lines);
    scenes[scene] = sceneObject;
    m_data.insert(QStringLiteral("scenes"), scenes);
    Save();
}

QString HearthVoiceStudio::LineHash(const QJsonObject& line)
{
    const QString key = QStringLiteral("%1|%2|%3|%4|%5")
                            .arg(line.value(QStringLiteral("text")).toString(), line.value(QStringLiteral("voice")).toString(),
                                 line.value(QStringLiteral("mood")).toString())
                            .arg(line.value(QStringLiteral("speed")).toDouble(1.0), 0, 'f', 2)
                            .arg(line.value(QStringLiteral("room")).toInt());
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
}

QString HearthVoiceStudio::LineState(const QJsonObject& line) const
{
    const QString made = line.value(QStringLiteral("made")).toObject().value(QStringLiteral("hash")).toString();
    if (made.isEmpty())
    {
        return tr("New");
    }
    return made == LineHash(line) ? tr("Made") : tr("Changed");
}

// ---------------------------------------------------------------- screen

void HearthVoiceStudio::RenderVoices()
{
    while (QLayoutItem* item = m_voiceList->takeAt(0))
    {
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
    static const QStringList colors = { QStringLiteral("#E8743B"), QStringLiteral("#8FB3E0"), QStringLiteral("#A6DD97"), QStringLiteral("#F0D17A"),
                                        QStringLiteral("#D9A0E8"), QStringLiteral("#ECE6DE") };
    int index = 0;
    for (const QJsonValue value : m_voices)
    {
        const QJsonObject voice = value.toObject();
        auto* row = new QFrame();
        row->setObjectName(QStringLiteral("VoiceRow"));
        row->setStyleSheet(QStringLiteral("QFrame#VoiceRow { background-color: rgba(255, 255, 255, 8); border-radius: 10px; }"));
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(8, 6, 8, 6);
        const QString name = voice.value(QStringLiteral("name")).toString();
        auto* avatar = new QLabel(name.left(1).toUpper(), row);
        avatar->setFixedSize(30, 30);
        avatar->setAlignment(Qt::AlignCenter);
        avatar->setStyleSheet(QStringLiteral("background-color: %1; color: #1A0F08; border-radius: 15px; font-weight: 600;").arg(colors[index++ % colors.size()]));
        layout->addWidget(avatar);
        auto* texts = new QVBoxLayout();
        texts->setSpacing(0);
        texts->addWidget(Hearth::MakeLabel(name, QStringLiteral("body"), row));
        const QString kind = voice.value(QStringLiteral("kind")).toString();
        texts->addWidget(Hearth::MakeLabel(kind == QLatin1String("designed") ? tr("Designed") : (kind == QLatin1String("cloned") ? tr("Cloned") : tr("Ready-made")),
                                           QStringLiteral("small"), row));
        layout->addLayout(texts, 1);
        m_voiceList->addWidget(row);
    }
    if (m_voices.isEmpty())
    {
        m_voiceList->addWidget(Hearth::MakeLabel(m_serverOk ? tr("No voices yet: press New voice.") : tr("Connect your Hearth server to see its voices."),
                                                 QStringLiteral("dim"), this));
    }
}

void HearthVoiceStudio::RenderScenes()
{
    while (QLayoutItem* item = m_sceneTabs->takeAt(0))
    {
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
    m_sceneTabs->addWidget(Hearth::MakeLabel(tr("Lines"), QStringLiteral("title"), this));
    const QJsonArray scenes = m_data.value(QStringLiteral("scenes")).toArray();
    for (int i = 0; i < scenes.size(); ++i)
    {
        auto* tab = new QPushButton(scenes[i].toObject().value(QStringLiteral("name")).toString(), this);
        tab->setCheckable(true);
        tab->setChecked(i == m_scene);
        tab->setCursor(Qt::PointingHandCursor);
        tab->setStyleSheet(QStringLiteral("QPushButton { border: 1px solid rgba(255, 255, 255, 26); border-radius: 14px; padding: 5px 12px; color: #ECE6DE; background: transparent; }"
                                          "QPushButton:checked { background-color: #ECE6DE; color: #1A0F08; }"));
        connect(tab, &QPushButton::clicked, this, [this, i]()
        {
            m_scene = i;
            m_line = -1;
            RenderScenes();
            RenderLines();
        });
        m_sceneTabs->addWidget(tab);
    }
    auto* add = new QPushButton(QStringLiteral("+"), this);
    add->setFixedSize(28, 28);
    add->setStyleSheet(QStringLiteral("QPushButton { border: 1px solid rgba(255, 255, 255, 26); border-radius: 14px; color: #ECE6DE; background: transparent; }"));
    connect(add, &QPushButton::clicked, this, [this]()
    {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New scene"), tr("Scene name"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (ok && !name.isEmpty())
        {
            QJsonArray scenes = m_data.value(QStringLiteral("scenes")).toArray();
            scenes.append(QJsonObject{ { QStringLiteral("name"), name }, { QStringLiteral("lines"), QJsonArray() } });
            m_data.insert(QStringLiteral("scenes"), scenes);
            m_scene = scenes.size() - 1;
            Save();
            RenderScenes();
            RenderLines();
        }
    });
    m_sceneTabs->addWidget(add);
    m_sceneTabs->addStretch(1);
}

void HearthVoiceStudio::RenderLines()
{
    const QJsonArray scenes = m_data.value(QStringLiteral("scenes")).toArray();
    int total = 0;
    int changed = 0;
    for (const QJsonValue scene : scenes)
    {
        for (const QJsonValue line : scene.toObject().value(QStringLiteral("lines")).toArray())
        {
            ++total;
            changed += LineState(line.toObject()) != tr("Made") ? 1 : 0;
        }
    }
    const auto projectName = AZ::Utils::GetProjectName();
    m_subtitle->setText(tr("%1 · %2 lines, %3 voices").arg(QString::fromUtf8(projectName.c_str())).arg(total).arg(m_voices.size()));
    m_generateChanged->setText(tr("Generate changed lines (%1)").arg(changed));
    m_generateChanged->setEnabled(changed > 0);

    const QSignalBlocker blocker(m_lines);
    m_lines->clear();
    const QJsonArray lines = m_scene < scenes.size() ? scenes[m_scene].toObject().value(QStringLiteral("lines")).toArray() : QJsonArray();
    for (int i = 0; i < lines.size(); ++i)
    {
        const QJsonObject line = lines[i].toObject();
        auto* item = new QListWidgetItem(m_lines);
        auto* row = new QWidget();
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(12, 8, 12, 8);
        layout->setSpacing(14);
        auto* play = new QPushButton(QStringLiteral("▶"), row);
        play->setFixedSize(30, 30);
        play->setCursor(Qt::PointingHandCursor);
        play->setStyleSheet(QStringLiteral("QPushButton { background-color: rgba(255, 255, 255, 15); color: #ECE6DE; border: none; border-radius: 15px; }"
                                           "QPushButton:hover { background-color: #E8743B; color: #1A0F08; }"));
        const QString file = line.value(QStringLiteral("made")).toObject().value(QStringLiteral("file")).toString();
        connect(play, &QPushButton::clicked, this, [this, file]() { Play(file); });
        layout->addWidget(play);
        auto* who = new QVBoxLayout();
        who->setSpacing(0);
        who->addWidget(Hearth::MakeLabel(line.value(QStringLiteral("who")).toString(), QStringLiteral("body"), row));
        who->addWidget(Hearth::MakeLabel(line.value(QStringLiteral("mood")).toString(QStringLiteral("Calm")), QStringLiteral("small"), row));
        auto* whoBox = new QWidget(row);
        whoBox->setFixedWidth(150);
        whoBox->setLayout(who);
        layout->addWidget(whoBox);
        QLabel* text = Hearth::MakeLabel(line.value(QStringLiteral("text")).toString(), QStringLiteral("body"), row);
        layout->addWidget(text, 1);
        const QString state = LineState(line);
        layout->addWidget(Hearth::MakeChip(state, state == tr("Made") ? QStringLiteral("good") : (state == tr("Changed") ? QStringLiteral("warn") : QStringLiteral("ember")), row));
        item->setSizeHint(QSize(0, qMax(54, row->sizeHint().height())));
        m_lines->setItemWidget(item, row);
    }
    if (m_line >= 0 && m_line < m_lines->count())
    {
        m_lines->setCurrentRow(m_line);
    }
    RenderDetails();
}

void HearthVoiceStudio::Select(int line)
{
    m_line = line;
    RenderDetails();
}

void HearthVoiceStudio::RenderDetails()
{
    const QJsonArray scenes = m_data.value(QStringLiteral("scenes")).toArray();
    const QJsonArray lines = m_scene < scenes.size() ? scenes[m_scene].toObject().value(QStringLiteral("lines")).toArray() : QJsonArray();
    const bool has = m_line >= 0 && m_line < lines.size();
    m_details->setEnabled(has);
    if (!has)
    {
        m_detailTitle->setText(tr("Pick a line"));
        m_wave->SetFile({});
        return;
    }
    const QJsonObject line = lines[m_line].toObject();
    m_detailTitle->setText(tr("%1, line %2").arg(line.value(QStringLiteral("who")).toString()).arg(m_line + 1));
    const QJsonObject made = line.value(QStringLiteral("made")).toObject();
    m_wave->SetFile(made.isEmpty() ? QString() : QDir(VoiceFolder()).filePath(made.value(QStringLiteral("file")).toString()));
    const QString mood = line.value(QStringLiteral("mood")).toString(QStringLiteral("Calm"));
    for (int i = 0; i < m_moods->count(); ++i)
    {
        if (auto* chip = qobject_cast<QPushButton*>(m_moods->itemAt(i)->widget()))
        {
            chip->setChecked(QString::fromLatin1(Moods[i]) == mood);
        }
    }
    {
        const QSignalBlocker a(m_speed);
        const QSignalBlocker b(m_room);
        m_speed->setValue(qRound(line.value(QStringLiteral("speed")).toDouble(1.0) * 100));
        m_room->setValue(line.value(QStringLiteral("room")).toInt(0));
    }
    m_speedValue->setText(QStringLiteral("%1×").arg(m_speed->value() / 100.0, 0, 'f', 2));
    m_roomValue->setText(tr(Rooms[qBound(0, m_room->value(), 4)]));
    const bool fresh = !made.isEmpty() && made.value(QStringLiteral("hash")).toString() == LineHash(line);
    auto state = [fresh](bool exists, QLabel* label)
    {
        label->setText(!exists ? QObject::tr("Not made") : (fresh ? QObject::tr("Made") : QObject::tr("Out of date")));
        label->setStyleSheet(exists && fresh ? QStringLiteral("background: transparent; color: #A6DD97;") : QStringLiteral("background: transparent; color: #F0D17A;"));
    };
    state(!made.value(QStringLiteral("subtitles")).toString().isEmpty(), m_subtitles);
    state(!made.value(QStringLiteral("lipsync")).toString().isEmpty(), m_lipSync);
    m_usedIn->setText(line.value(QStringLiteral("usedIn")).toString(tr("not used yet")));

    while (QLayoutItem* item = m_takes->takeAt(0))
    {
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
    const QJsonArray takes = made.value(QStringLiteral("takes")).toArray();
    for (int i = 0; i < takes.size(); ++i)
    {
        const QString take = takes[i].toString();
        auto* row = new QWidget(m_details);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        QPushButton* play = Hearth::MakeButton(tr("▶ Take %1").arg(i + 1), false, row);
        connect(play, &QPushButton::clicked, this, [this, take]() { Play(take); });
        QPushButton* pick = Hearth::MakeButton(take == made.value(QStringLiteral("file")).toString() ? tr("Picked") : tr("Use this"), false, row);
        connect(pick, &QPushButton::clicked, this, [this, take]()
        {
            UpdateLine(m_scene, m_line, [take](QJsonObject& l)
            {
                QJsonObject m = l.value(QStringLiteral("made")).toObject();
                m.insert(QStringLiteral("file"), take);
                l.insert(QStringLiteral("made"), m);
            });
            QTimer::singleShot(10, this, [this]() { RenderDetails(); });
        });
        layout->addWidget(play);
        layout->addWidget(pick);
        m_takes->addWidget(row);
    }
}

// ---------------------------------------------------------------- making lines

void HearthVoiceStudio::Generate(int scene, int line, int takes, std::function<void(bool)> done)
{
    const QJsonObject l = GetLine(scene, line);
    const QString text = l.value(QStringLiteral("text")).toString();
    if (text.isEmpty())
    {
        if (done)
        {
            done(false);
        }
        return;
    }
    const QString mood = l.value(QStringLiteral("mood")).toString(QStringLiteral("Calm"));
    int moodIndex = 0;
    for (int i = 0; i < 4; ++i)
    {
        moodIndex = mood == QLatin1String(Moods[i]) ? i : moodIndex;
    }
    const int room = qBound(0, l.value(QStringLiteral("room")).toInt(0), 4);
    const QString sceneName = m_data.value(QStringLiteral("scenes")).toArray()[scene].toObject().value(QStringLiteral("name")).toString();
    const QString sceneFolder = sceneName.toLower().replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    QDir(VoiceFolder()).mkpath(sceneFolder);
    QString id = l.value(QStringLiteral("id")).toString();
    if (id.isEmpty())
    {
        id = QUuid::createUuid().toString(QUuid::Id128).left(10);
        UpdateLine(scene, line, [id](QJsonObject& changed) { changed.insert(QStringLiteral("id"), id); });
    }
    const QJsonObject body{ { QStringLiteral("input"), text },
                            { QStringLiteral("voice"), l.value(QStringLiteral("voice")).toString() },
                            { QStringLiteral("speed"), l.value(QStringLiteral("speed")).toDouble(1.0) },
                            { QStringLiteral("response_format"), QStringLiteral("wav") },
                            { QStringLiteral("instructions"), QString::fromLatin1(MoodInstructions[moodIndex]) } };
    Toast(takes > 1 ? tr("Making %1 takes of \"%2\" on the Hearth server…").arg(takes).arg(text.left(40)) : tr("Making \"%1\" on the Hearth server…").arg(text.left(40)));

    auto results = std::make_shared<QStringList>();
    auto remaining = std::make_shared<int>(takes);
    for (int t = 0; t < takes; ++t)
    {
        const QString relative = takes > 1 ? QStringLiteral("%1/%2_take%3.wav").arg(sceneFolder, id).arg(t + 1) : QStringLiteral("%1/%2.wav").arg(sceneFolder, id);
        Request(QStringLiteral("POST"), QStringLiteral("/v1/audio/speech"), QJsonDocument(body).toJson(QJsonDocument::Compact), QStringLiteral("application/json"),
                [this, scene, line, relative, room, moodIndex, text, results, remaining, takes, done](int status, const QByteArray& data)
                {
                    if (status == 200 && data.startsWith("RIFF"))
                    {
                        const QString final = QDir(VoiceFolder()).filePath(relative);
                        QString filter = QString::fromLatin1(RoomFilters[room]);
                        if (moodIndex == 3)
                        {
                            filter = QStringLiteral("highpass=f=300,lowpass=f=3200") + (filter.isEmpty() ? QString() : QStringLiteral(",") + filter);
                        }
                        const QString raw = final + QStringLiteral(".raw.wav");
                        QFile out(filter.isEmpty() ? final : raw);
                        if (out.open(QIODevice::WriteOnly))
                        {
                            out.write(data);
                            out.close();
                        }
                        if (!filter.isEmpty())
                        {
                            // The room and the radio are added here, on this computer (ffmpeg).
                            const int code = QProcess::execute(QStringLiteral("ffmpeg"), { QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                                                                                           QStringLiteral("-i"), raw, QStringLiteral("-af"), filter, final });
                            if (code != 0)
                            {
                                QFile::remove(final);
                                QFile::rename(raw, final);
                            }
                            QFile::remove(raw);
                        }
                        results->append(relative);
                    }
                    else
                    {
                        Toast(tr("The Hearth server couldn't make it: %1").arg(QString::fromUtf8(data.left(200))));
                    }
                    if (--*remaining > 0)
                    {
                        return;
                    }
                    if (results->isEmpty())
                    {
                        if (done)
                        {
                            done(false);
                        }
                        return;
                    }
                    // Subtitles and lip sync from the first take.
                    const QString firstFile = QDir(VoiceFolder()).filePath(results->first());
                    int rate = 24000;
                    const QVector<qint16> samples = ReadWav(firstFile, &rate);
                    const double seconds = samples.size() / static_cast<double>(qMax(1, rate));
                    const QString base = results->first().section(QStringLiteral("_take"), 0, 0).section(QStringLiteral(".wav"), 0, 0);
                    WriteSubtitles(QDir(VoiceFolder()).filePath(base + QStringLiteral(".srt")), text, seconds);
                    WriteLipSync(QDir(VoiceFolder()).filePath(base + QStringLiteral(".lipsync.json")), samples, rate);
                    UpdateLine(scene, line, [results, takes, base, seconds](QJsonObject& l2)
                    {
                        l2.insert(QStringLiteral("made"), QJsonObject{ { QStringLiteral("hash"), LineHash(l2) },
                                                                       { QStringLiteral("file"), results->first() },
                                                                       { QStringLiteral("takes"), takes > 1 ? QJsonArray::fromStringList(*results) : QJsonArray() },
                                                                       { QStringLiteral("subtitles"), base + QStringLiteral(".srt") },
                                                                       { QStringLiteral("lipsync"), base + QStringLiteral(".lipsync.json") },
                                                                       { QStringLiteral("seconds"), seconds } });
                    });
                    QTimer::singleShot(10, this, [this, done]()
                    {
                        RenderLines();
                        Toast(tr("Made. Subtitles and lip sync are saved next to it in the project's Voice folder."));
                        if (done)
                        {
                            done(true);
                        }
                    });
                });
    }
}

void HearthVoiceStudio::GenerateChanged()
{
    // One at a time, so the Hearth server isn't flooded.
    auto queue = std::make_shared<QVector<QPair<int, int>>>();
    const QJsonArray scenes = m_data.value(QStringLiteral("scenes")).toArray();
    for (int s = 0; s < scenes.size(); ++s)
    {
        const QJsonArray lines = scenes[s].toObject().value(QStringLiteral("lines")).toArray();
        for (int l = 0; l < lines.size(); ++l)
        {
            if (LineState(lines[l].toObject()) != tr("Made"))
            {
                queue->push_back({ s, l });
            }
        }
    }
    auto next = std::make_shared<std::function<void()>>();
    *next = [this, queue, next]()
    {
        if (queue->isEmpty())
        {
            *next = nullptr; // break the cycle
            return;
        }
        const auto item = queue->takeFirst();
        Generate(item.first, item.second, 1, [next](bool) { if (*next) { QTimer::singleShot(0, [next]() { if (*next) { (*next)(); } }); } });
    };
    (*next)();
}

void HearthVoiceStudio::Play(const QString& file)
{
    if (file.isEmpty())
    {
        Toast(tr("Not made yet: press Generate again."));
        return;
    }
    const QString path = QDir(VoiceFolder()).filePath(file);
    for (const QString& player : { QStringLiteral("pw-play"), QStringLiteral("paplay"), QStringLiteral("aplay") })
    {
        if (!QStandardPaths::findExecutable(player).isEmpty())
        {
            QProcess::startDetached(player, { path });
            return;
        }
    }
    Toast(tr("No audio player found (pw-play, paplay or aplay)."));
}

void HearthVoiceStudio::NewVoice()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New voice"));
    auto* form = new QFormLayout(&dialog);
    auto* name = new QLineEdit(&dialog);
    auto* how = new QComboBox(&dialog);
    how->addItems({ tr("Design it from a description"), tr("Clone it from a recording") });
    auto* description = new QLineEdit(&dialog);
    description->setPlaceholderText(tr("an old man with a deep British voice"));
    auto* file = new QLineEdit(&dialog);
    auto* browse = Hearth::MakeButton(tr("Choose recording…"), false, &dialog);
    auto* consent = new QCheckBox(tr("I have the rights to this voice (it's mine, or I have permission)"), &dialog);
    form->addRow(tr("Name"), name);
    form->addRow(tr("How"), how);
    form->addRow(tr("Description"), description);
    form->addRow(tr("Recording"), file);
    form->addRow(QString(), browse);
    form->addRow(QString(), consent);
    connect(browse, &QPushButton::clicked, &dialog, [&dialog, file]()
    {
        const QString chosen = QFileDialog::getOpenFileName(&dialog, QObject::tr("Recording"), QDir::homePath(), QObject::tr("Audio (*.wav *.mp3 *.ogg *.flac)"));
        if (!chosen.isEmpty())
        {
            file->setText(chosen);
        }
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || name->text().trimmed().isEmpty())
    {
        return;
    }
    auto finished = [this](int status, const QByteArray& data)
    {
        if (status == 200)
        {
            Toast(tr("Voice made."));
            CheckServer();
        }
        else
        {
            const QJsonObject error = QJsonDocument::fromJson(data).object();
            Toast(tr("Couldn't make the voice: %1").arg(error.value(QStringLiteral("error")).toString(QString::fromUtf8(data.left(200)))));
        }
    };
    if (how->currentIndex() == 0)
    {
        Request(QStringLiteral("POST"), QStringLiteral("/api/voices/design"),
                QJsonDocument(QJsonObject{ { QStringLiteral("name"), name->text().trimmed() }, { QStringLiteral("description"), description->text().trimmed() } })
                    .toJson(QJsonDocument::Compact),
                QStringLiteral("application/json"), finished, 300000);
        Toast(tr("Designing the voice on the Hearth server…"));
        return;
    }
    if (!consent->isChecked())
    {
        Toast(tr("Cloning needs the box ticked: only clone voices you have the rights to."));
        return;
    }
    QFile recording(file->text());
    if (!recording.open(QIODevice::ReadOnly))
    {
        Toast(tr("Couldn't read the recording."));
        return;
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("name"), name->text().trimmed());
    query.addQueryItem(QStringLiteral("consent"), QStringLiteral("1"));
    Request(QStringLiteral("POST"), QStringLiteral("/api/voices/clone?") + query.toString(QUrl::FullyEncoded), recording.readAll(),
            QStringLiteral("application/octet-stream"), finished, 600000);
    Toast(tr("Cloning the voice on the Hearth server…"));
}

void HearthVoiceStudio::AddLine()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Add a line"));
    auto* form = new QFormLayout(&dialog);
    auto* voice = new QComboBox(&dialog);
    for (const QJsonValue value : m_voices)
    {
        voice->addItem(value.toObject().value(QStringLiteral("name")).toString(), value.toObject().value(QStringLiteral("id")).toString());
    }
    auto* text = new QPlainTextEdit(&dialog);
    text->setMinimumHeight(90);
    auto* usedIn = new QLineEdit(&dialog);
    usedIn->setPlaceholderText(tr("where the game plays it, like Terminal, intro"));
    form->addRow(tr("Voice"), voice);
    form->addRow(tr("What they say"), text);
    form->addRow(tr("Used in"), usedIn);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || text->toPlainText().trimmed().isEmpty())
    {
        return;
    }
    QJsonArray scenes = m_data.value(QStringLiteral("scenes")).toArray();
    if (m_scene >= scenes.size())
    {
        return;
    }
    QJsonObject scene = scenes[m_scene].toObject();
    QJsonArray lines = scene.value(QStringLiteral("lines")).toArray();
    lines.append(QJsonObject{ { QStringLiteral("id"), QUuid::createUuid().toString(QUuid::Id128).left(10) },
                              { QStringLiteral("voice"), voice->currentData().toString() },
                              { QStringLiteral("who"), voice->currentText().isEmpty() ? tr("Someone") : voice->currentText() },
                              { QStringLiteral("text"), text->toPlainText().trimmed() },
                              { QStringLiteral("mood"), QStringLiteral("Calm") },
                              { QStringLiteral("speed"), 1.0 },
                              { QStringLiteral("room"), 0 },
                              { QStringLiteral("usedIn"), usedIn->text().trimmed() } });
    scene.insert(QStringLiteral("lines"), lines);
    scenes[m_scene] = scene;
    m_data.insert(QStringLiteral("scenes"), scenes);
    m_line = lines.size() - 1;
    Save();
    RenderLines();
}

void HearthVoiceStudio::Toast(const QString& text)
{
    m_toast->setText(text);
    m_toast->show();
    QTimer::singleShot(7000, m_toast, &QWidget::hide);
}
