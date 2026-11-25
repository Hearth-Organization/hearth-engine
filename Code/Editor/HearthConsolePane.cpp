/*
 * Hearth Engine "Console and logs" window. Built on O3DE (Apache-2.0 OR MIT). See HearthConsolePane.h.
 */

#include "EditorDefs.h"

#include "HearthConsolePane.h"

#include <AzCore/Console/IConsole.h>
#include <AzCore/Interface/Interface.h>
#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QLabel>
#include <QLocale>
#include <QLineEdit>
#include <QListWidget>
#include <QMutexLocker>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;
    constexpr int MaxLines = 20000;

    const char* SourceNames[] = { "Editor", "Game", "Asset processing", "Build", "Hub and setup" };

    QString ProjectFolder()
    {
        const auto path = AZ::Utils::GetProjectPath();
        return QString::fromUtf8(path.c_str());
    }

    HearthConsolePane::Level Guess(const QString& text)
    {
        const QString lower = text.toLower();
        if (lower.contains(QLatin1String("error")) || lower.contains(QLatin1String("failed")) || lower.contains(QLatin1String("fatal")) ||
            lower.contains(QLatin1String("assert")))
        {
            return HearthConsolePane::Level::Error;
        }
        if (lower.contains(QLatin1String("warn")))
        {
            return HearthConsolePane::Level::Warning;
        }
        return HearthConsolePane::Level::Info;
    }

    QString LevelName(HearthConsolePane::Level level)
    {
        switch (level)
        {
        case HearthConsolePane::Level::Error:
            return QObject::tr("Error");
        case HearthConsolePane::Level::Warning:
            return QObject::tr("Warning");
        default:
            return QObject::tr("Info");
        }
    }

    QString LevelColor(HearthConsolePane::Level level)
    {
        switch (level)
        {
        case HearthConsolePane::Level::Error:
            return QStringLiteral("#FF9F8F");
        case HearthConsolePane::Level::Warning:
            return QStringLiteral("#F0D17A");
        default:
            return QStringLiteral("#8D857B");
        }
    }

    //! A plain-words title for an error line.
    QString TitleFor(const QString& message)
    {
        if (message.contains(QLatin1String("Could not compile asset")) || message.contains(QLatin1String("failed to process")))
        {
            return QObject::tr("An asset couldn't be processed");
        }
        if (message.contains(QLatin1String("Assert"), Qt::CaseInsensitive))
        {
            return QObject::tr("The engine hit a check that failed");
        }
        if (message.contains(QLatin1String("not found"), Qt::CaseInsensitive) || message.contains(QLatin1String("missing"), Qt::CaseInsensitive))
        {
            return QObject::tr("Something the level needs is missing");
        }
        QString first = message.simplified();
        if (first.size() > 70)
        {
            first = first.left(67) + QStringLiteral("…");
        }
        return first;
    }

    QPushButton* FilterChip(const QString& text, QWidget* parent)
    {
        auto* chip = new QPushButton(text, parent);
        chip->setCheckable(true);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setFixedHeight(28);
        return chip;
    }
} // namespace

void HearthConsolePane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::BottomDockWidgetArea;
    options.showInMenu = true;
    options.isDisabledInComponentMode = false;
    options.isDisabledInImGuiMode = false;
    AzToolsFramework::RegisterViewPane<HearthConsolePane>("Console and logs", "Tools", options);
}

HearthConsolePane::HearthConsolePane(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthConsole"));
    setStyleSheet(QStringLiteral("QWidget#HearthConsole { background-color: #0D0C0B; }"));
    BuildUi();
    LoadFiles();
    Rebuild();
    AZ::Debug::TraceMessageBus::Handler::BusConnect();

    m_timer = new QTimer(this);
    m_timer->setInterval(500);
    connect(m_timer, &QTimer::timeout, this, &HearthConsolePane::FlushQueue);
    m_timer->start();
}

HearthConsolePane::~HearthConsolePane()
{
    AZ::Debug::TraceMessageBus::Handler::BusDisconnect();
}

// ---------------------------------------------------------------- building

void HearthConsolePane::BuildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Console and logs"),
                                             tr("Saved to %1").arg(QDir::toNativeSeparators(Hearth::LogsFolder()).replace(QDir::homePath(), QStringLiteral("~"))));
    header->addWidget(Hearth::MakeChip(tr("✓ Claude can read these"), QStringLiteral("good"), this));
    QPushButton* openFolder = Hearth::MakeButton(tr("Open logs folder"), false, this);
    connect(openFolder, &QPushButton::clicked, this, []() { QDesktopServices::openUrl(QUrl::fromLocalFile(Hearth::LogsFolder())); });
    header->addWidget(openFolder);
    QPushButton* ask = Hearth::MakeButton(tr("Ask Claude about this error"), true, this);
    connect(ask, &QPushButton::clicked, this, [this]()
    {
        Hearth::AskClaude(tr("A Hearth Engine error"), CurrentReport(), ProjectFolder());
    });
    header->addWidget(ask);
    root->addLayout(header);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);

    // Left: sources and earlier runs.
    QVBoxLayout* leftLayout = nullptr;
    QFrame* left = Hearth::MakeCard(this, &leftLayout);
    left->setFixedWidth(240);
    const QString listStyle = QStringLiteral(
        "QListWidget { background: transparent; border: none; outline: none; }"
        "QListWidget::item { padding: 7px 10px; border-radius: 8px; color: #ECE6DE; }"
        "QListWidget::item:selected { background-color: rgba(232, 116, 59, 41); color: #FFCFB4; }"
        "QListWidget::item:hover:!selected { background-color: rgba(255, 255, 255, 10); }");
    leftLayout->addWidget(Hearth::MakeLabel(tr("Sources"), QStringLiteral("title"), left));
    m_sources = new QListWidget(left);
    m_sources->setStyleSheet(listStyle);
    m_sources->setFixedHeight(6 * 34);
    leftLayout->addWidget(m_sources);
    connect(m_sources, &QListWidget::currentRowChanged, this, [this](int row)
    {
        m_source = row <= 0 ? QString() : m_sources->item(row)->data(Qt::UserRole).toString();
        Rebuild();
    });
    leftLayout->addWidget(Hearth::MakeLabel(tr("Earlier runs"), QStringLiteral("title"), left));
    m_runs = new QListWidget(left);
    m_runs->setStyleSheet(listStyle);
    leftLayout->addWidget(m_runs, 1);
    connect(m_runs, &QListWidget::itemClicked, this, [this](QListWidgetItem* item)
    {
        const QString path = item->data(Qt::UserRole).toString();
        const QString source = tr("Run %1").arg(QFileInfo(path).completeBaseName());
        LoadFile(path, source, 4000);
        m_source = source;
        m_sources->blockSignals(true);
        m_sources->setCurrentRow(-1);
        m_sources->blockSignals(false);
        Rebuild();
    });
    body->addWidget(left);

    // Middle: filters, the messages, the command line.
    auto* middle = new QVBoxLayout();
    middle->setSpacing(10);
    auto* filters = new QHBoxLayout();
    filters->setSpacing(6);
    m_allChip = FilterChip(tr("All"), this);
    m_errorChip = FilterChip(tr("Errors"), this);
    m_warningChip = FilterChip(tr("Warnings"), this);
    m_infoChip = FilterChip(tr("Info"), this);
    m_allChip->setChecked(true);
    const QVector<QPair<QPushButton*, int>> chips = { { m_allChip, -1 }, { m_errorChip, static_cast<int>(Level::Error) },
                                                      { m_warningChip, static_cast<int>(Level::Warning) }, { m_infoChip, static_cast<int>(Level::Info) } };
    for (const auto& chip : chips)
    {
        QPushButton* button = chip.first;
        const int level = chip.second;
        filters->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, level, chips]()
        {
            m_levelFilter = level;
            for (const auto& other : chips)
            {
                other.first->setChecked(other.second == level);
            }
            Rebuild();
        });
    }
    filters->addStretch(1);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter messages"));
    m_filter->setFixedWidth(280);
    connect(m_filter, &QLineEdit::textChanged, this, &HearthConsolePane::Rebuild);
    filters->addWidget(m_filter);
    middle->addLayout(filters);

    m_view = new QListWidget(this);
    m_view->setObjectName(QStringLiteral("HearthLog"));
    m_view->setStyleSheet(QStringLiteral(
        "QListWidget#HearthLog { background-color: #100F0E; border: 1px solid rgba(255, 255, 255, 15); border-radius: 14px;"
        " padding: 8px; font-family: \"Geist Mono\"; font-size: 12px; outline: none; }"
        "QListWidget#HearthLog::item { padding: 2px 6px; border-radius: 6px; }"
        "QListWidget#HearthLog::item:selected { background-color: rgba(232, 116, 59, 31); }"));
    m_view->setUniformItemSizes(true);
    m_view->setTextElideMode(Qt::ElideRight);
    connect(m_view, &QListWidget::currentRowChanged, this, &HearthConsolePane::ShowDetails);
    middle->addWidget(m_view, 1);

    m_command = new QLineEdit(this);
    m_command->setPlaceholderText(tr("Console command, for example r_fullscreen 1"));
    m_command->setStyleSheet(QStringLiteral("QLineEdit { font-family: \"Geist Mono\"; padding-left: 10px; }"));
    connect(m_command, &QLineEdit::returnPressed, this, &HearthConsolePane::RunCommand);
    middle->addWidget(m_command);
    body->addLayout(middle, 1);

    // Right: the selected error.
    QVBoxLayout* detailLayout = nullptr;
    QFrame* details = Hearth::MakeCard(this, &detailLayout);
    m_details = details;
    details->setFixedWidth(340);
    m_detailKind = Hearth::MakeLabel(QString(), QStringLiteral("title"), details);
    detailLayout->addWidget(m_detailKind);
    m_detailTitle = Hearth::MakeLabel(QString(), QStringLiteral("title"), details);
    m_detailTitle->setWordWrap(true);
    m_detailTitle->setStyleSheet(QStringLiteral("background: transparent; border: none; color: #ECE6DE; font-size: 15px; font-weight: 600;"));
    detailLayout->addWidget(m_detailTitle);
    m_detailText = Hearth::MakeLabel(QString(), QStringLiteral("body"), details);
    detailLayout->addWidget(m_detailText);
    m_detailRaw = new QLabel(details);
    m_detailRaw->setWordWrap(true);
    m_detailRaw->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detailRaw->setStyleSheet(QStringLiteral("background-color: #100F0E; color: #ECE6DE; border-radius: 8px; padding: 10px;"
                                              " font-family: \"Geist Mono\"; font-size: 11px;"));
    detailLayout->addWidget(m_detailRaw);
    detailLayout->addWidget(Hearth::MakeLabel(tr("What Claude found"), QStringLiteral("title"), details));
    m_detailClaude = Hearth::MakeLabel(QString(), QStringLiteral("body"), details);
    detailLayout->addWidget(m_detailClaude);
    detailLayout->addStretch(1);
    QPushButton* fix = Hearth::MakeButton(tr("Let Claude fix it"), true, details);
    connect(fix, &QPushButton::clicked, this, [this]()
    {
        Hearth::AskClaude(m_detailTitle->text(), CurrentReport(), ProjectFolder());
    });
    detailLayout->addWidget(fix);
    QPushButton* copy = Hearth::MakeButton(tr("Copy for chat"), false, details);
    connect(copy, &QPushButton::clicked, this, [this]() { QApplication::clipboard()->setText(CurrentReport()); });
    detailLayout->addWidget(copy);
    body->addWidget(details);

    root->addLayout(body, 1);
    ShowDetails(-1);
}

// ---------------------------------------------------------------- data

bool HearthConsolePane::OnPreError(const char* window, const char*, int, const char*, const char* message)
{
    Queue(Level::Error, window, message);
    return false;
}

bool HearthConsolePane::OnPreWarning(const char* window, const char*, int, const char*, const char* message)
{
    Queue(Level::Warning, window, message);
    return false;
}

bool HearthConsolePane::OnPrintf(const char* window, const char* message)
{
    Queue(Level::Info, window, message);
    return false;
}

void HearthConsolePane::Queue(Level level, const char* window, const char* message)
{
    Line line;
    line.time = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    line.level = level;
    line.source = QStringLiteral("Editor");
    line.message = QString::fromUtf8(message ? message : "").trimmed();
    if (window && *window && QString::fromUtf8(window) != QLatin1String("None"))
    {
        line.message = QStringLiteral("[%1] %2").arg(QString::fromUtf8(window), line.message);
    }
    if (line.message.isEmpty())
    {
        return;
    }
    QMutexLocker lock(&m_pendingMutex);
    if (m_pending.size() < MaxLines)
    {
        m_pending.push_back(line);
    }
}

void HearthConsolePane::FlushQueue()
{
    QVector<Line> pending;
    {
        QMutexLocker lock(&m_pendingMutex);
        pending.swap(m_pending);
    }
    if (pending.isEmpty())
    {
        return;
    }
    m_lines += pending;
    if (m_lines.size() > MaxLines)
    {
        m_lines.erase(m_lines.begin(), m_lines.begin() + (m_lines.size() - MaxLines));
    }
    const bool atBottom = m_view->verticalScrollBar()->value() >= m_view->verticalScrollBar()->maximum() - 4;
    Rebuild();
    if (atBottom)
    {
        m_view->scrollToBottom();
    }
}

void HearthConsolePane::LoadFile(const QString& path, const QString& source, int maxLines)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return;
    }
    if (file.size() > 4 * 1024 * 1024)
    {
        file.seek(file.size() - 4 * 1024 * 1024);
    }
    const QStringList all = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    static const QRegularExpression timePattern(QStringLiteral("(\\d{2}:\\d{2}:\\d{2})"));
    for (int i = qMax(0, all.size() - maxLines); i < all.size(); ++i)
    {
        Line line;
        line.source = source;
        line.message = all[i].trimmed();
        line.level = Guess(line.message);
        const QRegularExpressionMatch time = timePattern.match(line.message.left(40));
        line.time = time.hasMatch() ? time.captured(1) : QString();
        m_lines.push_back(line);
    }
}

void HearthConsolePane::LoadFiles()
{
    const QDir project(ProjectFolder());
    LoadFile(project.filePath(QStringLiteral("user/log/Game.log")), QStringLiteral("Game"));
    LoadFile(project.filePath(QStringLiteral("user/log/AP_GUI.log")), QStringLiteral("Asset processing"));
    LoadFile(project.filePath(QStringLiteral("user/HearthAgent/build.log")), QStringLiteral("Build"));

    // The Hub's and the launcher's own logs, and the list of earlier runs.
    const QDir logs(Hearth::LogsFolder());
    const QFileInfoList runs = logs.entryInfoList({ QStringLiteral("*.log") }, QDir::Files, QDir::Time);
    bool hubLoaded = false;
    int listed = 0;
    for (const QFileInfo& run : runs)
    {
        if (run.fileName() == QLatin1String("latest.log") || run.isSymLink())
        {
            continue;
        }
        const QString base = run.completeBaseName();   // 2026-11-25_03-35-10-Editor
        const QString tool = base.section(QLatin1Char('-'), -1);
        if (!hubLoaded && tool == QLatin1String("o3de"))
        {
            LoadFile(run.absoluteFilePath(), QStringLiteral("Hub and setup"));
            hubLoaded = true;
        }
        if (listed >= 8)
        {
            continue;
        }
        ++listed;
        int errors = 0;
        QFile file(run.absoluteFilePath());
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            while (!file.atEnd())
            {
                if (Guess(QString::fromUtf8(file.readLine())) == Level::Error)
                {
                    ++errors;
                }
            }
        }
        const QDateTime when = run.lastModified();
        const QString day = when.date() == QDate::currentDate() ? tr("Today") : when.toString(QStringLiteral("MMM d"));
        const QString toolName = tool == QLatin1String("o3de") ? tr("Hub") : tool;
        auto* item = new QListWidgetItem(QStringLiteral("%1 %2 · %3\n%4")
                                             .arg(day, when.toString(QStringLiteral("HH:mm")), toolName,
                                                  errors == 0 ? tr("No errors") : tr("%n error(s)", nullptr, errors)),
                                         m_runs);
        item->setData(Qt::UserRole, run.absoluteFilePath());
        item->setForeground(errors ? Hearth::Ember() : Hearth::Text());
    }
}

void HearthConsolePane::Rebuild()
{
    // Counts for the sources and the chips.
    QHash<QString, int> perSource;
    int errors = 0;
    int warnings = 0;
    for (const Line& line : m_lines)
    {
        perSource[line.source]++;
        if (m_source.isEmpty() || line.source == m_source)
        {
            errors += line.level == Level::Error ? 1 : 0;
            warnings += line.level == Level::Warning ? 1 : 0;
        }
    }
    {
        const QSignalBlocker blocker(m_sources);
        const int current = m_sources->currentRow();
        m_sources->clear();
        auto add = [this](const QString& label, const QString& key, int count)
        {
            auto* item = new QListWidgetItem(QStringLiteral("%1   %2").arg(label, QLocale().toString(count)), m_sources);
            item->setData(Qt::UserRole, key);
        };
        add(tr("Everything"), QString(), m_lines.size());
        for (const char* name : SourceNames)
        {
            add(tr(name), QString::fromLatin1(name), perSource.value(QString::fromLatin1(name)));
        }
        m_sources->setCurrentRow(m_source.isEmpty() ? 0 : current);
    }
    m_errorChip->setText(tr("Errors %1").arg(errors));
    m_warningChip->setText(tr("Warnings %1").arg(warnings));
    m_errorChip->setStyleSheet(QStringLiteral("QPushButton { border: 1px solid rgba(255, 159, 143, 110); border-radius: 14px; padding: 0 12px; color: #FF9F8F; background: transparent; }"
                                              "QPushButton:checked { background-color: rgba(232, 80, 59, 41); }"));
    m_warningChip->setStyleSheet(QStringLiteral("QPushButton { border: 1px solid rgba(240, 209, 122, 110); border-radius: 14px; padding: 0 12px; color: #F0D17A; background: transparent; }"
                                                "QPushButton:checked { background-color: rgba(232, 190, 59, 36); }"));
    const QString plain = QStringLiteral("QPushButton { border: 1px solid rgba(255, 255, 255, 26); border-radius: 14px; padding: 0 12px; color: #ECE6DE; background: transparent; }"
                                         "QPushButton:checked { background-color: #ECE6DE; color: #1A0F08; }");
    m_allChip->setStyleSheet(plain);
    m_infoChip->setStyleSheet(plain);

    // The messages.
    const QString filter = m_filter->text().trimmed();
    m_view->setUpdatesEnabled(false);
    m_view->clear();
    m_shown.clear();
    for (int i = 0; i < m_lines.size(); ++i)
    {
        const Line& line = m_lines[i];
        if (!m_source.isEmpty() && line.source != m_source)
        {
            continue;
        }
        if (m_levelFilter >= 0 && static_cast<int>(line.level) != m_levelFilter)
        {
            continue;
        }
        if (!filter.isEmpty() && !line.message.contains(filter, Qt::CaseInsensitive))
        {
            continue;
        }
        m_shown.push_back(i);
        auto* item = new QListWidgetItem(QStringLiteral("%1  %2  %3").arg(line.time.isEmpty() ? QStringLiteral("        ") : line.time,
                                                                         line.source.leftJustified(10, QLatin1Char(' '), true), line.message),
                                         m_view);
        item->setForeground(line.level == Level::Info ? Hearth::Text() : QColor(LevelColor(line.level)));
    }
    m_view->setUpdatesEnabled(true);
}

void HearthConsolePane::ShowDetails(int row)
{
    m_selected = (row >= 0 && row < m_shown.size()) ? m_shown[row] : -1;
    if (m_selected < 0)
    {
        m_detailKind->setText(tr("Nothing picked"));
        m_detailKind->setStyleSheet(QStringLiteral("background: transparent; border: none; color: #8D857B; font-weight: 600;"));
        m_detailTitle->setText(tr("Pick a message to see it here"));
        m_detailText->setText(tr("Errors and warnings get a plain explanation and a button to hand them to Claude."));
        m_detailRaw->hide();
        m_detailClaude->setText(QString());
        return;
    }
    const Line& line = m_lines[m_selected];
    m_detailKind->setText(QStringLiteral("ⓘ  ") + LevelName(line.level));
    m_detailKind->setStyleSheet(QStringLiteral("background: transparent; border: none; font-weight: 600; color: %1;").arg(LevelColor(line.level)));
    m_detailTitle->setText(TitleFor(line.message));
    m_detailText->setText(tr("From %1%2.").arg(line.source, line.time.isEmpty() ? QString() : tr(" at %1").arg(line.time)));
    m_detailRaw->setText(line.message);
    m_detailRaw->show();

    // If Claude already left a note about this message, show it.
    QString found = tr("Claude hasn't looked at this yet. \"Let Claude fix it\" hands it the message and the logs.");
    const QDir results(QDir(Hearth::HomeFolder()).filePath(QStringLiteral("claude-results")));
    for (const QFileInfo& info : results.entryInfoList({ QStringLiteral("*.json") }, QDir::Files, QDir::Time))
    {
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
        {
            continue;
        }
        const QJsonObject note = QJsonDocument::fromJson(file.readAll()).object();
        const QString match = note.value(QStringLiteral("match")).toString();
        if (!match.isEmpty() && line.message.contains(match))
        {
            found = note.value(QStringLiteral("body")).toString();
            break;
        }
    }
    m_detailClaude->setText(found);
}

QString HearthConsolePane::CurrentReport() const
{
    QStringList report;
    if (m_selected >= 0 && m_selected < m_lines.size())
    {
        const Line& line = m_lines[m_selected];
        report << QStringLiteral("%1 %2 (%3): %4").arg(line.time, LevelName(line.level), line.source, line.message);
        report << QString();
    }
    // A little of what came before, for context.
    report << QStringLiteral("Recent errors:");
    int added = 0;
    for (int i = m_lines.size() - 1; i >= 0 && added < 15; --i)
    {
        if (m_lines[i].level == Level::Error)
        {
            report << QStringLiteral("%1 (%2) %3").arg(m_lines[i].time, m_lines[i].source, m_lines[i].message);
            ++added;
        }
    }
    return report.join(QLatin1Char('\n'));
}

void HearthConsolePane::RunCommand()
{
    const QString command = m_command->text().trimmed();
    if (command.isEmpty())
    {
        return;
    }
    m_command->clear();
    Queue(Level::Info, "", QStringLiteral("> %1").arg(command).toUtf8().constData());
    const QByteArray utf8 = command.toUtf8();
    bool done = false;
    if (auto* console = AZ::Interface<AZ::IConsole>::Get())
    {
        done = console->PerformCommand(utf8.constData()).IsSuccess();
    }
    if (!done && gEnv && gEnv->pConsole)
    {
        gEnv->pConsole->ExecuteString(utf8.constData());
    }
}
