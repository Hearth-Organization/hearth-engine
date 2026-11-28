/*
 * Hearth Engine workspace: one window for everything, with tools that can be pulled out into a window of their own.
 * See HearthWorkspace.h. Built on O3DE (Apache-2.0 OR MIT).
 */
#include "EditorDefs.h"

#include "HearthWorkspace.h"

#include "QtViewPaneManager.h"

#include <QCloseEvent>
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

#include <functional>

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
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_tabs = new QTabBar(this);
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

    m_stack = new QStackedWidget(this);
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
    m_tabs->setCurrentIndex(tab);
    m_stack->setCurrentWidget(dock);
    if (QWidget* content = dock->widget())
    {
        content->setFocus();
    }
}

void HearthWorkspace::ShowWorld()
{
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
