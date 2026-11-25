/*
 * Hearth Engine "Assets" summary. Built on O3DE (Apache-2.0 OR MIT). See HearthAssetsSummary.h.
 */

#include "EditorDefs.h"

#include "HearthAssetsSummary.h"
#include "HearthEditorShell.h"
#include "LyViewPaneNames.h"
#include "QtViewPaneManager.h"

#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>

#include <QDir>
#include <QDirIterator>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    struct Kind
    {
        const char* name;
        QStringList patterns;
        const char* search;
    };

    const QVector<Kind>& Kinds()
    {
        static const QVector<Kind> kinds = {
            { "Materials", { QStringLiteral("*.material") }, "@ .material" },
            { "Textures", { QStringLiteral("*.png"), QStringLiteral("*.tga"), QStringLiteral("*.jpg"), QStringLiteral("*.tif"), QStringLiteral("*.dds") }, "@ .png" },
            { "Prefabs", { QStringLiteral("*.prefab") }, "@ .prefab" },
            { "Sounds", { QStringLiteral("*.wav"), QStringLiteral("*.ogg"), QStringLiteral("*.mp3"), QStringLiteral("*.flac") }, "@ .wav" },
        };
        return kinds;
    }
} // namespace

void HearthAssetsSummary::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::LeftDockWidgetArea;
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthAssetsSummary>("Assets", "Tools", options);
}

HearthAssetsSummary::HearthAssetsSummary(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthAssets"));
    setStyleSheet(QStringLiteral("QWidget#HearthAssets { background-color: #171513; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);
    root->addWidget(Hearth::MakeLabel(tr("Assets"), QStringLiteral("title"), this));
    auto* grid = new QGridLayout();
    grid->setSpacing(8);
    for (int i = 0; i < Kinds().size(); ++i)
    {
        auto* tile = new QPushButton(this);
        tile->setCursor(Qt::PointingHandCursor);
        tile->setMinimumHeight(56);
        tile->setStyleSheet(QStringLiteral("QPushButton { background-color: rgba(255, 255, 255, 8); border: none; border-radius: 12px; text-align: left; }"
                                           "QPushButton:hover { background-color: rgba(232, 116, 59, 31); }"));
        auto* layout = new QVBoxLayout(tile);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(0);
        m_counts[i] = Hearth::MakeLabel(QStringLiteral("…"), QStringLiteral("title"), tile);
        m_counts[i]->setStyleSheet(QStringLiteral("background: transparent; color: #ECE6DE; font-size: 18px; font-weight: 600;"));
        m_counts[i]->setAttribute(Qt::WA_TransparentForMouseEvents);
        QLabel* name = Hearth::MakeLabel(tr(Kinds()[i].name), QStringLiteral("small"), tile);
        name->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(m_counts[i]);
        layout->addWidget(name);
        const QString search = QString::fromLatin1(Kinds()[i].search);
        connect(tile, &QPushButton::clicked, this, [search]()
        {
            if (HearthEditorShell* shell = HearthEditorShell::Instance())
            {
                shell->OpenCommandPalette(search); // @ .material lists the materials; picking one shows it in the Asset Browser
            }
        });
        grid->addWidget(tile, i / 2, i % 2);
    }
    root->addLayout(grid);
    root->addStretch(1);

    auto* timer = new QTimer(this);
    timer->setInterval(30000);
    connect(timer, &QTimer::timeout, this, &HearthAssetsSummary::Count);
    timer->start();
    QTimer::singleShot(500, this, &HearthAssetsSummary::Count);
}

void HearthAssetsSummary::Count()
{
    const auto projectPath = AZ::Utils::GetProjectPath();
    const QDir project(QString::fromUtf8(projectPath.c_str()));
    for (int i = 0; i < Kinds().size(); ++i)
    {
        int count = 0;
        QDirIterator it(project.absolutePath(), Kinds()[i].patterns, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            const QString relative = project.relativeFilePath(it.next());
            if (relative.startsWith(QLatin1String("build/")) || relative.startsWith(QLatin1String("Cache/")) ||
                relative.startsWith(QLatin1String("user/")) || relative.contains(QLatin1String("/_autobackup/")))
            {
                continue;
            }
            ++count;
        }
        m_counts[i]->setText(QString::number(count));
    }
}
