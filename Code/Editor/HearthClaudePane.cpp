/*
 * Hearth Engine "Claude" panel. Built on O3DE (Apache-2.0 OR MIT). See HearthClaudePane.h.
 */

#include "EditorDefs.h"

#include "HearthClaudePane.h"

#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/FlowLayout.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    QLabel* Bubble(const QString& text, bool fromOwner, QWidget* parent)
    {
        auto* bubble = new QLabel(text, parent);
        bubble->setWordWrap(true);
        bubble->setTextInteractionFlags(Qt::TextSelectableByMouse);
        bubble->setStyleSheet(fromOwner ? QStringLiteral("background-color: #E8743B; color: #1A0F08; border-radius: 12px; padding: 9px 12px; font-size: 13px;")
                                        : QStringLiteral("background-color: rgba(255, 255, 255, 13); color: #ECE6DE; border-radius: 12px; padding: 9px 12px; font-size: 13px;"));
        return bubble;
    }
} // namespace

void HearthClaudePane::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::RightDockWidgetArea;
    options.showInMenu = true;
    options.isDisabledInComponentMode = false;
    AzToolsFramework::RegisterViewPane<HearthClaudePane>("Claude", "Tools", options);
}

HearthClaudePane::HearthClaudePane(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthClaude"));
    setStyleSheet(QStringLiteral("QWidget#HearthClaude { background-color: #171513; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    auto* top = new QHBoxLayout();
    top->addWidget(Hearth::MakeLabel(tr("Claude"), QStringLiteral("title"), this));
    top->addStretch(1);
    m_status = Hearth::MakeLabel(QString(), QStringLiteral("small"), this);
    top->addWidget(m_status);
    root->addLayout(top);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
    auto* holder = new QWidget(scroll);
    holder->setStyleSheet(QStringLiteral("background: transparent;"));
    m_messages = new QVBoxLayout(holder);
    m_messages->setContentsMargins(0, 0, 0, 0);
    m_messages->setSpacing(8);
    m_messages->addStretch(1);
    scroll->setWidget(holder);
    root->addWidget(scroll, 1);

    m_recent = Hearth::MakeLabel(QString(), QStringLiteral("small"), this);
    m_recent->setWordWrap(true);
    root->addWidget(m_recent);

    // What Claude can do in this editor.
    QVBoxLayout* canLayout = nullptr;
    QFrame* can = Hearth::MakeCard(this, &canLayout);
    can->setStyleSheet(QStringLiteral("QFrame#HearthCard { background-color: rgba(255, 255, 255, 8); border: none; border-radius: 12px; }"));
    canLayout->addWidget(Hearth::MakeLabel(tr("Claude can"), QStringLiteral("dim"), can));
    m_abilities = new QWidget(can);
    auto* flow = new FlowLayout(m_abilities);
    for (const QString& ability : { tr("Edit scene"), tr("Run and drive the game"), tr("Screenshots"), tr("Rebuild"), tr("Restart editor"),
                                    tr("Read crash reports") })
    {
        flow->addWidget(Hearth::MakeChip(ability, QStringLiteral("ember"), m_abilities));
    }
    canLayout->addWidget(m_abilities);
    root->addWidget(can);

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(tr("Describe a change…"));
    m_input->setMinimumHeight(40);
    m_input->setStyleSheet(QStringLiteral("QLineEdit { border-radius: 12px; padding: 0 14px; }"));
    connect(m_input, &QLineEdit::returnPressed, this, &HearthClaudePane::Send);
    root->addWidget(m_input);

    m_timer = new QTimer(this);
    m_timer->setInterval(2000);
    connect(m_timer, &QTimer::timeout, this, &HearthClaudePane::Refresh);
    m_timer->start();
    Refresh();
}

QString HearthClaudePane::BridgeFolder() const
{
    const auto project = AZ::Utils::GetProjectPath();
    return QDir(QString::fromUtf8(project.c_str())).filePath(QStringLiteral("user/ClaudeBridge"));
}

void HearthClaudePane::Refresh()
{
    const QDir bridge(BridgeFolder());

    // Connected: Claude used the bridge in the last 10 minutes (the activity list), and the bridge is running.
    const QFileInfo heartbeat(bridge.filePath(QStringLiteral("heartbeat.json")));
    const QFileInfo activity(bridge.filePath(QStringLiteral("activity.jsonl")));
    const bool bridgeUp = heartbeat.exists() && heartbeat.lastModified().secsTo(QDateTime::currentDateTime()) < 15;
    const bool recent = activity.exists() && activity.lastModified().secsTo(QDateTime::currentDateTime()) < 600;
    if (bridgeUp && recent)
    {
        m_status->setText(tr("● connected"));
        m_status->setStyleSheet(QStringLiteral("background: transparent; color: #A6DD97; font-size: 11px;"));
    }
    else if (bridgeUp)
    {
        m_status->setText(tr("● ready"));
        m_status->setStyleSheet(QStringLiteral("background: transparent; color: #F0D17A; font-size: 11px;"));
    }
    else
    {
        m_status->setText(tr("not connected"));
        m_status->setStyleSheet(QStringLiteral("background: transparent; color: #8D857B; font-size: 11px;"));
    }
    m_abilities->setEnabled(bridgeUp);

    // What Claude did lately.
    QFile activityFile(activity.absoluteFilePath());
    if (activityFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        const QList<QByteArray> lines = activityFile.readAll().split('\n');
        QStringList done;
        for (int i = lines.size() - 1; i >= 0 && done.size() < 3; --i)
        {
            const QJsonObject entry = QJsonDocument::fromJson(lines[i]).object();
            if (entry.isEmpty())
            {
                continue;
            }
            const QString when = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(entry.value(QStringLiteral("time")).toDouble())).toString(QStringLiteral("HH:mm"));
            done << QStringLiteral("%1  %2").arg(when, entry.value(QStringLiteral("what")).toString());
        }
        m_recent->setText(done.isEmpty() ? QString() : tr("Lately: ") + done.join(QStringLiteral("  ·  ")));
    }

    // The conversation (owner's messages and Claude's replies).
    const QDir chat(bridge.filePath(QStringLiteral("chat")));
    const QFileInfoList files = chat.entryInfoList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
    QString signature;
    for (const QFileInfo& file : files)
    {
        signature += file.fileName() + QString::number(file.size());
    }
    if (signature == m_lastSignature)
    {
        return;
    }
    m_lastSignature = signature;
    while (m_messages->count() > 1)
    {
        QLayoutItem* item = m_messages->takeAt(0);
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
    const int start = qMax(0, files.size() - 30);
    for (int i = start; i < files.size(); ++i)
    {
        QFile file(files[i].absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
        {
            continue;
        }
        const QJsonObject message = QJsonDocument::fromJson(file.readAll()).object();
        const bool fromOwner = message.value(QStringLiteral("from")).toString() != QLatin1String("claude");
        auto* row = new QWidget();
        auto* rowLayout = new QVBoxLayout(row);
        rowLayout->setContentsMargins(fromOwner ? 16 : 0, 0, 0, 0);
        rowLayout->setSpacing(6);
        rowLayout->addWidget(Bubble(message.value(QStringLiteral("text")).toString(), fromOwner, row));
        const QJsonArray chips = message.value(QStringLiteral("chips")).toArray();
        if (!chips.isEmpty())
        {
            auto* chipRow = new QHBoxLayout();
            chipRow->setSpacing(6);
            for (const QJsonValue chip : chips)
            {
                chipRow->addWidget(Hearth::MakeChip(chip.toString(), QStringLiteral("plain"), row));
            }
            chipRow->addStretch(1);
            rowLayout->addLayout(chipRow);
        }
        m_messages->insertWidget(m_messages->count() - 1, row);
    }
}

void HearthClaudePane::Send()
{
    const QString text = m_input->text().trimmed();
    if (text.isEmpty())
    {
        return;
    }
    m_input->clear();
    QDir chat(QDir(BridgeFolder()).filePath(QStringLiteral("chat")));
    chat.mkpath(QStringLiteral("."));
    const QString name = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")) + QStringLiteral("-owner.json");
    QFile file(chat.filePath(name));
    if (file.open(QIODevice::WriteOnly))
    {
        file.write(QJsonDocument(QJsonObject{ { QStringLiteral("from"), QStringLiteral("owner") }, { QStringLiteral("text"), text },
                                              { QStringLiteral("time"), static_cast<double>(QDateTime::currentSecsSinceEpoch()) } })
                       .toJson(QJsonDocument::Compact));
    }
    // Make sure it reaches a Claude session: open Claude with the request.
    const auto project = AZ::Utils::GetProjectPath();
    Hearth::AskClaude(tr("Change request from the Hearth Engine editor: %1").arg(text),
                      tr("Reply by writing {\"from\": \"claude\", \"text\": \"...\", \"chips\": [...]} to %1 (any new .json file).").arg(chat.absolutePath()),
                      QString::fromUtf8(project.c_str()));
    Refresh();
}
