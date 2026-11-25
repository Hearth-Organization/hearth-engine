/*
 * Hearth Engine command palette. Built on O3DE (Apache-2.0 OR MIT). See HearthCommandPalette.h.
 */

#include "EditorDefs.h"

#include "HearthCommandPalette.h"
#include "HearthEditorShell.h"
#include "LyViewPaneNames.h"
#include "QtViewPaneManager.h"

#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/Entity.h>
#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/AssetBrowser/AssetBrowserBus.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>

#include <QAction>
#include <QApplication>
#include <QFileInfo>
#include <QPointer>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    void WalkMenu(QMenu* menu, const QString& path, QVector<HearthCommandPalette::Entry>& out)
    {
        for (QAction* action : menu->actions())
        {
            if (action->isSeparator() || !action->isVisible())
            {
                continue;
            }
            const QString text = action->text().remove(QLatin1Char('&')).trimmed();
            if (text.isEmpty())
            {
                continue;
            }
            if (QMenu* sub = action->menu())
            {
                WalkMenu(sub, path + QStringLiteral(" › ") + text, out);
                continue;
            }
            QPointer<QAction> guard(action);
            out.push_back({ QStringLiteral("›"), text, path, action->shortcut().toString(QKeySequence::NativeText),
                            [guard]()
                            {
                                if (guard && guard->isEnabled())
                                {
                                    guard->trigger();
                                }
                            } });
        }
    }
} // namespace

HearthCommandPalette::HearthCommandPalette(QMainWindow* mainWindow, const QString& prefix)
    : QDialog(mainWindow, Qt::FramelessWindowHint | Qt::Popup)
    , m_mainWindow(mainWindow)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setFixedWidth(640);
    setObjectName(QStringLiteral("HearthPalette"));
    setStyleSheet(QStringLiteral(
        "QDialog#HearthPalette { background-color: #171513; border: 1px solid rgba(255, 255, 255, 31); border-radius: 16px; }"
        "QListWidget { background: transparent; border: none; outline: none; }"
        "QListWidget::item { border-radius: 9px; padding: 0px; margin: 1px 0px; }"
        "QListWidget::item:selected { background-color: rgba(232, 116, 59, 41); }"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(4);

    auto* searchRow = new QHBoxLayout();
    searchRow->setContentsMargins(12, 6, 12, 10);
    QLabel* caret = Hearth::MakeLabel(QStringLiteral("›"), QStringLiteral("title"), this);
    caret->setStyleSheet(QStringLiteral("background: transparent; color: #FFCFB4; font-size: 16px;"));
    searchRow->addWidget(caret);
    m_search = new QLineEdit(prefix, this);
    m_search->setPlaceholderText(tr("Search or run a command"));
    m_search->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: none; color: #ECE6DE; font-size: 14px; }"));
    searchRow->addWidget(m_search, 1);
    searchRow->addWidget(Hearth::MakeLabel(tr("Ctrl+K"), QStringLiteral("small"), this));
    layout->addLayout(searchRow);
    auto* line = new QFrame(this);
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background-color: rgba(255, 255, 255, 15); border: none;"));
    layout->addWidget(line);

    m_list = new QListWidget(this);
    m_list->setMinimumHeight(330);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_list, 1);

    auto* hint = Hearth::MakeLabel(tr("Type > for commands · @ for assets · # for entities · ? for help"), QStringLiteral("small"), this);
    hint->setContentsMargins(12, 8, 12, 2);
    layout->addWidget(hint);

    CollectCommands();
    connect(m_search, &QLineEdit::textChanged, this, &HearthCommandPalette::UpdateResults);
    connect(m_list, &QListWidget::itemActivated, this, [this]() { RunSelected(); });
    connect(m_list, &QListWidget::itemClicked, this, [this]() { RunSelected(); });
    m_search->installEventFilter(this);

    // Sit near the top of the editor, centred.
    const QRect area = mainWindow->geometry();
    adjustSize();
    move(area.center().x() - width() / 2, area.top() + 110);
    UpdateResults();
    QTimer::singleShot(0, m_search, [this]() { m_search->setFocus(); m_search->end(false); });
}

void HearthCommandPalette::CollectCommands()
{
    // Hearth commands first (the ones the mockup shows), then every menu command.
    HearthEditorShell* shell = HearthEditorShell::Instance();
    if (shell)
    {
        m_commands.push_back({ QStringLiteral("▶"), tr("Play in the viewport"), tr("Game"), QStringLiteral("Ctrl+G"), [shell]() { shell->PlayInViewport(); } });
        m_commands.push_back({ QStringLiteral("▶"), tr("Play full screen"), tr("Game"), QStringLiteral("F11"), [shell]() { shell->PlayFullScreen(); } });
        m_commands.push_back({ QStringLiteral("▶"), tr("Play in a standalone game window"), tr("Game"), QStringLiteral("Ctrl+F11"),
                               [shell]() { shell->StartStandaloneGame(); } });
        m_commands.push_back({ QStringLiteral("◎"), tr("Co-op test with 2 players"), tr("Game"), QString(), [shell]() { shell->OpenCoopTest(); } });
        m_commands.push_back({ QStringLiteral("⌁"), tr("Rebuild game code"), tr("Build"), QStringLiteral("Ctrl+B"), [shell]() { shell->RequestBuild(); } });
    }
    for (const auto& pane : QVector<QPair<QString, QString>>{ { QStringLiteral("Level Generator"), QStringLiteral("Ctrl+Shift+L") },
                                                               { QStringLiteral("Console and logs"), QString() },
                                                               { QStringLiteral("Claude"), QString() },
                                                               { QStringLiteral("Lighting and look"), QString() },
                                                               { QStringLiteral("Co-op test"), QString() },
                                                               { QStringLiteral("Profiler"), QString() },
                                                               { QStringLiteral("Voice Studio"), QString() },
                                                               { QStringLiteral("Hearth Settings"), QString() },
                                                               { QStringLiteral("Browse assets"), QString() },
                                                               { QStringLiteral("Import"), QString() },
                                                               { QStringLiteral("Asset processing"), QString() },
                                                               { QStringLiteral("Material editor"), QString() },
                                                               { QStringLiteral("Project settings"), QString() },
                                                               { QStringLiteral("Package the game"), QString() },
                                                               { QStringLiteral("Physics"), QString() },
                                                               { QStringLiteral("Audio"), QString() },
                                                               { QStringLiteral("Prefab editing"), QString() } })
    {
        const QString name = pane.first;
        m_commands.push_back({ QStringLiteral("◫"), tr("Open %1").arg(name), tr("Tools"), pane.second,
                               [name]() { QtViewPaneManager::instance()->OpenPane(name); } });
    }
    m_commands.push_back({ QStringLiteral("!"), tr("Open the logs folder"), tr("Console"), QString(),
                           []() { QDesktopServices::openUrl(QUrl::fromLocalFile(Hearth::LogsFolder())); } });

    if (QMenuBar* bar = m_mainWindow->menuBar())
    {
        for (QAction* top : bar->actions())
        {
            if (QMenu* menu = top->menu())
            {
                WalkMenu(menu, top->text().remove(QLatin1Char('&')).trimmed(), m_commands);
            }
        }
    }
}

void HearthCommandPalette::CollectAssets()
{
    if (m_assetsCollected)
    {
        return;
    }
    m_assetsCollected = true;
    const auto projectPath = AZ::Utils::GetProjectPath();
    const QDir project(QString::fromUtf8(projectPath.c_str()));
    static const QStringList kinds = { QStringLiteral("*.prefab"), QStringLiteral("*.material"), QStringLiteral("*.png"),
                                       QStringLiteral("*.tga"), QStringLiteral("*.jpg"), QStringLiteral("*.fbx"),
                                       QStringLiteral("*.wav"), QStringLiteral("*.ogg"), QStringLiteral("*.mp3"),
                                       QStringLiteral("*.lua"), QStringLiteral("*.scriptcanvas"), QStringLiteral("*.uicanvas"),
                                       QStringLiteral("*.azasset"), QStringLiteral("*.physxmaterial") };
    QDirIterator it(project.absolutePath(), kinds, QDir::Files, QDirIterator::Subdirectories);
    int count = 0;
    while (it.hasNext() && count < 5000)
    {
        const QString path = it.next();
        const QString relative = project.relativeFilePath(path);
        if (relative.startsWith(QLatin1String("build/")) || relative.startsWith(QLatin1String("Cache/")) ||
            relative.startsWith(QLatin1String("user/")) || relative.contains(QLatin1String("/_autobackup/")))
        {
            continue;
        }
        ++count;
        const QString name = QFileInfo(path).fileName();
        m_assets.push_back({ QStringLiteral("◫"), tr("Go to: %1").arg(name), tr("Asset · %1").arg(QFileInfo(relative).path()), QString(),
                             [path]()
                             {
                                 QtViewPaneManager::instance()->OpenPane(LyViewPane::AssetBrowser);
                                 const AZStd::string assetPath(path.toUtf8().constData());
                                 AzToolsFramework::AssetBrowser::AssetBrowserViewRequestBus::Broadcast(
                                     &AzToolsFramework::AssetBrowser::AssetBrowserViewRequestBus::Events::SelectFileAtPath, assetPath);
                             } });
    }
}

void HearthCommandPalette::CollectEntities()
{
    if (m_entitiesCollected)
    {
        return;
    }
    m_entitiesCollected = true;
    QVector<QPair<AZ::EntityId, QString>> found;
    AZ::ComponentApplicationBus::Broadcast(&AZ::ComponentApplicationRequests::EnumerateEntities,
        [&found](AZ::Entity* entity)
        {
            if (!entity)
            {
                return;
            }
            bool editorEntity = false;
            AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(
                editorEntity, &AzToolsFramework::EditorEntityContextRequests::IsEditorEntity, entity->GetId());
            if (editorEntity)
            {
                found.push_back({ entity->GetId(), QString::fromUtf8(entity->GetName().c_str()) });
            }
        });
    for (const auto& item : found)
    {
        const AZ::EntityId id = item.first;
        m_entities.push_back({ QStringLiteral("#"), item.second, tr("Entity"), QString(),
                               [id]()
                               {
                                   AzToolsFramework::ToolsApplicationRequestBus::Broadcast(
                                       &AzToolsFramework::ToolsApplicationRequests::SetSelectedEntities, AzToolsFramework::EntityIdList{ id });
                               } });
    }
}

int HearthCommandPalette::Score(const QString& text, const QString& query)
{
    if (query.isEmpty())
    {
        return 1;
    }
    const QString lower = text.toLower();
    const int at = lower.indexOf(query);
    if (at == 0)
    {
        return 1000 - text.size();
    }
    if (at > 0)
    {
        return 600 - at - text.size() / 4;
    }
    // every letter in order
    int pos = 0;
    for (const QChar c : query)
    {
        pos = lower.indexOf(c, pos);
        if (pos < 0)
        {
            return 0;
        }
        ++pos;
    }
    return 100 - text.size() / 4;
}

void HearthCommandPalette::UpdateResults()
{
    QString query = m_search->text().trimmed();
    QChar mode;
    if (!query.isEmpty() && QStringLiteral(">@#?").contains(query.front()))
    {
        mode = query.front();
        query = query.mid(1).trimmed();
    }
    query = query.toLower();

    static const QVector<Entry> help = {
        { QStringLiteral("?"), QStringLiteral("> run a command"), QStringLiteral("Help"), QString(), {} },
        { QStringLiteral("?"), QStringLiteral("@ find an asset and show it in the Asset Browser"), QStringLiteral("Help"), QString(), {} },
        { QStringLiteral("?"), QStringLiteral("# find an entity in the level and select it"), QStringLiteral("Help"), QString(), {} },
    };

    QVector<QPair<int, const Entry*>> scored;
    auto consider = [&](const QVector<Entry>& entries)
    {
        for (const Entry& entry : entries)
        {
            const int score = qMax(Score(entry.name, query), Score(entry.category + QLatin1Char(' ') + entry.name, query) / 2);
            if (score > 0)
            {
                scored.push_back({ score, &entry });
            }
        }
    };
    if (mode == QLatin1Char('?'))
    {
        consider(help);
    }
    else
    {
        if (mode.isNull() || mode == QLatin1Char('>'))
        {
            consider(m_commands);
        }
        if (mode == QLatin1Char('@') || (mode.isNull() && query.size() >= 2))
        {
            CollectAssets();
            consider(m_assets);
        }
        if (mode == QLatin1Char('#') || (mode.isNull() && query.size() >= 2))
        {
            CollectEntities();
            consider(m_entities);
        }
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    m_list->clear();
    m_shown.clear();
    const int limit = qMin(scored.size(), 40);
    for (int i = 0; i < limit; ++i)
    {
        const Entry* entry = scored[i].second;
        m_shown.push_back(entry);
        auto* item = new QListWidgetItem(m_list);
        auto* row = new QWidget();
        row->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(12, 7, 12, 7);
        rowLayout->setSpacing(12);
        auto* icon = new QLabel(entry->icon, row);
        icon->setFixedSize(26, 26);
        icon->setAlignment(Qt::AlignCenter);
        icon->setStyleSheet(QStringLiteral("background-color: rgba(255, 255, 255, 13); color: #FFCFB4; border-radius: 8px; font-size: 12px;"));
        rowLayout->addWidget(icon);
        auto* texts = new QVBoxLayout();
        texts->setSpacing(1);
        texts->addWidget(Hearth::MakeLabel(entry->name, QStringLiteral("body"), row));
        texts->addWidget(Hearth::MakeLabel(entry->category, QStringLiteral("small"), row));
        rowLayout->addLayout(texts, 1);
        if (!entry->shortcut.isEmpty())
        {
            QLabel* keys = Hearth::MakeLabel(entry->shortcut, QStringLiteral("small"), row);
            keys->setStyleSheet(QStringLiteral("background: transparent; color: #8D857B; font-family: \"Geist Mono\"; font-size: 11px;"));
            rowLayout->addWidget(keys);
        }
        item->setSizeHint(QSize(0, 48));
        m_list->setItemWidget(item, row);
    }
    if (m_list->count() > 0)
    {
        m_list->setCurrentRow(0);
    }
}

void HearthCommandPalette::RunSelected()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_shown.size())
    {
        return;
    }
    std::function<void()> run = m_shown[row]->run;
    if (!run)
    {
        return;
    }
    close();
    QTimer::singleShot(0, qApp, run);
}

bool HearthCommandPalette::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_search && event->type() == QEvent::KeyPress)
    {
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key())
        {
        case Qt::Key_Down:
            m_list->setCurrentRow(qMin(m_list->currentRow() + 1, m_list->count() - 1));
            return true;
        case Qt::Key_Up:
            m_list->setCurrentRow(qMax(m_list->currentRow() - 1, 0));
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            RunSelected();
            return true;
        case Qt::Key_Escape:
            close();
            return true;
        default:
            break;
        }
    }
    return QDialog::eventFilter(watched, event);
}
