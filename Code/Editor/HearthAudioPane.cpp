/*
 * Hearth Engine Audio and mixing. Built on O3DE (Apache-2.0 OR MIT). See HearthAudioPane.h.
 */

#include "EditorDefs.h"

#include "HearthAudioPane.h"
#include "HearthImportPane.h"
#include "QtViewPaneManager.h"

#include <AzCore/Console/IConsole.h>
#include <AzCore/Interface/Interface.h>
#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMap>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSlider>
#include <functional>
#include <QLinearGradient>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPainter>
#include <QStandardPaths>
#include <QTimer>
#include <QDirIterator>
#include <QtEndian>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    const char* const AudioBuses[] = { "Master", "Ambience", "Lights", "Player", "Entities", "Music" };

    QString AudioProject()
    {
        return QString::fromUtf8(AZ::Utils::GetProjectPath().c_str());
    }

    QString AudioStatsPath()
    {
        return QDir(AudioProject()).filePath(QStringLiteral("user/HearthAudio/stats.json"));
    }

    double AudioBusDecibels(const QString& bus)
    {
        double value = 0.0;
        if (auto* registry = AZ::SettingsRegistry::Get())
        {
            registry->Get(value, QStringLiteral("/Hearth/Audio/Buses/%1").arg(bus).toUtf8().constData());
        }
        return value;
    }

    QString AudioDbText(double db)
    {
        if (db <= -60.0)
        {
            return QObject::tr("off");
        }
        return QStringLiteral("%1 dB").arg(qRound(db) > 0 ? QStringLiteral("+%1").arg(qRound(db)) : QString::number(qRound(db)));
    }

    //! "step_carpet_1" -> ("step_carpet", 1); "level0_hum" -> ("level0_hum", -1).
    QPair<QString, int> AudioVariant(const QString& stem)
    {
        const QRegularExpressionMatch match = QRegularExpression(QStringLiteral("^(.*?)[_-]?(\\d+)$")).match(stem);
        if (match.hasMatch() && !match.captured(1).isEmpty() && !match.captured(1).endsWith(QLatin1String("level")))
        {
            return { match.captured(1), match.captured(2).toInt() };
        }
        return { stem, -1 };
    }
} // namespace

void HearthAudioPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(110, 80, 1360, 840);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthAudioPane>("Audio", "Tools", options);
}

//! One bus in the mixer, as in the mockup: a tall dark bar that fills (ember to gold) with how loud the bus is
//! playing, and a light handle across it that is the fader. Drag or scroll the handle; double-click resets to 0 dB.
class HearthBusStrip : public QWidget
{
public:
    explicit HearthBusStrip(QWidget* parent)
        : QWidget(parent)
    {
        setFixedSize(54, 180);
        setCursor(Qt::SizeVerCursor);
        setToolTip(QObject::tr("Drag to change how loud this bus plays. Double-click for 0 dB."));
    }
    void SetDecibels(double decibels)
    {
        m_db = qBound(-60.0, decibels, 6.0);
        update();
    }
    void SetLevel(double level)
    {
        if (!qFuzzyCompare(1.0 + level, 1.0 + m_level))
        {
            m_level = level;
            update();
        }
    }
    std::function<void(double)> m_changed;

protected:
    // The fader goes from -60 dB (off, at the bottom) to +6 dB (at the top).
    double FaderFraction() const
    {
        return (m_db + 60.0) / 66.0;
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF bar(4, 0, 46, height());
        QPainterPath clip;
        clip.addRoundedRect(bar, 12, 12);
        painter.fillPath(clip, QColor(0x12, 0x10, 0x0E));
        painter.save();
        painter.setClipPath(clip);
        const double fill = bar.height() * qBound(0.0, m_level, 1.0);
        QLinearGradient gradient(bar.bottomLeft(), bar.topLeft());
        gradient.setColorAt(0.0, QColor(0xE8, 0x74, 0x3B));
        gradient.setColorAt(1.0, QColor(0xF2, 0xCF, 0x86));
        painter.fillRect(QRectF(bar.left(), bar.bottom() - fill, bar.width(), fill), gradient);
        painter.restore();
        const double y = bar.bottom() - bar.height() * FaderFraction();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0xEC, 0xE6, 0xDE));
        painter.drawRoundedRect(QRectF(0, qBound(0.0, y - 4.0, bar.bottom() - 8.0), width(), 8), 4, 4);
    }
    void SetFromY(double y)
    {
        const double fraction = qBound(0.0, 1.0 - y / height(), 1.0);
        m_db = qRound((fraction * 66.0 - 60.0) * 10.0) / 10.0;
        update();
        if (m_changed)
        {
            m_changed(m_db);
        }
    }
    void mousePressEvent(QMouseEvent* event) override { SetFromY(event->position().y()); }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (event->buttons() & Qt::LeftButton)
        {
            SetFromY(event->position().y());
        }
    }
    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        m_db = 0.0;
        update();
        if (m_changed)
        {
            m_changed(m_db);
        }
    }
    void wheelEvent(QWheelEvent* event) override
    {
        m_db = qBound(-60.0, m_db + (event->angleDelta().y() > 0 ? 1.0 : -1.0), 6.0);
        update();
        if (m_changed)
        {
            m_changed(m_db);
        }
    }

private:
    double m_db = 0.0;
    double m_level = 0.0;
};

//! Where a sound's file is in the project, by its name: the first audio file under Assets or Sounds called that.
static QString AudioFindSoundFile(const QString& stem)
{
    const QString project = QString::fromUtf8(AZ::Utils::GetProjectPath().c_str());
    for (const QString& folder : { QStringLiteral("Assets"), QStringLiteral("Sounds") })
    {
        QDirIterator it(QDir(project).filePath(folder), { stem + QStringLiteral(".wav"), stem + QStringLiteral(".ogg"), stem + QStringLiteral(".mp3"), stem + QStringLiteral(".flac") },
                        QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext())
        {
            return it.next();
        }
    }
    return {};
}

//! A sound's waveform: peaks of a 16-bit PCM .wav, drawn as thin bars. Other formats show an even stripe.
class HearthWaveform : public QWidget
{
public:
    explicit HearthWaveform(QWidget* parent)
        : QWidget(parent)
    {
        setFixedHeight(70);
    }
    void Load(const QString& path)
    {
        m_peaks.clear();
        QFile file(path);
        if (path.endsWith(QLatin1String(".wav"), Qt::CaseInsensitive) && file.open(QIODevice::ReadOnly))
        {
            const QByteArray data = file.read(16 * 1024 * 1024);
            // Find the "fmt " and "data" chunks.
            int channels = 1;
            int bits = 16;
            int pos = 12;
            while (pos + 8 <= data.size())
            {
                const QByteArray id = data.mid(pos, 4);
                const quint32 size = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + pos + 4));
                if (id == "fmt " && pos + 24 <= data.size())
                {
                    channels = qMax(1, int(qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(data.constData() + pos + 10))));
                    bits = qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(data.constData() + pos + 22));
                }
                else if (id == "data" && bits == 16)
                {
                    const int start = pos + 8;
                    const int frames = int(qMin<qint64>(size, data.size() - start) / (2 * channels));
                    const int buckets = 120;
                    for (int b = 0; b < buckets && frames > 0; ++b)
                    {
                        int peak = 0;
                        for (int f = frames * b / buckets; f < frames * (b + 1) / buckets; ++f)
                        {
                            const qint16 sample = qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(data.constData() + start + f * 2 * channels));
                            peak = qMax(peak, qAbs(int(sample)));
                        }
                        m_peaks.push_back(peak / 32768.0);
                    }
                    break;
                }
                pos += 8 + int(size) + int(size & 1);
            }
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()), 10, 10);
        painter.fillPath(clip, QColor(0x24, 0x21, 0x1E));
        painter.setClipPath(clip);
        const QColor bar(0xB8, 0xAF, 0xA4, 205);
        const double middle = height() / 2.0;
        if (m_peaks.isEmpty())
        {
            for (int x = 3; x < width(); x += 4)
            {
                painter.fillRect(QRectF(x, 0, 1, height()), bar);
            }
            return;
        }
        const double step = double(width()) / m_peaks.size();
        for (int i = 0; i < m_peaks.size(); ++i)
        {
            const double h = qMax(2.0, m_peaks[i] * (height() - 12));
            painter.fillRect(QRectF(i * step + step * 0.25, middle - h / 2.0, qMax(1.0, step * 0.5), h), bar);
        }
    }

private:
    QVector<double> m_peaks;
};

HearthAudioPane::HearthAudioPane(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthAudio"));
    setStyleSheet(QStringLiteral("QWidget#HearthAudio { background-color: #0D0C0B; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);
    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Audio"), tr("Every sound in the level and how loud it plays"));
    if (auto* titles = header->itemAt(1) ? header->itemAt(1)->layout() : nullptr)
    {
        m_subtitle = titles->count() > 1 ? qobject_cast<QLabel*>(titles->itemAt(1)->widget()) : nullptr;
    }
    header->addStretch(1);
    m_state = Hearth::MakeChip(QString(), QStringLiteral("good"), this);
    header->addWidget(m_state);
    QPushButton* add = Hearth::MakeButton(tr("Add sound"), true, this);
    connect(add, &QPushButton::clicked, this, [this]()
    {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Add sound"), QStandardPaths::writableLocation(QStandardPaths::MusicLocation),
                                                                tr("Sounds (*.wav *.ogg *.mp3 *.flac)"));
        if (files.isEmpty())
        {
            return;
        }
        if (const QtViewPane* pane = QtViewPaneManager::instance()->OpenPane(QStringLiteral("Import")))
        {
            if (auto* import = qobject_cast<HearthImportPane*>(pane->Widget()))
            {
                import->AddFiles(files);
            }
        }
    });
    header->addWidget(add);
    root->addLayout(header);

    // Mixer.
    QVBoxLayout* mixer = nullptr;
    QFrame* mixerCard = Hearth::MakeCard(this, &mixer);
    mixer->addWidget(Hearth::MakeLabel(tr("Mixer"), QStringLiteral("title"), mixerCard));
    auto* strips = new QHBoxLayout();
    strips->setSpacing(18);
    for (const char* busName : AudioBuses)
    {
        BusWidgets bus;
        bus.name = QString::fromLatin1(busName);
        auto* strip = new QVBoxLayout();
        strip->setSpacing(8);
        bus.strip = new HearthBusStrip(mixerCard);
        bus.strip->SetDecibels(AudioBusDecibels(bus.name));
        strip->addWidget(bus.strip, 0, Qt::AlignHCenter);
        strip->addWidget(Hearth::MakeLabel(tr(busName), QStringLiteral("body"), mixerCard), 0, Qt::AlignHCenter);
        bus.db = Hearth::MakeLabel(AudioDbText(AudioBusDecibels(bus.name)), QStringLiteral("mono"), mixerCard);
        strip->addWidget(bus.db, 0, Qt::AlignHCenter);
        auto* holder = new QWidget(mixerCard);
        holder->setLayout(strip);
        holder->setMinimumWidth(100);
        strips->addWidget(holder, 1);
        const QString name = bus.name;
        QLabel* dbLabel = bus.db;
        bus.strip->m_changed = [this, name, dbLabel](double decibels)
        {
            dbLabel->setText(AudioDbText(decibels));
            SetBus(name, decibels);
        };
        m_buses << bus;
    }
    mixer->addLayout(strips);
    root->addWidget(mixerCard);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);
    QVBoxLayout* soundsLayout = nullptr;
    QFrame* soundsCard = Hearth::MakeCard(this, &soundsLayout);
    soundsLayout->addWidget(Hearth::MakeLabel(tr("Sounds"), QStringLiteral("title"), soundsCard));
    m_list = new QListWidget(soundsCard);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setStyleSheet(QStringLiteral("QListWidget { background: transparent; outline: none; } QListWidget::item { border: none; border-radius: 8px; }"
                                         "QListWidget::item:selected { background-color: rgba(232, 116, 59, 31); }"));
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row)
    {
        if (row >= 0)
        {
            m_picked = row;
            ShowSound(row);
        }
    });
    soundsLayout->addWidget(m_list, 1);
    body->addWidget(soundsCard, 1);
    m_detail = Hearth::MakeCard(this, &m_detailLayout);
    m_detail->setFixedWidth(400);
    body->addWidget(m_detail);
    root->addLayout(body, 1);

    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &HearthAudioPane::Refresh);
}

void HearthAudioPane::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    Refresh();
    m_timer->start();
}

void HearthAudioPane::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_timer->stop();
}

void HearthAudioPane::WriteAudioRegistry(const QStringList& keys, const QJsonValue& value)
{
    // In the running editor now, and in Registry/hearth_audio.setreg for next time and for the packaged game.
    if (auto* registry = AZ::SettingsRegistry::Get())
    {
        const QString path = QStringLiteral("/Hearth/Audio/") + keys.join(QLatin1Char('/'));
        if (value.isDouble())
        {
            registry->Set(path.toUtf8().constData(), value.toDouble());
        }
        else
        {
            registry->Set(path.toUtf8().constData(), value.toString().toUtf8().constData());
        }
    }
    const QString file = QDir(AudioProject()).filePath(QStringLiteral("Registry/hearth_audio.setreg"));
    QJsonObject all;
    QFile in(file);
    if (in.open(QIODevice::ReadOnly))
    {
        all = QJsonDocument::fromJson(in.readAll()).object();
        in.close();
    }
    QJsonObject hearth = all.value(QStringLiteral("Hearth")).toObject();
    QJsonObject audio = hearth.value(QStringLiteral("Audio")).toObject();
    QJsonObject section = audio.value(keys[0]).toObject();
    section.insert(keys[1], value);
    audio.insert(keys[0], section);
    hearth.insert(QStringLiteral("Audio"), audio);
    all.insert(QStringLiteral("Hearth"), hearth);
    QSaveFile out(file);
    if (out.open(QIODevice::WriteOnly))
    {
        out.write(QJsonDocument(all).toJson(QJsonDocument::Indented));
        out.commit();
    }
    if (auto* console = AZ::Interface<AZ::IConsole>::Get())
    {
        console->PerformCommand("hearth_audioRefresh");
    }
}

void HearthAudioPane::SetBus(const QString& bus, double decibels)
{
    WriteAudioRegistry({ QStringLiteral("Buses"), bus }, decibels);
}

void HearthAudioPane::Assign(const QStringList& stems, const QString& bus)
{
    for (const QString& stem : stems)
    {
        WriteAudioRegistry({ QStringLiteral("Assign"), stem }, bus);
    }
}

void HearthAudioPane::Refresh()
{
    if (auto* console = AZ::Interface<AZ::IConsole>::Get())
    {
        const QByteArray command = QStringLiteral("hearth_audioStats %1").arg(AudioStatsPath()).toUtf8();
        console->PerformCommand(command.constData());
    }
    QFile file(AudioStatsPath());
    if (!file.open(QIODevice::ReadOnly))
    {
        m_state->setText(tr("No sounds yet (the Sound part may be off)"));
        return;
    }
    const QJsonArray all = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("sounds")).toArray();
    // In game mode, the game's sounds; otherwise the editor's.
    const bool inGame = GetIEditor() && GetIEditor()->IsInGameMode();
    QJsonArray sounds;
    int playing = 0;
    for (const QJsonValue value : all)
    {
        const QJsonObject sound = value.toObject();
        if (sound.value(QStringLiteral("game")).toBool() == inGame)
        {
            sounds.append(sound);
            playing += sound.value(QStringLiteral("playing")).toBool() ? 1 : 0;
        }
    }
    m_sounds = sounds;
    const QString level = GetIEditor() ? GetIEditor()->GetLevelName() : QString();
    if (m_subtitle)
    {
        m_subtitle->setText(level.isEmpty() ? tr("Every sound and how loud it plays") : tr("Every sound in %1 and how loud it plays").arg(level));
    }
    m_state->setText(inGame ? tr("Listening in game, %n playing", nullptr, playing) : tr("In the editor, %n playing", nullptr, playing));

    // Meters: the loudest playing sound on each bus.
    QMap<QString, double> loudest;
    for (const QJsonValue value : sounds)
    {
        const QJsonObject sound = value.toObject();
        if (sound.value(QStringLiteral("playing")).toBool())
        {
            const double level = sound.value(QStringLiteral("volume")).toDouble() / 100.0 * sound.value(QStringLiteral("gain")).toDouble();
            const QString bus = sound.value(QStringLiteral("bus")).toString();
            loudest[bus] = qMax(loudest.value(bus), level);
            loudest[QStringLiteral("Master")] = qMax(loudest.value(QStringLiteral("Master")), level);
        }
    }
    for (const BusWidgets& bus : m_buses)
    {
        bus.strip->SetLevel(qBound(0.0, loudest.value(bus.name), 1.0));
    }
    FillSounds();
}

void HearthAudioPane::FillSounds()
{
    // Numbered variants together: step_carpet_1..4 is one row.
    QMap<QString, Group> groups;
    QMap<QString, QList<int>> numbers;
    for (const QJsonValue value : m_sounds)
    {
        const QJsonObject sound = value.toObject();
        const QString stem = sound.value(QStringLiteral("sound")).toString();
        if (stem.isEmpty())
        {
            continue;
        }
        const auto variant = AudioVariant(stem);
        Group& group = groups[variant.first];
        if (!group.stems.contains(stem))
        {
            group.stems << stem;
        }
        group.sounds.append(sound);
        if (variant.second >= 0 && !numbers[variant.first].contains(variant.second))
        {
            numbers[variant.first] << variant.second;
        }
    }
    QVector<Group> list;
    for (auto it = groups.begin(); it != groups.end(); ++it)
    {
        Group group = it.value();
        QList<int> found = numbers.value(it.key());
        std::sort(found.begin(), found.end());
        group.name = group.stems.size() > 1 && found.size() > 1 ? tr("%1_%2 to %3").arg(it.key()).arg(found.first()).arg(found.last()) : group.stems.first();
        list << group;
    }
    // Only rebuild the list when something in it changed (it's refreshed every second).
    QString signature;
    for (const Group& group : list)
    {
        signature += group.name;
        for (const QJsonValue value : group.sounds)
        {
            const QJsonObject sound = value.toObject();
            signature += QStringLiteral("|%1%2%3%4%5").arg(sound.value(QStringLiteral("bus")).toString()).arg(sound.value(QStringLiteral("playing")).toBool())
                             .arg(sound.value(QStringLiteral("volume")).toDouble()).arg(sound.value(QStringLiteral("requests")).toInt()).arg(sound.value(QStringLiteral("heard")).toInt());
        }
        signature += QLatin1Char(';');
    }
    if (signature == m_signature && !list.isEmpty())
    {
        m_groups = list;
        return;
    }
    m_signature = signature;
    const int keep = m_list->currentRow();
    m_groups = list;
    m_list->blockSignals(true);
    m_list->clear();
    for (const Group& group : m_groups)
    {
        // Where it plays from: one entity by name, or how many ("49 lights").
        QStringList entities;
        bool loop = false;
        bool playing = false;
        bool silent = false;
        double volume = 0.0;
        for (const QJsonValue value : group.sounds)
        {
            const QJsonObject sound = value.toObject();
            entities << sound.value(QStringLiteral("entity")).toString();
            loop |= sound.value(QStringLiteral("loop")).toBool();
            playing |= sound.value(QStringLiteral("playing")).toBool();
            silent |= sound.value(QStringLiteral("requests")).toInt() > 0 && sound.value(QStringLiteral("heard")).toInt() == 0;
            volume = qMax(volume, sound.value(QStringLiteral("volume")).toDouble());
        }
        QStringList unique = entities;
        unique.removeDuplicates();
        QString where;
        if (unique.size() == 1)
        {
            where = unique.first();
        }
        else
        {
            QString common = unique.first().section(QRegularExpression(QStringLiteral("[_\\s\\d]")), 0, 0);
            common.remove(QRegularExpression(QStringLiteral("^L\\d+")));
            where = tr("%n %1", nullptr, int(entities.size())).arg(common.isEmpty() ? tr("places") : common.toLower() + QStringLiteral("s"));
        }
        if (loop)
        {
            where += tr(", loops");
        }
        const QString state = silent && !playing ? tr("Silent") : (playing ? tr("Playing") : tr("Waiting"));
        auto* row = new QWidget();
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(8, 6, 8, 6);
        layout->setSpacing(12);
        // Round play button, then the name with where it plays from under it (mockup).
        auto* play = new QPushButton(QStringLiteral("\u25B6"), row);
        play->setFixedSize(30, 30);
        play->setToolTip(tr("Play %1").arg(group.name));
        play->setStyleSheet(QStringLiteral("QPushButton { background-color: #24211E; color: #ECE6DE; border: none; border-radius: 15px; font-size: 10px; padding: 0; }"
                                           "QPushButton:hover { background-color: #34302B; }"));
        const QString firstStem = group.stems.first();
        connect(play, &QPushButton::clicked, this, [firstStem]()
        {
            if (auto* console = AZ::Interface<AZ::IConsole>::Get())
            {
                console->PerformCommand(QStringLiteral("hearth_audioPlay %1").arg(firstStem).toUtf8().constData());
            }
        });
        layout->addWidget(play);
        auto* names = new QVBoxLayout();
        names->setSpacing(0);
        QLabel* name = Hearth::MakeLabel(group.name, QStringLiteral("body"), row);
        names->addWidget(name);
        QLabel* whereLabel = Hearth::MakeLabel(where, QStringLiteral("small"), row);
        names->addWidget(whereLabel);
        layout->addLayout(names, 1);
        auto* bus = new QComboBox(row);
        for (const char* busName : AudioBuses)
        {
            if (QLatin1String(busName) != QLatin1String("Master"))
            {
                bus->addItem(tr(busName), QString::fromLatin1(busName));
            }
        }
        bus->setCurrentIndex(qMax(0, bus->findData(group.sounds.first().toObject().value(QStringLiteral("bus")).toString())));
        const QStringList stems = group.stems;
        connect(bus, QOverload<int>::of(&QComboBox::activated), this, [this, bus, stems](int) { Assign(stems, bus->currentData().toString()); });
        layout->addWidget(bus);
        QLabel* vol = Hearth::MakeLabel(QStringLiteral("%1%").arg(qRound(volume)), QStringLiteral("mono"), row);
        vol->setFixedWidth(48);
        vol->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(vol);
        layout->addWidget(Hearth::MakeChip(state, state == tr("Silent") ? QStringLiteral("bad") : (state == tr("Playing") ? QStringLiteral("good") : QStringLiteral("plain")), row));
        auto* item = new QListWidgetItem(m_list);
        item->setSizeHint(QSize(10, 50));
        if (state == tr("Silent"))
        {
            row->setStyleSheet(QStringLiteral("background-color: rgba(255, 122, 102, 20); border-radius: 8px;"));
        }
        m_list->setItemWidget(item, row);
    }
    if (m_groups.isEmpty())
    {
        auto* item = new QListWidgetItem(tr("No sounds are loaded. Sounds show here once a level with sounds is open."), m_list);
        item->setForeground(Hearth::Dim());
    }
    m_list->blockSignals(false);
    const int row = qBound(0, keep >= 0 ? keep : m_picked, qMax(0, int(m_groups.size()) - 1));
    if (!m_groups.isEmpty())
    {
        m_list->setCurrentRow(row);
        ShowSound(row);
    }
}

void HearthAudioPane::ShowSound(int index)
{
    while (QLayoutItem* item = m_detailLayout->takeAt(0))
    {
        if (QLayout* inner = item->layout())
        {
            while (QLayoutItem* child = inner->takeAt(0))
            {
                if (child->widget())
                {
                    child->widget()->deleteLater();
                }
                delete child;
            }
        }
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
    if (index < 0 || index >= m_groups.size())
    {
        return;
    }
    const Group& group = m_groups[index];
    const QJsonObject first = group.sounds.first().toObject();
    int requests = 0;
    int heard = 0;
    bool loaded = true;
    for (const QJsonValue value : group.sounds)
    {
        const QJsonObject sound = value.toObject();
        requests += sound.value(QStringLiteral("requests")).toInt();
        heard += sound.value(QStringLiteral("heard")).toInt();
        loaded &= sound.value(QStringLiteral("loaded")).toBool();
    }
    QLabel* name = Hearth::MakeLabel(group.name, QStringLiteral("title"), m_detail);
    m_detailLayout->addWidget(name);
    // The sound's shape, from its file (mockup: a waveform under the name).
    auto* wave = new HearthWaveform(m_detail);
    wave->Load(AudioFindSoundFile(group.stems.first()));
    m_detailLayout->addWidget(wave);
    auto fact = [this](const QString& label, const QString& value)
    {
        auto* row = new QHBoxLayout();
        row->addWidget(Hearth::MakeLabel(label, QStringLiteral("dim"), m_detail));
        row->addStretch(1);
        row->addWidget(Hearth::MakeLabel(value, QStringLiteral("body"), m_detail));
        m_detailLayout->addLayout(row);
    };
    fact(tr("Length"), tr("%1 s").arg(first.value(QStringLiteral("seconds")).toDouble(), 0, 'f', 2));
    fact(tr("Plays from"), first.value(QStringLiteral("entity")).toString());
    const bool spatial = first.value(QStringLiteral("spatial")).toBool();
    const QString entity = first.value(QStringLiteral("entity")).toString().toLower();
    const bool ownFeet = entity.contains(QLatin1String("foot")) || entity.contains(QLatin1String("step")) || entity.contains(QLatin1String("player"));
    fact(tr("3D position"), spatial ? tr("On (heard up to %1 m)").arg(qRound(first.value(QStringLiteral("maxDistance")).toDouble()))
                                    : (ownFeet ? tr("Off (your own feet)") : tr("Off (same everywhere)")));
    fact(tr("Bus"), first.value(QStringLiteral("bus")).toString());

    QString story;
    QString tone = QStringLiteral("plain");
    if (!loaded)
    {
        story = tr("Asked to play %n time(s), heard 0. The sound didn't load: its file may have failed to process.", nullptr, requests);
        tone = QStringLiteral("bad");
    }
    else if (requests > 0 && heard == 0)
    {
        story = tr("Asked to play %1 times, heard %2 times. The sound is loaded but nothing comes out.").arg(requests).arg(heard);
        tone = QStringLiteral("bad");
    }
    else if (requests > 0)
    {
        story = tr("Asked to play %1 times, heard %2 times.").arg(requests).arg(heard);
    }
    else
    {
        story = tr("Not asked to play yet.");
    }
    QLabel* storyLabel = Hearth::MakeLabel(story, QStringLiteral("body"), m_detail);
    storyLabel->setWordWrap(true);
    if (tone == QLatin1String("bad"))
    {
        storyLabel->setStyleSheet(QStringLiteral("background-color: rgba(255, 122, 102, 20); color: #FFB38F; border-radius: 8px; padding: 8px;"));
    }
    m_detailLayout->addWidget(storyLabel);
    m_detailLayout->addStretch(1);
    QPushButton* claude = Hearth::MakeButton(tr("Let Claude look into it"), true, m_detail);
    const QJsonArray sounds = group.sounds;
    const QString groupName = group.name;
    connect(claude, &QPushButton::clicked, this, [sounds, groupName, story]()
    {
        const QString details = QStringLiteral("In the Audio window, %1: %2\n\nWhat the engine reports for it (hearth_audioStats):\n%3")
                                    .arg(groupName, story, QString::fromUtf8(QJsonDocument(sounds).toJson(QJsonDocument::Indented)));
        Hearth::AskClaude(QStringLiteral("Sound problem: %1").arg(groupName), details, AudioProject());
    });
    m_detailLayout->addWidget(claude);
}
