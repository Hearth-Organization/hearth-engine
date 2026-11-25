/*
 * Hearth Engine first-run setup. Built on O3DE (Apache-2.0 OR MIT). See FirstRunDialog.h.
 */

#include <FirstRunDialog.h>

#include <AzQtComponents/Components/HearthWidgets.h>

#include <QAbstractButton>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStackedWidget>
#include <QStorageInfo>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace O3DE::ProjectManager
{
    namespace Hearth = AzQtComponents::Hearth;

    namespace
    {
        const char* StepNames[] = { "Welcome", "Engine and folders", "Your laptop", "Voice Studio", "Claude and logs", "Ready" };
        constexpr int StepCount = 6;

        QString RunQuiet(const QString& program, const QStringList& args)
        {
            QProcess process;
            process.start(program, args);
            if (!process.waitForFinished(3000))
            {
                process.kill();
                return {};
            }
            return QString::fromUtf8(process.readAllStandardOutput());
        }

        QWidget* HardwareTile(QWidget* parent, const QString& key, const QString& value, const QString& note)
        {
            auto* tile = new QFrame(parent);
            tile->setObjectName(QStringLiteral("HwTile"));
            tile->setStyleSheet(QStringLiteral(
                "QFrame#HwTile { background-color: rgba(255, 255, 255, 8); border: 1px solid rgba(255, 255, 255, 15); border-radius: 12px; }"));
            auto* layout = new QVBoxLayout(tile);
            layout->setContentsMargins(12, 12, 12, 12);
            layout->setSpacing(4);
            layout->addWidget(Hearth::MakeLabel(key, QStringLiteral("small"), tile));
            QLabel* big = Hearth::MakeLabel(value, QStringLiteral("title"), tile);
            big->setStyleSheet(QStringLiteral("background: transparent; border: none; color: #ECE6DE; font-size: 15px; font-weight: 600;"));
            layout->addWidget(big);
            layout->addWidget(Hearth::MakeLabel(note, QStringLiteral("small"), tile));
            return tile;
        }
    } // namespace

    bool FirstRunDialog::ShouldShow()
    {
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        return !settings->value(QStringLiteral("firstRun/done"), false).toBool();
    }

    FirstRunDialog::Hardware FirstRunDialog::DetectHardware()
    {
        Hardware hw;
        hw.cores = qMax(1, QThread::idealThreadCount());

        QFile cpuinfo(QStringLiteral("/proc/cpuinfo"));
        if (cpuinfo.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            for (const QString& line : QString::fromUtf8(cpuinfo.readAll()).split(QLatin1Char('\n')))
            {
                if (line.startsWith(QLatin1String("model name")))
                {
                    QString name = line.section(QLatin1Char(':'), 1).trimmed();
                    // "Intel(R) Core(TM) i3-N305" -> "Intel i3-N305"
                    name.remove(QStringLiteral("(R)")).remove(QStringLiteral("(TM)")).remove(QStringLiteral(" CPU"));
                    name.replace(QStringLiteral("Core "), QString());
                    name = name.section(QLatin1Char('@'), 0, 0).simplified();
                    hw.cpuName = name;
                    break;
                }
            }
        }

        QFile meminfo(QStringLiteral("/proc/meminfo"));
        if (meminfo.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            for (const QString& line : QString::fromUtf8(meminfo.readAll()).split(QLatin1Char('\n')))
            {
                const QStringList parts = line.simplified().split(QLatin1Char(' '));
                if (parts.size() >= 2 && parts[0] == QLatin1String("MemTotal:"))
                {
                    hw.memoryGb = parts[1].toDouble() / (1024.0 * 1024.0);
                }
                else if (parts.size() >= 2 && parts[0] == QLatin1String("SwapTotal:"))
                {
                    hw.swapGb = parts[1].toDouble() / (1024.0 * 1024.0);
                }
            }
        }

        // Graphics: the VGA line from lspci ("Intel Corporation Alder Lake-N [UHD Graphics]" -> "Intel UHD Graphics").
        const QString lspci = RunQuiet(QStringLiteral("lspci"), {});
        for (const QString& line : lspci.split(QLatin1Char('\n')))
        {
            if (line.contains(QLatin1String("VGA")) || line.contains(QLatin1String("3D controller")))
            {
                QString name = line.section(QLatin1Char(':'), 2).trimmed();
                const QRegularExpressionMatch bracket = QRegularExpression(QStringLiteral("\\[([^\\]]+)\\]")).match(name);
                const QString vendor = name.section(QLatin1Char(' '), 0, 0);
                hw.gpuName = bracket.hasMatch() ? vendor + QLatin1Char(' ') + bracket.captured(1) : name;
                hw.gpuName.replace(QStringLiteral(" Graphics"), QString());
                if (!line.contains(QLatin1String("Intel")))
                {
                    break; // a separate graphics card wins over the built-in one
                }
            }
        }
        if (hw.gpuName.isEmpty())
        {
            hw.gpuName = QObject::tr("Graphics");
        }
        const QString vulkan = RunQuiet(QStringLiteral("vulkaninfo"), { QStringLiteral("--summary") });
        const QRegularExpressionMatch api = QRegularExpression(QStringLiteral("apiVersion\\s*=\\s*(\\d+)\\.(\\d+)")).match(vulkan);
        hw.gpuApi = api.hasMatch() ? QStringLiteral("Vulkan %1.%2").arg(api.captured(1), api.captured(2)) : QStringLiteral("Vulkan");

        const QStorageInfo storage(QDir::homePath());
        hw.freeDiskGb = storage.bytesAvailable() / (1024.0 * 1024.0 * 1024.0);
        hw.diskPath = QDir::homePath();
        return hw;
    }

    FirstRunDialog::FirstRunDialog(QWidget* parent)
        : QDialog(parent)
        , m_hardware(DetectHardware())
    {
        setWindowTitle(tr("Hearth Engine setup"));
        setWindowIcon(Hearth::AppIcon());
        setModal(true);
        setMinimumSize(1100, 720);
        setStyleSheet(QStringLiteral("QDialog { background-color: #0D0C0B; }"));

        // Pick the preset that suits this computer.
        if (m_hardware.memoryGb >= 15.0 && m_hardware.cores >= 8)
        {
            m_preset = Preset::Full;
        }
        else if (m_hardware.memoryGb >= 11.0)
        {
            m_preset = Preset::Balanced;
        }

        auto* outer = new QHBoxLayout(this);
        outer->setContentsMargins(30, 30, 30, 30);
        outer->setSpacing(10);

        // Left: the steps.
        QVBoxLayout* stepsLayout = nullptr;
        QFrame* stepsCard = Hearth::MakeCard(this, &stepsLayout);
        stepsCard->setFixedWidth(240);
        auto* brand = new QHBoxLayout();
        auto* tile = new QLabel(stepsCard);
        tile->setPixmap(Hearth::FlameTile(30, devicePixelRatioF()));
        tile->setFixedSize(30, 30);
        tile->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        brand->addWidget(tile);
        QLabel* brandName = Hearth::MakeLabel(tr("Hearth Engine %1").arg(Hearth::Version()), QStringLiteral("title"), stepsCard);
        brandName->setStyleSheet(QStringLiteral("background: transparent; border: none; color: #ECE6DE; font-size: 15px; font-weight: 600;"));
        brand->addWidget(brandName, 1);
        stepsLayout->addLayout(brand);
        m_stepCounter = Hearth::MakeLabel(QString(), QStringLiteral("dim"), stepsCard);
        stepsLayout->addWidget(m_stepCounter);
        for (int i = 0; i < StepCount; ++i)
        {
            auto* row = new QWidget(stepsCard);
            row->setObjectName(QStringLiteral("StepRow"));
            auto* rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(10, 9, 10, 9);
            rowLayout->setSpacing(10);
            auto* number = new QLabel(QString::number(i + 1), row);
            number->setFixedSize(22, 22);
            number->setAlignment(Qt::AlignCenter);
            rowLayout->addWidget(number);
            auto* name = new QLabel(tr(StepNames[i]), row);
            rowLayout->addWidget(name, 1);
            stepsLayout->addWidget(row);
            m_stepRows.push_back(row);
            m_stepNumbers.push_back(number);
            m_stepNames.push_back(name);
        }
        stepsLayout->addStretch(1);
        outer->addWidget(stepsCard);

        // Right: the current step.
        QVBoxLayout* mainLayout = nullptr;
        QFrame* mainCard = Hearth::MakeCard(this, &mainLayout);
        mainLayout->setContentsMargins(26, 26, 26, 26);
        m_pages = new QStackedWidget(mainCard);
        m_pages->setStyleSheet(QStringLiteral("QStackedWidget { background: transparent; border: none; }"));
        m_pages->addWidget(BuildWelcome());
        m_pages->addWidget(BuildFolders());
        m_pages->addWidget(BuildLaptop());
        m_pages->addWidget(BuildVoice());
        m_pages->addWidget(BuildClaude());
        m_pages->addWidget(BuildReady());
        mainLayout->addWidget(m_pages, 1);

        auto* buttons = new QHBoxLayout();
        buttons->setSpacing(8);
        m_back = Hearth::MakeButton(tr("Back"), false, mainCard);
        m_skip = Hearth::MakeButton(tr("Skip setup"), false, mainCard);
        m_next = Hearth::MakeButton(QString(), true, mainCard);
        buttons->addWidget(m_back);
        buttons->addStretch(1);
        buttons->addWidget(m_skip);
        buttons->addWidget(m_next);
        mainLayout->addLayout(buttons);
        outer->addWidget(mainCard, 1);

        connect(m_back, &QPushButton::clicked, this, [this]() { GoTo(m_step - 1); });
        connect(m_skip, &QPushButton::clicked, this, [this]()
        {
            Save(true);
            accept();
        });
        connect(m_next, &QPushButton::clicked, this, [this]()
        {
            if (m_step == StepCount - 1)
            {
                Save(false);
                accept();
            }
            else
            {
                GoTo(m_step + 1);
            }
        });

        SelectPreset(m_preset);
        GoTo(0);
    }

    QWidget* FirstRunDialog::Page(const QString& title, const QString& description, QWidget* content)
    {
        auto* page = new QWidget();
        page->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);
        QLabel* heading = Hearth::MakeLabel(title, QStringLiteral("big"), page);
        heading->setStyleSheet(QStringLiteral("background: transparent; border: none; color: #ECE6DE; font-size: 24px; font-weight: 600;"));
        layout->addWidget(heading);
        QLabel* text = Hearth::MakeLabel(description, QStringLiteral("dim"), page);
        text->setStyleSheet(QStringLiteral("background: transparent; border: none; color: #8D857B; font-size: 13px;"));
        layout->addWidget(text);
        if (content)
        {
            content->setParent(page);
            layout->addWidget(content);
        }
        layout->addStretch(1);
        return page;
    }

    QWidget* FirstRunDialog::BuildWelcome()
    {
        auto* content = new QWidget();
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(0, 6, 0, 0);
        layout->setSpacing(8);
        const QStringList points = {
            tr("The Hub: your games, their builds, and the engine parts each one uses."),
            tr("The editor in Hearth's own look, with the Level Generator, Voice Studio and co-op testing built in."),
            tr("A log of every run, so problems can be read afterwards, by you or by Claude."),
        };
        for (const QString& point : points)
        {
            auto* row = new QHBoxLayout();
            auto* flame = new QLabel();
            flame->setPixmap(Hearth::Flame(14, Hearth::Ember(), devicePixelRatioF()));
            flame->setFixedSize(14, 14);
            flame->setStyleSheet(QStringLiteral("background: transparent;"));
            row->addWidget(flame, 0, Qt::AlignTop);
            row->addWidget(Hearth::MakeLabel(point, QStringLiteral("body")), 1);
            layout->addLayout(row);
        }
        return Page(tr("Welcome to Hearth Engine"),
                    tr("A few quick questions so the engine fits this computer. It takes about a minute, and you can change "
                       "everything later in Settings."),
                    content);
    }

    QWidget* FirstRunDialog::BuildFolders()
    {
        auto* content = new QWidget();
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        m_projectsFolder = new QLineEdit(settings->value(QStringLiteral("folders/projects"),
                                                         QDir::homePath() + QStringLiteral("/O3DE/Projects")).toString());
        m_projectsFolder->setMinimumWidth(360);
        auto* browseRow = new QWidget();
        auto* browseLayout = new QHBoxLayout(browseRow);
        browseLayout->setContentsMargins(0, 0, 0, 0);
        browseLayout->addWidget(m_projectsFolder, 1);
        QPushButton* browse = Hearth::MakeButton(tr("Choose…"), false);
        browseLayout->addWidget(browse);
        connect(browse, &QPushButton::clicked, this, [this]()
        {
            const QString folder = QFileDialog::getExistingDirectory(this, tr("Projects folder"), m_projectsFolder->text());
            if (!folder.isEmpty())
            {
                m_projectsFolder->setText(folder);
            }
        });

        QLabel* engine = Hearth::MakeLabel(QDir::toNativeSeparators(Hearth::EngineRoot()), QStringLiteral("mono"));
        QLabel* logs = Hearth::MakeLabel(QDir::toNativeSeparators(Hearth::LogsFolder()), QStringLiteral("mono"));
        layout->addWidget(Hearth::MakeSettingRow(tr("Engine"), tr("Where Hearth Engine %1 is installed").arg(Hearth::Version()), engine));
        layout->addWidget(Hearth::MakeSettingRow(tr("Projects folder"), tr("New games go here"), browseRow));
        layout->addWidget(Hearth::MakeSettingRow(tr("Logs"), tr("Every run of the engine writes a log here"), logs));
        return Page(tr("Engine and folders"), tr("Where things live on this computer."), content);
    }

    QWidget* FirstRunDialog::BuildLaptop()
    {
        auto* content = new QWidget();
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(10);

        auto* tiles = new QHBoxLayout();
        tiles->setSpacing(8);
        tiles->addWidget(HardwareTile(content, tr("Processor"), m_hardware.cpuName.isEmpty() ? tr("Processor") : m_hardware.cpuName,
                                      tr("%n core(s)", nullptr, m_hardware.cores)));
        tiles->addWidget(HardwareTile(content, tr("Graphics"), m_hardware.gpuName, m_hardware.gpuApi));
        tiles->addWidget(HardwareTile(content, tr("Memory"), tr("%1 GB").arg(QString::number(m_hardware.memoryGb, 'f', 1)),
                                      m_hardware.swapGb >= 0.5 ? tr("+ %1 GB swap").arg(QString::number(m_hardware.swapGb, 'f', 0)) : tr("no swap")));
        tiles->addWidget(HardwareTile(content, tr("Free disk"), tr("%1 GB").arg(QString::number(m_hardware.freeDiskGb, 'f', 0)), m_hardware.diskPath));
        layout->addLayout(tiles);

        QLabel* question = Hearth::MakeLabel(tr("How hard should the engine push?"), QStringLiteral("title"), content);
        layout->addSpacing(6);
        layout->addWidget(question);

        struct PresetText
        {
            Preset preset;
            QString title;
            QString about;
        };
        const QVector<PresetText> presets = {
            { Preset::Light, tr("Light"), tr("Viewport at 50% scale, %1 build jobs, asset processing paused while you play.").arg(JobsFor(Preset::Light)) },
            { Preset::Balanced, tr("Balanced"), tr("Full viewport, %1 build jobs, shadows on.").arg(JobsFor(Preset::Balanced)) },
            { Preset::Full, tr("Full power"), tr("For a big PC: everything on, %1 build jobs.").arg(JobsFor(Preset::Full)) },
        };
        const Preset suggested = m_preset;
        auto* cards = new QHBoxLayout();
        cards->setSpacing(8);
        for (const PresetText& text : presets)
        {
            auto* card = new QPushButton(content);
            card->setCheckable(true);
            card->setCursor(Qt::PointingHandCursor);
            card->setMinimumHeight(96);
            card->setProperty("preset", static_cast<int>(text.preset));
            auto* cardLayout = new QVBoxLayout(card);
            cardLayout->setContentsMargins(14, 12, 14, 12);
            cardLayout->setSpacing(6);
            auto* top = new QHBoxLayout();
            QLabel* name = Hearth::MakeLabel(text.title, QStringLiteral("title"), card);
            name->setAttribute(Qt::WA_TransparentForMouseEvents);
            top->addWidget(name);
            if (text.preset == suggested)
            {
                QLabel* chip = Hearth::MakeChip(tr("Picked for you"), QStringLiteral("ember"), card);
                chip->setAttribute(Qt::WA_TransparentForMouseEvents);
                top->addWidget(chip);
            }
            top->addStretch(1);
            cardLayout->addLayout(top);
            QLabel* about = Hearth::MakeLabel(text.about, QStringLiteral("dim"), card);
            about->setAttribute(Qt::WA_TransparentForMouseEvents);
            cardLayout->addWidget(about);
            cardLayout->addStretch(1);
            connect(card, &QPushButton::clicked, this, [this, p = text.preset]() { SelectPreset(p); });
            cards->addWidget(card, 1);
            m_presetCards.push_back(card);
        }
        layout->addLayout(cards);

        m_jobsLabel = Hearth::MakeLabel(QString(), QStringLiteral("mono"), content);
        m_jobsLabel->setStyleSheet(QStringLiteral("background: transparent; border: none; color: #FFCFB4; font-family: \"Geist Mono\"; font-size: 12px;"));
        layout->addWidget(Hearth::MakeSettingRow(tr("Limit build memory"), tr("Keeps builds from filling RAM, so the computer never freezes."), m_jobsLabel));
        auto* pause = new Hearth::Toggle(content);
        m_pauseAssets = pause;
        layout->addWidget(Hearth::MakeSettingRow(tr("Pause asset processing while a game runs"), tr("So playing on this computer stays smooth."), pause));

        return Page(tr("Your laptop"),
                    tr("We checked what this computer has, so the engine never asks more of it than it can give."), content);
    }

    QWidget* FirstRunDialog::BuildVoice()
    {
        auto* content = new QWidget();
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        m_voiceServer = new QLineEdit(settings->value(QStringLiteral("voice/server")).toString());
        m_voiceServer->setPlaceholderText(tr("Hearth Server address, like http://192.168.1.40:8080"));
        m_voiceServer->setMinimumWidth(340);
        auto* testRow = new QWidget();
        auto* testLayout = new QHBoxLayout(testRow);
        testLayout->setContentsMargins(0, 0, 0, 0);
        testLayout->addWidget(m_voiceServer, 1);
        QPushButton* test = Hearth::MakeButton(tr("Test"), false);
        testLayout->addWidget(test);
        layout->addWidget(Hearth::MakeSettingRow(tr("Hearth Server"), tr("Voice Studio makes the voices on your Hearth Server"), testRow));
        m_voiceStatus = Hearth::MakeLabel(tr("Not tested yet. You can leave this empty and connect later."), QStringLiteral("small"), content);
        layout->addSpacing(8);
        layout->addWidget(m_voiceStatus);

        auto* network = new QNetworkAccessManager(content);
        connect(test, &QPushButton::clicked, this, [this, network]()
        {
            const QUrl url = QUrl::fromUserInput(m_voiceServer->text().trimmed());
            if (!url.isValid() || url.host().isEmpty())
            {
                m_voiceStatus->setText(tr("That doesn't look like an address."));
                return;
            }
            m_voiceStatus->setText(tr("Testing %1…").arg(url.toString()));
            QNetworkReply* reply = network->get(QNetworkRequest(url));
            QPointer<QNetworkReply> guard(reply);
            QTimer::singleShot(6000, reply, [guard]()
            {
                if (guard && guard->isRunning())
                {
                    guard->abort();
                }
            });
            connect(reply, &QNetworkReply::finished, this, [this, reply]()
            {
                reply->deleteLater();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status > 0)
                {
                    m_voiceStatus->setText(tr("The Hearth Server answered. Voice Studio will use it."));
                    m_voiceStatus->setStyleSheet(QStringLiteral("background: transparent; border: none; color: #A6DD97; font-size: 11px;"));
                }
                else
                {
                    m_voiceStatus->setText(tr("No answer: %1").arg(reply->errorString()));
                    m_voiceStatus->setStyleSheet(QStringLiteral("background: transparent; border: none; color: #FF9F8F; font-size: 11px;"));
                }
            });
        });
        return Page(tr("Voice Studio"),
                    tr("Write a line, pick or design a voice, and Voice Studio makes it on your Hearth Server, with subtitles and lip sync."),
                    content);
    }

    QWidget* FirstRunDialog::BuildClaude()
    {
        auto* content = new QWidget();
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        QLabel* logs = Hearth::MakeLabel(QDir::toNativeSeparators(Hearth::LogsFolder()), QStringLiteral("mono"));
        layout->addWidget(Hearth::MakeSettingRow(tr("Logs"), tr("Every run of the editor, the game and the tools is saved here (the newest 30)."), logs));
        auto* help = new Hearth::Toggle();
        help->setChecked(true);
        m_claudeHelp = help;
        layout->addWidget(Hearth::MakeSettingRow(tr("Let Claude help"),
                                                 tr("When something breaks, the Hub offers to hand the error and its log to Claude."), help));
        return Page(tr("Claude and logs"),
                    tr("Hearth Engine keeps a log of everything, so you or Claude can always see what went wrong."), content);
    }

    QWidget* FirstRunDialog::BuildReady()
    {
        auto* content = new QWidget();
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(0, 0, 0, 0);
        m_readySummary = Hearth::MakeLabel(QString(), QStringLiteral("body"), content);
        m_readySummary->setTextFormat(Qt::RichText);
        layout->addWidget(m_readySummary);
        return Page(tr("Ready"), tr("That's everything. Here is what was set:"), content);
    }

    int FirstRunDialog::JobsFor(Preset preset) const
    {
        // Each build job needs about 1.5 GB; never more jobs than cores.
        const int byMemory = qMax(1, static_cast<int>(m_hardware.memoryGb / 1.5));
        switch (preset)
        {
        case Preset::Light:
            return qBound(1, qMin(m_hardware.cores / 4, byMemory / 2), 2);
        case Preset::Balanced:
            return qBound(1, qMin(m_hardware.cores / 2, byMemory - 1), 4);
        case Preset::Full:
        default:
            return qBound(1, qMin(m_hardware.cores, byMemory - 1), 32);
        }
    }

    void FirstRunDialog::SelectPreset(Preset preset)
    {
        m_preset = preset;
        for (QWidget* widget : m_presetCards)
        {
            auto* card = static_cast<QPushButton*>(widget);
            const bool on = card->property("preset").toInt() == static_cast<int>(preset);
            card->setChecked(on);
            card->setStyleSheet(on ? QStringLiteral("QPushButton { background-color: rgba(232, 116, 59, 31); border: 1px solid rgba(232, 116, 59, 128);"
                                                    " border-radius: 12px; text-align: left; }")
                                   : QStringLiteral("QPushButton { background-color: rgba(255, 255, 255, 8); border: 1px solid rgba(255, 255, 255, 15);"
                                                    " border-radius: 12px; text-align: left; }"
                                                    "QPushButton:hover { background-color: rgba(255, 255, 255, 15); }"));
        }
        if (m_jobsLabel)
        {
            m_jobsLabel->setText(tr("%1 jobs · 1.5 GB each").arg(JobsFor(preset)));
        }
        if (m_pauseAssets)
        {
            m_pauseAssets->setChecked(preset == Preset::Light);
        }
    }

    void FirstRunDialog::GoTo(int step)
    {
        m_step = qBound(0, step, StepCount - 1);
        m_pages->setCurrentIndex(m_step);
        m_stepCounter->setText(tr("Setting up, step %1 of %2").arg(m_step + 1).arg(StepCount));
        for (int i = 0; i < StepCount; ++i)
        {
            const bool done = i < m_step;
            const bool current = i == m_step;
            m_stepRows[i]->setStyleSheet(current ? QStringLiteral("QWidget#StepRow { background-color: rgba(232, 116, 59, 41); border-radius: 9px; }")
                                                 : QStringLiteral("QWidget#StepRow { background: transparent; }"));
            m_stepNumbers[i]->setText(done ? QStringLiteral("✓") : QString::number(i + 1));
            m_stepNumbers[i]->setStyleSheet(done ? QStringLiteral("background-color: #E8743B; color: #1A0F08; border-radius: 11px; font-size: 11px;")
                                                 : (current ? QStringLiteral("background: transparent; color: #FFCFB4; border: 1px solid #E8743B; border-radius: 11px; font-size: 11px;")
                                                            : QStringLiteral("background: transparent; color: #8D857B; border: 1px solid rgba(255, 255, 255, 31); border-radius: 11px; font-size: 11px;")));
            m_stepNames[i]->setStyleSheet(current ? QStringLiteral("background: transparent; color: #FFCFB4;")
                                                  : (i > m_step ? QStringLiteral("background: transparent; color: #8D857B;")
                                                                : QStringLiteral("background: transparent; color: #ECE6DE;")));
        }
        m_back->setEnabled(m_step > 0);
        m_skip->setVisible(m_step < StepCount - 1);
        m_next->setText(m_step == StepCount - 1 ? tr("Open the Hub") : tr("Next: %1").arg(tr(StepNames[m_step + 1])));
        if (m_step == StepCount - 1)
        {
            UpdateReadySummary();
        }
    }

    void FirstRunDialog::UpdateReadySummary()
    {
        const QString presetName = m_preset == Preset::Light ? tr("Light") : (m_preset == Preset::Balanced ? tr("Balanced") : tr("Full power"));
        QStringList lines;
        lines << tr("Projects go in <b>%1</b>").arg(m_projectsFolder->text().toHtmlEscaped());
        lines << tr("Engine setting: <b>%1</b> (%2 build jobs%3)")
                     .arg(presetName)
                     .arg(JobsFor(m_preset))
                     .arg(m_pauseAssets->isChecked() ? tr(", asset processing pauses while a game runs") : QString());
        lines << (m_voiceServer->text().trimmed().isEmpty() ? tr("Voice Studio: connect your Hearth Server later in Settings")
                                                            : tr("Voice Studio uses <b>%1</b>").arg(m_voiceServer->text().trimmed().toHtmlEscaped()));
        lines << (m_claudeHelp->isChecked() ? tr("Claude can help when something breaks") : tr("Claude's help buttons are off"));
        m_readySummary->setText(QStringLiteral("<p style='line-height:150%'>") + lines.join(QStringLiteral("<br>")) + QStringLiteral("</p>"));
    }

    void FirstRunDialog::Save(bool skipped)
    {
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        settings->setValue(QStringLiteral("firstRun/done"), true);
        settings->setValue(QStringLiteral("firstRun/skipped"), skipped);
        settings->setValue(QStringLiteral("folders/projects"), m_projectsFolder->text().trimmed());
        QDir().mkpath(m_projectsFolder->text().trimmed());
        const char* presetKey = m_preset == Preset::Light ? "light" : (m_preset == Preset::Balanced ? "balanced" : "full");
        settings->setValue(QStringLiteral("laptop/preset"), QLatin1String(presetKey));
        settings->setValue(QStringLiteral("laptop/buildJobs"), JobsFor(m_preset));
        settings->setValue(QStringLiteral("laptop/buildMemoryGb"), 1.5);
        settings->setValue(QStringLiteral("laptop/pauseAssetsWhileGaming"), m_pauseAssets->isChecked());
        settings->setValue(QStringLiteral("laptop/viewportScale"), m_preset == Preset::Light ? 0.5 : 1.0);
        settings->setValue(QStringLiteral("laptop/shadows"), m_preset != Preset::Light);
        settings->setValue(QStringLiteral("voice/server"), m_voiceServer->text().trimmed());
        settings->setValue(QStringLiteral("claude/help"), m_claudeHelp->isChecked());
        settings->sync();
    }
} // namespace O3DE::ProjectManager
