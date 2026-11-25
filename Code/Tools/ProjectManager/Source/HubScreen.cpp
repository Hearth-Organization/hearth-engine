/*
 * Hearth Engine Hub. Built on O3DE (Apache-2.0 OR MIT). See HubScreen.h.
 */

#include <HubScreen.h>

#include <PythonBindingsInterface.h>
#include <ScreenDefs.h>

#include <AzQtComponents/Components/HearthWidgets.h>

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QCursor>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace O3DE::ProjectManager
{
    namespace Hearth = AzQtComponents::Hearth;

    namespace
    {
        //! NoclipProtocol -> Noclip Protocol
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

        //! "just now", "5 min ago", "3 h ago", "tonight" style times.
        QString Ago(double seconds)
        {
            if (seconds <= 0)
            {
                return {};
            }
            const QDateTime when = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(seconds));
            const qint64 diff = when.secsTo(QDateTime::currentDateTime());
            if (diff < 90)
            {
                return QObject::tr("just now");
            }
            if (diff < 3600)
            {
                return QObject::tr("%1 min ago").arg(diff / 60);
            }
            if (when.date() == QDate::currentDate())
            {
                return when.time().hour() >= 18 || when.time().hour() < 5 ? QObject::tr("tonight") : QObject::tr("today");
            }
            if (diff < 86400 * 2)
            {
                return QObject::tr("yesterday");
            }
            return QObject::tr("%1 days ago").arg(diff / 86400);
        }

        QString ChipStyle(const QString& tone)
        {
            QString colors = QStringLiteral("background-color: rgba(0, 0, 0, 120); color: #ECE6DE;");
            if (tone == QLatin1String("good"))
            {
                colors = QStringLiteral("background-color: rgba(20, 30, 18, 200); color: #A6DD97;");
            }
            else if (tone == QLatin1String("warn"))
            {
                colors = QStringLiteral("background-color: rgba(40, 32, 10, 200); color: #F0D17A;");
            }
            else if (tone == QLatin1String("bad"))
            {
                colors = QStringLiteral("background-color: rgba(48, 16, 12, 210); color: #FF9F8F;");
            }
            return colors + QStringLiteral(" border-radius: 10px; padding: 3px 10px; font-size: 11px; border: none;");
        }

        QFrame* StatCard(QWidget* parent, const QString& key, QLabel** value, QLabel** note)
        {
            QVBoxLayout* layout = nullptr;
            QFrame* card = Hearth::MakeCard(parent, &layout);
            layout->setSpacing(2);
            layout->addWidget(Hearth::MakeLabel(key, QStringLiteral("dim"), card));
            *value = Hearth::MakeLabel(QStringLiteral("—"), QStringLiteral("title"), card);
            (*value)->setStyleSheet(QStringLiteral("background: transparent; border: none; color: #ECE6DE; font-size: 20px; font-weight: 600;"));
            layout->addWidget(*value);
            *note = Hearth::MakeLabel(QString(), QStringLiteral("small"), card);
            layout->addWidget(*note);
            return card;
        }
    } // namespace

    HubScreen::HubScreen(QWidget* parent)
        : ScreenWidget(parent)
    {
        setObjectName(QStringLiteral("HearthHub"));
        setStyleSheet(QStringLiteral("QFrame#HearthHub { background-color: #0D0C0B; }"));
        m_network = new QNetworkAccessManager(this);
        BuildUi();

        m_refreshTimer = new QTimer(this);
        m_refreshTimer->setInterval(3000);
        connect(m_refreshTimer, &QTimer::timeout, this, &HubScreen::Refresh);
        m_refreshTimer->start();

        QTimer::singleShot(0, this, &HubScreen::Refresh);
    }

    ProjectManagerScreen HubScreen::GetScreenEnum()
    {
        return ProjectManagerScreen::Hub;
    }

    void HubScreen::NotifyCurrentScreen()
    {
        Refresh();
    }

    // ---------------------------------------------------------------- building the screen

    void HubScreen::BuildUi()
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(10, 10, 10, 10);
        root->setSpacing(10);

        QHBoxLayout* header = Hearth::MakeHeader(this, tr("Hearth Engine"), QString());
        // MakeHeader's subtitle label is replaced by ours so it can update.
        m_engineLine = Hearth::MakeLabel(tr("%1 · built on O3DE (Apache 2.0)").arg(Hearth::Version()), QStringLiteral("small"), this);
        if (auto* titles = qobject_cast<QVBoxLayout*>(header->itemAt(1)->layout()))
        {
            titles->addWidget(m_engineLine);
        }
        QPushButton* openProject = Hearth::MakeButton(tr("Open project"), false, this);
        QPushButton* newProject = Hearth::MakeButton(tr("New project"), true, this);
        connect(openProject, &QPushButton::clicked, this, &HubScreen::HandleOpenProject);
        connect(newProject, &QPushButton::clicked, this, &HubScreen::HandleNewProject);
        header->addWidget(openProject);
        header->addWidget(newProject);
        root->addLayout(header);

        auto* body = new QHBoxLayout();
        body->setSpacing(10);

        auto* left = new QVBoxLayout();
        left->setSpacing(10);
        left->addWidget(BuildProjectCard());
        left->addWidget(BuildStats());
        auto* lower = new QHBoxLayout();
        lower->setSpacing(10);
        lower->addWidget(BuildCrashCard(), 1, Qt::AlignTop);
        lower->addWidget(BuildToastColumn(), 0, Qt::AlignTop);
        left->addLayout(lower);
        left->addStretch(1);
        body->addLayout(left, 1);

        body->addWidget(BuildParts(), 0, Qt::AlignTop);
        root->addLayout(body, 1);

        m_toast = new QLabel(this);
        m_toast->setWordWrap(true);
        m_toast->setStyleSheet(QStringLiteral(
            "background-color: #1D1A17; color: #ECE6DE; border: 1px solid rgba(255, 255, 255, 26); border-radius: 12px;"
            " padding: 10px 16px; font-size: 13px;"));
        m_toast->hide();
        root->addWidget(m_toast, 0, Qt::AlignHCenter);
        m_toastTimer = new QTimer(this);
        m_toastTimer->setSingleShot(true);
        connect(m_toastTimer, &QTimer::timeout, m_toast, &QWidget::hide);
    }

    QWidget* HubScreen::BuildProjectCard()
    {
        QVBoxLayout* layout = nullptr;
        QFrame* card = Hearth::MakeCard(this, &layout);
        layout->setContentsMargins(10, 10, 10, 10);

        // Cover: the newest screenshot of the game (from the agent), with the build chip and the project name on it.
        m_cover = new QLabel(card);
        m_cover->setObjectName(QStringLiteral("HubCover"));
        m_cover->setMinimumHeight(230);
        m_cover->setStyleSheet(QStringLiteral("QLabel#HubCover { background-color: #100F0E; border-radius: 12px; }"));
        auto* coverLayout = new QVBoxLayout(m_cover);
        coverLayout->setContentsMargins(16, 16, 16, 16);
        coverLayout->setSpacing(6);
        coverLayout->addStretch(1);
        m_buildChip = new QLabel(tr("Checking…"), m_cover);
        m_buildChip->setStyleSheet(ChipStyle(QStringLiteral("plain")));
        coverLayout->addWidget(m_buildChip, 0, Qt::AlignLeft);
        m_projectName = new QLabel(QStringLiteral("…"), m_cover);
        m_projectName->setStyleSheet(QStringLiteral("background: transparent; color: #FFFFFF; font-size: 28px; font-weight: 600;"));
        auto* shadow = new QGraphicsDropShadowEffect(m_projectName);
        shadow->setBlurRadius(18);
        shadow->setOffset(0, 1);
        shadow->setColor(QColor(0, 0, 0, 200));
        m_projectName->setGraphicsEffect(shadow);
        coverLayout->addWidget(m_projectName);
        layout->addWidget(m_cover);

        auto* row = new QHBoxLayout();
        row->setContentsMargins(6, 4, 2, 2);
        row->setSpacing(8);
        m_projectLine = Hearth::MakeLabel(QString(), QStringLiteral("dim"), card);
        m_projectLine->setWordWrap(false);
        row->addWidget(m_projectLine, 1);
        m_buildButton = Hearth::MakeButton(tr("Build"), false, card);
        m_playButton = Hearth::MakeButton(tr("Play standalone"), false, card);
        m_editorButton = Hearth::MakeButton(tr("Open editor"), true, card);
        connect(m_buildButton, &QPushButton::clicked, this, &HubScreen::HandleBuild);
        connect(m_playButton, &QPushButton::clicked, this, &HubScreen::HandlePlay);
        connect(m_editorButton, &QPushButton::clicked, this, [this]() { HandleEditor(); });
        row->addWidget(m_buildButton);
        row->addWidget(m_playButton);
        row->addWidget(m_editorButton);
        layout->addLayout(row);
        return card;
    }

    QWidget* HubScreen::BuildStats()
    {
        auto* holder = new QWidget(this);
        auto* row = new QHBoxLayout(holder);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(10);
        QLabel* partsNote = nullptr;
        row->addWidget(StatCard(holder, tr("Game memory"), &m_gameMemory, &m_gameMemoryNote));
        row->addWidget(StatCard(holder, tr("Editor memory"), &m_editorMemory, &m_editorMemoryNote));
        row->addWidget(StatCard(holder, tr("Parts switched on"), &m_partsCount, &partsNote));
        m_gameMemoryNote->setText(tr("standalone launcher"));
        partsNote->setText(tr("others ready to turn on"));
        return holder;
    }

    QWidget* HubScreen::BuildParts()
    {
        QVBoxLayout* layout = nullptr;
        QFrame* card = Hearth::MakeCard(this, &layout);
        card->setFixedWidth(380);
        auto* top = new QHBoxLayout();
        QLabel* title = Hearth::MakeLabel(tr("Engine parts"), QStringLiteral("title"), card);
        title->setStyleSheet(QStringLiteral("background: transparent; border: none; color: #ECE6DE; font-size: 15px; font-weight: 600;"));
        top->addWidget(title);
        top->addStretch(1);
        top->addWidget(Hearth::MakeLabel(tr("changes need a rebuild"), QStringLiteral("small"), card));
        layout->addLayout(top);
        m_partsLayout = new QVBoxLayout();
        m_partsLayout->setSpacing(6);
        layout->addLayout(m_partsLayout);
        m_rebuildButton = Hearth::MakeButton(tr("Rebuild with these parts"), true, card);
        m_rebuildButton->hide();
        connect(m_rebuildButton, &QPushButton::clicked, this, &HubScreen::HandleBuild);
        layout->addWidget(m_rebuildButton);
        return card;
    }

    QWidget* HubScreen::BuildCrashCard()
    {
        QVBoxLayout* layout = nullptr;
        m_crashCard = Hearth::MakeCard(this, &layout);
        layout->setContentsMargins(16, 16, 16, 16);

        auto* top = new QHBoxLayout();
        top->setSpacing(10);
        auto* icon = new QLabel(QStringLiteral("!"), m_crashCard);
        icon->setAlignment(Qt::AlignCenter);
        icon->setFixedSize(28, 28);
        icon->setStyleSheet(QStringLiteral(
            "background-color: rgba(232, 80, 59, 41); color: #FF9F8F; border: 1px solid rgba(255, 159, 143, 120);"
            " border-radius: 14px; font-weight: 700;"));
        top->addWidget(icon);
        auto* titles = new QVBoxLayout();
        titles->setSpacing(0);
        m_crashTitle = Hearth::MakeLabel(tr("The editor closed unexpectedly"), QStringLiteral("title"), m_crashCard);
        m_crashTitle->setStyleSheet(QStringLiteral("background: transparent; border: none; color: #ECE6DE; font-size: 15px; font-weight: 600;"));
        titles->addWidget(m_crashTitle);
        m_crashWhen = Hearth::MakeLabel(QString(), QStringLiteral("small"), m_crashCard);
        titles->addWidget(m_crashWhen);
        top->addLayout(titles, 1);
        layout->addLayout(top);

        m_crashSafe = new QLabel(m_crashCard);
        m_crashSafe->setWordWrap(true);
        m_crashSafe->setStyleSheet(QStringLiteral(
            "background-color: rgba(127, 207, 106, 26); color: #CFEBC6; border: 1px solid rgba(127, 207, 106, 50);"
            " border-radius: 10px; padding: 10px 12px; font-size: 12px;"));
        layout->addWidget(m_crashSafe);

        m_crashSummary = Hearth::MakeLabel(QString(), QStringLiteral("body"), m_crashCard);
        layout->addWidget(m_crashSummary);

        m_crashLogToggle = new QPushButton(tr("▸  What the log says"), m_crashCard);
        m_crashLogToggle->setCursor(Qt::PointingHandCursor);
        m_crashLogToggle->setStyleSheet(QStringLiteral(
            "QPushButton { text-align: left; background-color: rgba(255, 255, 255, 8); color: #ECE6DE; border: none;"
            " border-radius: 8px; padding: 8px 10px; font-size: 12px; }"));
        layout->addWidget(m_crashLogToggle);
        m_crashLog = new QLabel(m_crashCard);
        m_crashLog->setWordWrap(true);
        m_crashLog->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_crashLog->setStyleSheet(QStringLiteral(
            "background-color: #100F0E; color: #ECE6DE; border-radius: 8px; padding: 10px 12px;"
            " font-family: \"Geist Mono\"; font-size: 11px;"));
        m_crashLog->hide();
        layout->addWidget(m_crashLog);
        connect(m_crashLogToggle, &QPushButton::clicked, this, [this]()
        {
            const bool show = !m_crashLog->isVisible();
            m_crashLog->setVisible(show);
            m_crashLogToggle->setText(show ? tr("▾  What the log says") : tr("▸  What the log says"));
        });

        auto* buttons = new QHBoxLayout();
        buttons->addStretch(1);
        QPushButton* reopen = Hearth::MakeButton(tr("Just reopen"), false, m_crashCard);
        QPushButton* claude = Hearth::MakeButton(tr("Let Claude fix it and reopen"), true, m_crashCard);
        connect(reopen, &QPushButton::clicked, this, [this]()
        {
            Call(QStringLiteral("dismiss_crash"), {}, [](const QJsonObject&) {});
            m_crashCard->hide();
            HandleEditor();
        });
        connect(claude, &QPushButton::clicked, this, [this]()
        {
            AskClaude(tr("The Hearth Engine editor closed unexpectedly"),
                      m_crashWhen->text() + QStringLiteral("\n\n") + m_crashLog->text());
            Call(QStringLiteral("dismiss_crash"), {}, [](const QJsonObject&) {});
            m_crashCard->hide();
        });
        buttons->addWidget(reopen);
        buttons->addWidget(claude);
        layout->addLayout(buttons);

        m_crashCard->hide();
        return m_crashCard;
    }

    QWidget* HubScreen::BuildToastColumn()
    {
        auto* column = new QWidget(this);
        column->setFixedWidth(300);
        m_noticeLayout = new QVBoxLayout(column);
        m_noticeLayout->setContentsMargins(0, 0, 0, 0);
        m_noticeLayout->setSpacing(10);
        return column;
    }

    // ---------------------------------------------------------------- the agent

    bool HubScreen::ReadAgentFile()
    {
        const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        QFile file(QDir(config).filePath(QStringLiteral("hearth-engine/agent.json")));
        if (!file.open(QIODevice::ReadOnly))
        {
            return false;
        }
        const QJsonObject agent = QJsonDocument::fromJson(file.readAll()).object();
        m_token = agent.value(QStringLiteral("token")).toString();
        m_port = agent.value(QStringLiteral("port")).toInt(47830);
        m_agentProject = agent.value(QStringLiteral("project")).toString();
        return !m_token.isEmpty();
    }

    QString HubScreen::DefaultProjectPath() const
    {
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        const QString remembered = settings->value(QStringLiteral("hub/project")).toString();
        if (!remembered.isEmpty() && QFile::exists(QDir(remembered).filePath(QStringLiteral("project.json"))))
        {
            return remembered;
        }
        if (auto* python = PythonBindingsInterface::Get())
        {
            auto projects = python->GetProjects();
            if (projects.IsSuccess() && !projects.GetValue().isEmpty())
            {
                return projects.GetValue().first().m_path;
            }
        }
        return {};
    }

    void HubScreen::StartAgent(const QString& projectPath)
    {
        const QString agent = QDir(Hearth::EngineRoot()).filePath(QStringLiteral("scripts/hearth/agent/hearth_agent.py"));
        if (!QFile::exists(agent))
        {
            Toast(tr("The Hearth Engine agent isn't installed (%1).").arg(agent), 8000);
            return;
        }
        const bool started = QProcess::startDetached(QStringLiteral("python3"), { agent, QStringLiteral("--project"), projectPath });
        if (!started)
        {
            Toast(tr("Couldn't start the Hearth Engine agent (python3 missing?)."), 8000);
        }
    }

    void HubScreen::EnsureAgent()
    {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (now - m_agentStartTried < 20)
        {
            return; // give the last start time to come up
        }
        m_agentStartTried = now;
        const QString project = DefaultProjectPath();
        if (project.isEmpty())
        {
            m_buildChip->setText(tr("No project yet · press New project"));
            m_buildChip->setStyleSheet(ChipStyle(QStringLiteral("plain")));
            return;
        }
        StartAgent(project);
    }

    void HubScreen::Call(const QString& op, const QJsonObject& args, Callback done, int timeoutMs)
    {
        if (m_token.isEmpty() && !ReadAgentFile())
        {
            done(QJsonObject{ { QStringLiteral("ok"), false }, { QStringLiteral("error"), tr("the agent isn't running") },
                              { QStringLiteral("unreachable"), true } });
            return;
        }
        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/api/%2").arg(m_port).arg(op)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("X-Hearth-Token", m_token.toUtf8());
        QNetworkReply* reply = m_network->post(request, QJsonDocument(args).toJson(QJsonDocument::Compact));
        QPointer<QNetworkReply> guard(reply);
        QTimer::singleShot(timeoutMs, reply, [guard]()
        {
            if (guard && guard->isRunning())
            {
                guard->abort();
            }
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, done]()
        {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError && status == 0)
            {
                m_token.clear(); // re-read agent.json next time (the agent may have restarted with a new token)
                done(QJsonObject{ { QStringLiteral("ok"), false }, { QStringLiteral("error"), reply->errorString() },
                                  { QStringLiteral("unreachable"), true } });
                return;
            }
            if (status == 403)
            {
                m_token.clear();
            }
            const QJsonObject answer = QJsonDocument::fromJson(reply->readAll()).object();
            done(answer);
        });
    }

    // ---------------------------------------------------------------- updating

    void HubScreen::Refresh()
    {
        if (!isVisible() && !m_info.isEmpty())
        {
            return;
        }
        Call(QStringLiteral("hub_info"), {}, [this](const QJsonObject& info)
        {
            if (info.value(QStringLiteral("unreachable")).toBool() || !info.contains(QStringLiteral("project")))
            {
                m_agentReachable = false;
                m_buildChip->setText(tr("Starting the Hearth Engine agent…"));
                m_buildChip->setStyleSheet(ChipStyle(QStringLiteral("warn")));
                EnsureAgent();
                return;
            }
            const bool firstTime = m_info.isEmpty();
            m_agentReachable = true;
            m_info = info;
            Render();
            if (firstTime)
            {
                LoadCover();
            }
        });
        CheckLastExit();
        CheckClaudeResults();
    }

    void HubScreen::Render()
    {
        const QJsonObject project = m_info.value(QStringLiteral("project")).toObject();
        const QJsonObject engine = m_info.value(QStringLiteral("engine")).toObject();
        const QJsonObject running = m_info.value(QStringLiteral("running")).toObject();
        const QJsonObject build = m_info.value(QStringLiteral("build")).toObject();
        const QJsonObject last = build.value(QStringLiteral("last")).toObject();
        const QJsonObject memory = m_info.value(QStringLiteral("memory")).toObject();

        const QString o3de = engine.value(QStringLiteral("o3de_version")).toString();
        m_engineLine->setText(o3de.isEmpty() ? tr("%1 · built on O3DE (Apache 2.0)").arg(Hearth::Version())
                                             : tr("%1 · built on O3DE %2 (Apache 2.0)").arg(Hearth::Version(), o3de));

        const QString name = Pretty(project.value(QStringLiteral("name")).toString());
        m_projectName->setText(name);
        window()->setWindowTitle(tr("Hearth Engine · %1").arg(name));
        QStringList line;
        const QString level = project.value(QStringLiteral("level")).toString();
        if (!level.isEmpty())
        {
            line << level;
        }
        const QString opened = Ago(project.value(QStringLiteral("last_opened")).toDouble());
        if (!opened.isEmpty())
        {
            line << tr("last opened %1").arg(opened);
        }
        m_projectLine->setText(line.join(QStringLiteral(" · ")));

        // Build chip
        const bool building = build.value(QStringLiteral("running")).toBool() || m_busyBuild;
        if (building)
        {
            m_buildChip->setText(tr("Building…"));
            m_buildChip->setStyleSheet(ChipStyle(QStringLiteral("warn")));
        }
        else if (m_info.value(QStringLiteral("pending_rebuild")).toBool())
        {
            m_buildChip->setText(tr("Rebuild needed · parts changed"));
            m_buildChip->setStyleSheet(ChipStyle(QStringLiteral("warn")));
        }
        else if (!last.isEmpty() && last.value(QStringLiteral("ok")).toBool())
        {
            const QString steps = last.value(QStringLiteral("steps")).toString();
            m_buildChip->setText(steps.isEmpty() ? tr("Build OK") : tr("Build OK · %1 steps").arg(steps));
            m_buildChip->setStyleSheet(ChipStyle(QStringLiteral("good")));
        }
        else if (!last.isEmpty())
        {
            m_buildChip->setText(tr("Build failed · see build log"));
            m_buildChip->setStyleSheet(ChipStyle(QStringLiteral("bad")));
        }
        else
        {
            m_buildChip->setText(tr("Not built from the Hub yet"));
            m_buildChip->setStyleSheet(ChipStyle(QStringLiteral("plain")));
        }

        m_buildButton->setEnabled(!building);
        m_buildButton->setText(building ? tr("Building…") : tr("Build"));
        const bool gameRunning = running.value(QStringLiteral("game")).toBool();
        m_playButton->setText(gameRunning ? tr("Stop game") : tr("Play standalone"));
        m_playButton->setEnabled(!m_busyPlay);
        const bool editorRunning = running.value(QStringLiteral("editor")).toBool();
        m_editorButton->setText(editorRunning ? tr("Editor is open") : (m_busyEditor ? tr("Opening…") : tr("Open editor")));
        m_editorButton->setEnabled(!editorRunning && !m_busyEditor);

        // Memory cards
        auto peak = [](const QJsonValue& value) { return value.toObject().value(QStringLiteral("peak_mb")).toInt(); };
        auto peakWhen = [](const QJsonValue& value) { return value.toObject().value(QStringLiteral("when")).toDouble(); };
        const int gameNow = memory.value(QStringLiteral("game_now_mb")).toInt();
        const int gamePeak = peak(memory.value(QStringLiteral("game_peak")));
        const int game = gameNow ? gameNow : gamePeak;
        m_gameMemory->setText(game ? tr("%1 MB").arg(game) : tr("Not measured yet"));
        m_gameMemoryNote->setText(gameNow ? tr("standalone launcher, running now")
                                          : (gamePeak ? tr("standalone launcher, peak %1").arg(Ago(peakWhen(memory.value(QStringLiteral("game_peak")))))
                                                      : tr("standalone launcher")));
        const int editorNow = memory.value(QStringLiteral("editor_now_mb")).toInt();
        const int editorPeak = peak(memory.value(QStringLiteral("editor_peak")));
        const int editor = editorNow ? editorNow : editorPeak;
        m_editorMemory->setText(editor ? tr("≈ %1 MB").arg(editor) : tr("Not measured yet"));
        m_editorMemoryNote->setText(editorNow ? tr("open now")
                                              : (editorPeak ? tr("measured %1").arg(Ago(peakWhen(memory.value(QStringLiteral("editor_peak"))))) : QString()));

        m_partsCount->setText(tr("%1 of %2").arg(m_info.value(QStringLiteral("parts_on")).toInt()).arg(m_info.value(QStringLiteral("parts_total")).toInt()));
        RenderParts(m_info.value(QStringLiteral("parts")).toArray());
        m_rebuildButton->setVisible(m_info.value(QStringLiteral("pending_rebuild")).toBool() && !building);

        // Crash card
        const QJsonObject crash = m_info.value(QStringLiteral("last_crash")).toObject();
        const qint64 crashWhen = static_cast<qint64>(crash.value(QStringLiteral("when")).toDouble());
        if (!crash.isEmpty() && !crash.contains(QStringLiteral("dismissed")) && !editorRunning &&
            QDateTime::currentSecsSinceEpoch() - crashWhen < 86400)
        {
            const QString during = crash.value(QStringLiteral("during")).toString();
            const QString time = QDateTime::fromSecsSinceEpoch(crashWhen).toString(QStringLiteral("HH:mm"));
            m_crashWhen->setText(during.isEmpty() ? time : time + QStringLiteral(" · ") + during);
            const QJsonObject backup = crash.value(QStringLiteral("backup")).toObject();
            const QString crashLevel = crash.value(QStringLiteral("level")).toString();
            if (!backup.isEmpty())
            {
                const qint64 minutes = qMax<qint64>(0, (crashWhen - static_cast<qint64>(backup.value(QStringLiteral("when")).toDouble())) / 60);
                m_crashSafe->setText(tr("Your work is safe: %1 was backed up %2 before. The backup is in %3.")
                                         .arg(crashLevel.isEmpty() ? tr("the level") : crashLevel,
                                              minutes < 1 ? tr("moments") : tr("%n minute(s)", nullptr, static_cast<int>(minutes)),
                                              QDir::toNativeSeparators(backup.value(QStringLiteral("path")).toString())));
                m_crashSafe->show();
            }
            else
            {
                m_crashSafe->setText(tr("Your last save is safe. Turn on Auto Backup in Settings so unsaved work is kept too."));
                m_crashSafe->show();
            }
            QStringList lines;
            for (const QJsonValue value : crash.value(QStringLiteral("lines")).toArray())
            {
                lines << value.toString();
            }
            m_crashSummary->setText(tr("Claude can read the crash log, fix what caused it and reopen the editor for you."));
            m_crashLog->setText(lines.join(QLatin1Char('\n')));
            m_crashLogPath = crash.value(QStringLiteral("log")).toString();
            m_crashCard->show();
            if (crashWhen != m_lastCrashShown)
            {
                m_lastCrashShown = crashWhen;
            }
        }
        else
        {
            m_crashCard->hide();
        }

        // Failed build notice (once per build)
        if (!last.isEmpty() && !last.value(QStringLiteral("ok")).toBool())
        {
            QScopedPointer<QSettings> settings(Hearth::NewSettings());
            const double finished = last.value(QStringLiteral("finished")).toDouble();
            if (settings->value(QStringLiteral("hub/buildFailureShown")).toDouble() != finished)
            {
                settings->setValue(QStringLiteral("hub/buildFailureShown"), finished);
                const QJsonArray errors = last.value(QStringLiteral("errors")).toArray();
                const QString first = errors.isEmpty() ? tr("See the build log for details.") : errors.first().toString().trimmed();
                const QString logPath = last.value(QStringLiteral("log")).toString();
                QStringList all;
                for (const QJsonValue error : errors)
                {
                    all << error.toString();
                }
                AddNotice(QStringLiteral("bad"), tr("Build failed"), first, tr("Let Claude fix it"),
                          [this, all]() { AskClaude(tr("The Hearth Engine build failed"), all.join(QLatin1Char('\n'))); },
                          tr("Open build log"),
                          [logPath]() { QDesktopServices::openUrl(QUrl::fromLocalFile(logPath)); });
            }
        }

        // Memory getting low
        const int freeMb = m_info.value(QStringLiteral("free_mb")).toInt();
        if (freeMb > 0 && freeMb < 1200 && !m_lowMemoryShown)
        {
            m_lowMemoryShown = true;
            AddNotice(QStringLiteral("warn"), tr("Memory is getting low"),
                      tr("%1 GB left. Close something, or turn on Pause asset processing while a game runs in Settings.")
                          .arg(QString::number(freeMb / 1024.0, 'f', 1)),
                      QString(), {});
        }
        else if (freeMb >= 1800)
        {
            m_lowMemoryShown = false;
        }
    }

    void HubScreen::RenderParts(const QJsonArray& parts)
    {
        // Rebuild the rows (10 small rows; cheap).
        while (QLayoutItem* item = m_partsLayout->takeAt(0))
        {
            if (QWidget* widget = item->widget())
            {
                widget->deleteLater();
            }
            delete item;
        }
        for (const QJsonValue value : parts)
        {
            const QJsonObject part = value.toObject();
            const QString name = part.value(QStringLiteral("name")).toString();
            const bool on = part.value(QStringLiteral("on")).toBool();
            const bool locked = part.value(QStringLiteral("locked")).toBool();
            QString about = part.value(QStringLiteral("about")).toString();
            const QString note = part.value(QStringLiteral("note")).toString();
            if (!on && !note.isEmpty())
            {
                about += QStringLiteral(" · ") + note;
            }
            if (locked)
            {
                about += tr(" · always on");
            }

            auto* row = new QFrame();
            row->setObjectName(QStringLiteral("HubPart"));
            row->setStyleSheet(QStringLiteral("QFrame#HubPart { background-color: rgba(255, 255, 255, 8); border-radius: 10px; }"));
            auto* layout = new QHBoxLayout(row);
            layout->setContentsMargins(10, 8, 10, 8);
            auto* texts = new QVBoxLayout();
            texts->setSpacing(1);
            texts->addWidget(Hearth::MakeLabel(name, QStringLiteral("body"), row));
            texts->addWidget(Hearth::MakeLabel(about, QStringLiteral("small"), row));
            layout->addLayout(texts, 1);
            auto* toggle = new Hearth::Toggle(row);
            toggle->setChecked(on);
            toggle->setEnabled(!locked);
            toggle->setToolTip(locked ? tr("%1 is always on: the project needs it.").arg(name) : QString());
            connect(toggle, &QAbstractButton::clicked, this, [this, name](bool checked) { HandleSetPart(name, checked); });
            layout->addWidget(toggle, 0, Qt::AlignVCenter);
            m_partsLayout->addWidget(row);
        }
    }

    void HubScreen::LoadCover()
    {
        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/cover").arg(m_port)));
        QNetworkReply* reply = m_network->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply]()
        {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError)
            {
                return; // no screenshots yet: keep the dark cover
            }
            const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
            QDir().mkpath(cache);
            const QString path = QDir(cache).filePath(QStringLiteral("hearth-hub-cover.png"));
            QFile file(path);
            if (file.open(QIODevice::WriteOnly))
            {
                file.write(reply->readAll());
                file.close();
                m_cover->setStyleSheet(QStringLiteral(
                    "QLabel#HubCover { border-radius: 12px; border-image: url(%1) 0 0 0 0 stretch stretch; }")
                                           .arg(QDir::fromNativeSeparators(path)));
            }
        });
    }

    void HubScreen::CheckLastExit()
    {
        // Tools started from the hearth-engine command record how they ended (see scripts/hearth/make-deb.sh).
        for (const QString& tool : { QStringLiteral("Editor"), QStringLiteral("AssetProcessor") })
        {
            QFile file(QDir(Hearth::LogsFolder()).filePath(QStringLiteral("last-exit-") + tool));
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            {
                continue;
            }
            QHash<QString, QString> values;
            for (const QByteArray& line : file.readAll().split('\n'))
            {
                const int eq = line.indexOf('=');
                if (eq > 0)
                {
                    values.insert(QString::fromUtf8(line.left(eq)), QString::fromUtf8(line.mid(eq + 1)).trimmed());
                }
            }
            const int code = values.value(QStringLiteral("code")).toInt();
            const qint64 when = values.value(QStringLiteral("time")).toLongLong();
            QScopedPointer<QSettings> settings(Hearth::NewSettings());
            const QString key = QStringLiteral("hub/exitShown-") + tool;
            if (code == 0 || code == 130 || code == 143 || settings->value(key).toLongLong() == when)
            {
                continue; // fine, stopped on purpose, or already shown
            }
            settings->setValue(key, when);
            const QString log = values.value(QStringLiteral("log"));
            AddNotice(QStringLiteral("bad"), tool == QLatin1String("Editor") ? tr("The editor stopped") : tr("Asset processing stopped"),
                      tr("It ended with code %1. Its log is saved.").arg(code), tr("Let Claude look"),
                      [this, tool, log]() { AskClaude(tr("Hearth Engine %1 stopped").arg(tool), tr("Log file: %1").arg(log)); },
                      tr("Open log"), [log]() { QDesktopServices::openUrl(QUrl::fromLocalFile(log)); });
        }
    }

    void HubScreen::CheckMemory()
    {
        // Covered by hub_info's free_mb in Render().
    }

    void HubScreen::CheckClaudeResults()
    {
        // When Claude finishes a fix it leaves a note in ~/O3DE/HearthEngine/claude-results/*.json
        // ({"title": "...", "body": "...", "time": seconds}); the Hub shows each one once.
        QDir results(QDir(Hearth::HomeFolder()).filePath(QStringLiteral("claude-results")));
        if (!results.exists())
        {
            return;
        }
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        const qint64 seen = settings->value(QStringLiteral("hub/claudeResultsSeen")).toLongLong();
        qint64 newest = seen;
        const QFileInfoList files = results.entryInfoList({ QStringLiteral("*.json") }, QDir::Files, QDir::Time | QDir::Reversed);
        for (const QFileInfo& info : files)
        {
            const qint64 modified = info.lastModified().toSecsSinceEpoch();
            if (modified <= seen)
            {
                continue;
            }
            newest = qMax(newest, modified);
            QFile file(info.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly))
            {
                continue;
            }
            const QJsonObject note = QJsonDocument::fromJson(file.readAll()).object();
            AddNotice(QStringLiteral("good"), note.value(QStringLiteral("title")).toString(tr("Claude fixed it")),
                      note.value(QStringLiteral("body")).toString(), QString(), {});
        }
        if (newest != seen)
        {
            settings->setValue(QStringLiteral("hub/claudeResultsSeen"), newest);
        }
    }

    void HubScreen::AddNotice(const QString& kind, const QString& title, const QString& body, const QString& primaryText,
                              std::function<void()> primary, const QString& secondaryText, std::function<void()> secondary)
    {
        QVBoxLayout* layout = nullptr;
        QFrame* card = Hearth::MakeCard(nullptr, &layout);
        layout->setSpacing(6);
        auto* top = new QHBoxLayout();
        QString color = QStringLiteral("#ECE6DE");
        if (kind == QLatin1String("bad"))
        {
            color = QStringLiteral("#FF9F8F");
        }
        else if (kind == QLatin1String("warn"))
        {
            color = QStringLiteral("#F0D17A");
        }
        else if (kind == QLatin1String("good"))
        {
            color = QStringLiteral("#A6DD97");
        }
        QLabel* titleLabel = Hearth::MakeLabel(title, QStringLiteral("title"), card);
        titleLabel->setStyleSheet(QStringLiteral("background: transparent; border: none; font-weight: 600; color: %1;").arg(color));
        top->addWidget(titleLabel, 1);
        QLabel* when = Hearth::MakeLabel(tr("just now"), QStringLiteral("small"), card);
        top->addWidget(when);
        QPushButton* close = new QPushButton(QStringLiteral("×"), card);
        close->setFixedSize(20, 20);
        close->setCursor(Qt::PointingHandCursor);
        close->setStyleSheet(QStringLiteral("QPushButton { background: transparent; color: #8D857B; border: none; font-size: 14px; }"));
        top->addWidget(close);
        layout->addLayout(top);
        if (!body.isEmpty())
        {
            layout->addWidget(Hearth::MakeLabel(body, QStringLiteral("body"), card));
        }
        if (!primaryText.isEmpty() || !secondaryText.isEmpty())
        {
            auto* buttons = new QHBoxLayout();
            buttons->setSpacing(6);
            if (!primaryText.isEmpty())
            {
                QPushButton* button = Hearth::MakeButton(primaryText, true, card);
                connect(button, &QPushButton::clicked, card, [primary, card]()
                {
                    if (primary)
                    {
                        primary();
                    }
                    card->deleteLater();
                });
                buttons->addWidget(button);
            }
            if (!secondaryText.isEmpty())
            {
                QPushButton* button = Hearth::MakeButton(secondaryText, false, card);
                connect(button, &QPushButton::clicked, card, [secondary]()
                {
                    if (secondary)
                    {
                        secondary();
                    }
                });
                buttons->addWidget(button);
            }
            buttons->addStretch(1);
            layout->addLayout(buttons);
        }
        connect(close, &QPushButton::clicked, card, &QObject::deleteLater);

        // Update "just now" -> "5 min ago" while it's shown.
        const qint64 created = QDateTime::currentSecsSinceEpoch();
        auto* ticker = new QTimer(card);
        ticker->setInterval(30000);
        connect(ticker, &QTimer::timeout, when, [when, created]() { when->setText(Ago(static_cast<double>(created))); });
        ticker->start();

        m_noticeLayout->insertWidget(0, card);
        while (m_noticeLayout->count() > 3)
        {
            QLayoutItem* item = m_noticeLayout->takeAt(m_noticeLayout->count() - 1);
            if (item->widget())
            {
                item->widget()->deleteLater();
            }
            delete item;
        }
    }

    void HubScreen::Toast(const QString& text, int ms)
    {
        m_toast->setText(text);
        m_toast->show();
        m_toastTimer->start(ms);
    }

    // ---------------------------------------------------------------- buttons

    void HubScreen::HandleOpenProject()
    {
        QMenu menu(this);
        menu.setStyleSheet(QStringLiteral("QMenu { min-width: 260px; }"));
        const QString current = m_info.value(QStringLiteral("project")).toObject().value(QStringLiteral("path")).toString();
        if (auto* python = PythonBindingsInterface::Get())
        {
            auto projects = python->GetProjects();
            if (projects.IsSuccess())
            {
                for (const ProjectInfo& project : projects.GetValue())
                {
                    const QString label = Pretty(project.m_displayName.isEmpty() ? project.m_projectName : project.m_displayName);
                    QAction* action = menu.addAction(QDir::cleanPath(project.m_path) == QDir::cleanPath(current) ? label + tr("  (open now)") : label);
                    action->setData(project.m_path);
                }
            }
        }
        if (!menu.isEmpty())
        {
            menu.addSeparator();
        }
        QAction* browse = menu.addAction(tr("Find a project folder…"));
        QAction* chosen = menu.exec(QCursor::pos());
        if (!chosen)
        {
            return;
        }
        QString path = chosen->data().toString();
        if (chosen == browse)
        {
            path = QFileDialog::getExistingDirectory(this, tr("Open project"), QDir::homePath() + QStringLiteral("/O3DE/Projects"));
            if (path.isEmpty())
            {
                return;
            }
            if (!QFile::exists(QDir(path).filePath(QStringLiteral("project.json"))))
            {
                Toast(tr("That folder isn't a project (no project.json)."));
                return;
            }
        }
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        settings->setValue(QStringLiteral("hub/project"), path);
        Toast(tr("Opening %1…").arg(QFileInfo(path).fileName()));
        Call(QStringLiteral("switch_project"), QJsonObject{ { QStringLiteral("project_path"), path } }, [this, path](const QJsonObject& answer)
        {
            if (answer.value(QStringLiteral("unreachable")).toBool())
            {
                StartAgent(path);
            }
            else if (!answer.value(QStringLiteral("ok")).toBool())
            {
                Toast(tr("Couldn't switch: %1").arg(answer.value(QStringLiteral("error")).toString()), 8000);
                return;
            }
            m_info = QJsonObject();
            m_token.clear();
            QTimer::singleShot(2500, this, &HubScreen::Refresh);
        });
    }

    void HubScreen::HandleNewProject()
    {
        emit ChangeScreenRequest(ProjectManagerScreen::CreateProject);
    }

    void HubScreen::HandleBuild()
    {
        m_busyBuild = true;
        Render();
        Toast(tr("Building. The editor closes for the build and opens again after."), 8000);
        const bool reconfigure = m_info.value(QStringLiteral("pending_rebuild")).toBool();
        Call(QStringLiteral("build"),
             QJsonObject{ { QStringLiteral("reconfigure"), reconfigure }, { QStringLiteral("stop_editor"), true },
                          { QStringLiteral("reopen_editor"), true } },
             [this](const QJsonObject& answer)
             {
                 m_busyBuild = false;
                 if (answer.value(QStringLiteral("ok")).toBool())
                 {
                     Toast(tr("Build finished in %1 s.").arg(answer.value(QStringLiteral("seconds")).toInt()), 8000);
                 }
                 else if (!answer.value(QStringLiteral("unreachable")).toBool())
                 {
                     Toast(tr("Build failed. See the card on the right."), 8000);
                 }
                 Refresh();
             },
             3 * 3600 * 1000);
    }

    void HubScreen::HandlePlay()
    {
        m_busyPlay = true;
        Render();
        const bool running = m_info.value(QStringLiteral("running")).toObject().value(QStringLiteral("game")).toBool();
        Call(running ? QStringLiteral("game_stop") : QStringLiteral("game_start"),
             running ? QJsonObject() : QJsonObject{ { QStringLiteral("fullscreen"), true } },
             [this](const QJsonObject& answer)
             {
                 m_busyPlay = false;
                 if (answer.contains(QStringLiteral("ok")) && !answer.value(QStringLiteral("ok")).toBool())
                 {
                     Toast(tr("Couldn't start the game: %1").arg(answer.value(QStringLiteral("error")).toString()), 8000);
                 }
                 Refresh();
             },
             120000);
    }

    void HubScreen::HandleEditor(const QString& projectPath)
    {
        m_busyEditor = true;
        Render();
        Toast(tr("Opening the editor…"));
        QJsonObject args{ { QStringLiteral("wait_seconds"), 240 } };
        if (!projectPath.isEmpty())
        {
            args.insert(QStringLiteral("project_path"), projectPath);
        }
        Call(QStringLiteral("editor_start"), args, [this](const QJsonObject& answer)
        {
            m_busyEditor = false;
            Toast(answer.value(QStringLiteral("ok")).toBool() ? tr("Editor is open.")
                                                              : tr("Couldn't open the editor: %1").arg(answer.value(QStringLiteral("error")).toString()),
                  8000);
            Refresh();
        },
        260000);
    }

    void HubScreen::HandleSetPart(const QString& name, bool on)
    {
        Call(QStringLiteral("set_part"), QJsonObject{ { QStringLiteral("name"), name }, { QStringLiteral("on"), on } },
             [this, name, on](const QJsonObject& answer)
             {
                 if (!answer.value(QStringLiteral("ok")).toBool())
                 {
                     Toast(answer.value(QStringLiteral("error")).toString(tr("Couldn't change that part.")));
                 }
                 else
                 {
                     Toast(tr("%1 %2. Rebuild to apply.").arg(name, on ? tr("on") : tr("off")));
                 }
                 Refresh();
             });
    }

    void HubScreen::AskClaude(const QString& title, const QString& details)
    {
        // Leave the full report where Claude can read it, and open Claude with a short message about it.
        QDir requests(QDir(Hearth::HomeFolder()).filePath(QStringLiteral("claude-requests")));
        requests.mkpath(QStringLiteral("."));
        const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
        const QString path = requests.filePath(stamp + QStringLiteral(".md"));
        const QString project = m_info.value(QStringLiteral("project")).toObject().value(QStringLiteral("path")).toString();
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QString text = QStringLiteral("# %1\n\n- Time: %2\n- Hearth Engine: %3\n- Project: %4\n- Logs: %5\n\n```\n%6\n```\n")
                               .arg(title, QDateTime::currentDateTime().toString(Qt::ISODate), Hearth::Version(), project,
                                    Hearth::LogsFolder(), details);
            file.write(text.toUtf8());
        }
        const QString message = tr("%1. The report is in %2 and the logs in %3. Please read them, fix the cause, "
                                   "reopen the editor, and leave a note in %4 when it's done.")
                                    .arg(title, path, Hearth::LogsFolder(), QDir(Hearth::HomeFolder()).filePath(QStringLiteral("claude-results")));
        QApplication::clipboard()->setText(message);
        QUrl url(QStringLiteral("https://claude.ai/new"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("q"), message);
        url.setQuery(query);
        QDesktopServices::openUrl(url);
        Toast(tr("Opened Claude with the report (the message is also on your clipboard)."), 8000);
    }
} // namespace O3DE::ProjectManager
