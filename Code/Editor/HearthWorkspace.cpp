/*
 * Hearth Engine workspace: one window for everything. See HearthWorkspace.h. Built on O3DE (Apache-2.0 OR MIT).
 */
#include "EditorDefs.h"

#include "HearthWorkspace.h"

#include "QtViewPaneManager.h"

#include <QDockWidget>
#include <QSet>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
    HearthWorkspace* s_instance = nullptr;

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

    const char* kWorldTab = "";   // the World tab's data: no pane name
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
    m_tabs->setStyleSheet(QStringLiteral(
        "QTabBar#HearthWorkspaceTabs { background-color: #0D0C0B; }"
        "QTabBar#HearthWorkspaceTabs::tab { background-color: transparent; color: #B8AFA4; border: none;"
        " border-radius: 15px; padding: 6px 14px; margin: 6px 2px 6px 0px; min-height: 18px; }"
        "QTabBar#HearthWorkspaceTabs::tab:hover { background-color: #1C1917; color: #ECE6DE; }"
        "QTabBar#HearthWorkspaceTabs::tab:selected { background-color: #ECE6DE; color: #1A0F08; font-weight: 600; }"
        "QTabBar#HearthWorkspaceTabs::close-button { subcontrol-position: right; margin-left: 6px; }"));
    layout->addWidget(m_tabs);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(world);
    layout->addWidget(m_stack, 1);

    const int worldTab = m_tabs->addTab(tr("World"));
    m_tabs->setTabData(worldTab, QString::fromLatin1(kWorldTab));
    m_tabs->setTabToolTip(worldTab, tr("Your level in 3D"));
    // The World tab can't be closed.
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
    connect(m_stack, &QStackedWidget::widgetRemoved, this, [this](int) { Forget(nullptr); });
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
    const QPointer<QDockWidget> dock = m_tools.value(paneName);
    return dock && m_stack->indexOf(dock) >= 0;
}

void HearthWorkspace::Adopt(const QString& paneName, QDockWidget* dock)
{
    if (!dock)
    {
        return;
    }
    // Saved window layouts know tools by their dock widget's name; a new name keeps a layout saved before the
    // workspace from pulling the tool back out into a window of its own.
    dock->setObjectName(QStringLiteral("HearthWorkspace/") + paneName);
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setTitleBarWidget(new QWidget(dock));   // the tab is its title
    dock->setFloating(false);
    m_stack->addWidget(dock);
    dock->show();
    m_tools.insert(paneName, dock);

    const int tab = m_tabs->addTab(paneName);
    m_tabs->setTabData(tab, paneName);
    m_tabs->setTabToolTip(tab, tr("Close %1").arg(paneName));
    m_tabs->setVisible(true);

    connect(dock, &QObject::destroyed, this, [this]() { QTimer::singleShot(0, this, [this]() { Forget(nullptr); }); });
}

void HearthWorkspace::Show(const QString& paneName)
{
    const QPointer<QDockWidget> dock = m_tools.value(paneName);
    const int tab = TabFor(paneName);
    if (!dock || tab < 0 || m_stack->indexOf(dock) < 0)
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

void HearthWorkspace::CloseTab(int index)
{
    const QString name = m_tabs->tabData(index).toString();
    if (name.isEmpty())
    {
        return;   // World
    }
    // The same as closing the tool's window: it may ask first (unsaved changes), and an open popup keeps it open.
    QtViewPaneManager::instance()->ClosePane(name, QtViewPane::CloseMode::Destroy);
    Forget(nullptr);
}

void HearthWorkspace::Forget(QWidget*)
{
    for (auto it = m_tools.begin(); it != m_tools.end();)
    {
        const QPointer<QDockWidget>& dock = it.value();
        if (!dock || m_stack->indexOf(dock) < 0)
        {
            const int tab = TabFor(it.key());
            if (tab >= 0)
            {
                m_tabs->removeTab(tab);
            }
            it = m_tools.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (m_stack->currentWidget() == nullptr || m_stack->indexOf(m_stack->currentWidget()) < 0)
    {
        ShowWorld();
    }
    m_tabs->setVisible(m_tabs->count() > 1);
}
