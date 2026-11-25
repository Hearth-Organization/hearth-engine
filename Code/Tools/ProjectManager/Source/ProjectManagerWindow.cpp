/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "ProjectManagerWindow.h"
#include "DownloadController.h"
#include "ProjectManagerBuses.h"
#include "PythonBindingsInterface.h"
#include "ScreensCtrl.h"

#include <QCloseEvent>
#include <QMessageBox>

namespace O3DE::ProjectManager
{
    ProjectManagerWindow::ProjectManagerWindow(QWidget* parent, const AZ::IO::PathView& projectPath, ProjectManagerScreen startScreen)
        : QMainWindow(parent)
    {
        if (auto engineInfoOutcome = PythonBindingsInterface::Get()->GetEngineInfo(); engineInfoOutcome)
        {
            auto engineInfo = engineInfoOutcome.GetValue<EngineInfo>();
            auto versionToDisplay = engineInfo.m_displayVersion == "00.00" ?
                                        engineInfo.m_version : engineInfo.m_displayVersion;
            Q_UNUSED(versionToDisplay);
            setWindowTitle(tr("Hearth Engine"));
        }
        else
        {
            setWindowTitle(tr("Hearth Engine"));
        }

        m_downloadController = new DownloadController(this);

        ScreensCtrl* screensCtrl = new ScreensCtrl(nullptr, m_downloadController);

        // currently the tab order on the home page is based on the order of this list
        QVector<ProjectManagerScreen> screenEnums =
        {
            ProjectManagerScreen::Hub,
            ProjectManagerScreen::Projects,
            ProjectManagerScreen::CreateGem,
            ProjectManagerScreen::EditGem,
            ProjectManagerScreen::GemCatalog,
            ProjectManagerScreen::Engine,
            ProjectManagerScreen::CreateProject,
            ProjectManagerScreen::UpdateProject,
            ProjectManagerScreen::GemsGemRepos
        };
        screensCtrl->BuildScreens(screenEnums);

        setCentralWidget(screensCtrl);

        // Hearth Engine: the Hub is the home screen (it replaces the project list). Other screens open on top of it,
        // so Back always comes home to the Hub.
        screensCtrl->ForceChangeToScreen(ProjectManagerScreen::Hub, false);
        if (startScreen != ProjectManagerScreen::Projects && startScreen != ProjectManagerScreen::Hub)
        {
            screensCtrl->ForceChangeToScreen(startScreen);
        }

        if (!projectPath.empty())
        {
            const QString path = QString::fromUtf8(projectPath.Native().data(), aznumeric_cast<int>(projectPath.Native().size()));
            emit screensCtrl->NotifyCurrentProject(path);
        }
    }

    void ProjectManagerWindow::closeEvent(QCloseEvent* event)
    {
        bool canClose = true;
        ProjectManagerUtilityRequestsBus::Broadcast(&ProjectManagerUtilityRequestsBus::Events::CanCloseProjectManager, canClose);

        if (!canClose)
        {
            QMessageBox::StandardButton reply = QMessageBox::question(
                this,
                tr("Project action ongoing"),
                tr("A project action is currently going on. Are you sure you want to exit?"),
                QMessageBox::Yes | QMessageBox::No);

            if (reply == QMessageBox::No)
            {
                event->ignore();
                return;
            }
        }

        QMainWindow::closeEvent(event);
    }
} // namespace O3DE::ProjectManager
