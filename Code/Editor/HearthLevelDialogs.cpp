/*
 * Hearth Engine level dialogs. Built on O3DE (Apache-2.0 OR MIT). See HearthLevelDialogs.h.
 */

#include "EditorDefs.h"

#include "HearthLevelDialogs.h"
#include "CryEditDoc.h"
#include "Settings.h"

#include <AzCore/Interface/Interface.h>
#include <AzCore/JSON/stringbuffer.h>
#include <AzCore/JSON/writer.h>
#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Prefab/PrefabLoaderInterface.h>
#include <AzToolsFramework/Prefab/PrefabSystemComponentInterface.h>
#include <AzToolsFramework/Prefab/Template/Template.h>
#include <AzToolsFramework/UI/UICore/WidgetHelpers.h>

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSet>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;
    namespace Prefab = AzToolsFramework::Prefab;

    QString LevelProject()
    {
        return QString::fromUtf8(AZ::Utils::GetProjectPath().c_str());
    }

    QString LevelProjectName()
    {
        const auto name = AZ::Utils::GetProjectName();
        return QString::fromUtf8(name.c_str(), static_cast<int>(name.size()));
    }

    //! "Level0" -> "Level 0"
    QString LevelPretty(const QString& name)
    {
        QString text = name;
        text.replace(QRegularExpression(QStringLiteral("([a-z])([A-Z0-9])")), QStringLiteral("\\1 \\2"));
        text.replace(QLatin1Char('_'), QLatin1Char(' '));
        return text.simplified();
    }

    //! A level's own title from Levels/<name>/hearth_level.json ("The Lobby").
    QString LevelTitle(const QString& folder)
    {
        QFile file(QDir(folder).filePath(QStringLiteral("hearth_level.json")));
        if (file.open(QIODevice::ReadOnly))
        {
            return QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("title")).toString();
        }
        return QString();
    }

    QString LevelAgo(qint64 seconds)
    {
        const qint64 age = QDateTime::currentSecsSinceEpoch() - seconds;
        if (seconds <= 0) return QObject::tr("Not started");
        if (age < 120) return QObject::tr("Open now");
        if (age < 3600) return QObject::tr("Opened %n minute(s) ago", nullptr, int(age / 60));
        if (age < 86400) return QObject::tr("Opened %n hour(s) ago", nullptr, int(age / 3600));
        if (age < 86400 * 2) return QObject::tr("Opened yesterday");
        if (age < 86400 * 7) return QObject::tr("Opened %n day(s) ago", nullptr, int(age / 86400));
        if (age < 86400 * 14) return QObject::tr("Opened last week");
        return QObject::tr("Opened %1").arg(QDateTime::fromSecsSinceEpoch(seconds).toString(QStringLiteral("d MMM")));
    }

    QStringList LevelFolders()
    {
        QStringList levels;
        QDirIterator it(QDir(LevelProject()).filePath(QStringLiteral("Levels")), QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            const QString folder = it.next();
            if (QFileInfo::exists(QDir(folder).filePath(QFileInfo(folder).fileName() + QStringLiteral(".prefab"))))
            {
                levels << folder;
            }
        }
        levels.sort(Qt::CaseInsensitive);
        return levels;
    }

    QPushButton* LevelChoice(QWidget* parent, const QString& title, const QString& about)
    {
        auto* card = new QPushButton(parent);
        card->setCheckable(true);
        card->setMinimumHeight(64);
        card->setCursor(Qt::PointingHandCursor);
        card->setStyleSheet(QStringLiteral("QPushButton { background-color: rgba(255, 255, 255, 8); border: 1px solid rgba(255, 255, 255, 15); border-radius: 12px; }"
                                           "QPushButton:checked { background-color: rgba(232, 116, 59, 31); border-color: rgba(232, 116, 59, 140); }"
                                           "QPushButton:disabled { color: #5A544C; }"));
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(12, 8, 12, 8);
        QLabel* name = Hearth::MakeLabel(title, QStringLiteral("body"), card);
        QLabel* text = Hearth::MakeLabel(about, QStringLiteral("small"), card);
        name->setAttribute(Qt::WA_TransparentForMouseEvents);
        text->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(name);
        layout->addWidget(text);
        return card;
    }

    void LevelDialogLook(QDialog* dialog, const QString& title)
    {
        dialog->setWindowTitle(title);
        dialog->setObjectName(QStringLiteral("HearthLevelDialog"));
        dialog->setStyleSheet(QStringLiteral("QDialog#HearthLevelDialog { background-color: #171513; }"));
        dialog->setWindowIcon(Hearth::AppIcon());
    }

    // What changed since the level was saved, in words.
    void LevelFlatten(const QJsonValue& value, const QString& path, QMap<QString, QJsonValue>& out)
    {
        if (value.isObject())
        {
            const QJsonObject object = value.toObject();
            for (auto it = object.begin(); it != object.end(); ++it)
            {
                if (it.key() != QLatin1String("Id") && it.key() != QLatin1String("$type"))
                {
                    LevelFlatten(it.value(), path.isEmpty() ? it.key() : path + QLatin1Char('/') + it.key(), out);
                }
            }
        }
        else if (value.isArray() && !value.toArray().isEmpty() && !value.toArray().first().isObject())
        {
            out.insert(path, value);
        }
        else if (value.isArray())
        {
            const QJsonArray array = value.toArray();
            for (int i = 0; i < array.size(); ++i)
            {
                LevelFlatten(array[i], path + QStringLiteral("/%1").arg(i), out);
            }
        }
        else
        {
            out.insert(path, value);
        }
    }

    QString LevelSoundWords(const QString& hint)
    {
        // "assets/sounds/level0/level0_roomtone.wav.miniaudio" -> "room tone"
        QString stem = QFileInfo(hint).fileName().section(QLatin1Char('.'), 0, 0);
        stem.remove(QRegularExpression(QStringLiteral("^level\\d+_"), QRegularExpression::CaseInsensitiveOption));
        static const QMap<QString, QString> words = { { QStringLiteral("roomtone"), QStringLiteral("room tone") },
                                                      { QStringLiteral("footstep"), QStringLiteral("footstep") } };
        QStringList parts = stem.split(QLatin1Char('_'), Qt::SkipEmptyParts);
        for (QString& part : parts)
        {
            part = words.value(part.toLower(), part.toLower());
        }
        parts.removeAll(QString());
        return parts.join(QLatin1Char(' '));
    }

    QStringList LevelChanges(Prefab::TemplateId root)
    {
        auto* system = AZ::Interface<Prefab::PrefabSystemComponentInterface>::Get();
        auto* loader = AZ::Interface<Prefab::PrefabLoaderInterface>::Get();
        if (!system || !loader || root == Prefab::InvalidTemplateId)
        {
            return {};
        }
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        system->FindTemplateDom(root).Accept(writer);
        const QJsonObject now = QJsonDocument::fromJson(QByteArray(buffer.GetString(), int(buffer.GetSize()))).object();
        QJsonObject saved;
        auto templateRef = system->FindTemplate(root);
        if (templateRef.has_value())
        {
            QFile file(QString::fromUtf8(loader->GetFullPath(templateRef->get().GetFilePath()).c_str()));
            if (file.open(QIODevice::ReadOnly))
            {
                saved = QJsonDocument::fromJson(file.readAll()).object();
            }
        }
        QMap<QString, QJsonValue> before;
        QMap<QString, QJsonValue> after;
        LevelFlatten(saved.value(QStringLiteral("Entities")), QStringLiteral("E"), before);
        LevelFlatten(now.value(QStringLiteral("Entities")), QStringLiteral("E"), after);
        LevelFlatten(saved.value(QStringLiteral("Instances")), QStringLiteral("I"), before);
        LevelFlatten(now.value(QStringLiteral("Instances")), QStringLiteral("I"), after);
        QSet<QString> keys;
        for (auto it = before.begin(); it != before.end(); ++it)
        {
            keys.insert(it.key());
        }
        for (auto it = after.begin(); it != after.end(); ++it)
        {
            keys.insert(it.key());
        }
        const QJsonObject entitiesNow = now.value(QStringLiteral("Entities")).toObject();
        const QJsonObject entitiesThen = saved.value(QStringLiteral("Entities")).toObject();
        const QJsonObject instancesNow = now.value(QStringLiteral("Instances")).toObject();
        QStringList phrases;
        for (const QString& key : keys)
        {
            if (before.value(key) == after.value(key) || key.contains(QLatin1String("Child Entity Order")))
            {
                continue;
            }
            const QString kind = key.section(QLatin1Char('/'), 0, 0);
            const QString alias = key.section(QLatin1Char('/'), 1, 1);
            QString phrase;
            if (kind == QLatin1String("I"))
            {
                const QString source = instancesNow.value(alias).toObject().value(QStringLiteral("Source")).toString();
                phrase = QObject::tr("a %1").arg(QFileInfo(source).completeBaseName().isEmpty() ? QObject::tr("prefab") : QFileInfo(source).completeBaseName());
            }
            else
            {
                const QJsonObject entity = entitiesNow.contains(alias) ? entitiesNow.value(alias).toObject() : entitiesThen.value(alias).toObject();
                const QString name = entity.value(QStringLiteral("Name")).toString();
                const QJsonObject components = entity.value(QStringLiteral("Components")).toObject();
                const QString component = key.section(QLatin1Char('/'), 3, 3);
                const QString field = key.section(QLatin1Char('/'), -1);
                const bool isLight = QJsonDocument(components).toJson().contains("AreaLightComponent");
                if (!entitiesThen.contains(alias))
                {
                    phrase = QObject::tr("a new %1").arg(LevelPretty(name).toLower());
                }
                else if (!entitiesNow.contains(alias))
                {
                    phrase = QObject::tr("removed %1").arg(LevelPretty(name).toLower());
                }
                else if (key.contains(QLatin1String("Transform Data")))
                {
                    phrase = isLight ? QObject::tr("a light's position") : QObject::tr("%1's position").arg(LevelPretty(name).toLower());
                }
                else if (component.contains(QLatin1String("MiniAudio")) && field == QLatin1String("Volume"))
                {
                    const QString hint = components.value(component).toObject().value(QStringLiteral("Controller")).toObject().value(QStringLiteral("Config")).toObject()
                                             .value(QStringLiteral("Sound")).toObject().value(QStringLiteral("assetHint")).toString();
                    phrase = QObject::tr("the %1 volume").arg(LevelSoundWords(hint));
                }
                else
                {
                    phrase = QObject::tr("the %1").arg(LevelPretty(name).toLower());
                }
            }
            if (!phrases.contains(phrase))
            {
                phrases << phrase;
            }
        }
        // Several changes inside the same thing ("the footsteps") count once; a parent covers its children.
        return phrases;
    }

    QString LevelJoin(const QStringList& phrases)
    {
        if (phrases.size() <= 1)
        {
            return phrases.join(QString());
        }
        return QStringList(phrases.mid(0, phrases.size() - 1)).join(QStringLiteral(", ")) + QObject::tr(" and ") + phrases.last();
    }
} // namespace

HearthNewLevelDialog::HearthNewLevelDialog(QWidget* parent)
    : QDialog(parent)
{
    LevelDialogLook(this, tr("New level"));
    resize(620, 420);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(12);
    root->addWidget(Hearth::MakeLabel(tr("New level"), QStringLiteral("title"), this));
    root->addWidget(Hearth::MakeLabel(tr("Name"), QStringLiteral("small"), this));
    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(tr("Level1"));
    m_name->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[A-Za-z][A-Za-z0-9_]{0,40}")), m_name));
    root->addWidget(m_name);
    root->addWidget(Hearth::MakeLabel(tr("Start from"), QStringLiteral("small"), this));

    auto* grid = new QGridLayout();
    grid->setSpacing(8);
    // Empty: the engine's default level (a camera and a light).
    const QString engine = QString::fromUtf8(AZ::Utils::GetEnginePath().c_str());
    m_choices << LevelChoice(this, tr("Empty"), tr("Just a camera and light"));
    m_templates << QDir(engine).filePath(QStringLiteral("Assets/Editor/Prefabs/Default_Level.prefab"));
    // The project's own templates (Templates/Levels/templates.json).
    QFile list(QDir(LevelProject()).filePath(QStringLiteral("Templates/Levels/templates.json")));
    if (list.open(QIODevice::ReadOnly))
    {
        for (const QJsonValue& value : QJsonDocument::fromJson(list.readAll()).object().value(QStringLiteral("templates")).toArray())
        {
            const QJsonObject entry = value.toObject();
            const QString file = QDir(LevelProject()).filePath(QStringLiteral("Templates/Levels/") + entry.value(QStringLiteral("file")).toString());
            QPushButton* card = LevelChoice(this, entry.value(QStringLiteral("title")).toString(), entry.value(QStringLiteral("about")).toString());
            card->setEnabled(QFileInfo::exists(file));
            m_choices << card;
            m_templates << file;
        }
    }
    // Copy a level.
    const QStringList levels = LevelFolders();
    QPushButton* copy = LevelChoice(this, tr("Copy a level"), levels.isEmpty() ? tr("No levels yet") : tr("Start from %1").arg(LevelPretty(QFileInfo(levels.first()).fileName())));
    copy->setEnabled(!levels.isEmpty());
    m_choices << copy;
    m_templates << QString();
    for (int i = 0; i < m_choices.size(); ++i)
    {
        grid->addWidget(m_choices[i], i / 2, i % 2);
        connect(m_choices[i], &QPushButton::clicked, this, [this, i]()
        {
            m_picked = i;
            for (int j = 0; j < m_choices.size(); ++j)
            {
                m_choices[j]->setChecked(j == i);
            }
            m_copyFrom->setVisible(m_templates[i].isEmpty());
        });
    }
    m_choices.first()->setChecked(true);
    root->addLayout(grid);
    m_copyFrom = new QComboBox(this);
    for (const QString& folder : levels)
    {
        const QString name = QFileInfo(folder).fileName();
        m_copyFrom->addItem(LevelPretty(name), QDir(folder).filePath(name + QStringLiteral(".prefab")));
    }
    m_copyFrom->setVisible(false);
    connect(m_copyFrom, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, copy]()
    {
        copy->findChildren<QLabel*>().last()->setText(tr("Start from %1").arg(m_copyFrom->currentText()));
    });
    root->addWidget(m_copyFrom);
    root->addStretch(1);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    QPushButton* cancel = Hearth::MakeButton(tr("Cancel"), false, this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    m_create = Hearth::MakeButton(tr("Create level"), true, this);
    m_create->setEnabled(false);
    connect(m_create, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_name, &QLineEdit::textChanged, this, [this](const QString& text)
    {
        const bool taken = QFileInfo::exists(QDir(GetLevelsFolder()).filePath(text));
        m_create->setEnabled(!text.isEmpty() && !taken);
        m_create->setToolTip(taken ? tr("There's already a level called %1").arg(text) : QString());
    });
    buttons->addWidget(cancel);
    buttons->addWidget(m_create);
    root->addLayout(buttons);
    m_name->setFocus();
}

QString HearthNewLevelDialog::GetLevel() const
{
    return m_name->text().trimmed();
}

QString HearthNewLevelDialog::GetTemplateName() const
{
    const QString file = m_templates.value(m_picked);
    return file.isEmpty() ? m_copyFrom->currentData().toString() : file;
}

QString HearthNewLevelDialog::GetLevelsFolder() const
{
    return QDir(LevelProject()).filePath(QStringLiteral("Levels"));
}

HearthOpenLevelDialog::HearthOpenLevelDialog(QWidget* parent)
    : QDialog(parent)
{
    LevelDialogLook(this, tr("Open level"));
    resize(560, 440);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(12);
    root->addWidget(Hearth::MakeLabel(tr("Open level"), QStringLiteral("title"), this));
    m_levels = new QListWidget(this);
    m_levels->setFrameShape(QFrame::NoFrame);
    m_levels->setStyleSheet(QStringLiteral("QListWidget { background: transparent; outline: none; } QListWidget::item { border-radius: 10px; }"
                                           "QListWidget::item:selected { background-color: rgba(232, 116, 59, 31); }"));
    QScopedPointer<QSettings> settings(Hearth::NewSettings());
    struct Entry
    {
        QString folder;
        qint64 opened;
    };
    QVector<Entry> entries;
    for (const QString& folder : LevelFolders())
    {
        const QString name = QFileInfo(folder).fileName();
        entries.append({ folder, settings->value(QStringLiteral("levels/%1/%2").arg(LevelProjectName(), name)).toLongLong() });
    }
    // The ones you opened most recently first.
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.opened > b.opened; });
    for (const Entry& entry : entries)
    {
        const QString name = QFileInfo(entry.folder).fileName();
        const QString title = LevelTitle(entry.folder);
        auto* row = new QWidget();
        auto* layout = new QVBoxLayout(row);
        layout->setContentsMargins(12, 8, 12, 8);
        layout->setSpacing(2);
        layout->addWidget(Hearth::MakeLabel(title.isEmpty() ? LevelPretty(name) : tr("%1 · %2").arg(LevelPretty(name), title), QStringLiteral("body"), row));
        layout->addWidget(Hearth::MakeLabel(LevelAgo(entry.opened), QStringLiteral("small"), row));
        auto* item = new QListWidgetItem(m_levels);
        item->setSizeHint(QSize(10, 54));
        item->setData(Qt::UserRole, QDir(entry.folder).filePath(name + QStringLiteral(".prefab")));
        m_levels->setItemWidget(item, row);
    }
    if (m_levels->count() > 0)
    {
        m_levels->setCurrentRow(0);
    }
    root->addWidget(m_levels, 1);
    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    QPushButton* cancel = Hearth::MakeButton(tr("Cancel"), false, this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    QPushButton* open = Hearth::MakeButton(tr("Open"), true, this);
    open->setEnabled(m_levels->count() > 0);
    connect(open, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_levels, &QListWidget::itemDoubleClicked, this, &QDialog::accept);
    buttons->addWidget(cancel);
    buttons->addWidget(open);
    root->addLayout(buttons);
}

QString HearthOpenLevelDialog::GetFileName() const
{
    return m_levels->currentItem() ? m_levels->currentItem()->data(Qt::UserRole).toString() : QString();
}

int HearthAskToSaveLevel(AZ::u64 rootTemplateId)
{
    const QString level = GetIEditor() ? GetIEditor()->GetLevelName() : QString();
    QDialog dialog(AzToolsFramework::GetActiveWindow());
    LevelDialogLook(&dialog, QObject::tr("Save"));
    dialog.resize(520, 240);
    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(10);
    root->addWidget(Hearth::MakeLabel(QObject::tr("Save %1 first?").arg(level.isEmpty() ? QObject::tr("the level") : LevelPretty(level)), QStringLiteral("title"), &dialog));
    const QStringList changes = LevelChanges(Prefab::TemplateId(rootTemplateId));
    QString what;
    if (changes.isEmpty())
    {
        what = QObject::tr("You changed things since you last saved.");
    }
    else if (changes.size() <= 4)
    {
        what = QObject::tr("You changed %n thing(s) since you last saved: %1.", nullptr, int(changes.size())).arg(LevelJoin(changes));
    }
    else
    {
        what = QObject::tr("You changed %n things since you last saved, like %1.", nullptr, int(changes.size())).arg(LevelJoin(changes.mid(0, 3)));
    }
    QLabel* whatLabel = Hearth::MakeLabel(what, QStringLiteral("body"), &dialog);
    whatLabel->setWordWrap(true);
    root->addWidget(whatLabel);
    const QString backup = gSettings.autoBackupEnabled
        ? QObject::tr("Hearth Engine keeps a backup every %n minute(s), so nothing is lost if it closes.", nullptr, qMax(1, gSettings.autoBackupTime))
        : QObject::tr("Backups are off (Hearth Settings › Editor), so unsaved changes are lost if it closes.");
    QLabel* backupLabel = Hearth::MakeLabel(backup, QStringLiteral("small"), &dialog);
    backupLabel->setWordWrap(true);
    root->addWidget(backupLabel);
    root->addStretch(1);
    auto* buttons = new QHBoxLayout();
    QPushButton* cancel = Hearth::MakeButton(QObject::tr("Cancel"), false, &dialog);
    buttons->addWidget(cancel);
    buttons->addStretch(1);
    QPushButton* discard = Hearth::MakeButton(QObject::tr("Don't save"), false, &dialog);
    QPushButton* save = Hearth::MakeButton(QObject::tr("Save"), true, &dialog);
    save->setDefault(true);
    buttons->addWidget(discard);
    buttons->addWidget(save);
    root->addLayout(buttons);
    int answer = 0;
    QObject::connect(cancel, &QPushButton::clicked, &dialog, [&dialog, &answer]() { answer = 0; dialog.reject(); });
    QObject::connect(discard, &QPushButton::clicked, &dialog, [&dialog, &answer]() { answer = 2; dialog.accept(); });
    QObject::connect(save, &QPushButton::clicked, &dialog, [&dialog, &answer, rootTemplateId]()
    {
        // Every changed prefab the level uses, and the level.
        if (auto* system = AZ::Interface<Prefab::PrefabSystemComponentInterface>::Get())
        {
            system->SaveAllDirtyTemplates(Prefab::TemplateId(rootTemplateId));
        }
        if (GetIEditor() && GetIEditor()->GetDocument())
        {
            GetIEditor()->GetDocument()->SetModifiedFlag(false);
        }
        answer = 1;
        dialog.accept();
    });
    dialog.exec();
    return answer;
}
