/*
 * Hearth Engine editor shell. Built on O3DE (Apache-2.0 OR MIT). See HearthEditorShell.h.
 */

#include "EditorDefs.h"

#include "HearthEditorShell.h"
#include "HearthCommandPalette.h"
#include "QtViewPaneManager.h"

#include <AzCore/Console/IConsole.h>
#include <AzCore/Interface/Interface.h>
#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/ActionManager/Action/ActionManagerInterface.h>

#include <QAction>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;
    HearthEditorShell* s_instance = nullptr;

    QString Pretty(const QString& name)
    {
        QString spaced;
        for (int i = 0; i < name.size(); ++i)
        {
            if (i > 0 && name[i].isUpper() && name[i - 1].isLower())
            {
                spaced += QLatin1Char(' ');
            }
            spaced += name[i];
        }
        return spaced;
    }

    QString ChipStyle(const QString& tone)
    {
        if (tone == QLatin1String("good"))
        {
            return QStringLiteral("background-color: rgba(127, 207, 106, 31); color: #A6DD97; border-radius: 13px; padding: 4px 12px; font-size: 12px;");
        }
        if (tone == QLatin1String("warn"))
        {
            return QStringLiteral("background-color: rgba(232, 190, 59, 36); color: #F0D17A; border-radius: 13px; padding: 4px 12px; font-size: 12px;");
        }
        if (tone == QLatin1String("bad"))
        {
            return QStringLiteral("background-color: rgba(232, 80, 59, 41); color: #FF9F8F; border-radius: 13px; padding: 4px 12px; font-size: 12px;");
        }
        return QStringLiteral("background-color: rgba(255, 255, 255, 15); color: #8D857B; border-radius: 13px; padding: 4px 12px; font-size: 12px;");
    }
} // namespace

void HearthEditorShell::Install(QMainWindow* mainWindow)
{
    if (!s_instance && mainWindow)
    {
        s_instance = new HearthEditorShell(mainWindow);
    }
}

HearthEditorShell* HearthEditorShell::Instance()
{
    return s_instance;
}

HearthEditorShell::HearthEditorShell(QMainWindow* mainWindow)
    : QObject(mainWindow)
    , m_mainWindow(mainWindow)
{
    m_network = new QNetworkAccessManager(this);
    BuildHeader();

    auto* palette = new QShortcut(QKeySequence(QStringLiteral("Ctrl+K")), mainWindow);
    connect(palette, &QShortcut::activated, this, [this]() { OpenCommandPalette(); });
    auto* fullScreen = new QShortcut(QKeySequence(Qt::Key_F11), mainWindow);
    connect(fullScreen, &QShortcut::activated, this, &HearthEditorShell::PlayFullScreen);
    auto* standalone = new QShortcut(QKeySequence(QStringLiteral("Ctrl+F11")), mainWindow);
    connect(standalone, &QShortcut::activated, this, &HearthEditorShell::StartStandaloneGame);
    auto* levelGen = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+L")), mainWindow);
    connect(levelGen, &QShortcut::activated, this, []() { QtViewPaneManager::instance()->OpenPane(QStringLiteral("Level Generator")); });
    auto* build = new QShortcut(QKeySequence(QStringLiteral("Ctrl+B")), mainWindow);
    connect(build, &QShortcut::activated, this, &HearthEditorShell::RequestBuild);

    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    int ticks = 0;
    connect(m_timer, &QTimer::timeout, this, [this, ticks]() mutable
    {
        UpdateTitle();
        WatchFullScreen();
        if (ticks++ % 6 == 0)
        {
            UpdateBuildChip();
        }
    });
    m_timer->start();

    // The renderer is ready a moment after the main window: apply the laptop settings then.
    QTimer::singleShot(4000, this, &HearthEditorShell::ApplyLaptopSettings);

    // First start of the Hearth layout: put the panels where the Hearth Engine design has them, once.
    // (After that the owner's own arrangement is kept; View > Layouts > Restore default brings it back.)
    QTimer::singleShot(1500, this, []()
    {
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        if (settings->value(QStringLiteral("editor/layout")).toString() != QLatin1String("hearth-1"))
        {
            settings->setValue(QStringLiteral("editor/layout"), QStringLiteral("hearth-1"));
            QtViewPaneManager::instance()->RestoreDefaultLayout(true);
        }
    });
}

void HearthEditorShell::BuildHeader()
{
    QMenuBar* bar = m_mainWindow->menuBar();
    bar->setStyleSheet(QStringLiteral("QMenuBar { min-height: 52px; background-color: #0D0C0B; }"));

    // Left: flame tile, project and level.
    auto* left = new QWidget(bar);
    auto* leftLayout = new QHBoxLayout(left);
    leftLayout->setContentsMargins(10, 6, 18, 6);
    leftLayout->setSpacing(10);
    auto* tile = new QLabel(left);
    tile->setPixmap(Hearth::FlameTile(30, left->devicePixelRatioF()));
    tile->setFixedSize(30, 30);
    leftLayout->addWidget(tile);
    auto* names = new QVBoxLayout();
    names->setSpacing(0);
    m_projectLabel = Hearth::MakeLabel(QString(), QStringLiteral("title"), left);
    m_levelLabel = Hearth::MakeLabel(QString(), QStringLiteral("small"), left);
    names->addWidget(m_projectLabel);
    names->addWidget(m_levelLabel);
    leftLayout->addLayout(names);
    bar->setCornerWidget(left, Qt::TopLeftCorner);

    // Right: search, build state, Build, Play.
    auto* right = new QWidget(bar);
    auto* rightLayout = new QHBoxLayout(right);
    rightLayout->setContentsMargins(10, 6, 10, 6);
    rightLayout->setSpacing(8);

    auto* search = new QPushButton(right);
    search->setCursor(Qt::PointingHandCursor);
    search->setMinimumWidth(270);
    search->setFixedHeight(32);
    search->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #1C1917; border: 1px solid rgba(255, 255, 255, 26); border-radius: 10px;"
        " text-align: left; padding: 0px 12px; color: #8D857B; }"
        "QPushButton:hover { border-color: rgba(232, 116, 59, 140); }"));
    auto* searchLayout = new QHBoxLayout(search);
    searchLayout->setContentsMargins(12, 0, 8, 0);
    QLabel* searchText = Hearth::MakeLabel(tr("Search or run a command"), QStringLiteral("dim"), search);
    searchText->setAttribute(Qt::WA_TransparentForMouseEvents);
    searchLayout->addWidget(searchText, 1);
    QLabel* keys = new QLabel(QStringLiteral("Ctrl K"), search);
    keys->setAttribute(Qt::WA_TransparentForMouseEvents);
    keys->setStyleSheet(QStringLiteral("background-color: rgba(255, 255, 255, 15); color: #8D857B; border-radius: 6px; padding: 1px 6px;"
                                       " font-family: \"Geist Mono\"; font-size: 11px;"));
    searchLayout->addWidget(keys);
    connect(search, &QPushButton::clicked, this, [this]() { OpenCommandPalette(); });
    rightLayout->addWidget(search);

    m_buildChip = new QLabel(tr("Build"), right);
    m_buildChip->setStyleSheet(ChipStyle(QString()));
    rightLayout->addWidget(m_buildChip);

    m_buildButton = Hearth::MakeButton(tr("Build"), false, right);
    connect(m_buildButton, &QPushButton::clicked, this, &HearthEditorShell::RequestBuild);
    rightLayout->addWidget(m_buildButton);

    // Play split button: the big part plays full screen, the arrow opens the other ways to play.
    auto* split = new QWidget(right);
    auto* splitLayout = new QHBoxLayout(split);
    splitLayout->setContentsMargins(0, 0, 0, 0);
    splitLayout->setSpacing(1);
    m_playButton = new QPushButton(tr("▶  Play full screen"), split);
    m_playButton->setCursor(Qt::PointingHandCursor);
    m_playButton->setFixedHeight(30);
    m_playButton->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #E8743B; color: #1A0F08; font-weight: 600; border: none;"
        " border-top-left-radius: 15px; border-bottom-left-radius: 15px; padding: 0px 16px; }"
        "QPushButton:hover { background-color: #F08A55; }"));
    connect(m_playButton, &QPushButton::clicked, this, &HearthEditorShell::PlayFullScreen);
    auto* arrow = new QPushButton(QStringLiteral("⌄"), split);
    arrow->setCursor(Qt::PointingHandCursor);
    arrow->setFixedSize(32, 30);
    arrow->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #E8743B; color: #1A0F08; font-weight: 700; border: none;"
        " border-top-right-radius: 15px; border-bottom-right-radius: 15px; }"
        "QPushButton:hover { background-color: #F08A55; }"
        "QPushButton::menu-indicator { width: 0px; }"));
    auto* menu = new QMenu(arrow);
    // Shortcuts are shown after a tab (not set on these actions: the editor and this shell already own them).
    QAction* inViewport = menu->addAction(tr("Play in viewport") + QStringLiteral("\tCtrl+G"));
    connect(inViewport, &QAction::triggered, this, &HearthEditorShell::PlayInViewport);
    QAction* full = menu->addAction(tr("Play full screen") + QStringLiteral("\tF11"));
    connect(full, &QAction::triggered, this, &HearthEditorShell::PlayFullScreen);
    QAction* window = menu->addAction(tr("Standalone game window") + QStringLiteral("\tCtrl+F11"));
    connect(window, &QAction::triggered, this, &HearthEditorShell::StartStandaloneGame);
    QAction* coop = menu->addAction(tr("Co-op test, 2 players"));
    connect(coop, &QAction::triggered, this, &HearthEditorShell::OpenCoopTest);
    menu->addSeparator();
    QAction* drive = menu->addAction(tr("Let Claude drive"));
    drive->setCheckable(true);
    {
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        drive->setChecked(settings->value(QStringLiteral("claude/drive"), true).toBool());
    }
    connect(drive, &QAction::toggled, this, [](bool on)
    {
        // Read by the Claude bridge: when off, Claude can watch the game but not press keys in it.
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        settings->setValue(QStringLiteral("claude/drive"), on);
    });
    arrow->setMenu(menu);
    splitLayout->addWidget(m_playButton);
    splitLayout->addWidget(arrow);
    rightLayout->addWidget(split);

    bar->setCornerWidget(right, Qt::TopRightCorner);
    UpdateTitle();
}

void HearthEditorShell::UpdateTitle()
{
    const auto projectName = AZ::Utils::GetProjectName();
    m_projectLabel->setText(Pretty(QString::fromUtf8(projectName.c_str(), static_cast<int>(projectName.size()))));
    const QString level = GetIEditor() ? GetIEditor()->GetLevelName() : QString();
    m_levelLabel->setText(level.isEmpty() ? tr("Hearth Engine") : tr("%1 · Hearth Engine").arg(level));
}

void HearthEditorShell::ApplyLaptopSettings()
{
    QScopedPointer<QSettings> settings(Hearth::NewSettings());
    auto* console = AZ::Interface<AZ::IConsole>::Get();
    if (!console)
    {
        return;
    }
    const double scale = qBound(0.25, settings->value(QStringLiteral("laptop/viewportScale"), 1.0).toDouble(), 1.0);
    const QByteArray scaleCommand = QStringLiteral("r_renderScale %1").arg(scale, 0, 'f', 2).toUtf8();
    console->PerformCommand(scaleCommand.constData());
    if (!settings->value(QStringLiteral("laptop/shadows"), true).toBool())
    {
        console->PerformCommand("r_directionalShadowFilteringMethod 0"); // cheapest shadows on the Light setting
    }
}

bool HearthEditorShell::TriggerAction(const char* identifier)
{
    auto* actions = AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get();
    return actions && actions->TriggerAction(identifier).IsSuccess();
}

void HearthEditorShell::PlayInViewport()
{
    TriggerAction("o3de.action.game.play");
}

void HearthEditorShell::PlayFullScreen()
{
    if (GetIEditor() && GetIEditor()->IsInGameMode())
    {
        return;
    }
    m_wasMaximized = m_mainWindow->isMaximized();
    m_fullScreenPlay = true;
    m_sawGameMode = false;
    m_mainWindow->showFullScreen();
    if (!TriggerAction("o3de.action.game.playMaximized"))
    {
        TriggerAction("o3de.action.game.play");
    }
}

void HearthEditorShell::WatchFullScreen()
{
    if (!m_fullScreenPlay || !GetIEditor())
    {
        return;
    }
    if (GetIEditor()->IsInGameMode())
    {
        m_sawGameMode = true;
        return;
    }
    if (m_sawGameMode)
    {
        // Play ended: give the editor its window back.
        m_fullScreenPlay = false;
        if (m_wasMaximized)
        {
            m_mainWindow->showMaximized();
        }
        else
        {
            m_mainWindow->showNormal();
        }
    }
}

void HearthEditorShell::OpenCommandPalette(const QString& prefix)
{
    if (m_palette)
    {
        m_palette->raise();
        return;
    }
    m_palette = new HearthCommandPalette(m_mainWindow, prefix);
    m_palette->show();
}

void HearthEditorShell::OpenCoopTest()
{
    QtViewPaneManager::instance()->OpenPane(QStringLiteral("Co-op test"));
}

void HearthEditorShell::PostToAgent(const QString& op, const QByteArray& json, std::function<void(const QByteArray&)> done)
{
    if (m_agentToken.isEmpty())
    {
        const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        QFile file(QDir(config).filePath(QStringLiteral("hearth-engine/agent.json")));
        if (file.open(QIODevice::ReadOnly))
        {
            const QJsonObject agent = QJsonDocument::fromJson(file.readAll()).object();
            m_agentToken = agent.value(QStringLiteral("token")).toString();
            m_agentPort = agent.value(QStringLiteral("port")).toInt(47830);
        }
    }
    if (m_agentToken.isEmpty())
    {
        if (done)
        {
            done(QByteArray());
        }
        return;
    }
    QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/api/%2").arg(m_agentPort).arg(op)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("X-Hearth-Token", m_agentToken.toUtf8());
    QNetworkReply* reply = m_network->post(request, json);
    connect(reply, &QNetworkReply::finished, this, [this, reply, done]()
    {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 0)
        {
            m_agentToken.clear();
            if (done)
            {
                done(QByteArray());
            }
            return;
        }
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 403)
        {
            m_agentToken.clear();
        }
        if (done)
        {
            done(reply->readAll());
        }
    });
}

void HearthEditorShell::UpdateBuildChip()
{
    PostToAgent(QStringLiteral("hub_info"), QByteArrayLiteral("{}"), [this](const QByteArray& data)
    {
        if (data.isEmpty())
        {
            m_buildChip->setText(tr("Agent offline"));
            m_buildChip->setStyleSheet(ChipStyle(QString()));
            return;
        }
        const QJsonObject info = QJsonDocument::fromJson(data).object();
        const QJsonObject build = info.value(QStringLiteral("build")).toObject();
        const QJsonObject last = build.value(QStringLiteral("last")).toObject();
        if (build.value(QStringLiteral("running")).toBool())
        {
            m_buildChip->setText(tr("● Building…"));
            m_buildChip->setStyleSheet(ChipStyle(QStringLiteral("warn")));
        }
        else if (info.value(QStringLiteral("pending_rebuild")).toBool())
        {
            m_buildChip->setText(tr("● Rebuild needed"));
            m_buildChip->setStyleSheet(ChipStyle(QStringLiteral("warn")));
        }
        else if (!last.isEmpty() && !last.value(QStringLiteral("ok")).toBool())
        {
            m_buildChip->setText(tr("● Build failed"));
            m_buildChip->setStyleSheet(ChipStyle(QStringLiteral("bad")));
        }
        else if (!last.isEmpty())
        {
            m_buildChip->setText(tr("● Build OK"));
            m_buildChip->setStyleSheet(ChipStyle(QStringLiteral("good")));
        }
        else
        {
            m_buildChip->setText(tr("Not built yet"));
            m_buildChip->setStyleSheet(ChipStyle(QString()));
        }
    });
}

void HearthEditorShell::RequestBuild()
{
    m_buildChip->setText(tr("● Building…"));
    m_buildChip->setStyleSheet(ChipStyle(QStringLiteral("warn")));
    // The agent saves the level, closes the editor, builds, and opens the editor again.
    PostToAgent(QStringLiteral("build"),
                QByteArrayLiteral("{\"stop_editor\": true, \"reopen_editor\": true}"),
                [this](const QByteArray& data)
                {
                    if (data.isEmpty())
                    {
                        m_buildChip->setText(tr("Agent offline"));
                        m_buildChip->setStyleSheet(ChipStyle(QString()));
                    }
                });
}

void HearthEditorShell::StartStandaloneGame()
{
    PostToAgent(QStringLiteral("game_start"), QByteArrayLiteral("{\"fullscreen\": false}"));
}
