/*
 * Hearth Engine Project settings. Built on O3DE (Apache-2.0 OR MIT). See HearthProjectPane.h.
 */

#include "EditorDefs.h"

#include "HearthProjectPane.h"
#include "HearthAgentClient.h"

#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    QString ProjectRoot()
    {
        return QString::fromUtf8(AZ::Utils::GetProjectPath().c_str());
    }

    QString ProjectSettingsPath()
    {
        return QDir(ProjectRoot()).filePath(QStringLiteral("Registry/hearth_project.setreg"));
    }

    //! "https://github.com/Owner/repo.git" or "git@github.com:Owner/repo.git" -> "Owner/repo".
    QString ProjectRepo()
    {
        QProcess git;
        git.setWorkingDirectory(ProjectRoot());
        git.start(QStringLiteral("git"), { QStringLiteral("remote"), QStringLiteral("get-url"), QStringLiteral("origin") });
        if (!git.waitForFinished(3000))
        {
            return QString();
        }
        const QString url = QString::fromUtf8(git.readAllStandardOutput()).trimmed();
        const QRegularExpressionMatch match = QRegularExpression(QStringLiteral("github\\.com[:/]([^/]+/[^/.]+)")).match(url);
        return match.hasMatch() ? match.captured(1) : url;
    }

    QWidget* ProjectRow(QWidget* parent, QVBoxLayout* into, const QString& label, QWidget* control)
    {
        auto* row = new QFrame(parent);
        row->setObjectName(QStringLiteral("ProjectRow"));
        row->setStyleSheet(QStringLiteral("QFrame#ProjectRow { border-top: 1px solid rgba(255, 255, 255, 15); }"));
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(4, 10, 4, 10);
        QLabel* name = Hearth::MakeLabel(label, QStringLiteral("dim"), row);
        name->setFixedWidth(170);
        layout->addWidget(name);
        control->setParent(row);
        layout->addWidget(control, 1);
        into->addWidget(row);
        return row;
    }
} // namespace

void HearthProjectPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(110, 80, 1360, 840);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthProjectPane>("Project settings", "Tools", options);
}

HearthProjectPane::HearthProjectPane(QWidget* parent)
    : QWidget(parent)
    , m_agent(std::make_unique<HearthAgentClient>(this))
{
    setObjectName(QStringLiteral("HearthProject"));
    setStyleSheet(QStringLiteral("QWidget#HearthProject { background-color: #0D0C0B; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Project settings"), QString());
    if (auto* titles = header->itemAt(1) ? header->itemAt(1)->layout() : nullptr)
    {
        m_subtitle = titles->count() > 1 ? qobject_cast<QLabel*>(titles->itemAt(1)->widget()) : nullptr;
    }
    header->addStretch(1);
    m_changedChip = Hearth::MakeChip(QString(), QStringLiteral("warn"), this);
    m_changedChip->hide();
    header->addWidget(m_changedChip);
    QScopedPointer<QSettings> settings(Hearth::NewSettings());
    const bool cloud = settings->value(QStringLiteral("laptop/cloudEngineBuilds"), true).toBool();
    m_rebuild = Hearth::MakeButton(cloud ? tr("Rebuild on GitHub") : tr("Rebuild here"), true, this);
    connect(m_rebuild, &QPushButton::clicked, this, [this]() { Rebuild(false); });
    header->addWidget(m_rebuild);
    root->addLayout(header);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);

    // The game.
    QVBoxLayout* game = nullptr;
    QFrame* gameCard = Hearth::MakeCard(this, &game);
    gameCard->setFixedWidth(520);
    game->addWidget(Hearth::MakeLabel(tr("The game"), QStringLiteral("title"), gameCard));
    m_name = new QLineEdit(gameCard);
    connect(m_name, &QLineEdit::editingFinished, this, [this]()
    {
        const QString name = m_name->text().trimmed();
        QJsonObject json = ProjectJson();
        if (!name.isEmpty() && json.value(QStringLiteral("display_name")).toString() != name)
        {
            json.insert(QStringLiteral("display_name"), name);
            SaveProjectJson(json);
            if (m_subtitle)
            {
                m_subtitle->setText(name);
            }
        }
    });
    ProjectRow(gameCard, game, tr("Name"), m_name);
    m_engine = Hearth::MakeLabel(QString(), QStringLiteral("body"), gameCard);
    ProjectRow(gameCard, game, tr("Engine"), m_engine);
    m_firstLevel = new QComboBox(gameCard);
    connect(m_firstLevel, QOverload<int>::of(&QComboBox::activated), this, [this](int)
    {
        // The game launcher runs autoexec.cfg: its LoadLevel line picks the first level.
        const QString level = m_firstLevel->currentData().toString();
        const QString path = QDir(ProjectRoot()).filePath(QStringLiteral("autoexec.cfg"));
        QStringList lines;
        QFile in(path);
        if (in.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            lines = QString::fromUtf8(in.readAll()).split(QLatin1Char('\n'));
            in.close();
        }
        lines.erase(std::remove_if(lines.begin(), lines.end(), [](const QString& l) { return l.trimmed().startsWith(QLatin1String("LoadLevel"), Qt::CaseInsensitive); }), lines.end());
        while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
        {
            lines.removeLast();
        }
        if (!level.isEmpty())
        {
            lines << QStringLiteral("LoadLevel %1").arg(level);
        }
        QSaveFile out(path);
        if (out.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            out.write((lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
            out.commit();
        }
    });
    ProjectRow(gameCard, game, tr("First level"), m_firstLevel);
    m_players = new QComboBox(gameCard);
    m_players->addItem(tr("1, on your own"), 1);
    m_players->addItem(tr("1 to 2, co-op"), 2);
    m_players->addItem(tr("1 to 4, co-op"), 4);
    m_players->addItem(tr("1 to 8, co-op"), 8);
    connect(m_players, QOverload<int>::of(&QComboBox::activated), this, [this](int)
    {
        SetProjectSetting(QStringLiteral("maxPlayers"), m_players->currentData().toInt());
    });
    ProjectRow(gameCard, game, tr("Players"), m_players);
    auto* iconHolder = new QWidget(gameCard);
    auto* iconLayout = new QHBoxLayout(iconHolder);
    iconLayout->setContentsMargins(0, 0, 0, 0);
    m_icon = new QLabel(iconHolder);
    m_icon->setFixedSize(48, 48);
    iconLayout->addWidget(m_icon);
    QPushButton* changeIcon = Hearth::MakeButton(tr("Change"), false, iconHolder);
    connect(changeIcon, &QPushButton::clicked, this, [this]()
    {
        const QString picked = QFileDialog::getOpenFileName(this, tr("Game icon"), QDir::homePath(), tr("Images (*.png *.jpg *.jpeg)"));
        if (picked.isEmpty())
        {
            return;
        }
        QJsonObject json = ProjectJson();
        const QString iconName = json.value(QStringLiteral("icon_path")).toString(QStringLiteral("preview.png"));
        const QString target = QDir(ProjectRoot()).filePath(iconName);
        QImage image(picked);
        if (image.isNull() || !image.scaled(256, 256, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation).save(target, "PNG"))
        {
            QMessageBox::warning(this, tr("Game icon"), tr("Couldn't use that picture."));
            return;
        }
        json.insert(QStringLiteral("icon_path"), iconName);
        SaveProjectJson(json);
        LoadGame();
    });
    iconLayout->addWidget(changeIcon);
    iconLayout->addStretch(1);
    ProjectRow(gameCard, game, tr("Icon"), iconHolder);
    m_repo = new QPushButton(gameCard);
    m_repo->setCursor(Qt::PointingHandCursor);
    m_repo->setStyleSheet(QStringLiteral("QPushButton { text-align: left; border: none; background: transparent; color: #FFCFB4; font-family: \"Geist Mono\"; }"));
    connect(m_repo, &QPushButton::clicked, this, [this]()
    {
        const QString repo = m_repo->text();
        if (repo.contains(QLatin1Char('/')) && !repo.contains(QLatin1Char(':')))
        {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/%1").arg(repo)));
        }
    });
    ProjectRow(gameCard, game, tr("Code and game repo"), m_repo);
    game->addStretch(1);
    body->addWidget(gameCard);

    // Engine parts.
    QVBoxLayout* parts = nullptr;
    QFrame* partsCard = Hearth::MakeCard(this, &parts);
    auto* partsTitle = new QHBoxLayout();
    partsTitle->addWidget(Hearth::MakeLabel(tr("Engine parts"), QStringLiteral("title"), partsCard));
    partsTitle->addStretch(1);
    m_partsSummary = Hearth::MakeLabel(QString(), QStringLiteral("dim"), partsCard);
    partsTitle->addWidget(m_partsSummary);
    parts->addLayout(partsTitle);
    auto* gridHolder = new QWidget(partsCard);
    m_partsGrid = new QGridLayout(gridHolder);
    m_partsGrid->setContentsMargins(0, 0, 0, 0);
    m_partsGrid->setSpacing(8);
    parts->addWidget(gridHolder);
    m_allToggle = new QPushButton(tr("All parts  ›"), partsCard);
    m_allToggle->setStyleSheet(QStringLiteral("QPushButton { text-align: left; border: none; background: transparent; color: #8D857B; padding: 6px 2px; }"
                                              "QPushButton:hover { color: #ECE6DE; }"));
    parts->addWidget(m_allToggle);
    auto* scroll = new QScrollArea(partsCard);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_allBody = new QWidget(scroll);
    m_allLayout = new QVBoxLayout(m_allBody);
    m_allLayout->setContentsMargins(0, 0, 0, 0);
    m_allLayout->setSpacing(2);
    scroll->setWidget(m_allBody);
    scroll->setVisible(false);
    parts->addWidget(scroll, 1);
    connect(m_allToggle, &QPushButton::clicked, this, [this, scroll]()
    {
        scroll->setVisible(!scroll->isVisible());
        m_allToggle->setText(scroll->isVisible() ? tr("All parts  ⌄") : tr("All parts  ›"));
    });
    parts->addStretch(0);
    body->addWidget(partsCard, 1);
    root->addLayout(body, 1);

    m_cloudTimer = new QTimer(this);
    m_cloudTimer->setInterval(5000);
    connect(m_cloudTimer, &QTimer::timeout, this, &HearthProjectPane::WatchCloud);
}

HearthProjectPane::~HearthProjectPane() = default;

void HearthProjectPane::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    LoadGame();
    LoadParts();
}

QJsonObject HearthProjectPane::ProjectJson() const
{
    QFile file(QDir(ProjectRoot()).filePath(QStringLiteral("project.json")));
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

void HearthProjectPane::SaveProjectJson(const QJsonObject& json)
{
    QSaveFile out(QDir(ProjectRoot()).filePath(QStringLiteral("project.json")));
    if (out.open(QIODevice::WriteOnly))
    {
        out.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
        out.commit();
    }
}

QJsonObject HearthProjectPane::ProjectSettings() const
{
    QFile file(ProjectSettingsPath());
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("Hearth")).toObject().value(QStringLiteral("Project")).toObject();
}

void HearthProjectPane::SetProjectSetting(const QString& key, const QJsonValue& value)
{
    // Registry/hearth_project.setreg: /Hearth/Project/<key>, read by the game and by the Hearth Engine agent.
    QJsonObject all;
    QFile file(ProjectSettingsPath());
    if (file.open(QIODevice::ReadOnly))
    {
        all = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
    }
    QJsonObject hearth = all.value(QStringLiteral("Hearth")).toObject();
    QJsonObject project = hearth.value(QStringLiteral("Project")).toObject();
    project.insert(key, value);
    hearth.insert(QStringLiteral("Project"), project);
    all.insert(QStringLiteral("Hearth"), hearth);
    QDir().mkpath(QFileInfo(ProjectSettingsPath()).path());
    QSaveFile out(ProjectSettingsPath());
    if (out.open(QIODevice::WriteOnly))
    {
        out.write(QJsonDocument(all).toJson(QJsonDocument::Indented));
        out.commit();
    }
}

void HearthProjectPane::LoadGame()
{
    const QJsonObject json = ProjectJson();
    const QString name = json.value(QStringLiteral("display_name")).toString(json.value(QStringLiteral("project_name")).toString());
    m_name->setText(name);
    if (m_subtitle)
    {
        m_subtitle->setText(name);
    }
    m_engine->setText(tr("Hearth Engine %1").arg(Hearth::Version()));

    // Levels, and the one autoexec.cfg loads first.
    QString first;
    QFile autoexec(QDir(ProjectRoot()).filePath(QStringLiteral("autoexec.cfg")));
    if (autoexec.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        for (const QString& line : QString::fromUtf8(autoexec.readAll()).split(QLatin1Char('\n')))
        {
            if (line.trimmed().startsWith(QLatin1String("LoadLevel"), Qt::CaseInsensitive))
            {
                first = line.trimmed().section(QLatin1Char(' '), 1).trimmed();
            }
        }
    }
    m_firstLevel->clear();
    m_firstLevel->addItem(tr("None (the game picks)"), QString());
    for (const QString& level : QDir(QDir(ProjectRoot()).filePath(QStringLiteral("Levels"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        m_firstLevel->addItem(level, level);
    }
    m_firstLevel->setCurrentIndex(qMax(0, m_firstLevel->findData(first, Qt::UserRole, Qt::MatchFixedString)));

    const int players = ProjectSettings().value(QStringLiteral("maxPlayers")).toInt(4);
    m_players->setCurrentIndex(qMax(0, m_players->findData(players)));

    const QString iconPath = QDir(ProjectRoot()).filePath(json.value(QStringLiteral("icon_path")).toString(QStringLiteral("preview.png")));
    QImageReader reader(iconPath);
    reader.setScaledSize(QSize(96, 96));
    const QImage icon = reader.read();
    if (!icon.isNull())
    {
        QPixmap pixmap = QPixmap::fromImage(icon);
        pixmap.setDevicePixelRatio(2.0);
        m_icon->setPixmap(pixmap);
    }
    else
    {
        m_icon->setPixmap(Hearth::FlameTile(48, devicePixelRatioF()));
    }
    const QString repo = ProjectRepo();
    m_repo->setText(repo.isEmpty() ? tr("No git repo") : repo);
}

void HearthProjectPane::LoadParts()
{
    m_agent->Post(QStringLiteral("project_parts"), {}, [this](const QJsonObject& answer, bool reached)
    {
        if (!reached || !answer.contains(QStringLiteral("parts")))
        {
            m_partsSummary->setText(reached ? answer.value(QStringLiteral("error")).toString() : tr("The Hearth Engine agent isn't running (the Hub starts it)."));
            return;
        }
        FillParts(answer);
    });
}

void HearthProjectPane::FillParts(const QJsonObject& answer)
{
    while (QLayoutItem* item = m_partsGrid->takeAt(0))
    {
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
    while (QLayoutItem* item = m_allLayout->takeAt(0))
    {
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
    m_partsSummary->setText(tr("%1 of %2 on · fewer parts, less memory").arg(answer.value(QStringLiteral("on")).toInt()).arg(answer.value(QStringLiteral("total")).toInt()));
    const int changed = answer.value(QStringLiteral("changed")).toInt();
    m_changedChip->setText(tr("%n part(s) changed · rebuild needed", nullptr, changed));
    m_changedChip->setVisible(changed > 0 || answer.value(QStringLiteral("pending_rebuild")).toBool());
    if (changed == 0 && answer.value(QStringLiteral("pending_rebuild")).toBool())
    {
        m_changedChip->setText(tr("Rebuild needed"));
    }

    auto addSwitch = [this](QWidget* parent, bool on, bool locked, std::function<void(bool)> changedTo)
    {
        auto* toggle = new Hearth::Toggle(parent);
        toggle->setChecked(on);
        toggle->setEnabled(!locked);
        connect(toggle, &QAbstractButton::toggled, this, [changedTo](bool value) { changedTo(value); });
        return toggle;
    };
    const QJsonArray parts = answer.value(QStringLiteral("parts")).toArray();
    for (int i = 0; i < parts.size(); ++i)
    {
        const QJsonObject part = parts[i].toObject();
        auto* tile = new QFrame();
        tile->setObjectName(QStringLiteral("PartTile"));
        tile->setStyleSheet(QStringLiteral("QFrame#PartTile { background-color: rgba(255, 255, 255, 8); border-radius: 12px; border: 1px solid %1; }")
                                .arg(part.value(QStringLiteral("changed")).toBool() ? QStringLiteral("rgba(242, 207, 134, 115)") : QStringLiteral("transparent")));
        auto* layout = new QHBoxLayout(tile);
        layout->setContentsMargins(12, 10, 12, 10);
        auto* texts = new QVBoxLayout();
        texts->setSpacing(2);
        texts->addWidget(Hearth::MakeLabel(part.value(QStringLiteral("name")).toString(), QStringLiteral("body"), tile));
        texts->addWidget(Hearth::MakeLabel(part.value(QStringLiteral("about")).toString(), QStringLiteral("small"), tile));
        layout->addLayout(texts, 1);
        const QString name = part.value(QStringLiteral("name")).toString();
        layout->addWidget(addSwitch(tile, part.value(QStringLiteral("on")).toBool(), part.value(QStringLiteral("locked")).toBool(), [this, name](bool on)
        {
            m_agent->Post(QStringLiteral("set_project_part"), QJsonObject{ { QStringLiteral("name"), name }, { QStringLiteral("on"), on } },
                          [this](const QJsonObject& result, bool)
            {
                if (!result.value(QStringLiteral("ok")).toBool())
                {
                    QMessageBox::warning(this, tr("Engine parts"), result.value(QStringLiteral("error")).toString());
                }
                LoadParts();
            });
        }));
        m_partsGrid->addWidget(tile, i / 3, i % 3);
    }
    for (const QJsonValue value : answer.value(QStringLiteral("all")).toArray())
    {
        const QJsonObject gem = value.toObject();
        auto* row = new QFrame(m_allBody);
        row->setObjectName(QStringLiteral("GemRow"));
        row->setStyleSheet(QStringLiteral("QFrame#GemRow { border-radius: 8px; border: 1px solid %1; } QFrame#GemRow:hover { background-color: rgba(255, 255, 255, 8); }")
                               .arg(gem.value(QStringLiteral("changed")).toBool() ? QStringLiteral("rgba(242, 207, 134, 115)") : QStringLiteral("transparent")));
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(8, 5, 8, 5);
        layout->addWidget(Hearth::MakeLabel(gem.value(QStringLiteral("gem")).toString(), QStringLiteral("mono"), row));
        QLabel* about = Hearth::MakeLabel(gem.value(QStringLiteral("about")).toString(), QStringLiteral("small"), row);
        about->setMaximumWidth(520);
        layout->addWidget(about, 1);
        const QString gemName = gem.value(QStringLiteral("gem")).toString();
        layout->addWidget(addSwitch(row, gem.value(QStringLiteral("on")).toBool(), false, [this, gemName](bool on)
        {
            m_agent->Post(QStringLiteral("set_project_part"), QJsonObject{ { QStringLiteral("gem"), gemName }, { QStringLiteral("on"), on } },
                          [this](const QJsonObject& result, bool)
            {
                if (!result.value(QStringLiteral("ok")).toBool())
                {
                    QMessageBox::warning(this, tr("Engine parts"), result.value(QStringLiteral("error")).toString());
                }
                LoadParts();
            });
        }));
        m_allLayout->addWidget(row);
    }
    m_allLayout->addStretch(1);
}

void HearthProjectPane::Rebuild(bool commit)
{
    QScopedPointer<QSettings> settings(Hearth::NewSettings());
    const bool cloud = settings->value(QStringLiteral("laptop/cloudEngineBuilds"), true).toBool();
    m_rebuild->setEnabled(false);
    if (!cloud)
    {
        // On the laptop, memory-limited, with the editor closed and reopened by the agent.
        if (QMessageBox::question(this, tr("Rebuild here"), tr("The editor closes while the game's code builds, then opens again. Rebuild now?")) != QMessageBox::Yes)
        {
            m_rebuild->setEnabled(true);
            return;
        }
        m_agent->Post(QStringLiteral("build"), QJsonObject{ { QStringLiteral("reconfigure"), true } }, [this](const QJsonObject&, bool) { m_rebuild->setEnabled(true); }, 3600000);
        return;
    }
    m_agent->Post(QStringLiteral("cloud_build"), QJsonObject{ { QStringLiteral("commit"), commit } }, [this](const QJsonObject& answer, bool reached)
    {
        if (answer.value(QStringLiteral("needs_commit")).toBool())
        {
            QStringList files;
            for (const QJsonValue f : answer.value(QStringLiteral("files")).toArray())
            {
                files << f.toString().trimmed();
            }
            const auto choice = QMessageBox::question(this, tr("Rebuild on GitHub"),
                                                      tr("GitHub builds what's pushed. Commit and push these to %1 first?\n\n%2")
                                                          .arg(m_repo->text(), files.join(QLatin1Char('\n'))));
            if (choice == QMessageBox::Yes)
            {
                Rebuild(true);
                return;
            }
            m_rebuild->setEnabled(true);
            return;
        }
        if (!reached || !answer.value(QStringLiteral("ok")).toBool())
        {
            QMessageBox::warning(this, tr("Rebuild on GitHub"), answer.value(QStringLiteral("error")).toString());
            m_rebuild->setEnabled(true);
            return;
        }
        m_changedChip->setText(tr("Starting on GitHub…"));
        m_changedChip->show();
        m_cloudTimer->start();
    }, 400000);
}

void HearthProjectPane::WatchCloud()
{
    m_agent->Post(QStringLiteral("cloud_status"), {}, [this](const QJsonObject& state, bool reached)
    {
        if (!reached)
        {
            return;
        }
        if (state.value(QStringLiteral("running")).toBool())
        {
            m_changedChip->setText(tr("GitHub: %1").arg(state.value(QStringLiteral("step")).toString()));
            return;
        }
        m_cloudTimer->stop();
        m_rebuild->setEnabled(true);
        const QString error = state.value(QStringLiteral("error")).toString();
        if (!error.isEmpty())
        {
            m_changedChip->setText(tr("GitHub build failed"));
            QMessageBox::warning(this, tr("Rebuild on GitHub"), error);
        }
        else
        {
            QMessageBox::information(this, tr("Rebuild on GitHub"), tr("The new build is home. Restart the editor to use it."));
        }
        LoadParts();
    });
}
