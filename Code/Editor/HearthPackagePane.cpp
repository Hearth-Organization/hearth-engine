/*
 * Hearth Engine Package the game. Built on O3DE (Apache-2.0 OR MIT). See HearthPackagePane.h.
 */

#include "EditorDefs.h"

#include "HearthPackagePane.h"
#include "HearthAgentClient.h"

#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>

#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    QString PackageProject()
    {
        return QString::fromUtf8(AZ::Utils::GetProjectPath().c_str());
    }

    //! "Level0" -> "Level 0", "FoundationSite" -> "Foundation site".
    QString PackageLevelTitle(const QString& level)
    {
        QString text = level;
        text.replace(QRegularExpression(QStringLiteral("([a-z])([A-Z0-9])")), QStringLiteral("\\1 \\2"));
        text.replace(QLatin1Char('_'), QLatin1Char(' '));
        text = text.simplified().toLower();
        if (!text.isEmpty())
        {
            text[0] = text[0].toUpper();
        }
        return text;
    }

    QFrame* PackageCard(QWidget* parent, const QString& title, QVBoxLayout** layout)
    {
        QFrame* card = Hearth::MakeCard(parent, layout);
        (*layout)->addWidget(Hearth::MakeLabel(title, QStringLiteral("title"), card));
        return card;
    }

    QLabel* PackageFact(QWidget* parent, QVBoxLayout* into, const QString& name)
    {
        auto* row = new QHBoxLayout();
        row->addWidget(Hearth::MakeLabel(name, QStringLiteral("dim"), parent));
        row->addStretch(1);
        QLabel* value = Hearth::MakeLabel(QStringLiteral("…"), QStringLiteral("body"), parent);
        row->addWidget(value);
        into->addLayout(row);
        return value;
    }
} // namespace

void HearthPackagePane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(120, 80, 1300, 820);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthPackagePane>("Package the game", "Tools", options);
}

HearthPackagePane::HearthPackagePane(QWidget* parent)
    : QWidget(parent)
    , m_agent(std::make_unique<HearthAgentClient>(this))
{
    setObjectName(QStringLiteral("HearthPackage"));
    setStyleSheet(QStringLiteral("QWidget#HearthPackage { background-color: #0D0C0B; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    QFile projectFile(QDir(PackageProject()).filePath(QStringLiteral("project.json")));
    QString name = QFileInfo(PackageProject()).fileName();
    if (projectFile.open(QIODevice::ReadOnly))
    {
        const QJsonObject json = QJsonDocument::fromJson(projectFile.readAll()).object();
        name = json.value(QStringLiteral("display_name")).toString(json.value(QStringLiteral("project_name")).toString(name));
    }
    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Package the game"), tr("%1 · make a build people can play").arg(name));
    header->addStretch(1);
    m_status = Hearth::MakeChip(QString(), QStringLiteral("ember"), this);
    m_status->hide();
    header->addWidget(m_status);
    m_package = Hearth::MakeButton(tr("Package"), true, this);
    connect(m_package, &QPushButton::clicked, this, &HearthPackagePane::Package);
    header->addWidget(m_package);
    root->addLayout(header);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);
    auto* left = new QVBoxLayout();
    left->setSpacing(10);

    // Where it's for.
    QVBoxLayout* whereLayout = nullptr;
    QFrame* where = PackageCard(this, tr("Where it's for"), &whereLayout);
    auto* targets = new QHBoxLayout();
    targets->setSpacing(8);
    const QVector<std::tuple<QString, QString, QString>> list = { { QStringLiteral("hearthos"), tr("Hearth OS"), tr("Hearth Store app, updates included") },
                                                                  { QStringLiteral("steam"), tr("Steam (Linux)"), tr("Steam Deck and desktops") },
                                                                  { QStringLiteral("test"), tr("Test build"), tr("A folder to try on another device") } };
    for (const auto& entry : list)
    {
        auto* card = new QPushButton(where);
        card->setCheckable(true);
        card->setChecked(std::get<0>(entry) == m_target);
        card->setMinimumHeight(66);
        card->setCursor(Qt::PointingHandCursor);
        card->setStyleSheet(QStringLiteral("QPushButton { background-color: rgba(255, 255, 255, 8); border: 1px solid rgba(255, 255, 255, 15); border-radius: 12px; }"
                                           "QPushButton:checked { background-color: rgba(232, 116, 59, 31); border-color: rgba(232, 116, 59, 140); }"));
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(12, 8, 12, 8);
        QLabel* title = Hearth::MakeLabel(std::get<1>(entry), QStringLiteral("body"), card);
        QLabel* about = Hearth::MakeLabel(std::get<2>(entry), QStringLiteral("small"), card);
        title->setAttribute(Qt::WA_TransparentForMouseEvents);
        about->setAttribute(Qt::WA_TransparentForMouseEvents);
        cardLayout->addWidget(title);
        cardLayout->addWidget(about);
        const QString key = std::get<0>(entry);
        connect(card, &QPushButton::clicked, this, [this, card, key]()
        {
            for (QPushButton* other : m_targets)
            {
                other->setChecked(other == card);
            }
            m_target = key;
        });
        m_targets << card;
        targets->addWidget(card, 1);
    }
    whereLayout->addLayout(targets);
    left->addWidget(where);

    // What goes in.
    QVBoxLayout* whatLayout = nullptr;
    QFrame* what = PackageCard(this, tr("What goes in"), &whatLayout);
    m_levels = new QListWidget(what);
    m_levels->setMaximumHeight(110);
    m_levels->setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; } QListWidget::item { padding: 3px; }"));
    whatLayout->addWidget(Hearth::MakeSettingRow(tr("Levels"), QString(), m_levels, what));
    m_startsIn = new QComboBox(what);
    whatLayout->addWidget(Hearth::MakeSettingRow(tr("Starts in"), QString(), m_startsIn, what));
    m_version = new QLineEdit(what);
    m_version->setPlaceholderText(tr("Ask Dragongamer9"));
    m_version->setFixedWidth(180);
    whatLayout->addWidget(Hearth::MakeSettingRow(tr("Version"), tr("The owner picks every version number"), m_version, what));
    m_leaveOut = new Hearth::Toggle(what);
    m_leaveOut->setChecked(true);
    whatLayout->addWidget(Hearth::MakeSettingRow(tr("Leave out editor-only files"), QString(), m_leaveOut, what));
    m_cloud = new Hearth::Toggle(what);
    QScopedPointer<QSettings> settings(Hearth::NewSettings());
    m_cloud->setChecked(settings->value(QStringLiteral("laptop/cloudEngineBuilds"), true).toBool());
    whatLayout->addWidget(Hearth::MakeSettingRow(tr("Build on GitHub (32-core)"), tr("Off: package on this laptop (slow, memory-limited)"), m_cloud, what));
    connect(m_cloud, &QAbstractButton::toggled, this, [this]() { RunChecks(); });
    left->addWidget(what);
    left->addStretch(1);
    body->addLayout(left, 1);

    // Checks, estimate, last package.
    auto* right = new QVBoxLayout();
    right->setSpacing(10);
    QVBoxLayout* checksOuter = nullptr;
    QFrame* checks = Hearth::MakeCard(this, &checksOuter);
    auto* checksTitle = new QPushButton(tr("Checks before packaging  ↻"), checks);
    checksTitle->setStyleSheet(QStringLiteral("QPushButton { text-align: left; border: none; background: transparent; color: #ECE6DE; font-weight: 600; font-size: 14px; }"));
    checksTitle->setToolTip(tr("Check again"));
    connect(checksTitle, &QPushButton::clicked, this, &HearthPackagePane::RunChecks);
    checksOuter->addWidget(checksTitle);
    auto* checksBody = new QWidget(checks);
    m_checksLayout = new QVBoxLayout(checksBody);
    m_checksLayout->setContentsMargins(0, 0, 0, 0);
    checksOuter->addWidget(checksBody);
    right->addWidget(checks);

    QVBoxLayout* estimate = nullptr;
    QFrame* estimateCard = PackageCard(this, tr("Estimate"), &estimate);
    m_size = PackageFact(estimateCard, estimate, tr("Download size"));
    m_minutes = PackageFact(estimateCard, estimate, tr("Time on GitHub"));
    m_cost = PackageFact(estimateCard, estimate, tr("Cost"));
    right->addWidget(estimateCard);

    QVBoxLayout* last = nullptr;
    QFrame* lastCard = PackageCard(this, tr("Last package"), &last);
    m_last = Hearth::MakeLabel(tr("None yet"), QStringLiteral("dim"), lastCard);
    m_last->setWordWrap(true);
    m_last->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_last->setOpenExternalLinks(true);
    last->addWidget(m_last);
    right->addWidget(lastCard);
    right->addStretch(1);
    auto* rightHolder = new QWidget(this);
    rightHolder->setFixedWidth(440);
    rightHolder->setLayout(right);
    body->addWidget(rightHolder);
    root->addLayout(body, 1);

    m_watch = new QTimer(this);
    m_watch->setInterval(5000);
    connect(m_watch, &QTimer::timeout, this, &HearthPackagePane::Watch);
}

HearthPackagePane::~HearthPackagePane() = default;

void HearthPackagePane::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    LoadLevels();
    RunChecks();
    Watch();
}

void HearthPackagePane::LoadLevels()
{
    QString first;
    QFile autoexec(QDir(PackageProject()).filePath(QStringLiteral("autoexec.cfg")));
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
    const QStringList levels = QDir(QDir(PackageProject()).filePath(QStringLiteral("Levels"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    m_levels->clear();
    m_startsIn->clear();
    for (const QString& level : levels)
    {
        auto* item = new QListWidgetItem(PackageLevelTitle(level), m_levels);
        item->setData(Qt::UserRole, level);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        // The default level is O3DE's empty starter; everything else goes in.
        item->setCheckState(level.compare(QLatin1String("DefaultLevel"), Qt::CaseInsensitive) == 0 ? Qt::Unchecked : Qt::Checked);
        m_startsIn->addItem(PackageLevelTitle(level), level);
    }
    const int at = m_startsIn->findData(first, Qt::UserRole, Qt::MatchFixedString);
    m_startsIn->setCurrentIndex(at >= 0 ? at : 0);
}

QStringList HearthPackagePane::PickedLevels() const
{
    QStringList picked;
    for (int i = 0; i < m_levels->count(); ++i)
    {
        if (m_levels->item(i)->checkState() == Qt::Checked)
        {
            picked << m_levels->item(i)->data(Qt::UserRole).toString();
        }
    }
    return picked;
}

void HearthPackagePane::RunChecks()
{
    while (QLayoutItem* item = m_checksLayout->takeAt(0))
    {
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
    m_checksLayout->addWidget(Hearth::MakeLabel(tr("Checking (Level 0's layout tests take a moment)…"), QStringLiteral("dim"), this));
    QJsonArray levels;
    for (const QString& level : PickedLevels())
    {
        levels.append(level);
    }
    m_agent->Post(QStringLiteral("package_checks"), QJsonObject{ { QStringLiteral("levels"), levels } }, [this](const QJsonObject& answer, bool reached)
    {
        while (QLayoutItem* item = m_checksLayout->takeAt(0))
        {
            if (item->widget())
            {
                item->widget()->deleteLater();
            }
            delete item;
        }
        if (!reached || !answer.contains(QStringLiteral("checks")))
        {
            m_checksLayout->addWidget(Hearth::MakeLabel(reached ? answer.value(QStringLiteral("error")).toString() : tr("The Hearth Engine agent isn't running (the Hub starts it)."),
                                                        QStringLiteral("dim"), this));
            return;
        }
        for (const QJsonValue value : answer.value(QStringLiteral("checks")).toArray())
        {
            const QJsonObject check = value.toObject();
            const bool ok = check.value(QStringLiteral("ok")).toBool();
            const bool warn = check.value(QStringLiteral("warn")).toBool();
            auto* row = new QWidget(this);
            auto* layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 4, 0, 4);
            QLabel* mark = Hearth::MakeLabel(warn ? QStringLiteral("!") : (ok ? QStringLiteral("✓") : QStringLiteral("✕")), QStringLiteral("body"), row);
            mark->setFixedWidth(18);
            mark->setStyleSheet(QStringLiteral("background: transparent; font-weight: 700; color: %1;").arg((warn ? Hearth::Warn() : (ok ? Hearth::Good() : Hearth::Bad())).name()));
            layout->addWidget(mark);
            QLabel* text = Hearth::MakeLabel(check.value(QStringLiteral("text")).toString(), QStringLiteral("body"), row);
            text->setWordWrap(true);
            QStringList files;
            for (const QJsonValue f : check.value(QStringLiteral("files")).toArray())
            {
                files << f.toString();
            }
            text->setToolTip(files.join(QLatin1Char('\n')));
            layout->addWidget(text, 1);
            m_checksLayout->addWidget(row);
        }
        const double bytes = answer.value(QStringLiteral("download_bytes")).toDouble();
        m_size->setText(bytes > 0 ? (bytes >= 1e9 ? tr("about %1 GB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 1) : tr("about %1 MB").arg(qRound(bytes / (1024.0 * 1024))))
                                  : tr("process the assets first"));
        const bool cloud = m_cloud->isChecked();
        m_minutes->setText(cloud ? tr("about %n min", nullptr, answer.value(QStringLiteral("github_minutes")).toInt()) : tr("on this laptop"));
        m_cost->setText(cloud ? tr("about $%1").arg(qMax(1.0, std::round(answer.value(QStringLiteral("github_cost")).toDouble())), 0, 'f', 0) : tr("free"));
        const QJsonObject last = answer.value(QStringLiteral("last_package")).toObject();
        if (!last.isEmpty())
        {
            const QString when = QLocale().toString(QDateTime::fromSecsSinceEpoch(qint64(last.value(QStringLiteral("when")).toDouble())), QLocale::ShortFormat);
            QStringList files;
            for (const QJsonValue f : last.value(QStringLiteral("files")).toArray())
            {
                files << QStringLiteral("<a style='color:#FFCFB4' href='%1'>%2</a>").arg(QUrl::fromLocalFile(QFileInfo(f.toString()).path()).toString(), QFileInfo(f.toString()).fileName());
            }
            m_last->setText(tr("%1 for %2 · %3 · %4<br>%5").arg(last.value(QStringLiteral("version")).toString(), last.value(QStringLiteral("target")).toString(), when,
                                                               last.value(QStringLiteral("where")).toString(), files.join(QStringLiteral("<br>"))));
        }
    }, 240000);
}

void HearthPackagePane::Package()
{
    const QString version = m_version->text().trimmed();
    if (version.isEmpty())
    {
        QMessageBox::information(this, tr("Package"), tr("Type the version for this package first. Hearth Engine never picks a version number for you."));
        m_version->setFocus();
        return;
    }
    const QStringList levels = PickedLevels();
    if (levels.isEmpty())
    {
        QMessageBox::information(this, tr("Package"), tr("Tick at least one level."));
        return;
    }
    // Starts in: the game launcher's autoexec.cfg LoadLevel (the same as Project settings' First level).
    const QString start = m_startsIn->currentData().toString();
    const QString autoexecPath = QDir(PackageProject()).filePath(QStringLiteral("autoexec.cfg"));
    QStringList lines;
    QFile in(autoexecPath);
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
    lines << QStringLiteral("LoadLevel %1").arg(start);
    QSaveFile out(autoexecPath);
    if (out.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        out.write((lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
        out.commit();
    }
    const bool cloud = m_cloud->isChecked();
    const QString where = m_target == QLatin1String("hearthos") ? tr("a draft release on GitHub for the Hearth Store (you publish it)")
                                                                : (m_target == QLatin1String("steam") ? tr("a folder ready for a Steam depot") : tr("a test build to copy to another device"));
    QString message = tr("Package version %1 as %2, with %3, starting in %4?").arg(version, where, levels.join(QStringLiteral(", ")), PackageLevelTitle(start));
    if (cloud)
    {
        message += QStringLiteral("\n\n") + tr("GitHub packages what's pushed: commit and push your latest changes first. It runs on hearth-32 (%1).").arg(m_cost->text());
    }
    if (QMessageBox::question(this, tr("Package"), message) != QMessageBox::Yes)
    {
        return;
    }
    QJsonArray levelArray;
    for (const QString& level : levels)
    {
        levelArray.append(level);
    }
    m_package->setEnabled(false);
    m_agent->Post(QStringLiteral("package"),
                  QJsonObject{ { QStringLiteral("target"), m_target }, { QStringLiteral("version"), version }, { QStringLiteral("levels"), levelArray },
                               { QStringLiteral("leave_out_editor"), m_leaveOut->isChecked() }, { QStringLiteral("cloud"), cloud } },
                  [this](const QJsonObject& answer, bool reached)
    {
        if (!reached || !answer.value(QStringLiteral("ok")).toBool())
        {
            QMessageBox::warning(this, tr("Package"), answer.value(QStringLiteral("error")).toString());
            m_package->setEnabled(true);
            return;
        }
        m_status->setText(tr("Packaging…"));
        m_status->show();
        m_watch->start();
    });
}

void HearthPackagePane::Watch()
{
    m_agent->Post(QStringLiteral("package_status"), {}, [this](const QJsonObject& state, bool reached)
    {
        if (!reached)
        {
            return;
        }
        if (state.value(QStringLiteral("running")).toBool())
        {
            m_status->setText(tr("Packaging · %1").arg(state.value(QStringLiteral("step")).toString()));
            m_status->show();
            m_package->setEnabled(false);
            m_watch->start();
            return;
        }
        if (!m_watch->isActive())
        {
            return;
        }
        m_watch->stop();
        m_package->setEnabled(true);
        const QString error = state.value(QStringLiteral("error")).toString();
        if (!error.isEmpty())
        {
            m_status->setText(tr("Packaging failed"));
            QMessageBox::warning(this, tr("Package"), error);
            return;
        }
        m_status->setText(tr("Packaged"));
        const QJsonArray files = state.value(QStringLiteral("result")).toObject().value(QStringLiteral("files")).toArray();
        if (!files.isEmpty())
        {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(files.first().toString()).path()));
        }
        RunChecks();
    });
}
