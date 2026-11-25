/*
 * Hearth Engine "Cinematics". Built on O3DE (Apache-2.0 OR MIT). See HearthCinematicsPane.h.
 */

#include "EditorDefs.h"

#include "HearthCinematicsPane.h"

#include "AnimationContext.h"
#include "MainWindow.h"
#include "QtViewPaneManager.h"
#include "EditorViewportCamera.h"
#include "TrackView/TrackViewAnimNode.h"
#include "TrackView/TrackViewSequence.h"
#include "TrackView/TrackViewSequenceManager.h"
#include "TrackView/TrackViewTrack.h"
#include "Undo/Undo.h"

#include <Atom/Feature/Utils/FrameCaptureBus.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/Entity.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/EditorPythonRunnerRequestsBus.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>
#include <AzToolsFramework/Component/EditorComponentAPIBus.h>
#include <CryCommon/Maestro/Types/AnimNodeType.h>
#include <CryCommon/Maestro/Types/AnimParamType.h>
#include <CryCommon/Maestro/Types/SequenceType.h>

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QCursor>
#include <QFileDialog>
#include <QAction>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <functional>
#include <memory>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    const QColor CineTrackColours[4] = { QColor(0xE8, 0x74, 0x3B), QColor(0x9A, 0xB8, 0xE8), QColor(0xC9, 0xB4, 0x6A), QColor(0x7F, 0xE0, 0xA0) };

    QString CineClock(double seconds)
    {
        const int whole = qMax(0, static_cast<int>(seconds));
        return QStringLiteral("%1:%2").arg(whole / 60).arg(whole % 60, 2, 10, QLatin1Char('0'));
    }

    //! Length of a .wav file in seconds (RIFF header: byte rate and data size), 0 if it can't be read.
    double CineWavSeconds(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return 0.0;
        }
        const QByteArray head = file.read(4096);
        if (head.size() < 44 || !head.startsWith("RIFF"))
        {
            return 0.0;
        }
        auto u32 = [&head](int at) { return static_cast<quint32>(static_cast<uchar>(head[at])) | (static_cast<quint32>(static_cast<uchar>(head[at + 1])) << 8)
                                            | (static_cast<quint32>(static_cast<uchar>(head[at + 2])) << 16) | (static_cast<quint32>(static_cast<uchar>(head[at + 3])) << 24); };
        quint32 byteRate = 0;
        int at = 12;
        while (at + 8 <= head.size())
        {
            const QByteArray id = head.mid(at, 4);
            const quint32 size = u32(at + 4);
            if (id == "fmt " && at + 20 <= head.size())
            {
                byteRate = u32(at + 16);
            }
            else if (id == "data")
            {
                // Streamed files can say 0xFFFFFFFF; fall back to the file size.
                const double bytes = size == 0xFFFFFFFFu ? static_cast<double>(file.size() - at - 8) : static_cast<double>(size);
                return byteRate > 0 ? bytes / byteRate : 0.0;
            }
            at += 8 + static_cast<int>(size) + (size & 1);
        }
        return 0.0;
    }

    //! Lens in mm (full-frame) to vertical field of view in degrees.
    double CineLensToFov(double mm)
    {
        return 2.0 * std::atan(24.0 / (2.0 * qMax(1.0, mm))) * 180.0 / 3.14159265358979;
    }

    AZ::Entity* CineFindEntity(const QString& name, const QString& idText)
    {
        AZ::Entity* entity = nullptr;
        if (!idText.isEmpty())
        {
            AZ::ComponentApplicationBus::BroadcastResult(entity, &AZ::ComponentApplicationRequests::FindEntity, AZ::EntityId(idText.toULongLong()));
        }
        if (!entity && !name.isEmpty())
        {
            const AZStd::string wanted(name.toUtf8().constData());
            AZ::ComponentApplicationBus::Broadcast(&AZ::ComponentApplicationRequests::EnumerateEntities, [&entity, &wanted](AZ::Entity* e)
            {
                if (!entity && e->GetName() == wanted)
                {
                    bool editorEntity = false;
                    AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(
                        editorEntity, &AzToolsFramework::EditorEntityContextRequests::IsEditorEntity, e->GetId());
                    entity = editorEntity ? e : nullptr;
                }
            });
        }
        return entity;
    }
} // namespace

const char* const HearthCinematicsPane::TrackNames[4] = { "Cameras", "Voices", "Sound", "Lights" };

//! The picture: the viewport through the shot's camera, the line being said, and which shot it is.
class HearthCinePreview : public QWidget
{
public:
    explicit HearthCinePreview(QWidget* parent)
        : QWidget(parent)
    {
        setMinimumSize(420, 236);
    }
    QImage m_image;
    QString m_subtitle;
    QString m_caption;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        // 16:9 inside the widget.
        QSizeF frame = QSizeF(16, 9).scaled(QSizeF(size()), Qt::KeepAspectRatio);
        const QRectF box(QPointF((width() - frame.width()) / 2.0, (height() - frame.height()) / 2.0), frame);
        QPainterPath clip;
        clip.addRoundedRect(box, 12, 12);
        p.setClipPath(clip);
        p.fillRect(box, QColor(0x0D, 0x0C, 0x0B));
        if (!m_image.isNull())
        {
            const QSizeF fill = QSizeF(m_image.size()).scaled(box.size(), Qt::KeepAspectRatioByExpanding);
            p.drawImage(QRectF(box.center() - QPointF(fill.width() / 2.0, fill.height() / 2.0), fill), m_image);
        }
        else
        {
            p.setPen(Hearth::Dim());
            p.drawText(box, Qt::AlignCenter, QObject::tr("Open a level to see the shot here"));
        }
        // Letterbox bars, like the mockup.
        p.fillRect(QRectF(box.left(), box.top(), box.width(), box.height() * 0.08), QColor(0, 0, 0, 200));
        p.fillRect(QRectF(box.left(), box.bottom() - box.height() * 0.08, box.width(), box.height() * 0.08), QColor(0, 0, 0, 200));
        QFont font = p.font();
        if (!m_subtitle.isEmpty())
        {
            font.setPixelSize(qMax(13, static_cast<int>(box.height() / 22)));
            p.setFont(font);
            const QRectF text(box.left() + box.width() * 0.1, box.bottom() - box.height() * 0.30, box.width() * 0.8, box.height() * 0.18);
            p.setPen(QColor(0, 0, 0, 180));
            p.drawText(text.translated(1, 1), Qt::AlignCenter | Qt::TextWordWrap, m_subtitle);
            p.setPen(QColor(0xEC, 0xE6, 0xDE));
            p.drawText(text, Qt::AlignCenter | Qt::TextWordWrap, m_subtitle);
        }
        if (!m_caption.isEmpty())
        {
            font.setPixelSize(12);
            p.setFont(font);
            const int w = p.fontMetrics().horizontalAdvance(m_caption) + 24;
            const QRectF chip(box.left() + 12, box.top() + box.height() * 0.08 + 10, w, 24);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(13, 12, 11, 220));
            p.drawRoundedRect(chip, 12, 12);
            p.setPen(QColor(0xEC, 0xE6, 0xDE));
            p.drawText(chip, Qt::AlignCenter, m_caption);
        }
    }
};

//! Four rows of clips, a ruler and the playhead. Click a clip to pick it, click or drag on the ruler to move in time.
class HearthCineTimeline : public QWidget
{
public:
    explicit HearthCineTimeline(QWidget* parent)
        : QWidget(parent)
    {
        setMinimumHeight(RulerHeight + 4 * RowHeight + 8);
        setCursor(Qt::PointingHandCursor);
    }
    QJsonArray m_tracks;
    double m_length = 60.0;
    double m_time = 0.0;
    int m_track = -1;
    int m_clip = -1;
    std::function<void(double)> m_onTime;
    std::function<void(int, int)> m_onPick;
    std::function<void(int, double)> m_onAdd; //!< double-click an empty spot on a row

    static constexpr int RulerHeight = 22;
    static constexpr int RowHeight = 34;
    static constexpr int NameWidth = 86;

protected:
    double X(double seconds) const { return NameWidth + (width() - NameWidth - 8) * seconds / qMax(1.0, m_length); }
    double Seconds(double x) const { return qBound(0.0, (x - NameWidth) / qMax(1.0, static_cast<double>(width() - NameWidth - 8)) * m_length, m_length); }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QFont font = p.font();
        font.setPixelSize(11);
        p.setFont(font);
        // Ruler: a mark every 10 s.
        p.setPen(Hearth::Dim());
        for (int s = 0; s <= static_cast<int>(m_length); s += 10)
        {
            const double x = X(s);
            p.drawLine(QPointF(x, RulerHeight - 6), QPointF(x, RulerHeight));
            p.drawText(QRectF(x + 3, 0, 40, RulerHeight - 4), Qt::AlignLeft | Qt::AlignVCenter, CineClock(s));
        }
        for (int t = 0; t < 4; ++t)
        {
            const double top = RulerHeight + t * RowHeight;
            p.setPen(Hearth::Dim());
            p.drawText(QRectF(0, top, NameWidth - 8, RowHeight), Qt::AlignVCenter | Qt::AlignLeft, QObject::tr(HearthCinematicsPane::TrackNames[t]));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 6));
            p.drawRoundedRect(QRectF(NameWidth, top + 3, width() - NameWidth - 8, RowHeight - 6), 8, 8);
            const QJsonArray clips = m_tracks.at(t).toArray();
            for (int c = 0; c < clips.size(); ++c)
            {
                const QJsonObject clip = clips[c].toObject();
                const double start = clip.value(QStringLiteral("start")).toDouble();
                const double length = clip.value(QStringLiteral("length")).toDouble();
                const QRectF box(X(start) + 1, top + 5, qMax(4.0, X(start + length) - X(start) - 2), RowHeight - 10);
                const bool picked = t == m_track && c == m_clip;
                QColor fill = CineTrackColours[t];
                fill.setAlpha(picked ? 255 : 150);
                p.setBrush(fill);
                p.setPen(picked ? QPen(QColor(0xEC, 0xE6, 0xDE), 2) : Qt::NoPen);
                p.drawRoundedRect(box, 7, 7);
                p.setPen(Hearth::Ink());
                p.drawText(box.adjusted(8, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft,
                           p.fontMetrics().elidedText(clip.value(QStringLiteral("label")).toString(), Qt::ElideRight, static_cast<int>(box.width()) - 14));
            }
        }
        // Playhead.
        const double x = X(m_time);
        p.setPen(QPen(QColor(0xEC, 0xE6, 0xDE), 2));
        p.drawLine(QPointF(x, 2), QPointF(x, height() - 2));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xEC, 0xE6, 0xDE));
        p.drawEllipse(QPointF(x, 6), 5, 5);
    }
    void mousePressEvent(QMouseEvent* event) override
    {
        const QPointF at = event->pos();
        const int row = static_cast<int>((at.y() - RulerHeight) / RowHeight);
        if (at.y() > RulerHeight && row >= 0 && row < 4 && at.x() > NameWidth)
        {
            const double seconds = Seconds(at.x());
            const QJsonArray clips = m_tracks.at(row).toArray();
            for (int c = 0; c < clips.size(); ++c)
            {
                const QJsonObject clip = clips[c].toObject();
                const double start = clip.value(QStringLiteral("start")).toDouble();
                if (seconds >= start && seconds <= start + clip.value(QStringLiteral("length")).toDouble())
                {
                    if (m_onPick)
                    {
                        m_onPick(row, c);
                    }
                    break;
                }
            }
        }
        if (m_onTime)
        {
            m_onTime(Seconds(at.x()));
        }
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        const QPointF at = event->pos();
        const int row = static_cast<int>((at.y() - RulerHeight) / RowHeight);
        if (at.y() > RulerHeight && row >= 0 && row < 4 && at.x() > NameWidth && m_onAdd)
        {
            m_onAdd(row, Seconds(at.x()));
        }
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if ((event->buttons() & Qt::LeftButton) && m_onTime)
        {
            m_onTime(Seconds(event->pos().x()));
        }
    }
};

void HearthCinematicsPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(120, 80, 1280, 820);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthCinematicsPane>("Cinematics", "Tools", options);
}

HearthCinematicsPane::HearthCinematicsPane(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthCinematics"));
    setStyleSheet(QStringLiteral("QWidget#HearthCinematics { background-color: #0D0C0B; }"));
    setFocusPolicy(Qt::StrongFocus);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Cinematics"), tr("No cutscene yet"));
    if (QLayoutItem* titles = header->itemAt(1); titles && titles->layout() && titles->layout()->itemAt(1))
    {
        m_subtitle = qobject_cast<QLabel*>(titles->layout()->itemAt(1)->widget());
    }
    m_pick = new QComboBox(this);
    m_pick->setMinimumWidth(180);
    header->addWidget(m_pick);
    QPushButton* add = Hearth::MakeButton(tr("Add camera shot"), false, this);
    QPushButton* watch = Hearth::MakeButton(tr("Watch full screen"), true, this);
    header->addWidget(add);
    header->addWidget(watch);
    root->addLayout(header);
    connect(add, &QPushButton::clicked, this, &HearthCinematicsPane::AddCameraShot);
    connect(watch, &QPushButton::clicked, this, &HearthCinematicsPane::WatchFullScreen);
    connect(m_pick, qOverload<int>(&QComboBox::activated), this, [this](int index)
    {
        const QString name = m_pick->itemData(index).toString();
        if (name.isEmpty())
        {
            NewCinematic();
        }
        else
        {
            Load(name);
        }
    });

    auto* body = new QHBoxLayout();
    body->setSpacing(10);
    root->addLayout(body, 1);

    // Left: the picture, the transport and the timeline.
    QVBoxLayout* mainLayout = nullptr;
    QFrame* mainCard = Hearth::MakeCard(this, &mainLayout);
    m_preview = new HearthCinePreview(mainCard);
    mainLayout->addWidget(m_preview, 1);
    auto* transport = new QHBoxLayout();
    m_play = Hearth::MakeButton(QStringLiteral("▶"), true, mainCard);
    m_play->setFixedWidth(44);
    connect(m_play, &QPushButton::clicked, this, [this]()
    {
        m_playing = !m_playing;
        if (m_playing && m_time >= Length() - 0.05)
        {
            SetTime(0.0);
        }
        m_lastTick = QDateTime::currentMSecsSinceEpoch();
        m_play->setText(m_playing ? QStringLiteral("❚❚") : QStringLiteral("▶"));
    });
    transport->addWidget(m_play);
    m_timeText = Hearth::MakeLabel(QStringLiteral("0:00 / 0:00"), QStringLiteral("mono"), mainCard);
    transport->addWidget(m_timeText);
    transport->addStretch(1);
    mainLayout->addLayout(transport);
    m_timeline = new HearthCineTimeline(mainCard);
    m_timeline->m_onTime = [this](double seconds) { SetTime(seconds); };
    m_timeline->m_onPick = [this](int track, int clip) { Pick(track, clip); };
    m_timeline->m_onAdd = [this](int track, double seconds) { AddClip(track, seconds); };
    m_timeline->setToolTip(tr("Click to move in time and pick a clip. Double-click a row to add a voice, sound or light change there."));
    mainLayout->addWidget(m_timeline);
    body->addWidget(mainCard, 1);

    // Right: the picked shot.
    QVBoxLayout* shotLayout = nullptr;
    QFrame* shotCard = Hearth::MakeCard(this, &shotLayout);
    m_shotCard = shotCard;
    shotCard->setFixedWidth(340);
    m_shotTitle = Hearth::MakeLabel(tr("Pick a shot"), QStringLiteral("title"), shotCard);
    shotLayout->addWidget(m_shotTitle);
    m_camera = new QComboBox(shotCard);
    m_camera->setEditable(false);
    shotLayout->addWidget(Hearth::MakeSettingRow(tr("Camera"), QString(), m_camera, shotCard));
    m_move = new QLineEdit(shotCard);
    m_move->setPlaceholderText(tr("Push in, 2 m"));
    shotLayout->addWidget(Hearth::MakeSettingRow(tr("Move"), QString(), m_move, shotCard));
    m_lens = new QDoubleSpinBox(shotCard);
    m_lens->setRange(12.0, 200.0);
    m_lens->setSuffix(tr(" mm"));
    m_lens->setDecimals(0);
    shotLayout->addWidget(Hearth::MakeSettingRow(tr("Lens"), QString(), m_lens, shotCard));
    m_length = new QDoubleSpinBox(shotCard);
    m_length->setRange(0.2, 120.0);
    m_length->setSingleStep(0.5);
    m_length->setDecimals(1);
    m_length->setSuffix(tr(" s"));
    shotLayout->addWidget(Hearth::MakeSettingRow(tr("Length"), QString(), m_length, shotCard));
    m_line = new QComboBox(shotCard);
    shotLayout->addWidget(Hearth::MakeSettingRow(tr("Line"), QString(), m_line, shotCard));
    m_note = Hearth::MakeLabel(tr("Lines come from Voice Studio. Edit a line there and this shot's timing follows."), QStringLiteral("small"), shotCard);
    m_note->setWordWrap(true);
    shotLayout->addWidget(m_note);
    QPushButton* openVoice = Hearth::MakeButton(tr("Open Voice Studio"), false, shotCard);
    connect(openVoice, &QPushButton::clicked, this, []() { QtViewPaneManager::instance()->OpenPane(QStringLiteral("Voice Studio")); });
    shotLayout->addWidget(openVoice);
    QPushButton* remove = Hearth::MakeButton(tr("Remove this clip"), false, shotCard);
    connect(remove, &QPushButton::clicked, this, [this]()
    {
        QJsonArray tracks = m_data.value(QStringLiteral("tracks")).toArray();
        QJsonArray clips = tracks.at(m_track).toArray();
        if (m_clip < 0 || m_clip >= clips.size())
        {
            return;
        }
        clips.removeAt(m_clip);
        tracks[m_track] = clips;
        m_data.insert(QStringLiteral("tracks"), tracks);
        m_clip = -1;
        FollowVoiceLines();
        Save();
    });
    shotLayout->addWidget(remove);
    shotLayout->addStretch(1);
    m_status = Hearth::MakeLabel(QString(), QStringLiteral("small"), shotCard);
    m_status->setWordWrap(true);
    shotLayout->addWidget(m_status);
    body->addWidget(shotCard);

    connect(m_camera, qOverload<int>(&QComboBox::activated), this, [this](int)
    {
        ChangePicked(QStringLiteral("camera"), m_camera->currentText());
        ChangePicked(QStringLiteral("cameraId"), m_camera->currentData().toString());
    });
    connect(m_move, &QLineEdit::editingFinished, this, [this]() { ChangePicked(QStringLiteral("move"), m_move->text()); });
    connect(m_lens, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double mm) { ChangePicked(QStringLiteral("lens"), mm); });
    connect(m_length, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double seconds) { ChangePicked(QStringLiteral("length"), seconds); });
    connect(m_line, qOverload<int>(&QComboBox::activated), this, [this](int) { ChangePicked(QStringLiteral("line"), m_line->currentData().toString()); });

    m_clock = new QTimer(this);
    m_clock->setInterval(33);
    connect(m_clock, &QTimer::timeout, this, &HearthCinematicsPane::Tick);
    m_clock->start();
    auto* captures = new QTimer(this);
    captures->setInterval(500);
    connect(captures, &QTimer::timeout, this, &HearthCinematicsPane::Capture);
    captures->start();

    ListCinematics();
}

HearthCinematicsPane::~HearthCinematicsPane()
{
    if (m_fullScreen)
    {
        StopFullScreen();
    }
}

QString HearthCinematicsPane::Folder() const
{
    const auto project = AZ::Utils::GetProjectPath();
    QDir folder(QDir(QString::fromUtf8(project.c_str())).filePath(QStringLiteral("Cinematics")));
    folder.mkpath(QStringLiteral("."));
    return folder.absolutePath();
}

void HearthCinematicsPane::ListCinematics()
{
    m_pick->clear();
    const QStringList files = QDir(Folder()).entryList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
    for (const QString& file : files)
    {
        m_pick->addItem(QFileInfo(file).completeBaseName(), QFileInfo(file).completeBaseName());
    }
    m_pick->addItem(tr("+ New cutscene…"), QString());
    if (!files.isEmpty())
    {
        Load(QFileInfo(files.first()).completeBaseName());
    }
}

void HearthCinematicsPane::NewCinematic()
{
    bool ok = false;
    const QString title = QInputDialog::getText(this, tr("New cutscene"), tr("Name"), QLineEdit::Normal, tr("Intro: the briefing"), &ok).trimmed();
    if (!ok || title.isEmpty())
    {
        return;
    }
    QString name = title.toLower();
    name.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    name = name.section(QLatin1Char('-'), 0, -1, QString::SectionSkipEmpty);
    m_data = QJsonObject{ { QStringLiteral("title"), title }, { QStringLiteral("tracks"), QJsonArray{ QJsonArray(), QJsonArray(), QJsonArray(), QJsonArray() } } };
    m_name = name;
    Save();
    ListCinematics();
    Load(name);
}

void HearthCinematicsPane::Load(const QString& name)
{
    QFile file(QDir(Folder()).filePath(name + QStringLiteral(".json")));
    if (!file.open(QIODevice::ReadOnly))
    {
        return;
    }
    m_data = QJsonDocument::fromJson(file.readAll()).object();
    m_name = name;
    QJsonArray tracks = m_data.value(QStringLiteral("tracks")).toArray();
    while (tracks.size() < 4)
    {
        tracks.append(QJsonArray());
    }
    m_data.insert(QStringLiteral("tracks"), tracks);
    m_pick->setCurrentIndex(qMax(0, m_pick->findData(name)));
    m_clip = -1;
    FollowVoiceLines();
    SyncTrackView();
    Pick(0, m_data.value(QStringLiteral("tracks")).toArray().at(0).toArray().isEmpty() ? -1 : 0);
    SetTime(0.0);
}

void HearthCinematicsPane::Save()
{
    if (m_name.isEmpty())
    {
        return;
    }
    SyncTrackView(); // first: it names the voice and sound entities in the clips
    QFile file(QDir(Folder()).filePath(m_name + QStringLiteral(".json")));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        file.write(QJsonDocument(m_data).toJson(QJsonDocument::Indented));
    }
    m_timeline->m_tracks = m_data.value(QStringLiteral("tracks")).toArray();
    m_timeline->m_length = qMax(10.0, Length());
    m_timeline->update();
    ShowPicked();
    SetTime(qMin(m_time, Length()));
}

double HearthCinematicsPane::Length() const
{
    double end = 0.0;
    for (const QJsonValue track : m_data.value(QStringLiteral("tracks")).toArray())
    {
        for (const QJsonValue clip : track.toArray())
        {
            const QJsonObject c = clip.toObject();
            end = qMax(end, c.value(QStringLiteral("start")).toDouble() + c.value(QStringLiteral("length")).toDouble());
        }
    }
    return end;
}

QJsonArray HearthCinematicsPane::VoiceLines() const
{
    const auto project = AZ::Utils::GetProjectPath();
    QFile file(QDir(QString::fromUtf8(project.c_str())).filePath(QStringLiteral("Voice/voice_lines.json")));
    QJsonArray lines;
    if (!file.open(QIODevice::ReadOnly))
    {
        return lines;
    }
    for (const QJsonValue scene : QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("scenes")).toArray())
    {
        const QString sceneName = scene.toObject().value(QStringLiteral("name")).toString();
        QHash<QString, int> perWho;
        for (const QJsonValue value : scene.toObject().value(QStringLiteral("lines")).toArray())
        {
            QJsonObject line = value.toObject();
            const QString who = line.value(QStringLiteral("who")).toString();
            line.insert(QStringLiteral("scene"), sceneName);
            line.insert(QStringLiteral("label"), QStringLiteral("%1 %2").arg(who).arg(++perWho[who]));
            if (!line.value(QStringLiteral("id")).toString().isEmpty())
            {
                lines.append(line);
            }
        }
    }
    return lines;
}

void HearthCinematicsPane::FollowVoiceLines()
{
    // Voice clips belong to the shot that says them: same start (+0.3 s), length of the audio, and the shot is as
    // long as its line (+0.8 s). Shots follow each other with no gaps, so a longer line pushes the later shots.
    const auto project = AZ::Utils::GetProjectPath();
    const QDir voiceFolder(QDir(QString::fromUtf8(project.c_str())).filePath(QStringLiteral("Voice")));
    QHash<QString, QJsonObject> byId;
    for (const QJsonValue value : VoiceLines())
    {
        byId.insert(value.toObject().value(QStringLiteral("id")).toString(), value.toObject());
    }
    QJsonArray tracks = m_data.value(QStringLiteral("tracks")).toArray();
    QJsonArray shots = tracks.at(0).toArray();
    QJsonArray voices;
    for (const QJsonValue value : tracks.at(1).toArray())
    {
        if (!value.toObject().contains(QStringLiteral("shot")))
        {
            voices.append(value); // a line placed on its own, not from a shot
        }
    }
    double at = 0.0;
    for (int i = 0; i < shots.size(); ++i)
    {
        QJsonObject shot = shots[i].toObject();
        shot.insert(QStringLiteral("start"), at);
        const QString lineId = shot.value(QStringLiteral("line")).toString();
        if (!lineId.isEmpty() && byId.contains(lineId))
        {
            const QJsonObject line = byId.value(lineId);
            const QString audio = line.value(QStringLiteral("made")).toObject().value(QStringLiteral("file")).toString();
            const double seconds = audio.isEmpty() ? 0.0 : CineWavSeconds(voiceFolder.filePath(audio));
            const double spoken = seconds > 0.0 ? seconds : qMax(1.5, line.value(QStringLiteral("text")).toString().size() / 14.0); // not made yet: about 14 letters a second
            shot.insert(QStringLiteral("length"), qMax(shot.value(QStringLiteral("length")).toDouble(), 0.0) > 0.0 && shot.value(QStringLiteral("lengthSetByHand")).toBool()
                                                      ? shot.value(QStringLiteral("length")).toDouble()
                                                      : spoken + 0.8);
            voices.append(QJsonObject{ { QStringLiteral("start"), at + 0.3 },
                                       { QStringLiteral("length"), spoken },
                                       { QStringLiteral("label"), line.value(QStringLiteral("label")).toString() },
                                       { QStringLiteral("line"), lineId },
                                       { QStringLiteral("audio"), audio },
                                       { QStringLiteral("text"), line.value(QStringLiteral("text")).toString() },
                                       { QStringLiteral("shot"), i } });
        }
        if (shot.value(QStringLiteral("length")).toDouble() <= 0.0)
        {
            shot.insert(QStringLiteral("length"), 4.0);
        }
        at += shot.value(QStringLiteral("length")).toDouble();
        shots[i] = shot;
    }
    tracks[0] = shots;
    tracks[1] = voices;
    m_data.insert(QStringLiteral("tracks"), tracks);
    m_timeline->m_tracks = tracks;
    m_timeline->m_length = qMax(10.0, Length());
    m_timeline->update();
}

void HearthCinematicsPane::SyncTrackView()
{
    if (m_name.isEmpty() || !GetIEditor() || GetIEditor()->GetLevelName().isEmpty())
    {
        return;
    }
    CTrackViewSequenceManager* manager = GetIEditor()->GetSequenceManager();
    if (!manager)
    {
        return;
    }
    CUndo undo("Hearth cinematic");
    CTrackViewSequence* sequence = manager->GetSequenceByName(m_name);
    if (!sequence)
    {
        manager->CreateSequence(m_name, SequenceType::SequenceComponent);
        sequence = manager->GetSequenceByName(m_name);
    }
    if (!sequence)
    {
        m_status->setText(tr("Couldn't make the Track View sequence yet; it's tried again on the next change."));
        return;
    }
    sequence->SetTimeRange(Range(0.0f, static_cast<float>(qMax(1.0, Length()))));
    CTrackViewAnimNodeBundle directors = sequence->GetAnimNodesByType(AnimNodeType::Director);
    CTrackViewAnimNode* director = directors.GetCount() > 0 ? directors.GetNode(0) : sequence->CreateSubNode(QStringLiteral("Director"), AnimNodeType::Director);
    if (!director)
    {
        return;
    }
    auto trackFor = [director](AnimParamType type)
    {
        CTrackViewTrack* track = director->GetTrackForParameter(type);
        return track ? track : director->CreateTrack(type);
    };
    auto clear = [](CTrackViewTrack* track)
    {
        while (track && track->GetKeyCount() > 0)
        {
            track->GetKey(track->GetKeyCount() - 1).Delete();
        }
    };

    // Camera shots.
    const QJsonArray tracks = m_data.value(QStringLiteral("tracks")).toArray();
    CTrackViewTrack* cameras = trackFor(AnimParamType::Camera);
    clear(cameras);
    for (const QJsonValue value : tracks.at(0).toArray())
    {
        const QJsonObject shot = value.toObject();
        AZ::Entity* camera = CineFindEntity(shot.value(QStringLiteral("camera")).toString(), shot.value(QStringLiteral("cameraId")).toString());
        if (!cameras || !camera)
        {
            continue;
        }
        CTrackViewKeyHandle key = cameras->CreateKey(static_cast<float>(shot.value(QStringLiteral("start")).toDouble()));
        if (!key.IsValid())
        {
            continue;
        }
        ISelectKey select;
        key.GetKey(&select);
        select.szSelection = camera->GetName();
        select.cameraAzEntityId = camera->GetId();
        select.fBlendTime = 0.0f;
        key.SetKey(&select);
        // The lens sets the camera's field of view.
        using Components = AzToolsFramework::EditorComponentAPIRequests;
        AZStd::vector<AZ::Uuid> types;
        AzToolsFramework::EditorComponentAPIBus::BroadcastResult(types, &Components::FindComponentTypeIdsByEntityType,
                                                                 AZStd::vector<AZStd::string>{ "Camera" }, Components::EntityType::Game);
        if (!types.empty())
        {
            Components::GetComponentOutcome found = AZ::Failure(AZStd::string());
            AzToolsFramework::EditorComponentAPIBus::BroadcastResult(found, &Components::GetComponentOfType, camera->GetId(), types.front());
            if (found.IsSuccess())
            {
                Components::PropertyOutcome set = AZ::Failure(AZStd::string());
                AzToolsFramework::EditorComponentAPIBus::BroadcastResult(set, &Components::SetComponentProperty, found.GetValue(),
                    AZStd::string_view("Controller|Configuration|Field of view"),
                    AZStd::any(static_cast<float>(CineLensToFov(shot.value(QStringLiteral("lens")).toDouble(35.0)))));
            }
        }
    }

    // Voices and sounds play from entities with a MiniAudio Playback component (one per clip, made here so the
    // game has them too); lights flicker or switch through the game's hearth_cine command. Both are console keys:
    // "hearth_cine play Cine_intro_voice_3", "hearth_cine light flicker 1.50 CeilingLight_12".
    QStringList wanted; // python tuples (entity, source file)
    const QString safeName = QString(m_name).replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9]+")), QStringLiteral("_"));
    QJsonArray changed = tracks;
    for (int t = 1; t <= 2; ++t)
    {
        QJsonArray clips = changed.at(t).toArray();
        for (int c = 0; c < clips.size(); ++c)
        {
            QJsonObject clip = clips[c].toObject();
            const QString source = t == 1 ? (clip.value(QStringLiteral("audio")).toString().isEmpty() ? QString() : QStringLiteral("Voice/") + clip.value(QStringLiteral("audio")).toString())
                                          : clip.value(QStringLiteral("file")).toString();
            if (source.isEmpty())
            {
                continue;
            }
            const QString entity = QStringLiteral("Cine_%1_%2_%3").arg(safeName, t == 1 ? QStringLiteral("voice") : QStringLiteral("sound")).arg(c + 1);
            clip.insert(QStringLiteral("entity"), entity);
            clips[c] = clip;
            QString quoted = source;
            quoted.replace(QLatin1Char('\\'), QStringLiteral("/")).replace(QLatin1Char('\''), QStringLiteral("\\'"));
            wanted << QStringLiteral("('%1', '%2')").arg(entity, quoted);
        }
        changed[t] = clips;
    }
    m_data.insert(QStringLiteral("tracks"), changed);
    if (!wanted.isEmpty())
    {
        const QString code = QStringLiteral(
            "import azlmbr.bus as b, azlmbr.editor as ed, azlmbr.entity as ent, azlmbr.asset as asset, azlmbr.math as m\n"
            "wanted = [%1]\n"
            "tid = ed.EditorComponentAPIBus(b.Broadcast, 'FindComponentTypeIdsByEntityType', ['MiniAudio Playback'], ent.EntityType().Game)\n"
            "names = {}\n"
            "for e in ed.ToolsApplicationRequestBus(b.Broadcast, 'GetAllEntities') or []:\n"
            "    names[ed.EditorEntityInfoRequestBus(b.Event, 'GetName', e)] = e\n"
            "for name, source in wanted:\n"
            "    e = names.get(name)\n"
            "    if e is None:\n"
            "        e = ed.ToolsApplicationRequestBus(b.Broadcast, 'CreateNewEntity', ent.EntityId())\n"
            "        ed.EditorEntityAPIBus(b.Event, 'SetName', e, name)\n"
            "    if not tid:\n"
            "        continue\n"
            "    got = ed.EditorComponentAPIBus(b.Broadcast, 'GetComponentOfType', e, tid[0])\n"
            "    comp = got.GetValue() if got.IsSuccess() else ed.EditorComponentAPIBus(b.Broadcast, 'AddComponentsOfType', e, [tid[0]]).GetValue()[0]\n"
            "    aid = asset.AssetCatalogRequestBus(b.Broadcast, 'GetAssetIdByPath', source.lower() + '.miniaudio', m.Uuid(), False)\n"
            "    for p in ed.EditorComponentAPIBus(b.Broadcast, 'BuildComponentPropertyList', comp):\n"
            "        if p.endswith('Sound Asset') and aid and aid.is_valid():\n"
            "            ed.EditorComponentAPIBus(b.Broadcast, 'SetComponentProperty', comp, p, aid)\n"
            "        elif p.endswith('Autoplay'):\n"
            "            ed.EditorComponentAPIBus(b.Broadcast, 'SetComponentProperty', comp, p, False)\n")
            .arg(wanted.join(QStringLiteral(", ")));
        AzToolsFramework::EditorPythonRunnerRequestBus::Broadcast(&AzToolsFramework::EditorPythonRunnerRequests::ExecuteByString,
                                                                  AZStd::string_view(code.toUtf8().constData()), false);
    }

    CTrackViewTrack* console = trackFor(AnimParamType::Console);
    clear(console);
    for (int t = 1; t < 4; ++t)
    {
        for (const QJsonValue value : changed.at(t).toArray())
        {
            const QJsonObject clip = value.toObject();
            QString command;
            if (t < 3 && !clip.value(QStringLiteral("entity")).toString().isEmpty())
            {
                command = QStringLiteral("hearth_cine play %1").arg(clip.value(QStringLiteral("entity")).toString());
            }
            else if (t == 3)
            {
                command = QStringLiteral("hearth_cine light %1 %2 %3").arg(clip.value(QStringLiteral("effect")).toString(QStringLiteral("flicker")))
                              .arg(clip.value(QStringLiteral("length")).toDouble(), 0, 'f', 2)
                              .arg(clip.value(QStringLiteral("target")).toString(QStringLiteral("*")));
            }
            if (!console || command.isEmpty())
            {
                continue;
            }
            CTrackViewKeyHandle key = console->CreateKey(static_cast<float>(clip.value(QStringLiteral("start")).toDouble()));
            if (key.IsValid())
            {
                IConsoleKey consoleKey;
                key.GetKey(&consoleKey);
                consoleKey.command = command.toUtf8().constData();
                key.SetKey(&consoleKey);
            }
        }
    }
    m_status->setText(tr("Saved. Track View sequence \"%1\" follows these tracks, so it plays in the game too.").arg(m_name));
}

void HearthCinematicsPane::AddCameraShot()
{
    if (!GetIEditor() || GetIEditor()->GetLevelName().isEmpty())
    {
        m_status->setText(tr("Open a level first."));
        return;
    }
    if (m_name.isEmpty())
    {
        NewCinematic();
        if (m_name.isEmpty())
        {
            return;
        }
    }
    QJsonArray tracks = m_data.value(QStringLiteral("tracks")).toArray();
    QJsonArray shots = tracks.at(0).toArray();
    const int number = shots.size() + 1;
    // A camera where the viewport is looking from now.
    AzToolsFramework::ScopedUndoBatch undo("Add camera shot");
    AZ::EntityId cameraId;
    AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(
        cameraId, &AzToolsFramework::EditorEntityContextRequests::CreateNewEditorEntity, QStringLiteral("ShotCam_%1").arg(number).toUtf8().constData());
    if (!cameraId.IsValid())
    {
        m_status->setText(tr("Couldn't make the camera."));
        return;
    }
    using Components = AzToolsFramework::EditorComponentAPIRequests;
    AZStd::vector<AZ::Uuid> types;
    AzToolsFramework::EditorComponentAPIBus::BroadcastResult(types, &Components::FindComponentTypeIdsByEntityType,
                                                             AZStd::vector<AZStd::string>{ "Camera" }, Components::EntityType::Game);
    if (!types.empty() && !types.front().IsNull())
    {
        Components::AddComponentsOutcome added = AZ::Failure(AZStd::string());
        AzToolsFramework::EditorComponentAPIBus::BroadcastResult(added, &Components::AddComponentOfType, cameraId, types.front());
    }
    AZ::TransformBus::Event(cameraId, &AZ::TransformBus::Events::SetWorldTM, SandboxEditor::GetDefaultViewportCameraTransform());
    AZ::Entity* camera = nullptr;
    AZ::ComponentApplicationBus::BroadcastResult(camera, &AZ::ComponentApplicationRequests::FindEntity, cameraId);
    shots.append(QJsonObject{ { QStringLiteral("label"), tr("Shot %1").arg(number) },
                              { QStringLiteral("camera"), camera ? QString::fromUtf8(camera->GetName().c_str()) : QString() },
                              { QStringLiteral("cameraId"), QString::number(static_cast<AZ::u64>(cameraId)) },
                              { QStringLiteral("move"), QString() },
                              { QStringLiteral("lens"), 35.0 },
                              { QStringLiteral("length"), 4.0 },
                              { QStringLiteral("line"), QString() } });
    tracks[0] = shots;
    m_data.insert(QStringLiteral("tracks"), tracks);
    FollowVoiceLines();
    Save();
    Pick(0, shots.size() - 1);
    const QJsonObject added = m_data.value(QStringLiteral("tracks")).toArray().at(0).toArray().last().toObject();
    SetTime(added.value(QStringLiteral("start")).toDouble());
}

void HearthCinematicsPane::AddClip(int track, double seconds)
{
    if (m_name.isEmpty())
    {
        return;
    }
    if (track == 0)
    {
        AddCameraShot();
        return;
    }
    QJsonObject clip{ { QStringLiteral("start"), seconds } };
    QMenu menu(this);
    if (track == 1)
    {
        // A line on its own (not tied to a shot).
        for (const QJsonValue value : VoiceLines())
        {
            const QJsonObject line = value.toObject();
            QAction* action = menu.addAction(QStringLiteral("%1 · %2").arg(line.value(QStringLiteral("label")).toString(), line.value(QStringLiteral("text")).toString().left(50)));
            action->setData(line);
        }
        if (menu.isEmpty())
        {
            m_status->setText(tr("No lines yet: write them in Voice Studio first."));
            return;
        }
        QAction* picked = menu.exec(QCursor::pos());
        if (!picked)
        {
            return;
        }
        const QJsonObject line = picked->data().toJsonObject();
        const QString audio = line.value(QStringLiteral("made")).toObject().value(QStringLiteral("file")).toString();
        const auto project = AZ::Utils::GetProjectPath();
        const double length = CineWavSeconds(QDir(QString::fromUtf8(project.c_str())).filePath(QStringLiteral("Voice/") + audio));
        clip.insert(QStringLiteral("label"), line.value(QStringLiteral("label")).toString());
        clip.insert(QStringLiteral("line"), line.value(QStringLiteral("id")).toString());
        clip.insert(QStringLiteral("audio"), audio);
        clip.insert(QStringLiteral("text"), line.value(QStringLiteral("text")).toString());
        clip.insert(QStringLiteral("length"), length > 0.0 ? length : 2.0);
    }
    else if (track == 2)
    {
        const auto project = AZ::Utils::GetProjectPath();
        const QString root = QString::fromUtf8(project.c_str());
        const QString file = QFileDialog::getOpenFileName(this, tr("Sound for the cutscene"), QDir(root).filePath(QStringLiteral("Sounds")),
                                                          tr("Sounds (*.wav *.ogg *.flac *.mp3)"));
        if (file.isEmpty())
        {
            return;
        }
        const QString relative = QDir(root).relativeFilePath(file);
        if (relative.startsWith(QLatin1String("..")))
        {
            m_status->setText(tr("Pick a sound inside the project (Import brings one in)."));
            return;
        }
        const double length = CineWavSeconds(file);
        clip.insert(QStringLiteral("label"), QFileInfo(file).completeBaseName().replace(QLatin1Char('_'), QLatin1Char(' ')));
        clip.insert(QStringLiteral("file"), relative);
        clip.insert(QStringLiteral("length"), length > 0.0 ? length : 5.0);
    }
    else
    {
        const struct
        {
            const char* effect;
            const char* label;
        } effects[] = { { "flicker", "Power flicker" }, { "off", "Lights go out" }, { "on", "Lights come back" }, { "dim", "Lights dim" } };
        for (const auto& effect : effects)
        {
            menu.addAction(tr(effect.label))->setData(QString::fromLatin1(effect.effect));
        }
        QAction* picked = menu.exec(QCursor::pos());
        if (!picked)
        {
            return;
        }
        // Which lights: the selected entities, or every light near the camera ("*") when nothing is selected.
        AzToolsFramework::EntityIdList selected;
        AzToolsFramework::ToolsApplicationRequestBus::BroadcastResult(selected, &AzToolsFramework::ToolsApplicationRequests::GetSelectedEntities);
        QString target = QStringLiteral("*");
        if (!selected.empty())
        {
            AZStd::string name;
            AZ::ComponentApplicationBus::BroadcastResult(name, &AZ::ComponentApplicationRequests::GetEntityName, selected.front());
            target = QString::fromUtf8(name.c_str()).replace(QLatin1Char(' '), QLatin1Char('_'));
        }
        clip.insert(QStringLiteral("label"), picked->text());
        clip.insert(QStringLiteral("effect"), picked->data().toString());
        clip.insert(QStringLiteral("target"), target);
        clip.insert(QStringLiteral("length"), picked->data().toString() == QLatin1String("flicker") ? 1.5 : 0.5);
    }
    QJsonArray tracks = m_data.value(QStringLiteral("tracks")).toArray();
    QJsonArray clips = tracks.at(track).toArray();
    clips.append(clip);
    tracks[track] = clips;
    m_data.insert(QStringLiteral("tracks"), tracks);
    FollowVoiceLines();
    Save();
    Pick(track, track == 1 ? m_data.value(QStringLiteral("tracks")).toArray().at(1).toArray().size() - 1 : clips.size() - 1);
}

void HearthCinematicsPane::Pick(int track, int clip)
{
    m_track = track;
    m_clip = clip;
    m_timeline->m_track = track;
    m_timeline->m_clip = clip;
    m_timeline->update();
    ShowPicked();
}

void HearthCinematicsPane::ShowPicked()
{
    const QJsonArray clips = m_data.value(QStringLiteral("tracks")).toArray().at(m_track).toArray();
    const bool valid = m_clip >= 0 && m_clip < clips.size();
    const QJsonObject clip = valid ? clips[m_clip].toObject() : QJsonObject();
    const bool shot = valid && m_track == 0;
    for (QWidget* field : { static_cast<QWidget*>(m_camera), static_cast<QWidget*>(m_move), static_cast<QWidget*>(m_lens),
                            static_cast<QWidget*>(m_length), static_cast<QWidget*>(m_line) })
    {
        field->setEnabled(shot || (valid && field == m_length && m_track > 1));
    }
    m_shotTitle->setText(valid ? (shot ? tr("Shot %1").arg(m_clip + 1) : clip.value(QStringLiteral("label")).toString()) : tr("Pick a shot"));

    const QSignalBlocker b1(m_camera), b2(m_lens), b3(m_length), b4(m_line);
    m_camera->clear();
    AzFramework::EntityContextId editor = AzFramework::EntityContextId::CreateNull();
    AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(editor, &AzToolsFramework::EditorEntityContextRequests::GetEditorEntityContextId);
    using Components = AzToolsFramework::EditorComponentAPIRequests;
    AZStd::vector<AZ::Uuid> types;
    AzToolsFramework::EditorComponentAPIBus::BroadcastResult(types, &Components::FindComponentTypeIdsByEntityType,
                                                             AZStd::vector<AZStd::string>{ "Camera" }, Components::EntityType::Game);
    AZ::ComponentApplicationBus::Broadcast(&AZ::ComponentApplicationRequests::EnumerateEntities, [this, &types](AZ::Entity* entity)
    {
        bool editorEntity = false;
        AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(editorEntity, &AzToolsFramework::EditorEntityContextRequests::IsEditorEntity, entity->GetId());
        bool hasCamera = false;
        if (editorEntity && !types.empty())
        {
            AzToolsFramework::EditorComponentAPIBus::BroadcastResult(hasCamera, &Components::HasComponentOfType, entity->GetId(), types.front());
        }
        if (hasCamera)
        {
            m_camera->addItem(QString::fromUtf8(entity->GetName().c_str()), QString::number(static_cast<AZ::u64>(entity->GetId())));
        }
    });
    m_camera->setCurrentIndex(m_camera->findText(clip.value(QStringLiteral("camera")).toString()));
    m_move->setText(clip.value(QStringLiteral("move")).toString());
    m_lens->setValue(clip.value(QStringLiteral("lens")).toDouble(35.0));
    m_length->setValue(clip.value(QStringLiteral("length")).toDouble(4.0));
    m_line->clear();
    m_line->addItem(tr("No line"), QString());
    for (const QJsonValue value : VoiceLines())
    {
        const QJsonObject line = value.toObject();
        m_line->addItem(tr("%1 (Voice Studio)").arg(line.value(QStringLiteral("label")).toString()), line.value(QStringLiteral("id")).toString());
        m_line->setItemData(m_line->count() - 1, line.value(QStringLiteral("text")).toString(), Qt::ToolTipRole);
    }
    m_line->setCurrentIndex(qMax(0, m_line->findData(clip.value(QStringLiteral("line")).toString())));
}

void HearthCinematicsPane::ChangePicked(const QString& key, const QJsonValue& value)
{
    QJsonArray tracks = m_data.value(QStringLiteral("tracks")).toArray();
    QJsonArray clips = tracks.at(m_track).toArray();
    if (m_clip < 0 || m_clip >= clips.size())
    {
        return;
    }
    QJsonObject clip = clips[m_clip].toObject();
    if (clip.value(key) == value)
    {
        return;
    }
    clip.insert(key, value);
    if (key == QLatin1String("length"))
    {
        clip.insert(QStringLiteral("lengthSetByHand"), true);
    }
    if (key == QLatin1String("line"))
    {
        clip.insert(QStringLiteral("lengthSetByHand"), false); // the line decides again
    }
    clips[m_clip] = clip;
    tracks[m_track] = clips;
    m_data.insert(QStringLiteral("tracks"), tracks);
    FollowVoiceLines();
    Save();
}

void HearthCinematicsPane::SetTime(double seconds, bool fromPlayback)
{
    const double previous = m_time;
    m_time = qBound(0.0, seconds, qMax(0.0, Length()));
    m_timeline->m_time = m_time;
    m_timeline->update();
    m_timeText->setText(QStringLiteral("%1 / %2").arg(CineClock(m_time), CineClock(Length())));
    const QString title = m_data.value(QStringLiteral("title")).toString(m_name);
    if (m_subtitle)
    {
        m_subtitle->setText(m_name.isEmpty() ? tr("No cutscene yet") : QStringLiteral("%1 · %2").arg(title, CineClock(Length())));
    }

    // The line being said and the shot being shown.
    const QJsonArray tracks = m_data.value(QStringLiteral("tracks")).toArray();
    m_preview->m_subtitle.clear();
    m_preview->m_caption.clear();
    const auto project = AZ::Utils::GetProjectPath();
    const QDir voiceFolder(QDir(QString::fromUtf8(project.c_str())).filePath(QStringLiteral("Voice")));
    for (const QJsonValue value : tracks.at(1).toArray())
    {
        const QJsonObject voice = value.toObject();
        const double start = voice.value(QStringLiteral("start")).toDouble();
        if (m_time >= start && m_time < start + voice.value(QStringLiteral("length")).toDouble())
        {
            m_preview->m_subtitle = voice.value(QStringLiteral("text")).toString();
        }
        if (fromPlayback && previous < start && m_time >= start && !voice.value(QStringLiteral("audio")).toString().isEmpty())
        {
            PlayAudio(voiceFolder.filePath(voice.value(QStringLiteral("audio")).toString()));
        }
    }
    if (fromPlayback)
    {
        for (const QJsonValue value : tracks.at(2).toArray())
        {
            const QJsonObject sound = value.toObject();
            const double start = sound.value(QStringLiteral("start")).toDouble();
            if (previous < start && m_time >= start && !sound.value(QStringLiteral("file")).toString().isEmpty())
            {
                PlayAudio(QDir(QString::fromUtf8(project.c_str())).filePath(sound.value(QStringLiteral("file")).toString()));
            }
        }
    }
    const QJsonArray shots = tracks.at(0).toArray();
    for (int i = 0; i < shots.size(); ++i)
    {
        const QJsonObject shot = shots[i].toObject();
        const double start = shot.value(QStringLiteral("start")).toDouble();
        if (m_time >= start && m_time < start + shot.value(QStringLiteral("length")).toDouble())
        {
            const QString move = shot.value(QStringLiteral("move")).toString();
            m_preview->m_caption = move.isEmpty() ? tr("Shot %1").arg(i + 1) : tr("Shot %1 · %2").arg(i + 1).arg(move);
        }
    }
    m_preview->update();

    // Move the real sequence there, so the viewport looks through the shot's camera.
    if (GetIEditor() && GetIEditor()->GetSequenceManager() && GetIEditor()->GetAnimation())
    {
        if (CTrackViewSequence* sequence = GetIEditor()->GetSequenceManager()->GetSequenceByName(m_name))
        {
            CAnimationContext* context = GetIEditor()->GetAnimation();
            if (context->GetSequence() != sequence)
            {
                context->SetSequence(sequence, false, false, true);
            }
            context->SetTime(static_cast<float>(m_time));
        }
    }
}

void HearthCinematicsPane::Tick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_playing)
    {
        m_lastTick = now;
        return;
    }
    const double next = m_time + (now - m_lastTick) / 1000.0;
    m_lastTick = now;
    if (next >= Length())
    {
        SetTime(Length(), true);
        m_playing = false;
        m_play->setText(QStringLiteral("▶"));
        if (m_fullScreen)
        {
            StopFullScreen();
        }
        return;
    }
    SetTime(next, true);
}

void HearthCinematicsPane::Capture()
{
    if (m_capturing || !isVisible() || m_fullScreen || !GetIEditor() || GetIEditor()->GetLevelName().isEmpty())
    {
        return;
    }
    bool canCapture = false;
    AZ::Render::FrameCaptureRequestBus::BroadcastResult(canCapture, &AZ::Render::FrameCaptureRequestBus::Events::CanCapture);
    if (!canCapture)
    {
        return;
    }
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/hearth-cinematics");
    QDir().mkpath(folder);
    const QString path = folder + QStringLiteral("/frame.png");
    QFile::remove(path);
    AZ::Render::FrameCaptureOutcome outcome;
    AZ::Render::FrameCaptureRequestBus::BroadcastResult(outcome, &AZ::Render::FrameCaptureRequestBus::Events::CaptureScreenshot, AZStd::string(path.toUtf8().constData()));
    if (!outcome.IsSuccess())
    {
        return;
    }
    m_capturing = true;
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto* poll = new QTimer(this);
    poll->setInterval(50);
    connect(poll, &QTimer::timeout, this, [this, poll, path, clock]()
    {
        if (QFileInfo(path).size() > 0 || clock->elapsed() > 2000)
        {
            poll->stop();
            poll->deleteLater();
            m_capturing = false;
            const QImage image(path);
            if (!image.isNull())
            {
                m_preview->m_image = image;
                m_preview->update();
            }
        }
    });
    poll->start();
}

void HearthCinematicsPane::PlayAudio(const QString& path)
{
    if (!QFileInfo::exists(path))
    {
        return;
    }
    for (const QString& player : { QStringLiteral("pw-play"), QStringLiteral("paplay"), QStringLiteral("aplay") })
    {
        if (!QStandardPaths::findExecutable(player).isEmpty())
        {
            QProcess::startDetached(player, { path });
            return;
        }
    }
}

void HearthCinematicsPane::WatchFullScreen()
{
    if (m_name.isEmpty() || Length() <= 0.0)
    {
        m_status->setText(tr("Add a camera shot first."));
        return;
    }
    // The editor goes full screen with only the viewport, and plays the cutscene from the start. Esc stops.
    auto* main = qobject_cast<QMainWindow*>(MainWindow::instance());
    if (!main)
    {
        return;
    }
    m_windowState = main->saveState();
    for (QDockWidget* dock : main->findChildren<QDockWidget*>())
    {
        if (dock->isVisible())
        {
            dock->hide();
        }
    }
    if (main->menuBar())
    {
        main->menuBar()->hide();
    }
    main->showFullScreen();
    m_fullScreen = true;
    main->grabKeyboard();
    main->installEventFilter(this);
    SetTime(0.0);
    m_playing = true;
    m_lastTick = QDateTime::currentMSecsSinceEpoch();
    m_play->setText(QStringLiteral("❚❚"));
}

void HearthCinematicsPane::StopFullScreen()
{
    auto* main = qobject_cast<QMainWindow*>(MainWindow::instance());
    m_fullScreen = false;
    m_playing = false;
    m_play->setText(QStringLiteral("▶"));
    if (!main)
    {
        return;
    }
    main->releaseKeyboard();
    main->removeEventFilter(this);
    if (main->menuBar())
    {
        main->menuBar()->show();
    }
    main->showMaximized();
    main->restoreState(m_windowState);
}

bool HearthCinematicsPane::eventFilter(QObject* watched, QEvent* event)
{
    // Full screen: the editor window has the keyboard; Esc comes back, Space pauses.
    if (m_fullScreen && event->type() == QEvent::KeyPress)
    {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Escape)
        {
            StopFullScreen();
            return true;
        }
        if (key == Qt::Key_Space)
        {
            m_play->click();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void HearthCinematicsPane::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && m_fullScreen)
    {
        StopFullScreen();
        return;
    }
    if (event->key() == Qt::Key_Space)
    {
        m_play->click();
        return;
    }
    QWidget::keyPressEvent(event);
}
