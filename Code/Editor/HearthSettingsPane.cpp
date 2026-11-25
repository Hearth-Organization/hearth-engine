/*
 * Hearth Engine Settings. Built on O3DE (Apache-2.0 OR MIT). See HearthSettingsPane.h.
 */

#include "EditorDefs.h"

#include "HearthSettingsPane.h"
#include "QtViewPaneManager.h"
#include "Settings.h"
#include "StartupLogoDialog.h"

#include <AzCore/Console/IConsole.h>
#include <AzCore/Interface/Interface.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>

#include <QComboBox>
#include <functional>
#include <memory>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    QVariant Get(const QString& key, const QVariant& fallback)
    {
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        return settings->value(key, fallback);
    }

    void Set(const QString& key, const QVariant& value)
    {
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        settings->setValue(key, value);
    }

    void Console(const QString& command)
    {
        // r_renderScale is an AZ cvar; sys_MaxFPS and ed_backgroundUpdatePeriod are older engine cvars.
        if (auto* console = AZ::Interface<AZ::IConsole>::Get())
        {
            if (console->PerformCommand(command.toUtf8().constData()).IsSuccess())
            {
                return;
            }
        }
        if (gEnv && gEnv->pConsole)
        {
            gEnv->pConsole->ExecuteString(command.toUtf8().constData());
        }
    }

    //! "Intel i3-N305 · 8 cores · Intel UHD graphics · 7.6 GB memory"
    QString DescribeComputer()
    {
        QString cpu;
        QFile cpuinfo(QStringLiteral("/proc/cpuinfo"));
        if (cpuinfo.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            for (const QString& line : QString::fromUtf8(cpuinfo.readAll()).split(QLatin1Char('\n')))
            {
                if (line.startsWith(QLatin1String("model name")))
                {
                    cpu = line.section(QLatin1Char(':'), 1).trimmed();
                    cpu.remove(QStringLiteral("(R)")).remove(QStringLiteral("(TM)")).remove(QStringLiteral(" CPU"));
                    cpu.replace(QStringLiteral("Core "), QString());
                    cpu = cpu.section(QLatin1Char('@'), 0, 0).simplified();
                    break;
                }
            }
        }
        double memory = 0.0;
        QFile meminfo(QStringLiteral("/proc/meminfo"));
        if (meminfo.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            const QStringList first = QString::fromUtf8(meminfo.readLine()).simplified().split(QLatin1Char(' '));
            memory = first.size() >= 2 ? first[1].toDouble() / (1024.0 * 1024.0) : 0.0;
        }
        QString gpu;
        QProcess lspci;
        lspci.start(QStringLiteral("lspci"), {});
        if (lspci.waitForFinished(2000))
        {
            for (const QString& line : QString::fromUtf8(lspci.readAllStandardOutput()).split(QLatin1Char('\n')))
            {
                if (line.contains(QLatin1String("VGA")) || line.contains(QLatin1String("3D controller")))
                {
                    gpu = line.section(QLatin1Char(':'), 2).trimmed().section(QLatin1Char('['), 1).section(QLatin1Char(']'), 0, 0);
                    gpu = gpu.isEmpty() ? line.section(QLatin1Char(':'), 2).trimmed() : line.section(QLatin1Char(':'), 2).trimmed().section(QLatin1Char(' '), 0, 0) + QLatin1Char(' ') + gpu;
                }
            }
        }
        QStringList parts;
        parts << (cpu.isEmpty() ? QObject::tr("This computer") : cpu) << QObject::tr("%n core(s)", nullptr, QThread::idealThreadCount());
        if (!gpu.isEmpty())
        {
            parts << gpu;
        }
        parts << QObject::tr("%1 GB memory").arg(memory, 0, 'f', 1);
        return parts.join(QStringLiteral(" · "));
    }

    Hearth::Toggle* SettingToggle(const QString& key, bool fallback, QWidget* parent, std::function<void(bool)> also = {})
    {
        auto* toggle = new Hearth::Toggle(parent);
        toggle->setChecked(Get(key, fallback).toBool());
        QObject::connect(toggle, &QAbstractButton::toggled, toggle, [key, also](bool on)
        {
            Set(key, on);
            if (also)
            {
                also(on);
            }
        });
        return toggle;
    }

    QWidget* Segmented(const QStringList& labels, int current, QWidget* parent, std::function<void(int)> changed)
    {
        auto* holder = new QWidget(parent);
        holder->setObjectName(QStringLiteral("Segmented"));
        holder->setAttribute(Qt::WA_StyledBackground);
        holder->setStyleSheet(QStringLiteral("QWidget#Segmented { background-color: rgba(255, 255, 255, 13); border-radius: 13px; }"
                                             "QPushButton { border: none; border-radius: 11px; padding: 3px 12px; color: #ECE6DE; background: transparent; }"
                                             "QPushButton:checked { background-color: #ECE6DE; color: #1A0F08; }"));
        auto* layout = new QHBoxLayout(holder);
        layout->setContentsMargins(2, 2, 2, 2);
        layout->setSpacing(2);
        QList<QPushButton*> buttons;
        for (int i = 0; i < labels.size(); ++i)
        {
            auto* button = new QPushButton(labels[i], holder);
            button->setCheckable(true);
            button->setChecked(i == current);
            buttons << button;
            layout->addWidget(button);
        }
        for (int i = 0; i < buttons.size(); ++i)
        {
            QObject::connect(buttons[i], &QPushButton::clicked, holder, [buttons, i, changed]()
            {
                for (int j = 0; j < buttons.size(); ++j)
                {
                    buttons[j]->setChecked(j == i);
                }
                changed(i);
            });
        }
        return holder;
    }

    QFrame* Section(QWidget* parent, const QString& title, QVBoxLayout** layout)
    {
        QFrame* card = Hearth::MakeCard(parent, layout);
        QLabel* heading = Hearth::MakeLabel(title, QStringLiteral("title"), card);
        heading->setStyleSheet(QStringLiteral("background: transparent; border: none; color: #ECE6DE; font-size: 15px; font-weight: 600;"));
        (*layout)->addWidget(heading);
        return card;
    }
} // namespace

void HearthSettingsPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(100, 70, 1280, 820);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthSettingsPane>("Hearth Settings", "Tools", options);
}

HearthSettingsPane::HearthSettingsPane(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthSettings"));
    setStyleSheet(QStringLiteral("QWidget#HearthSettings { background-color: #0D0C0B; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);
    root->addLayout(Hearth::MakeHeader(this, tr("Settings"), tr("Hearth Engine %1 · saved as you change them").arg(Hearth::Version())));

    auto* body = new QHBoxLayout();
    body->setSpacing(10);
    QVBoxLayout* navLayout = nullptr;
    QFrame* navCard = Hearth::MakeCard(this, &navLayout);
    navCard->setFixedWidth(240);
    m_nav = new QListWidget(navCard);
    m_nav->setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; outline: none; }"
                                        "QListWidget::item { padding: 8px 10px; border-radius: 8px; color: #ECE6DE; }"
                                        "QListWidget::item:selected { background-color: rgba(232, 116, 59, 41); color: #FFCFB4; }"));
    navLayout->addWidget(m_nav);
    body->addWidget(navCard);

    m_pages = new QStackedWidget(this);
    m_pages->setStyleSheet(QStringLiteral("QStackedWidget { background: transparent; }"));
    const QVector<QPair<QString, QWidget*>> pages = { { tr("Performance"), BuildPerformance() }, { tr("Editor"), BuildEditor() },
                                                      { tr("Viewport and play"), BuildViewport() }, { tr("Controls and keys"), BuildKeys() },
                                                      { tr("Claude"), BuildClaude() }, { tr("Logs"), BuildLogs() },
                                                      { tr("Engine parts"), BuildParts() }, { tr("Updates"), BuildUpdates() },
                                                      { tr("About"), BuildAbout() } };
    for (const auto& page : pages)
    {
        m_nav->addItem(page.first);
        auto* scroll = new QScrollArea(m_pages);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(page.second);
        m_pages->addWidget(scroll);
    }
    connect(m_nav, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    m_nav->setCurrentRow(0);
    body->addWidget(m_pages, 1);

    auto* side = new QVBoxLayout();
    side->setSpacing(10);
    side->addWidget(EngineCard());
    side->addWidget(ClaudeCard());
    side->addStretch(1);
    auto* sideHolder = new QWidget(this);
    sideHolder->setFixedWidth(260);
    sideHolder->setLayout(side);
    body->addWidget(sideHolder);
    root->addLayout(body, 1);
}

QWidget* HearthSettingsPane::Page(const QString&, const QString&)
{
    auto* page = new QWidget();
    page->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    return page;
}

QWidget* HearthSettingsPane::BuildPerformance()
{
    QWidget* page = Page(QString(), QString());
    auto* layout = static_cast<QVBoxLayout*>(page->layout());

    QVBoxLayout* laptop = nullptr;
    QFrame* laptopCard = Section(page, tr("This laptop"), &laptop);
    laptop->addWidget(Hearth::MakeLabel(DescribeComputer(), QStringLiteral("dim"), laptopCard));
    auto* presets = new QHBoxLayout();
    presets->setSpacing(8);
    struct Preset
    {
        QString key;
        QString title;
        QString about;
    };
    const QVector<Preset> list = { { QStringLiteral("light"), tr("Battery"), tr("30 fps cap, low lights, 2 build jobs") },
                                   { QStringLiteral("balanced"), tr("Balanced"), tr("Full viewport, 4 build jobs, shadows on") },
                                   { QStringLiteral("full"), tr("Full power"), tr("Plugged in, nothing else open") } };
    const QString current = Get(QStringLiteral("laptop/preset"), QStringLiteral("light")).toString();
    auto cards = std::make_shared<QList<QPushButton*>>();
    for (const Preset& preset : list)
    {
        auto* card = new QPushButton(laptopCard);
        card->setCheckable(true);
        card->setChecked(preset.key == current);
        card->setMinimumHeight(64);
        card->setCursor(Qt::PointingHandCursor);
        card->setStyleSheet(QStringLiteral("QPushButton { background-color: rgba(255, 255, 255, 8); border: 1px solid rgba(255, 255, 255, 15); border-radius: 12px; text-align: left; }"
                                           "QPushButton:checked { background-color: rgba(232, 116, 59, 31); border-color: rgba(232, 116, 59, 140); }"));
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(12, 8, 12, 8);
        QLabel* name = Hearth::MakeLabel(preset.title, QStringLiteral("title"), card);
        name->setAttribute(Qt::WA_TransparentForMouseEvents);
        QLabel* about = Hearth::MakeLabel(preset.key == current ? tr("Picked for this laptop") : preset.about, QStringLiteral("small"), card);
        about->setAttribute(Qt::WA_TransparentForMouseEvents);
        cardLayout->addWidget(name);
        cardLayout->addWidget(about);
        const QString key = preset.key;
        connect(card, &QPushButton::clicked, card, [cards, card, key]()
        {
            for (QPushButton* other : *cards)
            {
                other->setChecked(other == card);
            }
            Set(QStringLiteral("laptop/preset"), key);
            Set(QStringLiteral("laptop/viewportScale"), key == QLatin1String("light") ? 0.5 : 1.0);
            Set(QStringLiteral("laptop/shadows"), key != QLatin1String("light"));
            Set(QStringLiteral("laptop/pauseAssetsWhileGaming"), key == QLatin1String("light"));
            Set(QStringLiteral("laptop/fpsCap"), key == QLatin1String("light") ? 30 : 0);
            Console(QStringLiteral("r_renderScale %1").arg(key == QLatin1String("light") ? 0.5 : 1.0));
            Console(key == QLatin1String("light") ? QStringLiteral("sys_MaxFPS 30") : QStringLiteral("sys_MaxFPS 0"));
        });
        cards->append(card);
        presets->addWidget(card, 1);
    }
    laptop->addLayout(presets);
    layout->addWidget(laptopCard);

    QVBoxLayout* memory = nullptr;
    QFrame* memoryCard = Section(page, tr("Memory safety"), &memory);
    memory->addWidget(Hearth::MakeSettingRow(tr("Never let builds freeze the laptop"), tr("Builds slow down instead of using all memory"),
                                             SettingToggle(QStringLiteral("laptop/limitBuildMemory"), true, memoryCard), memoryCard));
    auto* jobs = new QComboBox(memoryCard);
    jobs->addItem(tr("Auto · %1").arg(qMax(1, QThread::idealThreadCount() / 3)), 0);
    for (int n = 1; n <= QThread::idealThreadCount(); ++n)
    {
        jobs->addItem(QString::number(n), n);
    }
    const int savedJobs = Get(QStringLiteral("laptop/buildJobsChoice"), 0).toInt();
    jobs->setCurrentIndex(qMax(0, jobs->findData(savedJobs)));
    connect(jobs, QOverload<int>::of(&QComboBox::currentIndexChanged), jobs, [jobs](int)
    {
        const int choice = jobs->currentData().toInt();
        Set(QStringLiteral("laptop/buildJobsChoice"), choice);
        Set(QStringLiteral("laptop/buildJobs"), choice > 0 ? choice : qMax(1, QThread::idealThreadCount() / 3));
    });
    memory->addWidget(Hearth::MakeSettingRow(tr("Build jobs at once"), tr("Automatic: about one per 900 MB free"), jobs, memoryCard));
    memory->addWidget(Hearth::MakeSettingRow(tr("Wait for builds before opening the editor"), tr("Opening both at once can run out of memory"),
                                             SettingToggle(QStringLiteral("laptop/waitForBuilds"), true, memoryCard), memoryCard));
    memory->addWidget(Hearth::MakeSettingRow(tr("Build in the cloud"), tr("Big engine builds run on GitHub (32-core), not on the laptop"),
                                             SettingToggle(QStringLiteral("laptop/cloudEngineBuilds"), true, memoryCard), memoryCard));
    layout->addWidget(memoryCard);

    QVBoxLayout* viewport = nullptr;
    QFrame* viewportCard = Section(page, tr("Viewport"), &viewport);
    const double scale = Get(QStringLiteral("laptop/viewportScale"), 1.0).toDouble();
    const int scaleIndex = scale <= 0.55 ? 0 : (scale <= 0.8 ? 1 : 2);
    viewport->addWidget(Hearth::MakeSettingRow(tr("Render scale in the editor"), tr("Lower is smoother on built-in graphics"),
                                               Segmented({ QStringLiteral("50%"), QStringLiteral("75%"), QStringLiteral("100%") }, scaleIndex, viewportCard, [](int i)
                                               {
                                                   const double value = i == 0 ? 0.5 : (i == 1 ? 0.75 : 1.0);
                                                   Set(QStringLiteral("laptop/viewportScale"), value);
                                                   Console(QStringLiteral("r_renderScale %1").arg(value));
                                               }),
                                               viewportCard));
    viewport->addWidget(Hearth::MakeSettingRow(tr("Pause the viewport when the editor isn't focused"), tr("Saves power while you're in another window"),
                                               SettingToggle(QStringLiteral("editor/pauseWhenUnfocused"), true, viewportCard, [](bool on)
                                               {
                                                   Console(on ? QStringLiteral("ed_backgroundUpdatePeriod 0") : QStringLiteral("ed_backgroundUpdatePeriod -1"));
                                               }),
                                               viewportCard));
    layout->addWidget(viewportCard);
    layout->addStretch(1);
    return page;
}

QWidget* HearthSettingsPane::BuildEditor()
{
    QWidget* page = Page(QString(), QString());
    auto* layout = static_cast<QVBoxLayout*>(page->layout());
    QVBoxLayout* backups = nullptr;
    QFrame* card = Section(page, tr("Backups"), &backups);
    auto* backupToggle = new Hearth::Toggle(card);
    backupToggle->setChecked(gSettings.autoBackupEnabled);
    connect(backupToggle, &QAbstractButton::toggled, backupToggle, [](bool on)
    {
        gSettings.autoBackupEnabled = on;
        gSettings.Save();
    });
    backups->addWidget(Hearth::MakeSettingRow(tr("Back up the level while you work"), tr("So a crash never loses more than a few minutes"), backupToggle, card));
    auto* minutes = new QSpinBox(card);
    minutes->setRange(1, 60);
    minutes->setSuffix(tr(" min"));
    minutes->setValue(qMax(1, gSettings.autoBackupTime));
    connect(minutes, QOverload<int>::of(&QSpinBox::valueChanged), minutes, [](int value)
    {
        gSettings.autoBackupTime = value;
        gSettings.Save();
    });
    backups->addWidget(Hearth::MakeSettingRow(tr("Every"), QString(), minutes, card));
    layout->addWidget(card);

    QVBoxLayout* look = nullptr;
    QFrame* lookCard = Section(page, tr("Layout"), &look);
    QPushButton* reset = Hearth::MakeButton(tr("Put the panels back"), false, lookCard);
    connect(reset, &QPushButton::clicked, reset, []() { QtViewPaneManager::instance()->RestoreDefaultLayout(true); });
    look->addWidget(Hearth::MakeSettingRow(tr("Hearth layout"), tr("Scene and Assets on the left, Inspector and Claude on the right, Console at the bottom"), reset, lookCard));
    QPushButton* setup = Hearth::MakeButton(tr("Run setup again"), false, lookCard);
    connect(setup, &QPushButton::clicked, setup, []()
    {
        Set(QStringLiteral("firstRun/done"), false);
        QProcess::startDetached(QStringLiteral("hearth-engine"), {});
    });
    look->addWidget(Hearth::MakeSettingRow(tr("First-run setup"), tr("Opens the Hub with setup again"), setup, lookCard));
    layout->addWidget(lookCard);
    layout->addStretch(1);
    return page;
}

QWidget* HearthSettingsPane::BuildViewport()
{
    QWidget* page = Page(QString(), QString());
    auto* layout = static_cast<QVBoxLayout*>(page->layout());
    QVBoxLayout* play = nullptr;
    QFrame* card = Section(page, tr("Play"), &play);
    play->addWidget(Hearth::MakeSettingRow(tr("Let Claude drive"), tr("Claude can press keys in the game while it plays"),
                                           SettingToggle(QStringLiteral("claude/drive"), true, card), card));
    play->addWidget(Hearth::MakeSettingRow(tr("Show the playtest bar"), tr("Seed, fps and the F8–F11 keys while testing"),
                                           SettingToggle(QStringLiteral("play/overlay"), true, card), card));
    play->addWidget(Hearth::MakeSettingRow(tr("Shadows"), tr("Off on the Battery setting"),
                                           SettingToggle(QStringLiteral("laptop/shadows"), true, card, [](bool on)
                                           {
                                               Console(on ? QStringLiteral("r_directionalShadowFilteringMethod -1") : QStringLiteral("r_directionalShadowFilteringMethod 0"));
                                           }),
                                           card));
    layout->addWidget(card);
    layout->addStretch(1);
    return page;
}

QWidget* HearthSettingsPane::BuildKeys()
{
    QWidget* page = Page(QString(), QString());
    auto* layout = static_cast<QVBoxLayout*>(page->layout());
    QVBoxLayout* keys = nullptr;
    QFrame* card = Section(page, tr("Hearth keys"), &keys);
    const QVector<QPair<QString, QString>> list = { { tr("Search or run a command"), QStringLiteral("Ctrl+K") },
                                                    { tr("Play in the viewport"), QStringLiteral("Ctrl+G") },
                                                    { tr("Play full screen"), QStringLiteral("F11") },
                                                    { tr("Standalone game window"), QStringLiteral("Ctrl+F11") },
                                                    { tr("Rebuild game code"), QStringLiteral("Ctrl+B") },
                                                    { tr("Level Generator"), QStringLiteral("Ctrl+Shift+L") },
                                                    { tr("Unlock all entities"), QStringLiteral("Ctrl+Alt+L") },
                                                    { tr("Playtest: pause / screenshot / free camera / windowed"), QStringLiteral("F8 F9 F10 F11") } };
    for (const auto& key : list)
    {
        QLabel* keysLabel = Hearth::MakeLabel(key.second, QStringLiteral("mono"), card);
        keys->addWidget(Hearth::MakeSettingRow(key.first, QString(), keysLabel, card));
    }
    keys->addWidget(Hearth::MakeLabel(tr("Every other key can be changed in Edit › Keyboard shortcuts."), QStringLiteral("dim"), card));
    layout->addWidget(card);
    layout->addStretch(1);
    return page;
}

QWidget* HearthSettingsPane::BuildClaude()
{
    QWidget* page = Page(QString(), QString());
    auto* layout = static_cast<QVBoxLayout*>(page->layout());
    QVBoxLayout* claude = nullptr;
    QFrame* card = Section(page, tr("Claude"), &claude);
    claude->addWidget(Hearth::MakeSettingRow(tr("Let Claude help"), tr("Crash, build-failure and console buttons hand problems to Claude"),
                                             SettingToggle(QStringLiteral("claude/help"), true, card), card));
    claude->addWidget(Hearth::MakeSettingRow(tr("Where Claude's notes go"), QString(),
                                             Hearth::MakeLabel(QDir::toNativeSeparators(QDir(Hearth::HomeFolder()).filePath(QStringLiteral("claude-results"))), QStringLiteral("mono"), card), card));
    layout->addWidget(card);
    layout->addStretch(1);
    return page;
}

QWidget* HearthSettingsPane::BuildLogs()
{
    QWidget* page = Page(QString(), QString());
    auto* layout = static_cast<QVBoxLayout*>(page->layout());
    QVBoxLayout* logs = nullptr;
    QFrame* card = Section(page, tr("Logs"), &logs);
    QPushButton* open = Hearth::MakeButton(tr("Open the logs folder"), false, card);
    connect(open, &QPushButton::clicked, open, []() { QDesktopServices::openUrl(QUrl::fromLocalFile(Hearth::LogsFolder())); });
    logs->addWidget(Hearth::MakeSettingRow(tr("Every run is logged"), QDir::toNativeSeparators(Hearth::LogsFolder()), open, card));
    auto* keep = new QSpinBox(card);
    keep->setRange(5, 200);
    keep->setValue(Get(QStringLiteral("logs/keep"), 30).toInt());
    connect(keep, QOverload<int>::of(&QSpinBox::valueChanged), keep, [](int value) { Set(QStringLiteral("logs/keep"), value); });
    logs->addWidget(Hearth::MakeSettingRow(tr("Keep this many logs"), tr("Older ones are deleted"), keep, card));
    QPushButton* console = Hearth::MakeButton(tr("Open Console and logs"), false, card);
    connect(console, &QPushButton::clicked, console, []() { QtViewPaneManager::instance()->OpenPane(QStringLiteral("Console and logs")); });
    logs->addWidget(Hearth::MakeSettingRow(tr("Read them here"), QString(), console, card));
    layout->addWidget(card);
    layout->addStretch(1);
    return page;
}

QWidget* HearthSettingsPane::BuildParts()
{
    QWidget* page = Page(QString(), QString());
    auto* layout = static_cast<QVBoxLayout*>(page->layout());
    QVBoxLayout* parts = nullptr;
    QFrame* card = Section(page, tr("Engine parts"), &parts);
    parts->addWidget(Hearth::MakeLabel(tr("Which parts of the engine this game uses is set in the Hub (changes need a rebuild)."), QStringLiteral("dim"), card));
    QPushButton* hub = Hearth::MakeButton(tr("Open the Hub"), true, card);
    connect(hub, &QPushButton::clicked, hub, []() { QProcess::startDetached(QStringLiteral("hearth-engine"), {}); });
    parts->addWidget(hub, 0, Qt::AlignLeft);
    layout->addWidget(card);
    layout->addStretch(1);
    return page;
}

QWidget* HearthSettingsPane::BuildUpdates()
{
    QWidget* page = Page(QString(), QString());
    auto* layout = static_cast<QVBoxLayout*>(page->layout());
    QVBoxLayout* updates = nullptr;
    QFrame* card = Section(page, tr("Updates"), &updates);
    updates->addWidget(Hearth::MakeSettingRow(tr("This version"), QString(), Hearth::MakeLabel(Hearth::Version(), QStringLiteral("mono"), card), card));
    QPushButton* check = Hearth::MakeButton(tr("See new versions"), false, card);
    connect(check, &QPushButton::clicked, check, []()
    {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/Hearth-Organization/hearth-engine/releases")));
    });
    updates->addWidget(Hearth::MakeSettingRow(tr("New versions"), tr("Hearth Engine comes as a .deb from GitHub"), check, card));
    layout->addWidget(card);
    layout->addStretch(1);
    return page;
}

QWidget* HearthSettingsPane::BuildAbout()
{
    QWidget* page = Page(QString(), QString());
    auto* layout = static_cast<QVBoxLayout*>(page->layout());
    QVBoxLayout* about = nullptr;
    QFrame* card = Section(page, tr("About"), &about);
    about->addWidget(Hearth::MakeLabel(tr("Made by Dragongamer9 for Hearth OS. Built on O3DE, the Open 3D Engine (Apache 2.0)."), QStringLiteral("dim"), card));
    QPushButton* open = Hearth::MakeButton(tr("About Hearth Engine"), false, card);
    connect(open, &QPushButton::clicked, this, [this]()
    {
        CStartupLogoDialog dialog(CStartupLogoDialog::About, QString(), QString(), this);
        dialog.exec();
    });
    about->addWidget(open, 0, Qt::AlignLeft);
    layout->addWidget(card);
    layout->addStretch(1);
    return page;
}

QWidget* HearthSettingsPane::EngineCard()
{
    QVBoxLayout* engine = nullptr;
    QFrame* card = Hearth::MakeCard(this, &engine);
    engine->addWidget(Hearth::MakeLabel(tr("Engine"), QStringLiteral("title"), card));
    auto row = [card, engine](const QString& key, const QString& value, const QString& color = QString())
    {
        auto* line = new QHBoxLayout();
        line->addWidget(Hearth::MakeLabel(key, QStringLiteral("dim"), card), 1);
        QLabel* label = Hearth::MakeLabel(value, QStringLiteral("mono"), card);
        if (!color.isEmpty())
        {
            label->setStyleSheet(QStringLiteral("background: transparent; color: %1; font-family: \"Geist Mono\"; font-size: 12px;").arg(color));
        }
        line->addWidget(label);
        engine->addLayout(line);
    };
    row(tr("Version"), Hearth::Version());
    row(tr("Installed in"), QDir::toNativeSeparators(Hearth::EngineRoot()));
    row(tr("Built"), Hearth::Build().isEmpty() ? tr("on this computer") : tr("GitHub · build %1").arg(Hearth::Build()));
    QLabel* status = Hearth::MakeChip(tr("Checking for updates…"), QStringLiteral("plain"), card);
    engine->addWidget(status, 0, Qt::AlignLeft);
    auto* network = new QNetworkAccessManager(card);
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/Hearth-Organization/hearth-engine/releases/latest")));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("HearthEngine"));
    QNetworkReply* reply = network->get(request);
    connect(reply, &QNetworkReply::finished, status, [reply, status]()
    {
        reply->deleteLater();
        const QString tag = QJsonDocument::fromJson(reply->readAll()).object().value(QStringLiteral("tag_name")).toString();
        QString latest = tag;
        latest.remove(QRegularExpression(QStringLiteral("^[^0-9]*")));
        if (reply->error() != QNetworkReply::NoError || latest.isEmpty())
        {
            status->setText(tr("Couldn't check for updates"));
            return;
        }
        const bool upToDate = QVersionNumber::fromString(latest) <= QVersionNumber::fromString(Hearth::Version());
        status->setText(upToDate ? tr("Up to date") : tr("%1 is out").arg(latest));
        status->setStyleSheet(upToDate ? QStringLiteral("background-color: rgba(166, 221, 151, 31); color: #A6DD97; border-radius: 9px; padding: 2px 8px;")
                                       : QStringLiteral("background-color: rgba(232, 116, 59, 41); color: #FFCFB4; border-radius: 9px; padding: 2px 8px;"));
    });
    QPushButton* check = Hearth::MakeButton(tr("Check for updates"), false, card);
    connect(check, &QPushButton::clicked, check, []()
    {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/Hearth-Organization/hearth-engine/releases")));
    });
    engine->addWidget(check);
    return card;
}

QWidget* HearthSettingsPane::ClaudeCard()
{
    QVBoxLayout* claude = nullptr;
    QFrame* card = Hearth::MakeCard(this, &claude);
    claude->addWidget(Hearth::MakeLabel(tr("Claude"), QStringLiteral("title"), card));
    auto row = [card, claude](const QString& name, const QString& key)
    {
        auto* line = new QHBoxLayout();
        line->addWidget(Hearth::MakeLabel(name, QStringLiteral("body"), card), 1);
        line->addWidget(SettingToggle(key, true, card));
        claude->addLayout(line);
    };
    row(tr("Can read logs"), QStringLiteral("claude/readLogs"));
    row(tr("Can build and play"), QStringLiteral("claude/buildAndPlay"));
    row(tr("Can edit levels"), QStringLiteral("claude/editLevels"));
    row(tr("Ask before big builds"), QStringLiteral("claude/askBigBuilds"));
    return card;
}
