/*
 * Hearth Engine Script console. Built on O3DE (Apache-2.0 OR MIT). See HearthScriptConsole.h.
 * Claude's commands come from the Claude bridge's activity list (<project>/user/ClaudeBridge/activity.jsonl); yours
 * run here through the editor's Python and are kept in owner_console.jsonl next to it.
 */

#include "EditorDefs.h"

#include "HearthScriptConsole.h"

#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/EditorPythonRunnerRequestsBus.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    QVector<QJsonObject> ScriptReadLines(const QString& path, int keep)
    {
        QVector<QJsonObject> entries;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            return entries;
        }
        if (file.size() > 512 * 1024)
        {
            file.seek(file.size() - 512 * 1024);
            file.readLine();
        }
        for (const QByteArray& line : file.readAll().split('\n'))
        {
            const QJsonObject entry = QJsonDocument::fromJson(line).object();
            if (!entry.isEmpty())
            {
                entries << entry;
            }
        }
        return entries.mid(qMax(0, entries.size() - keep));
    }

    bool ScriptSetting(const QString& key, bool fallback)
    {
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        return settings->value(key, fallback).toBool();
    }

    void ScriptSet(const QString& key, const QVariant& value)
    {
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        settings->setValue(key, value);
    }

    //! Python string literal of any text.
    QString ScriptQuote(const QString& text)
    {
        QString quoted = text;
        quoted.replace(QLatin1String("\\"), QLatin1String("\\\\")).replace(QLatin1String("'"), QLatin1String("\\'")).replace(QLatin1String("\n"), QLatin1String("\\n"));
        return QLatin1Char('\'') + quoted + QLatin1Char('\'');
    }

    //! The owner's prompt: Up / Down go through what you typed before.
    class ScriptPrompt : public QLineEdit
    {
    public:
        using QLineEdit::QLineEdit;
        std::function<void(int)> m_step;

    protected:
        void keyPressEvent(QKeyEvent* event) override
        {
            if ((event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) && m_step)
            {
                m_step(event->key() == Qt::Key_Up ? -1 : 1);
                return;
            }
            QLineEdit::keyPressEvent(event);
        }
    };
} // namespace

void HearthScriptConsole::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(120, 80, 1320, 820);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthScriptConsole>("Script console", "Tools", options);
}

HearthScriptConsole::HearthScriptConsole(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthScripts"));
    setStyleSheet(QStringLiteral("QWidget#HearthScripts { background-color: #0D0C0B; }"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);
    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Script console"), tr("Python in the editor · what Claude uses to drive it"));
    header->addStretch(1);
    m_claudeState = Hearth::MakeChip(QString(), QStringLiteral("ember"), this);
    header->addWidget(m_claudeState);
    m_pause = Hearth::MakeButton(tr("Pause Claude"), false, this);
    connect(m_pause, &QPushButton::clicked, this, [this]()
    {
        ScriptSet(QStringLiteral("claude/paused"), !ScriptSetting(QStringLiteral("claude/paused"), false));
        UpdatePause();
    });
    header->addWidget(m_pause);
    root->addLayout(header);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);

    // What was run, and the prompt.
    QVBoxLayout* consoleLayout = nullptr;
    QFrame* consoleCard = Hearth::MakeCard(this, &consoleLayout);
    m_scroll = new QScrollArea(consoleCard);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; } QScrollArea > QWidget > QWidget { background: transparent; }"));
    auto* holder = new QWidget(m_scroll);
    m_entries = new QVBoxLayout(holder);
    m_entries->setContentsMargins(0, 0, 0, 0);
    m_entries->setSpacing(10);
    m_entries->addStretch(1);
    m_scroll->setWidget(holder);
    consoleLayout->addWidget(m_scroll, 1);
    auto* promptRow = new QHBoxLayout();
    QLabel* chevrons = Hearth::MakeLabel(QStringLiteral(">>>"), QStringLiteral("mono"), consoleCard);
    chevrons->setStyleSheet(QStringLiteral("background: transparent; color: #E8743B; font-family: \"Geist Mono\";"));
    promptRow->addWidget(chevrons);
    auto* prompt = new ScriptPrompt(consoleCard);
    m_prompt = prompt;
    m_prompt->setPlaceholderText(tr("Python, run in the editor (general, editor, entity, bus are ready)"));
    m_prompt->setStyleSheet(QStringLiteral("QLineEdit { font-family: \"Geist Mono\"; background-color: #1C1917; border: 1px solid rgba(255, 255, 255, 15); border-radius: 8px; padding: 6px 10px; }"));
    prompt->m_step = [this](int direction)
    {
        if (m_history.isEmpty())
        {
            return;
        }
        m_historyAt = qBound(0, (m_historyAt < 0 ? m_history.size() : m_historyAt) + direction, m_history.size());
        m_prompt->setText(m_historyAt < m_history.size() ? m_history[m_historyAt] : QString());
    };
    connect(m_prompt, &QLineEdit::returnPressed, this, [this]()
    {
        const QString code = m_prompt->text();
        if (code.trimmed().isEmpty())
        {
            return;
        }
        m_history << code;
        m_historyAt = -1;
        m_prompt->clear();
        RunOwnerCode(code);
    });
    promptRow->addWidget(m_prompt, 1);
    consoleLayout->addLayout(promptRow);
    body->addWidget(consoleCard, 1);

    // Saved scripts and what Claude may do.
    auto* side = new QVBoxLayout();
    side->setSpacing(10);
    QVBoxLayout* scripts = nullptr;
    QFrame* scriptsCard = Hearth::MakeCard(this, &scripts);
    auto* scriptsTitle = new QHBoxLayout();
    scriptsTitle->addWidget(Hearth::MakeLabel(tr("Saved scripts"), QStringLiteral("title"), scriptsCard));
    scriptsTitle->addStretch(1);
    QPushButton* saveLast = Hearth::MakeButton(tr("Save the last command"), false, scriptsCard);
    connect(saveLast, &QPushButton::clicked, this, [this]()
    {
        if (m_history.isEmpty())
        {
            return;
        }
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Save script"), tr("Name"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || name.isEmpty())
        {
            return;
        }
        QString slug = name.toLower();
        slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
        slug = slug.mid(0, 60).remove(QRegularExpression(QStringLiteral("^-+|-+$")));
        QDir().mkpath(QDir(BridgeFolder()).filePath(QStringLiteral("scripts")));
        QSaveFile code(QDir(BridgeFolder()).filePath(QStringLiteral("scripts/%1.py").arg(slug)));
        if (code.open(QIODevice::WriteOnly))
        {
            code.write(m_history.last().toUtf8());
            code.commit();
        }
        QFile indexFile(QDir(BridgeFolder()).filePath(QStringLiteral("scripts/scripts.json")));
        QJsonObject index;
        if (indexFile.open(QIODevice::ReadOnly))
        {
            index = QJsonDocument::fromJson(indexFile.readAll()).object();
            indexFile.close();
        }
        QJsonObject entry = index.value(slug).toObject();
        entry.insert(QStringLiteral("title"), name);
        entry.insert(QStringLiteral("uses"), entry.value(QStringLiteral("uses")).toInt());
        index.insert(slug, entry);
        QSaveFile out(indexFile.fileName());
        if (out.open(QIODevice::WriteOnly))
        {
            out.write(QJsonDocument(index).toJson());
            out.commit();
        }
        FillScripts();
    });
    scriptsTitle->addWidget(saveLast);
    scripts->addLayout(scriptsTitle);
    auto* scriptsHolder = new QWidget(scriptsCard);
    m_scriptsLayout = new QVBoxLayout(scriptsHolder);
    m_scriptsLayout->setContentsMargins(0, 0, 0, 0);
    m_scriptsLayout->setSpacing(4);
    scripts->addWidget(scriptsHolder);
    side->addWidget(scriptsCard);

    QVBoxLayout* may = nullptr;
    QFrame* mayCard = Hearth::MakeCard(this, &may);
    may->addWidget(Hearth::MakeLabel(tr("Claude may"), QStringLiteral("title"), mayCard));
    may->addWidget(Hearth::MakeSettingRow(tr("Read the level"), QString(), Hearth::MakeLabel(tr("Always"), QStringLiteral("dim"), mayCard), mayCard));
    auto* change = new Hearth::Toggle(mayCard);
    change->setChecked(ScriptSetting(QStringLiteral("claude/editLevels"), true));
    connect(change, &QAbstractButton::toggled, this, [](bool on) { ScriptSet(QStringLiteral("claude/editLevels"), on); });
    may->addWidget(Hearth::MakeSettingRow(tr("Change and save it"), QString(), change, mayCard));
    auto* deletes = new QComboBox(mayCard);
    deletes->addItem(tr("Asks first"), true);
    deletes->addItem(tr("Without asking"), false);
    deletes->setCurrentIndex(ScriptSetting(QStringLiteral("claude/deleteAsks"), true) ? 0 : 1);
    connect(deletes, QOverload<int>::of(&QComboBox::activated), this, [deletes](int) { ScriptSet(QStringLiteral("claude/deleteAsks"), deletes->currentData().toBool()); });
    may->addWidget(Hearth::MakeSettingRow(tr("Delete things"), QString(), deletes, mayCard));
    auto* build = new Hearth::Toggle(mayCard);
    build->setChecked(ScriptSetting(QStringLiteral("claude/buildAndPlay"), true));
    connect(build, &QAbstractButton::toggled, this, [](bool on) { ScriptSet(QStringLiteral("claude/buildAndPlay"), on); });
    may->addWidget(Hearth::MakeSettingRow(tr("Build the game"), QString(), build, mayCard));
    side->addWidget(mayCard);
    side->addStretch(1);
    auto* sideHolder = new QWidget(this);
    sideHolder->setFixedWidth(400);
    sideHolder->setLayout(side);
    body->addWidget(sideHolder);
    root->addLayout(body, 1);

    m_timer = new QTimer(this);
    m_timer->setInterval(1500);
    connect(m_timer, &QTimer::timeout, this, [this]() { Refresh(false); });
}

QString HearthScriptConsole::BridgeFolder() const
{
    return QDir(QString::fromUtf8(AZ::Utils::GetProjectPath().c_str())).filePath(QStringLiteral("user/ClaudeBridge"));
}

void HearthScriptConsole::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    UpdatePause();
    FillScripts();
    Refresh(true);
    m_timer->start();
}

void HearthScriptConsole::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_timer->stop();
}

void HearthScriptConsole::UpdatePause()
{
    const bool paused = ScriptSetting(QStringLiteral("claude/paused"), false);
    m_pause->setText(paused ? tr("Let Claude carry on") : tr("Pause Claude"));
}

void HearthScriptConsole::Refresh(bool force)
{
    const QString claudePath = QDir(BridgeFolder()).filePath(QStringLiteral("activity.jsonl"));
    const QString ownerPath = QDir(BridgeFolder()).filePath(QStringLiteral("owner_console.jsonl"));
    const QDateTime newest = qMax(QFileInfo(claudePath).lastModified(), QFileInfo(ownerPath).lastModified());

    // Claude's state: commands in the last minute.
    const QVector<QJsonObject> claude = ScriptReadLines(claudePath, 80);
    const double now = QDateTime::currentMSecsSinceEpoch() / 1000.0;
    int lastMinute = 0;
    for (const QJsonObject& entry : claude)
    {
        lastMinute += now - entry.value(QStringLiteral("time")).toDouble() < 60.0 ? 1 : 0;
    }
    const bool paused = ScriptSetting(QStringLiteral("claude/paused"), false);
    m_claudeState->setText(paused ? tr("Claude is paused") : (lastMinute > 0 ? tr("Claude is working · %n command(s) this minute", nullptr, lastMinute) : tr("Claude is idle")));

    if (!force && newest == m_lastSeen)
    {
        return;
    }
    m_lastSeen = newest;
    QVector<QJsonObject> all = claude;
    for (QJsonObject entry : ScriptReadLines(ownerPath, 80))
    {
        entry.insert(QStringLiteral("who"), QStringLiteral("owner"));
        all << entry;
    }
    std::sort(all.begin(), all.end(), [](const QJsonObject& a, const QJsonObject& b) { return a.value(QStringLiteral("time")).toDouble() < b.value(QStringLiteral("time")).toDouble(); });
    all = all.mid(qMax(0, all.size() - 80));

    while (m_entries->count() > 1)
    {
        QLayoutItem* item = m_entries->takeAt(0);
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
    for (const QJsonObject& entry : all)
    {
        const bool owner = entry.value(QStringLiteral("who")).toString() == QLatin1String("owner");
        auto* row = new QWidget();
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(10);
        const QString me = QString::fromLocal8Bit(qgetenv("USER")).left(1).toUpper();
        auto* initial = new QLabel(owner ? (me.isEmpty() ? QStringLiteral("Y") : me) : QStringLiteral("C"), row);
        initial->setFixedSize(26, 26);
        initial->setAlignment(Qt::AlignCenter);
        initial->setStyleSheet(owner ? QStringLiteral("background-color: #24211E; color: #ECE6DE; border-radius: 13px; font-weight: 600;")
                                     : QStringLiteral("background-color: rgba(232, 116, 59, 51); color: #FFCFB4; border-radius: 13px; font-weight: 600;"));
        layout->addWidget(initial, 0, Qt::AlignTop);
        auto* texts = new QVBoxLayout();
        texts->setSpacing(3);
        auto* line = new QHBoxLayout();
        line->addWidget(Hearth::MakeLabel(owner ? tr("You") : tr("Claude"), QStringLiteral("body"), row));
        line->addWidget(Hearth::MakeLabel(QDateTime::fromMSecsSinceEpoch(qint64(entry.value(QStringLiteral("time")).toDouble() * 1000)).toString(QStringLiteral("HH:mm:ss")),
                                          QStringLiteral("small"), row));
        if (!entry.value(QStringLiteral("ok")).toBool(true))
        {
            line->addWidget(Hearth::MakeChip(tr("failed"), QStringLiteral("bad"), row));
        }
        line->addStretch(1);
        texts->addLayout(line);
        const QString code = entry.value(QStringLiteral("code")).toString(entry.value(QStringLiteral("what")).toString());
        QLabel* codeLabel = Hearth::MakeLabel(code, QStringLiteral("mono"), row);
        codeLabel->setWordWrap(true);
        codeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        codeLabel->setStyleSheet(QStringLiteral("background-color: #121110; color: #ECE6DE; border-radius: 8px; padding: 6px 8px; font-family: \"Geist Mono\"; font-size: 12px;"));
        texts->addWidget(codeLabel);
        const QString out = entry.value(QStringLiteral("out")).toString();
        if (!out.isEmpty())
        {
            QLabel* outLabel = Hearth::MakeLabel(out, QStringLiteral("mono"), row);
            outLabel->setWordWrap(true);
            outLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            outLabel->setStyleSheet(QStringLiteral("background: transparent; color: %1; font-family: \"Geist Mono\"; font-size: 11px;")
                                        .arg(entry.value(QStringLiteral("ok")).toBool(true) ? QStringLiteral("#8D857B") : QStringLiteral("#FFB38F")));
            texts->addWidget(outLabel);
        }
        layout->addLayout(texts, 1);
        m_entries->insertWidget(m_entries->count() - 1, row);
    }
    QTimer::singleShot(30, this, [this]() { m_scroll->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->maximum()); });
    FillScripts();
}

void HearthScriptConsole::FillScripts()
{
    while (QLayoutItem* item = m_scriptsLayout->takeAt(0))
    {
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        delete item;
    }
    QFile indexFile(QDir(BridgeFolder()).filePath(QStringLiteral("scripts/scripts.json")));
    QJsonObject index;
    if (indexFile.open(QIODevice::ReadOnly))
    {
        index = QJsonDocument::fromJson(indexFile.readAll()).object();
    }
    QStringList slugs = index.keys();
    std::sort(slugs.begin(), slugs.end(), [&index](const QString& a, const QString& b)
    {
        return index.value(a).toObject().value(QStringLiteral("uses")).toInt() > index.value(b).toObject().value(QStringLiteral("uses")).toInt();
    });
    for (const QString& slug : slugs)
    {
        const QJsonObject entry = index.value(slug).toObject();
        auto* row = new QPushButton(m_scriptsLayout->parentWidget());
        row->setCursor(Qt::PointingHandCursor);
        row->setToolTip(tr("Run it"));
        row->setMinimumHeight(44);
        row->setStyleSheet(QStringLiteral("QPushButton { text-align: left; background-color: rgba(255, 255, 255, 8); border: none; border-radius: 10px; }"
                                          "QPushButton:hover { background-color: rgba(232, 116, 59, 31); }"));
        auto* layout = new QVBoxLayout(row);
        layout->setContentsMargins(10, 6, 10, 6);
        layout->setSpacing(1);
        QLabel* title = Hearth::MakeLabel(entry.value(QStringLiteral("title")).toString(slug), QStringLiteral("body"), row);
        QLabel* uses = Hearth::MakeLabel(tr("Used %n time(s)", nullptr, entry.value(QStringLiteral("uses")).toInt()), QStringLiteral("small"), row);
        title->setAttribute(Qt::WA_TransparentForMouseEvents);
        uses->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(title);
        layout->addWidget(uses);
        connect(row, &QPushButton::clicked, this, [this, slug]()
        {
            QFile code(QDir(BridgeFolder()).filePath(QStringLiteral("scripts/%1.py").arg(slug)));
            if (code.open(QIODevice::ReadOnly))
            {
                RunOwnerCode(QString::fromUtf8(code.readAll()), slug);
            }
        });
        m_scriptsLayout->addWidget(row);
    }
    if (slugs.isEmpty())
    {
        m_scriptsLayout->addWidget(Hearth::MakeLabel(tr("None yet. Claude saves the ones it uses often; you can save your last command."), QStringLiteral("dim"),
                                                     m_scriptsLayout->parentWidget()));
    }
}

void HearthScriptConsole::RunOwnerCode(const QString& code, const QString& scriptSlug)
{
    // Run in the editor's Python with everything it prints caught in a file, like the Claude bridge does.
    QDir().mkpath(BridgeFolder());
    const QString outPath = QDir(BridgeFolder()).filePath(QStringLiteral("owner_console.out"));
    QFile::remove(outPath);
    const QString wrapped = QStringLiteral(
        "import sys, io, traceback\n"
        "_hearth_buffer = io.StringIO()\n"
        "_hearth_ok = True\n"
        "_hearth_out, _hearth_err = sys.stdout, sys.stderr\n"
        "sys.stdout = sys.stderr = _hearth_buffer\n"
        "try:\n"
        "    import azlmbr, azlmbr.bus as bus, azlmbr.editor as editor, azlmbr.entity as entity, azlmbr.legacy.general as general\n"
        "    exec(compile(%1, '<console>', 'exec'), globals())\n"
        "except Exception:\n"
        "    _hearth_ok = False\n"
        "    traceback.print_exc()\n"
        "finally:\n"
        "    sys.stdout, sys.stderr = _hearth_out, _hearth_err\n"
        "with open(%2, 'w', encoding='utf-8') as _hearth_file:\n"
        "    _hearth_file.write(('1' if _hearth_ok else '0') + _hearth_buffer.getvalue())\n")
                                .arg(ScriptQuote(code), ScriptQuote(outPath));
    AzToolsFramework::EditorPythonRunnerRequestBus::Broadcast(&AzToolsFramework::EditorPythonRunnerRequests::ExecuteByString,
                                                              AZStd::string_view(wrapped.toUtf8().constData()), false);
    QString output;
    bool ok = false;
    QFile result(outPath);
    if (result.open(QIODevice::ReadOnly))
    {
        const QString text = QString::fromUtf8(result.readAll());
        ok = text.startsWith(QLatin1Char('1'));
        output = text.mid(1).trimmed();
    }
    else
    {
        output = tr("Python isn't running in the editor (the Python scripting part may be off).");
    }
    QJsonObject entry{ { QStringLiteral("time"), QDateTime::currentMSecsSinceEpoch() / 1000.0 },
                       { QStringLiteral("who"), QStringLiteral("owner") },
                       { QStringLiteral("code"), code.left(2000) },
                       { QStringLiteral("out"), output.left(4000) },
                       { QStringLiteral("ok"), ok } };
    QFile log(QDir(BridgeFolder()).filePath(QStringLiteral("owner_console.jsonl")));
    if (log.open(QIODevice::Append | QIODevice::Text))
    {
        log.write(QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n');
    }
    if (!scriptSlug.isEmpty())
    {
        QFile indexFile(QDir(BridgeFolder()).filePath(QStringLiteral("scripts/scripts.json")));
        QJsonObject index;
        if (indexFile.open(QIODevice::ReadOnly))
        {
            index = QJsonDocument::fromJson(indexFile.readAll()).object();
            indexFile.close();
        }
        QJsonObject script = index.value(scriptSlug).toObject();
        script.insert(QStringLiteral("uses"), script.value(QStringLiteral("uses")).toInt() + 1);
        index.insert(scriptSlug, script);
        QSaveFile out(indexFile.fileName());
        if (out.open(QIODevice::WriteOnly))
        {
            out.write(QJsonDocument(index).toJson());
            out.commit();
        }
    }
    Refresh(true);
}
