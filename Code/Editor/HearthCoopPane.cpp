/*
 * Hearth Engine "Co-op test" window. Built on O3DE (Apache-2.0 OR MIT). See HearthCoopPane.h.
 */

#include "EditorDefs.h"

#include "HearthCoopPane.h"
#include "HearthAgentClient.h"

#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    const char* CoopSliderStyle =
        "QSlider::groove:horizontal { height: 4px; background: #2C2824; border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: #E8743B; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: #ECE6DE; width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; }";

    QWidget* PlayerRow(const QString& name, const QString& where, const QString& state, const QString& tone, const QString& avatarColor, QWidget* parent)
    {
        auto* row = new QFrame(parent);
        row->setObjectName(QStringLiteral("CoopPlayer"));
        row->setStyleSheet(QStringLiteral("QFrame#CoopPlayer { background-color: rgba(255, 255, 255, 8); border: 1px solid rgba(255, 255, 255, 15); border-radius: 12px; }"));
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(12);
        auto* avatar = new QLabel(name.left(1).toUpper(), row);
        avatar->setFixedSize(38, 38);
        avatar->setAlignment(Qt::AlignCenter);
        avatar->setStyleSheet(QStringLiteral("background-color: %1; color: #1A0F08; border-radius: 19px; font-weight: 600;").arg(avatarColor));
        layout->addWidget(avatar);
        auto* texts = new QVBoxLayout();
        texts->setSpacing(2);
        texts->addWidget(Hearth::MakeLabel(name, QStringLiteral("body"), row));
        texts->addWidget(Hearth::MakeLabel(where, QStringLiteral("small"), row));
        layout->addLayout(texts, 1);
        layout->addWidget(Hearth::MakeChip(state, tone, row));
        return row;
    }
} // namespace

void HearthCoopPane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(120, 80, 1180, 760);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthCoopPane>("Co-op test", "Tools", options);
}

HearthCoopPane::HearthCoopPane(QWidget* parent)
    : QWidget(parent)
    , m_agent(std::make_unique<HearthAgentClient>(this))
{
    setObjectName(QStringLiteral("HearthCoop"));
    setStyleSheet(QStringLiteral("QWidget#HearthCoop { background-color: #0D0C0B; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Co-op test"), tr("Play with more than one player"));
    QPushButton* invite = Hearth::MakeButton(tr("Copy invite"), false, this);
    m_start = Hearth::MakeButton(tr("Start for everyone"), true, this);
    header->addWidget(invite);
    header->addWidget(m_start);
    root->addLayout(header);
    connect(invite, &QPushButton::clicked, this, [this]()
    {
        // On the other computer: open the game's console and paste this.
        QApplication::clipboard()->setText(m_invite.isEmpty() ? tr("connect <this computer's address>:33450") : m_invite);
    });
    connect(m_start, &QPushButton::clicked, this, &HearthCoopPane::Start);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);

    QVBoxLayout* left = nullptr;
    QFrame* playersCard = Hearth::MakeCard(this, &left);
    auto* playersTop = new QHBoxLayout();
    playersTop->addWidget(Hearth::MakeLabel(tr("Players"), QStringLiteral("title"), playersCard), 1);
    m_playerCount = Hearth::MakeLabel(QString(), QStringLiteral("dim"), playersCard);
    playersTop->addWidget(m_playerCount);
    left->addLayout(playersTop);
    m_players = new QVBoxLayout();
    m_players->setSpacing(8);
    left->addLayout(m_players);
    left->addWidget(Hearth::MakeLabel(tr("Join log"), QStringLiteral("title"), playersCard));
    m_log = new QLabel(playersCard);
    m_log->setWordWrap(true);
    m_log->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_log->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_log->setMinimumHeight(140);
    m_log->setStyleSheet(QStringLiteral("background-color: #100F0E; color: #8D857B; border-radius: 10px; padding: 10px 12px; font-family: \"Geist Mono\"; font-size: 11px;"));
    left->addWidget(m_log, 1);
    body->addWidget(playersCard, 1);

    QVBoxLayout* right = nullptr;
    QFrame* settingsCard = Hearth::MakeCard(this, &right);
    settingsCard->setFixedWidth(440);
    right->addWidget(Hearth::MakeLabel(tr("Test settings"), QStringLiteral("title"), settingsCard));
    right->addWidget(Hearth::MakeSettingRow(tr("Level"), QString(), Hearth::MakeLabel(QStringLiteral("Level0"), QStringLiteral("mono"), settingsCard), settingsCard));
    m_seed = new QLineEdit(settingsCard);
    m_seed->setPlaceholderText(tr("same as the level"));
    m_seed->setFixedWidth(200);
    m_seed->setStyleSheet(QStringLiteral("QLineEdit { font-family: \"Geist Mono\"; color: #FFCFB4; }"));
    right->addWidget(Hearth::MakeSettingRow(tr("Seed"), tr("Same rooms for everyone"), m_seed, settingsCard));
    m_together = new Hearth::Toggle(settingsCard);
    m_together->setChecked(true);
    right->addWidget(Hearth::MakeSettingRow(tr("Start together in the spawn room"), QString(), m_together, settingsCard));

    auto makeSlider = [settingsCard](int max, int value, QSlider** slider, QLabel** label, const QString& unit)
    {
        auto* holder = new QWidget(settingsCard);
        auto* layout = new QHBoxLayout(holder);
        layout->setContentsMargins(0, 0, 0, 0);
        *slider = new QSlider(Qt::Horizontal, holder);
        (*slider)->setRange(0, max);
        (*slider)->setValue(value);
        (*slider)->setFixedWidth(150);
        (*slider)->setStyleSheet(QLatin1String(CoopSliderStyle));
        *label = Hearth::MakeLabel(QStringLiteral("%1 %2").arg(value).arg(unit), QStringLiteral("mono"), holder);
        (*label)->setFixedWidth(56);
        QLabel* valueLabel = *label;
        QObject::connect(*slider, &QSlider::valueChanged, valueLabel, [valueLabel, unit](int v) { valueLabel->setText(QStringLiteral("%1 %2").arg(v).arg(unit)); });
        layout->addWidget(*slider);
        layout->addWidget(*label);
        return holder;
    };
    right->addWidget(Hearth::MakeSettingRow(tr("Fake network lag"), tr("Test how it feels online"), makeSlider(300, 80, &m_lag, &m_lagValue, QStringLiteral("ms")), settingsCard));
    right->addWidget(Hearth::MakeSettingRow(tr("Packet loss"), QString(), makeSlider(20, 1, &m_loss, &m_lossValue, QStringLiteral("%")), settingsCard));
    m_monsters = new Hearth::Toggle(settingsCard);
    m_monsters->setChecked(true);
    right->addWidget(Hearth::MakeSettingRow(tr("Monsters"), QString(), m_monsters, settingsCard));
    m_secondWindow = new Hearth::Toggle(settingsCard);
    m_secondWindow->setChecked(true);
    right->addWidget(Hearth::MakeSettingRow(tr("Second window on this laptop"), tr("Uses about 1.2 GB more memory"), m_secondWindow, settingsCard));
    QLabel* note = new QLabel(tr("Each player's log is saved with this test, so Claude can read every error from every machine."), settingsCard);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("background-color: rgba(232, 116, 59, 26); color: #FFCFB4; border-radius: 10px; padding: 10px 12px; font-size: 12px;"));
    right->addWidget(note);
    right->addStretch(1);
    body->addWidget(settingsCard);
    root->addLayout(body, 1);

    m_timer = new QTimer(this);
    m_timer->setInterval(2000);
    connect(m_timer, &QTimer::timeout, this, &HearthCoopPane::Refresh);
    m_timer->start();
    Refresh();
}

HearthCoopPane::~HearthCoopPane() = default;

void HearthCoopPane::Start()
{
    m_start->setEnabled(false);
    m_start->setText(tr("Starting…"));
    QJsonObject args{ { QStringLiteral("seed"), m_seed->text().trimmed() },
                      { QStringLiteral("together"), m_together->isChecked() },
                      { QStringLiteral("lag_ms"), m_lag->value() },
                      { QStringLiteral("loss_percent"), m_loss->value() },
                      { QStringLiteral("monsters"), m_monsters->isChecked() },
                      { QStringLiteral("second_window"), m_secondWindow->isChecked() } };
    m_agent->Post(QStringLiteral("coop_start"), args, [this](const QJsonObject& answer, bool)
    {
        m_start->setEnabled(true);
        m_start->setText(tr("Start for everyone"));
        if (!answer.value(QStringLiteral("ok")).toBool())
        {
            m_log->setText(tr("Couldn't start: %1").arg(answer.value(QStringLiteral("error")).toString()));
            return;
        }
        m_invite = answer.value(QStringLiteral("invite")).toString();
        Refresh();
    },
    60000);
}

void HearthCoopPane::Refresh()
{
    if (!isVisible())
    {
        return;
    }
    m_agent->Post(QStringLiteral("coop_status"), {}, [this](const QJsonObject& answer, bool reached)
    {
        while (QLayoutItem* item = m_players->takeAt(0))
        {
            if (item->widget())
            {
                item->widget()->deleteLater();
            }
            delete item;
        }
        if (!reached)
        {
            m_playerCount->setText(tr("agent offline"));
            return;
        }
        m_invite = answer.value(QStringLiteral("invite")).toString(m_invite);
        const QJsonArray players = answer.value(QStringLiteral("players")).toArray();
        const QStringList colors = { QStringLiteral("#E8743B"), QStringLiteral("#8FB3E0"), QStringLiteral("#A6DD97"), QStringLiteral("#F0D17A") };
        int index = 0;
        for (const QJsonValue value : players)
        {
            const QJsonObject player = value.toObject();
            const QString state = player.value(QStringLiteral("state")).toString();
            const int mb = player.value(QStringLiteral("mb")).toInt();
            QString where = player.value(QStringLiteral("where")).toString();
            if (mb > 0)
            {
                where += tr(" · %1 MB").arg(mb);
            }
            m_players->addWidget(PlayerRow(player.value(QStringLiteral("name")).toString(), where,
                                           state == QLatin1String("ready") ? tr("Ready") : (state == QLatin1String("left") ? tr("Left") : state),
                                           state == QLatin1String("ready") ? QStringLiteral("good") : QStringLiteral("warn"), colors[index % colors.size()], this));
            ++index;
        }
        // The open seat: invite someone.
        m_players->addWidget(PlayerRow(tr("Empty seat"), tr("Invite a friend: Copy invite, then paste it in their game's console"), tr("Open"),
                                       QStringLiteral("plain"), QStringLiteral("#3A352F"), this));
        m_playerCount->setText(tr("%1 of 4").arg(players.size()));
        QStringList log;
        for (const QJsonValue line : answer.value(QStringLiteral("log")).toArray())
        {
            log << line.toString().toHtmlEscaped();
        }
        m_log->setText(log.isEmpty() ? tr("Not started yet. Press Start for everyone.") : log.join(QStringLiteral("<br>")));
    });
}
