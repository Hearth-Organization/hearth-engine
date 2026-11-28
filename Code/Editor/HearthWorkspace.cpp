/*
 * Hearth Engine workspace: one window for everything, with tools that can be pulled out into a window of their own.
 * See HearthWorkspace.h. Built on O3DE (Apache-2.0 OR MIT).
 */
#include "EditorDefs.h"

#include "HearthWorkspace.h"

#include "QtViewPaneManager.h"

#include <AzCore/Utils/Utils.h>

#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QToolButton>
#include <QWindow>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QDockWidget>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#if defined(Q_OS_LINUX)
#include <signal.h>
#include <unistd.h>
#else
#define SIGSTOP 19
#define SIGCONT 18
#endif

namespace
{
    HearthWorkspace* s_instance = nullptr;
    bool s_moving = false;   // a tool is moving between its tab and its own window: the stack's notices are expected

    // The Hearth tools that used to open as windows of their own (registered with no docking area). The ones that
    // dock at the sides or the bottom (Claude, Console and logs, Lighting and look, Assets) stay where they are.
    const QSet<QString>& WorkspaceTools()
    {
        static const QSet<QString> tools = {
            QStringLiteral("Animation"),        QStringLiteral("Browse assets"),     QStringLiteral("Audio"),
            QStringLiteral("Cinematics"),       QStringLiteral("Co-op test"),        QStringLiteral("Import"),
            QStringLiteral("Level Generator"),  QStringLiteral("Material canvas"),   QStringLiteral("Material editor"),
            QStringLiteral("Package the game"), QStringLiteral("Monster pathing"),   QStringLiteral("Physics"),
            QStringLiteral("Asset processing"), QStringLiteral("Profiler"),          QStringLiteral("Project settings"),
            QStringLiteral("Script Canvas"),    QStringLiteral("Script console"),    QStringLiteral("Hearth Settings"),
            QStringLiteral("Game UI editor"),   QStringLiteral("Voice Studio"),
        };
        return tools;
    }

    // How far a tab has to be dragged off the row before it comes out as its own window.
    constexpr int kPullOutDistance = 40;

    // A tool's own window: an ordinary window (the desktop's own title bar, so it moves, resizes and minimises like
    // any other), with the Hearth bar on top. Closing it asks the tool, like closing its tab.
    class ToolWindow : public QWidget
    {
    public:
        explicit ToolWindow(QWidget* parent, std::function<bool()> canClose)
            : QWidget(parent, Qt::Window)
            , m_canClose(std::move(canClose))
        {
        }

    protected:
        void closeEvent(QCloseEvent* event) override
        {
            if (m_canClose && !m_canClose())
            {
                event->ignore();
                return;
            }
            event->accept();
        }

    private:
        std::function<bool()> m_canClose;
    };
}

HearthWorkspace* HearthWorkspace::Install(QWidget* world)
{
    if (!s_instance)
    {
        s_instance = new HearthWorkspace(world);
    }
    return s_instance;
}

HearthWorkspace* HearthWorkspace::Instance()
{
    return s_instance;
}

bool HearthWorkspace::Handles(const QString& paneName)
{
    return WorkspaceTools().contains(paneName);
}

HearthWorkspace::HearthWorkspace(QWidget* world)
    : m_world(world)
{
    setObjectName(QStringLiteral("HearthWorkspace"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // The games row: this editor's game first, then the other games open in this window, then "+".
    m_gamesRow = new QWidget(this);
    m_gamesRow->setObjectName(QStringLiteral("HearthGamesRow"));
    m_gamesRow->setStyleSheet(QStringLiteral("QWidget#HearthGamesRow { background-color: #0D0C0B;"
                                             " border-bottom: 1px solid rgba(255, 255, 255, 14); }"));
    auto* gamesLayout = new QHBoxLayout(m_gamesRow);
    gamesLayout->setContentsMargins(8, 0, 8, 0);
    gamesLayout->setSpacing(4);
    m_games = new QTabBar(m_gamesRow);
    m_games->setObjectName(QStringLiteral("HearthGameTabs"));
    m_games->setDocumentMode(true);
    m_games->setExpanding(false);
    m_games->setDrawBase(false);
    m_games->setTabsClosable(true);
    m_games->setElideMode(Qt::ElideNone);
    m_games->setCursor(Qt::PointingHandCursor);
    m_games->setStyleSheet(QStringLiteral(
        "QTabBar#HearthGameTabs::tab { background-color: transparent; color: #8D857B; border: none;"
        " border-bottom: 2px solid transparent; padding: 7px 14px; margin: 0px; font-weight: 600; }"
        "QTabBar#HearthGameTabs::tab:hover { color: #ECE6DE; }"
        "QTabBar#HearthGameTabs::tab:selected { color: #ECE6DE; border-bottom: 2px solid #E8743B; }"));
    gamesLayout->addWidget(m_games);
    m_addGame = new QToolButton(m_gamesRow);
    m_addGame->setText(QStringLiteral("+"));
    m_addGame->setToolTip(tr("Open another game in this window"));
    m_addGame->setCursor(Qt::PointingHandCursor);
    m_addGame->setStyleSheet(QStringLiteral("QToolButton { color: #B8AFA4; background: transparent; border: none;"
                                            " font-size: 18px; padding: 2px 10px; } QToolButton:hover { color: #ECE6DE; }"));
    connect(m_addGame, &QToolButton::clicked, this, &HearthWorkspace::GamesMenu);
    gamesLayout->addWidget(m_addGame);
    gamesLayout->addStretch(1);
    outer->addWidget(m_gamesRow);

    m_gameStack = new QStackedWidget(this);
    outer->addWidget(m_gameStack, 1);
    m_local = new QWidget(m_gameStack);
    m_gameStack->addWidget(m_local);

    const QString here = QString::fromUtf8(AZ::Utils::GetProjectPath().c_str());
    QString hereName = QFileInfo(here).fileName();
    {
        QFile projectFile(QDir(here).filePath(QStringLiteral("project.json")));
        if (projectFile.open(QIODevice::ReadOnly))
        {
            const QJsonObject data = QJsonDocument::fromJson(projectFile.readAll()).object();
            const QString display = data.value(QStringLiteral("display_name")).toString();
            hereName = display.isEmpty() ? data.value(QStringLiteral("project_name")).toString(hereName) : display;
        }
    }
    const int hereTab = m_games->addTab(hereName);
    m_games->setTabData(hereTab, here);
    m_games->setTabButton(hereTab, QTabBar::RightSide, nullptr);   // this editor's own game isn't closed from here
    m_games->setTabButton(hereTab, QTabBar::LeftSide, nullptr);
    connect(m_games, &QTabBar::currentChanged, this, &HearthWorkspace::ShowGame);
    connect(m_games, &QTabBar::tabCloseRequested, this, &HearthWorkspace::CloseGame);

    if (Embedded())
    {
        // A game shown inside another editor: that editor has the games row. Say where our window is, once shown.
        m_gamesRow->setVisible(false);
        auto* tell = new QTimer(this);
        connect(tell, &QTimer::timeout, this, [this, tell]()
        {
            QWidget* top = window();
            if (!top || !top->isVisible() || !top->windowHandle())
            {
                return;
            }
            QSaveFile out(qEnvironmentVariable("HEARTH_EMBED_FILE"));
            if (out.open(QIODevice::WriteOnly))
            {
                QJsonObject data;
                data[QStringLiteral("winId")] = QString::number(static_cast<qulonglong>(top->winId()));
                data[QStringLiteral("pid")] = static_cast<qint64>(QCoreApplication::applicationPid());
                out.write(QJsonDocument(data).toJson(QJsonDocument::Compact));
                out.commit();
            }
            tell->stop();
            tell->deleteLater();
        });
        tell->start(200);
    }
    else
    {
        m_memoryTimer = new QTimer(this);
        connect(m_memoryTimer, &QTimer::timeout, this, &HearthWorkspace::LookAfterMemory);
        m_memoryTimer->start(5000);
    }

    auto* layout = new QVBoxLayout(m_local);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_tabs = new QTabBar(m_local);
    m_tabs->setObjectName(QStringLiteral("HearthWorkspaceTabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->setExpanding(false);
    m_tabs->setMovable(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setDrawBase(false);
    m_tabs->setElideMode(Qt::ElideNone);
    m_tabs->setCursor(Qt::PointingHandCursor);
    m_tabs->setContextMenuPolicy(Qt::DefaultContextMenu);
    m_tabs->setStyleSheet(QStringLiteral(
        "QTabBar#HearthWorkspaceTabs { background-color: #0D0C0B; }"
        "QTabBar#HearthWorkspaceTabs::tab { background-color: transparent; color: #B8AFA4; border: none;"
        " border-radius: 15px; padding: 6px 14px; margin: 6px 2px 6px 0px; min-height: 18px; }"
        "QTabBar#HearthWorkspaceTabs::tab:hover { background-color: #1C1917; color: #ECE6DE; }"
        "QTabBar#HearthWorkspaceTabs::tab:selected { background-color: #ECE6DE; color: #1A0F08; font-weight: 600; }"
        "QTabBar#HearthWorkspaceTabs::close-button { subcontrol-position: right; margin-left: 6px; }"));
    m_tabs->installEventFilter(this);
    layout->addWidget(m_tabs);

    m_stack = new QStackedWidget(m_local);
    m_stack->addWidget(world);
    layout->addWidget(m_stack, 1);

    const int worldTab = m_tabs->addTab(tr("World"));
    m_tabs->setTabData(worldTab, QString());
    m_tabs->setTabToolTip(worldTab, tr("Your level in 3D"));
    // The World tab can't be closed or pulled out.
    m_tabs->setTabButton(worldTab, QTabBar::RightSide, nullptr);
    m_tabs->setTabButton(worldTab, QTabBar::LeftSide, nullptr);
    m_tabs->setVisible(false);   // only World: no tab row needed

    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index)
    {
        if (index < 0)
        {
            return;
        }
        const QString name = m_tabs->tabData(index).toString();
        QWidget* page = name.isEmpty() ? m_world : m_tools.value(name).data();
        if (page && m_stack->indexOf(page) >= 0)
        {
            m_stack->setCurrentWidget(page);
        }
    });
    connect(m_tabs, &QTabBar::tabCloseRequested, this, &HearthWorkspace::CloseTab);
    // A tool closed some other way (its own Close button, Claude, a layout reset): its dock widget leaves the stack.
    connect(m_stack, &QStackedWidget::widgetRemoved, this, [this](int) { Forget(); });
}

int HearthWorkspace::TabFor(const QString& paneName) const
{
    for (int i = 0; i < m_tabs->count(); ++i)
    {
        if (m_tabs->tabData(i).toString() == paneName)
        {
            return i;
        }
    }
    return -1;
}

bool HearthWorkspace::IsOpen(const QString& paneName) const
{
    return !m_tools.value(paneName).isNull();
}

void HearthWorkspace::AddTab(const QString& paneName)
{
    if (TabFor(paneName) >= 0)
    {
        return;
    }
    const int tab = m_tabs->addTab(paneName);
    m_tabs->setTabData(tab, paneName);
    m_tabs->setTabToolTip(tab, tr("%1 · drag it off the row, or right-click, to open it in its own window").arg(paneName));
    m_tabs->setVisible(true);
}

void HearthWorkspace::Adopt(const QString& paneName, QDockWidget* dock)
{
    if (!dock)
    {
        return;
    }
    // Saved window layouts know tools by their dock widget's name; a new name keeps a layout saved before the
    // workspace from pulling the tool back out into a floating dock of its own.
    dock->setObjectName(QStringLiteral("HearthWorkspace/") + paneName);
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setTitleBarWidget(new QWidget(dock));   // the tab (or its own window's bar) is its title
    dock->setFloating(false);
    m_tools.insert(paneName, dock);
    m_stack->addWidget(dock);
    AddTab(paneName);

    connect(dock, &QObject::destroyed, this, [this]() { QTimer::singleShot(0, this, [this]() { Forget(); }); });
}

void HearthWorkspace::Show(const QString& paneName)
{
    const QPointer<QDockWidget> dock = m_tools.value(paneName);
    if (!dock)
    {
        return;
    }
    if (QWidget* window = m_windows.value(paneName).data())
    {
        // It's out in its own window: bring that to the front.
        window->show();
        if (window->isMinimized())
        {
            window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
        }
        window->raise();
        window->activateWindow();
        return;
    }
    const int tab = TabFor(paneName);
    if (tab < 0 || m_stack->indexOf(dock) < 0)
    {
        return;
    }
    m_games->setCurrentIndex(0);   // a tool of this editor's own game
    m_tabs->setCurrentIndex(tab);
    m_stack->setCurrentWidget(dock);
    if (QWidget* content = dock->widget())
    {
        content->setFocus();
    }
}

void HearthWorkspace::ShowWorld()
{
    if (m_games)
    {
        m_games->setCurrentIndex(0);
    }
    m_tabs->setCurrentIndex(0);
    m_stack->setCurrentWidget(m_world);
}

void HearthWorkspace::PopOut(const QString& paneName, const QPoint& at)
{
    const QPointer<QDockWidget> dock = m_tools.value(paneName);
    if (!dock || m_windows.value(paneName) || m_stack->indexOf(dock) < 0)
    {
        return;
    }
    const QSize size = dock->size().isValid() && dock->width() > 300 ? dock->size() : QSize(1200, 800);

    auto* window = new ToolWindow(this->window(), [paneName]()
    {
        // Closing the window closes the tool, the same as its tab's ×: it may ask first.
        return QtViewPaneManager::instance()->ClosePane(paneName, QtViewPane::CloseMode::Destroy);
    });
    window->setObjectName(QStringLiteral("HearthToolWindow"));
    window->setWindowTitle(tr("%1 · Hearth Engine").arg(paneName));
    window->setStyleSheet(QStringLiteral("QWidget#HearthToolWindow { background-color: #0D0C0B; }"));
    auto* layout = new QVBoxLayout(window);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* bar = new QWidget(window);
    bar->setObjectName(QStringLiteral("HearthToolWindowBar"));
    bar->setStyleSheet(QStringLiteral("QWidget#HearthToolWindowBar { background-color: #0D0C0B;"
                                      " border-bottom: 1px solid rgba(255, 255, 255, 18); }"));
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(14, 6, 10, 6);
    barLayout->setSpacing(8);
    auto* title = new QLabel(paneName, bar);
    title->setStyleSheet(QStringLiteral("color: #ECE6DE; font-weight: 600;"));
    barLayout->addWidget(title);
    barLayout->addStretch(1);
    auto* back = new QPushButton(tr("Put back in the tabs"), bar);
    back->setCursor(Qt::PointingHandCursor);
    back->setFixedHeight(28);
    back->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: transparent; color: #ECE6DE; border: 1px solid rgba(255, 255, 255, 30);"
        " border-radius: 14px; padding: 0px 14px; }"
        "QPushButton:hover { border-color: rgba(232, 116, 59, 160); }"));
    connect(back, &QPushButton::clicked, this, [this, paneName]() { PutBack(paneName); });
    barLayout->addWidget(back);
    layout->addWidget(bar);

    m_windows.insert(paneName, window);
    s_moving = true;
    layout->addWidget(dock, 1);   // out of the stack and into the window
    s_moving = false;
    dock->show();

    window->resize(size);
    QPoint topLeft = at.isNull() ? this->window()->geometry().center() - QPoint(size.width() / 2, size.height() / 2)
                                 : at - QPoint(size.width() / 3, 20);
    if (QScreen* screen = QGuiApplication::screenAt(at.isNull() ? topLeft : at))
    {
        const QRect room = screen->availableGeometry();
        topLeft.setX(qBound(room.left(), topLeft.x(), qMax(room.left(), room.right() - size.width())));
        topLeft.setY(qBound(room.top(), topLeft.y(), qMax(room.top(), room.bottom() - size.height())));
    }
    window->move(topLeft);
    window->show();
    window->raise();
    window->activateWindow();
    Forget();   // its tab goes; it's in its window now
}

void HearthWorkspace::PutBack(const QString& paneName)
{
    const QPointer<QDockWidget> dock = m_tools.value(paneName);
    QPointer<QWidget> window = m_windows.take(paneName);
    if (!dock)
    {
        if (window)
        {
            window->deleteLater();
        }
        return;
    }
    s_moving = true;
    m_stack->addWidget(dock);
    s_moving = false;
    AddTab(paneName);
    Show(paneName);
    if (window)
    {
        window->hide();
        window->deleteLater();
    }
}

void HearthWorkspace::CloseTab(int index)
{
    const QString name = m_tabs->tabData(index).toString();
    if (name.isEmpty())
    {
        return;   // World
    }
    // The same as closing the tool's window: it may ask first (unsaved changes), and an open popup keeps it open.
    QtViewPaneManager::instance()->ClosePane(name, QtViewPane::CloseMode::Destroy);
    Forget();
}

void HearthWorkspace::TabMenu(const QPoint& at)
{
    const int index = m_tabs->tabAt(at);
    const QString name = index >= 0 ? m_tabs->tabData(index).toString() : QString();
    if (name.isEmpty())
    {
        return;   // World, or the empty end of the row
    }
    QMenu menu(this);
    menu.addAction(tr("Open in its own window"), this, [this, name]() { PopOut(name, QCursor::pos()); });
    menu.addSeparator();
    menu.addAction(tr("Close"), this, [this, index]() { CloseTab(index); });
    menu.exec(m_tabs->mapToGlobal(at));
}

bool HearthWorkspace::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_tabs)
    {
        switch (event->type())
        {
        case QEvent::MouseButtonPress:
        {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton)
            {
                const QPoint at = mouse->position().toPoint();
                m_pressTab = m_tabs->tabAt(at);
                m_pressAt = at;
            }
            break;
        }
        case QEvent::MouseMove:
        {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (m_pressTab > 0 && (mouse->buttons() & Qt::LeftButton))
            {
                const QPoint at = mouse->position().toPoint();
                const QRect row = m_tabs->rect().adjusted(0, -kPullOutDistance, 0, kPullOutDistance);
                if (!row.contains(at))
                {
                    const QString name = m_tabs->tabData(m_pressTab).toString();
                    m_pressTab = -1;
                    // End the tab bar's own drag (it moves tabs along the row) before the tab goes.
                    QMouseEvent release(QEvent::MouseButtonRelease, mouse->position(), mouse->globalPosition(),
                                        Qt::LeftButton, Qt::NoButton, mouse->modifiers());
                    QCoreApplication::sendEvent(m_tabs, &release);
                    const QPoint global = mouse->globalPosition().toPoint();
                    QTimer::singleShot(0, this, [this, name, global]() { PopOut(name, global); });
                    return true;
                }
            }
            break;
        }
        case QEvent::MouseButtonRelease:
            m_pressTab = -1;
            break;
        case QEvent::ContextMenu:
            TabMenu(static_cast<QContextMenuEvent*>(event)->pos());
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void HearthWorkspace::Forget()
{
    if (s_moving)
    {
        return;
    }
    for (auto it = m_tools.begin(); it != m_tools.end();)
    {
        const QPointer<QDockWidget>& dock = it.value();
        const bool inTab = dock && m_stack->indexOf(dock) >= 0;
        const QPointer<QWidget> window = m_windows.value(it.key());
        const bool inWindow = dock && window && dock->parentWidget() == window.data();
        if (!inTab)
        {
            const int tab = TabFor(it.key());
            if (tab >= 0)
            {
                m_tabs->removeTab(tab);
            }
        }
        if (!inWindow && m_windows.contains(it.key()))
        {
            m_windows.remove(it.key());
            if (window)
            {
                window->hide();
                window->deleteLater();
            }
        }
        if (!inTab && !inWindow)
        {
            it = m_tools.erase(it);
        }
        else
        {
            ++it;
        }
    }
    QWidget* current = m_stack->currentWidget();
    if (!current || m_stack->indexOf(current) < 0 || (current != m_world && m_tabs->currentIndex() <= 0))
    {
        ShowWorld();
    }
    m_tabs->setVisible(m_tabs->count() > 1);
}

// ---------- games: one window, many games ----------

bool HearthWorkspace::Embedded()
{
    return qEnvironmentVariableIsSet("HEARTH_EMBED_FILE");
}

namespace
{
    // MB from /proc/meminfo (MemTotal, MemAvailable); 0 when it can't be read.
    qint64 MemInfo(const char* key)
    {
        QFile file(QStringLiteral("/proc/meminfo"));
        if (!file.open(QIODevice::ReadOnly))
        {
            return 0;
        }
        const QByteArray want = QByteArray(key) + ':';
        for (const QByteArray& line : file.readAll().split('\n'))
        {
            if (line.startsWith(want))
            {
                return line.mid(want.size()).trimmed().split(' ').value(0).toLongLong() / 1024;
            }
        }
        return 0;
    }

    QString GameName(const QString& path)
    {
        QFile projectFile(QDir(path).filePath(QStringLiteral("project.json")));
        if (projectFile.open(QIODevice::ReadOnly))
        {
            const QJsonObject data = QJsonDocument::fromJson(projectFile.readAll()).object();
            const QString display = data.value(QStringLiteral("display_name")).toString();
            return display.isEmpty() ? data.value(QStringLiteral("project_name")).toString(QFileInfo(path).fileName()) : display;
        }
        return QFileInfo(path).fileName();
    }

    // Asks a game's editor to save its level, through its Claude bridge's file inbox (the same as the helper does).
    void AskToSave(const QString& projectPath)
    {
        const QString folder = QDir(projectPath).filePath(QStringLiteral("user/ClaudeBridge"));
        QFile session(QDir(folder).filePath(QStringLiteral("session.json")));
        if (!session.open(QIODevice::ReadOnly))
        {
            return;
        }
        const QString token = QJsonDocument::fromJson(session.readAll()).object().value(QStringLiteral("token")).toString();
        if (token.isEmpty())
        {
            return;
        }
        QDir().mkpath(QDir(folder).filePath(QStringLiteral("inbox")));
        const QString id = QStringLiteral("workspace-%1").arg(QDateTime::currentMSecsSinceEpoch());
        QJsonObject request;
        request[QStringLiteral("id")] = id;
        request[QStringLiteral("token")] = token;
        request[QStringLiteral("op")] = QStringLiteral("python");
        request[QStringLiteral("code")] = QStringLiteral(
            "import azlmbr.legacy.general as g\nif g.is_in_game_mode(): g.exit_game_mode()\ng.save_level()\nprint('saved')\n");
        QSaveFile out(QDir(folder).filePath(QStringLiteral("inbox/%1.json").arg(id)));
        if (out.open(QIODevice::WriteOnly))
        {
            out.write(QJsonDocument(request).toJson(QJsonDocument::Compact));
            out.commit();
        }
    }

    void Signal(QProcess* process, [[maybe_unused]] int which)
    {
#if defined(Q_OS_LINUX)
        if (process && process->processId() > 0)
        {
            ::kill(static_cast<pid_t>(process->processId()), which);
        }
#endif
    }
}

int HearthWorkspace::GameTab(const QString& path) const
{
    for (int i = 0; i < m_games->count(); ++i)
    {
        if (QFileInfo(m_games->tabData(i).toString()).canonicalFilePath() == QFileInfo(path).canonicalFilePath())
        {
            return i;
        }
    }
    return -1;
}

HearthWorkspace::Game* HearthWorkspace::GameAt(int tab)
{
    const QString path = m_games->tabData(tab).toString();
    for (Game& game : m_otherGames)
    {
        if (game.path == path)
        {
            return &game;
        }
    }
    return nullptr;
}

void HearthWorkspace::GamesMenu()
{
    const QString here = m_games->tabData(0).toString();
    const QDir folder = QFileInfo(here).dir();
    QMenu menu(this);
    for (const QFileInfo& entry : folder.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
    {
        if (!QFileInfo::exists(QDir(entry.filePath()).filePath(QStringLiteral("project.json"))))
        {
            continue;
        }
        const QString path = entry.canonicalFilePath();
        const bool open = GameTab(path) >= 0;
        QAction* action = menu.addAction(GameName(path) + (open ? tr("  · open") : QString()));
        connect(action, &QAction::triggered, this, [this, path]() { OpenGame(path); });
    }
    if (menu.isEmpty())
    {
        menu.addAction(tr("No other games in %1").arg(folder.path()))->setEnabled(false);
    }
    menu.exec(m_addGame->mapToGlobal(QPoint(0, m_addGame->height())));
}

void HearthWorkspace::OpenGame(const QString& projectPath)
{
    const int existing = GameTab(projectPath);
    if (existing >= 0)
    {
        m_games->setCurrentIndex(existing);
        return;
    }
    Game game;
    game.path = QFileInfo(projectPath).canonicalFilePath();
    game.name = GameName(game.path);
    game.page = new QWidget(m_gameStack);
    game.page->setStyleSheet(QStringLiteral("background-color: #0D0C0B;"));
    auto* pageLayout = new QVBoxLayout(game.page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    game.note = new QLabel(game.page);
    game.note->setAlignment(Qt::AlignCenter);
    game.note->setStyleSheet(QStringLiteral("color: #B8AFA4; font-size: 15px;"));
    pageLayout->addWidget(game.note);
    m_gameStack->addWidget(game.page);
    m_otherGames.push_back(game);
    const int tab = m_games->addTab(game.name);
    m_games->setTabData(tab, game.path);
    m_games->setTabToolTip(tab, game.path);
    StartGame(m_otherGames.last());
    m_games->setCurrentIndex(tab);
}

void HearthWorkspace::StartGame(Game& game)
{
    game.closedForMemory = false;
    game.embedded = false;
    game.paused = false;
    game.note->setText(tr("Opening %1…").arg(game.name));
    game.note->show();
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR", QDir::tempPath());
    game.embedFile = QDir(runtime).filePath(QStringLiteral("hearth-embed-%1-%2.json")
        .arg(QCoreApplication::applicationPid()).arg(QDateTime::currentMSecsSinceEpoch()));
    QFile::remove(game.embedFile);
    auto* process = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("HEARTH_EMBED_FILE"), game.embedFile);
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("xcb"));   // a window this one can show inside it
    env.remove(QStringLiteral("LD_LIBRARY_PATH"));
    process->setProcessEnvironment(env);
    process->setWorkingDirectory(game.path);
    process->setProgram(QCoreApplication::applicationFilePath());
    process->setArguments({ QStringLiteral("--project-path=%1").arg(game.path) });
    process->setProcessChannelMode(QProcess::ForwardedChannels);
    game.process = process;
    const QString path = game.path;
    connect(process, &QProcess::finished, this, [this, path]()
    {
        const int tab = GameTab(path);
        Game* g = tab >= 0 ? GameAt(tab) : nullptr;
        if (!g || g->closedForMemory)
        {
            return;
        }
        // It closed (from its own File menu, or it stopped): its tab goes.
        if (g->page)
        {
            g->page->deleteLater();
        }
        m_games->removeTab(tab);
        m_otherGames.erase(std::remove_if(m_otherGames.begin(), m_otherGames.end(), [&](const Game& x) { return x.path == path; }),
                           m_otherGames.end());
    });
    process->start();

    // Waits for its window, then shows it in the tab.
    auto* wait = new QTimer(this);
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    connect(wait, &QTimer::timeout, this, [this, path, wait, started]()
    {
        const int tab = GameTab(path);
        Game* g = tab >= 0 ? GameAt(tab) : nullptr;
        if (!g || !g->process || g->process->state() == QProcess::NotRunning)
        {
            wait->deleteLater();
            return;
        }
        if (QFile::exists(g->embedFile))
        {
            wait->stop();
            wait->deleteLater();
            Embed(*g);
        }
        else if (QDateTime::currentMSecsSinceEpoch() - started > 5 * 60 * 1000)
        {
            wait->stop();
            wait->deleteLater();
            g->note->setText(tr("%1 is taking a long time to open. It's still starting; its own window will show when it's ready.").arg(g->name));
        }
    });
    wait->start(250);
}

void HearthWorkspace::Embed(Game& game)
{
    QFile file(game.embedFile);
    if (!file.open(QIODevice::ReadOnly))
    {
        return;
    }
    const WId id = static_cast<WId>(QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("winId")).toString().toULongLong());
    file.close();
    QFile::remove(game.embedFile);
    if (QGuiApplication::platformName() != QLatin1String("xcb") || !id)
    {
        // Only X11 lets a window be shown inside another: it stays its own window, still one click away here.
        game.note->setText(tr("%1 is open in its own window (this desktop can't show it inside this one).").arg(game.name));
        return;
    }
    QWindow* foreign = QWindow::fromWinId(id);
    if (!foreign)
    {
        game.note->setText(tr("%1 is open in its own window.").arg(game.name));
        return;
    }
    QWidget* container = QWidget::createWindowContainer(foreign, game.page);
    container->setFocusPolicy(Qt::StrongFocus);
    container->setMinimumSize(320, 200);
    game.page->layout()->addWidget(container);
    game.note->hide();
    game.embedded = true;
    if (m_gameStack->currentWidget() == game.page)
    {
        container->setFocus();
    }
}

void HearthWorkspace::ShowGame(int tab)
{
    if (tab < 0)
    {
        return;
    }
    if (tab == 0)
    {
        m_gameStack->setCurrentWidget(m_local);
        return;
    }
    Game* game = GameAt(tab);
    if (!game || !game->page)
    {
        return;
    }
    if (game->closedForMemory)
    {
        StartGame(*game);   // it was closed to free memory: open it again
    }
    else if (game->paused)
    {
        Signal(game->process, SIGCONT);
        game->paused = false;
    }
    game->lastShown = QDateTime::currentMSecsSinceEpoch();
    m_gameStack->setCurrentWidget(game->page);
}

void HearthWorkspace::SaveAndStop(Game& game, bool forMemory)
{
    QPointer<QProcess> process = game.process;
    if (!process)
    {
        return;
    }
    if (game.paused)
    {
        Signal(process, SIGCONT);
        game.paused = false;
    }
    game.closedForMemory = forMemory;
    AskToSave(game.path);
    // Gives the save a moment, then closes it; stops it for good if it hasn't closed after 20 seconds.
    QTimer::singleShot(4000, this, [process]() { if (process) process->terminate(); });
    QTimer::singleShot(20000, this, [process]() { if (process && process->state() != QProcess::NotRunning) process->kill(); });
    if (forMemory)
    {
        game.embedded = false;
        if (game.page && game.page->layout())
        {
            while (game.page->layout()->count() > 1)
            {
                QLayoutItem* item = game.page->layout()->takeAt(1);
                if (item->widget())
                {
                    item->widget()->deleteLater();
                }
                delete item;
            }
        }
        game.note->setText(tr("%1 was saved and closed to free memory. Click its tab to open it again.").arg(game.name));
        game.note->show();
        const int tab = GameTab(game.path);
        if (tab >= 0)
        {
            m_games->setTabText(tab, tr("%1 · closed").arg(game.name));
        }
    }
}

void HearthWorkspace::CloseGame(int tab)
{
    Game* game = GameAt(tab);
    if (!game)
    {
        return;
    }
    const QString path = game->path;
    QPointer<QWidget> page = game->page;
    SaveAndStop(*game, false);
    m_games->removeTab(tab);
    m_otherGames.erase(std::remove_if(m_otherGames.begin(), m_otherGames.end(), [&](const Game& x) { return x.path == path; }),
                       m_otherGames.end());
    if (page)
    {
        page->deleteLater();
    }
}

void HearthWorkspace::LookAfterMemory()
{
    if (m_otherGames.isEmpty())
    {
        return;
    }
    // A laptop-sized computer (under 12 GB): the games you aren't looking at are paused after half a minute.
    const qint64 total = MemInfo("MemTotal");
    const bool small = total > 0 && total < 12 * 1024;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QWidget* shown = m_gameStack->currentWidget();
    for (Game& game : m_otherGames)
    {
        if (game.page == shown)
        {
            game.lastShown = now;
            continue;
        }
        if (small && !game.paused && !game.closedForMemory && game.embedded && now - game.lastShown > 30 * 1000)
        {
            Signal(game.process, SIGSTOP);
            game.paused = true;
        }
    }
    // Memory running short: the background game used longest ago is saved and closed.
    const qint64 available = MemInfo("MemAvailable");
    if (available > 0 && available < 1200)
    {
        Game* oldest = nullptr;
        for (Game& game : m_otherGames)
        {
            if (game.page != shown && !game.closedForMemory && game.process && (!oldest || game.lastShown < oldest->lastShown))
            {
                oldest = &game;
            }
        }
        if (oldest)
        {
            SaveAndStop(*oldest, true);
        }
    }
}
