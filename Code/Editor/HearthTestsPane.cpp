/*
 * Hearth Engine "Tests". Built on O3DE (Apache-2.0 OR MIT). See HearthTestsPane.h.
 */

#include "EditorDefs.h"

#include "HearthTestsPane.h"
#include "HearthAgentClient.h"

#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>

#include <QComboBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    constexpr int TestsHistoryKeep = 8;
}

void HearthTestsPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::BottomDockWidgetArea;
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthTestsPane>("Tests", "Tools", options);
}

HearthTestsPane::HearthTestsPane(QWidget* parent)
    : QWidget(parent)
    , m_agent(std::make_unique<HearthAgentClient>(this))
{
    setObjectName(QStringLiteral("HearthTests"));
    setStyleSheet(QStringLiteral("QWidget#HearthTests { background-color: #171513; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(10);

    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Tests"), tr("Layout tests for the level, run with the game's own generator"));
    m_seeds = new QComboBox(this);
    m_seeds->addItem(tr("50 seeds"), 50);
    m_seeds->addItem(tr("200 seeds"), 200);
    header->addWidget(m_seeds);
    m_run = Hearth::MakeButton(tr("Run tests"), true, this);
    connect(m_run, &QPushButton::clicked, this, &HearthTestsPane::Run);
    header->addWidget(m_run);
    QPushButton* ask = Hearth::MakeButton(tr("Ask Claude"), false, this);
    connect(ask, &QPushButton::clicked, this, [this]()
    {
        const QString details = m_lastFailed.isEmpty()
            ? tr("The layout tests pass. Last result:\n%1").arg(m_summary->text())
            : tr("These layout tests failed:\n%1").arg(QString::fromUtf8(QJsonDocument(m_lastFailed).toJson(QJsonDocument::Indented)));
        Hearth::AskClaude(tr("Level layout tests"), details, QString());
    });
    header->addWidget(ask);
    root->addLayout(header);

    m_summary = Hearth::MakeLabel(tr("Not run yet in this session."), QStringLiteral("dim"), this);
    root->addWidget(m_summary);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_checks = new QWidget(scroll);
    m_checksLayout = new QVBoxLayout(m_checks);
    m_checksLayout->setContentsMargins(0, 0, 0, 0);
    m_checksLayout->setSpacing(6);
    m_checksLayout->addStretch(1);
    scroll->setWidget(m_checks);
    root->addWidget(scroll, 1);

    m_history = Hearth::MakeLabel(QString(), QStringLiteral("dim"), this);
    m_history->setWordWrap(true);
    root->addWidget(m_history);
    ShowHistory();
}

HearthTestsPane::~HearthTestsPane() = default;

void HearthTestsPane::Run()
{
    m_run->setEnabled(false);
    m_summary->setText(tr("Reading the level's settings…"));
    const int seeds = m_seeds->currentData().toInt();
    m_agent->Post(QStringLiteral("levelgen_get"), {}, [this, seeds](const QJsonObject& level, bool reached)
    {
        if (!reached || !level.value(QStringLiteral("ok")).toBool(false))
        {
            m_run->setEnabled(true);
            m_summary->setText(tr("Can't reach the Hearth Engine agent: %1").arg(level.value(QStringLiteral("error")).toString()));
            return;
        }
        m_summary->setText(tr("Running on %n seed(s)…", nullptr, seeds));
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        const QJsonObject args{ { QStringLiteral("values"), level.value(QStringLiteral("values")) }, { QStringLiteral("seeds"), seeds } };
        m_agent->Post(QStringLiteral("levelgen_tests"), args, [this, clock](const QJsonObject& answer, bool reached)
        {
            m_run->setEnabled(true);
            if (!reached || !answer.value(QStringLiteral("ok")).toBool(false))
            {
                m_summary->setText(tr("The tests couldn't run: %1").arg(answer.value(QStringLiteral("error")).toString()));
                return;
            }
            ShowResult(answer.value(QStringLiteral("data")).toObject(), clock->elapsed());
        }, 150000);
    });
}

void HearthTestsPane::AddCheck(const QString& tone, const QString& title, const QString& detail)
{
    QVBoxLayout* card = nullptr;
    QWidget* box = Hearth::MakeCard(m_checks, &card);
    auto* row = new QHBoxLayout();
    row->addWidget(Hearth::MakeChip(tone == QLatin1String("good") ? tr("✓ Pass") : (tone == QLatin1String("warn") ? tr("! Check") : tr("✕ Fail")), tone, box));
    row->addWidget(Hearth::MakeLabel(title, QStringLiteral("title"), box), 1);
    card->addLayout(row);
    QLabel* text = Hearth::MakeLabel(detail, QStringLiteral("dim"), box);
    text->setWordWrap(true);
    card->addWidget(text);
    m_checksLayout->insertWidget(m_checksLayout->count() - 1, box);
}

void HearthTestsPane::ShowResult(const QJsonObject& data, qint64 tookMs)
{
    while (m_checksLayout->count() > 1)
    {
        QLayoutItem* item = m_checksLayout->takeAt(0);
        delete item->widget();
        delete item;
    }
    m_lastFailed = {};
    const int seeds = data.value(QStringLiteral("seeds")).toInt();
    const bool same = data.value(QStringLiteral("deterministic")).toBool();
    const double reachable = data.value(QStringLiteral("reachable")).toDouble();
    const double total = data.value(QStringLiteral("total")).toDouble();
    const double minView = data.value(QStringLiteral("minView")).toDouble();
    const double avgView = data.value(QStringLiteral("avgView")).toDouble();
    const double walls = data.value(QStringLiteral("wallsPerChunk")).toDouble();
    const double lights = data.value(QStringLiteral("lightsPerChunk")).toDouble();
    int failed = 0;
    auto record = [&](bool ok, const char* key, const QJsonValue& value)
    {
        if (!ok)
        {
            ++failed;
            m_lastFailed.insert(QString::fromLatin1(key), value);
        }
    };

    AddCheck(same ? QStringLiteral("good") : QStringLiteral("bad"), tr("Same layout every time"),
             same ? tr("A seed always builds the same rooms, so co-op players and saves see one level.")
                  : tr("The same seed built a different chunk the second time. Players would see different levels."));
    record(same, "deterministic", same);

    const bool allReach = total > 0 && reachable >= total;
    const double share = total > 0 ? reachable / total * 100.0 : 0.0;
    AddCheck(allReach ? QStringLiteral("good") : (share >= 99.0 ? QStringLiteral("warn") : QStringLiteral("bad")), tr("Every room can be reached"),
             tr("%1 of %2 cells around the spawn can be walked to (%3%).").arg(qint64(reachable)).arg(qint64(total)).arg(share, 0, 'f', 1));
    record(allReach, "reachable", QJsonObject{ { QStringLiteral("reachable"), reachable }, { QStringLiteral("total"), total } });

    const bool viewOk = minView >= 3.0;
    AddCheck(viewOk ? QStringLiteral("good") : QStringLiteral("warn"), tr("Room to look around at the spawn"),
             tr("Closest wall from the spawn: %1 m at worst, %2 m on average.").arg(minView, 0, 'f', 1).arg(avgView, 0, 'f', 1));
    record(viewOk, "view", QJsonObject{ { QStringLiteral("min"), minView }, { QStringLiteral("average"), avgView } });

    const bool fill = walls > 0.0 && lights > 0.0;
    AddCheck(fill ? QStringLiteral("good") : QStringLiteral("bad"), tr("Chunks have walls and lights"),
             tr("About %1 walls and %2 lights in each chunk.").arg(walls, 0, 'f', 1).arg(lights, 0, 'f', 1));
    record(fill, "fill", QJsonObject{ { QStringLiteral("walls"), walls }, { QStringLiteral("lights"), lights } });

    const QString when = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    const QString line = failed == 0 ? tr("%1  Layout tests pass on %n seed(s)", nullptr, seeds).arg(when)
                                     : tr("%1  %2 of 4 checks failed on %n seed(s)", nullptr, seeds).arg(when).arg(failed);
    m_summary->setText(tr("%1 · took %2 s").arg(line).arg(tookMs / 1000.0, 0, 'f', 1));

    std::unique_ptr<QSettings> settings(Hearth::NewSettings());
    QStringList history = settings->value(QStringLiteral("tests/history")).toStringList();
    history.prepend(QStringLiteral("%1 %2").arg(QDate::currentDate().toString(QStringLiteral("MMM d")), line));
    while (history.size() > TestsHistoryKeep)
    {
        history.removeLast();
    }
    settings->setValue(QStringLiteral("tests/history"), history);
    ShowHistory();
}

void HearthTestsPane::ShowHistory()
{
    std::unique_ptr<QSettings> settings(Hearth::NewSettings());
    const QStringList history = settings->value(QStringLiteral("tests/history")).toStringList();
    m_history->setText(history.isEmpty() ? tr("Earlier runs show up here.") : tr("Earlier runs:\n%1").arg(history.join(QLatin1Char('\n'))));
}
