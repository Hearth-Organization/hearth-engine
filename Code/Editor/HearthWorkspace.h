/*
 * Hearth Engine workspace: one window for everything (the owner, Nov 28, 2026: "Can we build it to were there arnt
 * multiple windows that open when trying to say go to voice studios ect like it currently does"). Built on O3DE
 * (Apache-2.0 OR MIT).
 *
 * The editor's centre becomes a row of tabs over one area: "World" (the 3D view, always first) and one tab for each
 * Hearth tool that is open (Voice Studio, Animation, Level Generator...). A tool that used to open as a window of its
 * own opens as a tab here instead, wherever it is opened from (the Tools menu, Ctrl+K, a button in another tool, or
 * Claude). Closing its tab closes the tool, exactly as closing its window did.
 */
#pragma once

#include "Include/EditorCoreAPI.h"

#if !defined(Q_MOC_RUN)
#include <QHash>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QDockWidget)
QT_FORWARD_DECLARE_CLASS(QStackedWidget)
QT_FORWARD_DECLARE_CLASS(QTabBar)

class EDITOR_CORE_API HearthWorkspace : public QWidget
{
    Q_OBJECT

public:
    //! Wraps the 3D view (the main window's old centre) and returns the new centre to put in its place.
    static HearthWorkspace* Install(QWidget* world);
    static HearthWorkspace* Instance();

    //! True for the tools that open as tabs here (the Hearth tools that used to open as their own windows).
    static bool Handles(const QString& paneName);

    //! Takes a tool's freshly made dock widget into its own tab (no title bar, can't be dragged out).
    void Adopt(const QString& paneName, QDockWidget* dock);
    //! Brings a tool's tab to the front (it must have been adopted).
    void Show(const QString& paneName);
    //! Back to the 3D view (Play does this, so the game is never hidden behind a tool).
    void ShowWorld();
    bool IsOpen(const QString& paneName) const;

private:
    explicit HearthWorkspace(QWidget* world);

    void Forget(QWidget* page);
    void CloseTab(int index);
    int TabFor(const QString& paneName) const;

    QTabBar* m_tabs = nullptr;
    QStackedWidget* m_stack = nullptr;
    QWidget* m_world = nullptr;
    QHash<QString, QPointer<QDockWidget>> m_tools;
};
