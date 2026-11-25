/*
 * Hearth Engine asset browser. Built on O3DE (Apache-2.0 OR MIT). See HearthAssetBrowser.h.
 */

#include "EditorDefs.h"

#include "HearthAssetBrowser.h"
#include "HearthImportPane.h"
#include "HearthMaterialPane.h"
#include "CryEditDoc.h"
#include "QtViewPaneManager.h"

#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/Entity.h>
#include <AzCore/Interface/Interface.h>
#include <AzCore/Serialization/Json/JsonSerialization.h>
#include <AzCore/Serialization/Json/JsonUtils.h>
#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>
#include <AzToolsFramework/ActionManager/Action/ActionManagerInterface.h>
#include <AzToolsFramework/AssetBrowser/AssetBrowserBus.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>
#include <AzToolsFramework/Prefab/Instance/Instance.h>
#include <AzToolsFramework/Prefab/Instance/InstanceEntityMapperInterface.h>

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMenu>
#include <QMimeData>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    constexpr int BrowserIndexRole = Qt::UserRole + 1;
    constexpr int FolderAreaRole = Qt::UserRole + 2;
    constexpr int FolderKindRole = Qt::UserRole + 3;

    QString BrowserKindFor(const QString& suffix)
    {
        const QString s = suffix.toLower();
        if (s == QLatin1String("material"))
        {
            return QStringLiteral("MATERIAL");
        }
        if (s == QLatin1String("png") || s == QLatin1String("jpg") || s == QLatin1String("jpeg") || s == QLatin1String("tga")
            || s == QLatin1String("tif") || s == QLatin1String("tiff") || s == QLatin1String("dds") || s == QLatin1String("exr")
            || s == QLatin1String("bmp") || s == QLatin1String("hdr"))
        {
            return QStringLiteral("TEXTURE");
        }
        if (s == QLatin1String("fbx") || s == QLatin1String("gltf") || s == QLatin1String("glb") || s == QLatin1String("obj"))
        {
            return QStringLiteral("MODEL");
        }
        if (s == QLatin1String("wav") || s == QLatin1String("ogg") || s == QLatin1String("mp3") || s == QLatin1String("flac"))
        {
            return QStringLiteral("SOUND");
        }
        if (s == QLatin1String("prefab"))
        {
            return QStringLiteral("PREFAB");
        }
        return QString();
    }

    //! The folder for each kind (used for the area's children and for the type chips).
    QString BrowserKindPlural(const QString& kind)
    {
        if (kind == QLatin1String("MATERIAL")) return QObject::tr("Materials");
        if (kind == QLatin1String("TEXTURE")) return QObject::tr("Textures");
        if (kind == QLatin1String("MODEL")) return QObject::tr("Models");
        if (kind == QLatin1String("SOUND")) return QObject::tr("Sounds");
        if (kind == QLatin1String("PREFAB")) return QObject::tr("Prefabs");
        return kind;
    }

    const QStringList& BrowserKinds()
    {
        static const QStringList kinds = { QStringLiteral("MATERIAL"), QStringLiteral("TEXTURE"), QStringLiteral("MODEL"),
                                           QStringLiteral("SOUND"), QStringLiteral("PREFAB") };
        return kinds;
    }

    //! "Level0" -> "Level 0", "FoundationSite" -> "Foundation site", "foundation_site" -> "Foundation site".
    QString BrowserAreaTitle(const QString& area)
    {
        QString text = area;
        text.replace(QLatin1Char('_'), QLatin1Char(' ')).replace(QLatin1Char('-'), QLatin1Char(' '));
        text.replace(QRegularExpression(QStringLiteral("([a-z])([A-Z0-9])")), QStringLiteral("\\1 \\2"));
        text.replace(QRegularExpression(QStringLiteral("([A-Za-z])([0-9])")), QStringLiteral("\\1 \\2"));
        text = text.simplified().toLower();
        if (!text.isEmpty())
        {
            text[0] = text[0].toUpper();
        }
        return text;
    }

    QString BrowserAgo(const QDateTime& when)
    {
        const qint64 seconds = when.secsTo(QDateTime::currentDateTime());
        if (seconds < 90) return QObject::tr("just now");
        if (seconds < 3600) return QObject::tr("%n minute(s) ago", nullptr, int(seconds / 60));
        if (seconds < 86400) return QObject::tr("%n hour(s) ago", nullptr, int(seconds / 3600));
        if (seconds < 86400 * 2) return QObject::tr("yesterday");
        if (seconds < 86400 * 60) return QObject::tr("%n day(s) ago", nullptr, int(seconds / 86400));
        return when.toString(QStringLiteral("d MMM yyyy"));
    }

    QJsonObject BrowserReadJson(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return {};
        }
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    //! A .material file's values, in either layout ("propertyValues" with "group.name" keys, or nested "properties").
    QVariantMap BrowserMaterialValues(const QJsonObject& material)
    {
        QVariantMap values;
        const QJsonObject flat = material.value(QStringLiteral("propertyValues")).toObject();
        for (auto it = flat.begin(); it != flat.end(); ++it)
        {
            values.insert(it.key(), it.value().toVariant());
        }
        const QJsonObject nested = material.value(QStringLiteral("properties")).toObject();
        for (auto group = nested.begin(); group != nested.end(); ++group)
        {
            const QJsonObject inner = group.value().toObject();
            for (auto it = inner.begin(); it != inner.end(); ++it)
            {
                values.insert(group.key() + QLatin1Char('.') + it.key(), it.value().toVariant());
            }
        }
        return values;
    }

    QStringList BrowserMaterialTextures(const QVariantMap& values)
    {
        QStringList textures;
        for (auto it = values.begin(); it != values.end(); ++it)
        {
            if (it.value().typeId() == QMetaType::QString && BrowserKindFor(QFileInfo(it.value().toString()).suffix()) == QLatin1String("TEXTURE"))
            {
                textures << it.value().toString();
            }
        }
        textures.removeDuplicates();
        return textures;
    }

    //! A texture path from a .material: relative to the material, to the project, or to the project's Assets folder.
    QString BrowserResolveTexture(const QString& texture, const QString& materialPath, const QString& projectRoot)
    {
        const QStringList tries = { QFileInfo(materialPath).dir().filePath(texture), QDir(projectRoot).filePath(texture),
                                    QDir(projectRoot).filePath(QStringLiteral("Assets/") + texture) };
        for (const QString& path : tries)
        {
            if (QFileInfo::exists(path))
            {
                return QDir::cleanPath(path);
            }
        }
        // Texture paths in O3DE are matched case-insensitively.
        const QString wanted = QFileInfo(texture).fileName().toLower();
        QDirIterator it(QDir(projectRoot).filePath(QStringLiteral("Assets")), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            const QString path = it.next();
            if (QFileInfo(path).fileName().toLower() == wanted)
            {
                return path;
            }
        }
        return QString();
    }

    //! Length of a .wav file in seconds (0 when it isn't a plain PCM wav).
    double BrowserWavSeconds(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return 0.0;
        }
        const QByteArray head = file.read(4096);
        if (!head.startsWith("RIFF") || head.mid(8, 4) != "WAVE")
        {
            return 0.0;
        }
        quint32 byteRate = 0;
        int at = 12;
        while (at + 8 <= head.size())
        {
            const QByteArray id = head.mid(at, 4);
            const quint32 size = quint32(uchar(head[at + 4])) | (quint32(uchar(head[at + 5])) << 8) | (quint32(uchar(head[at + 6])) << 16)
                | (quint32(uchar(head[at + 7])) << 24);
            if (id == "fmt " && at + 20 <= head.size())
            {
                byteRate = quint32(uchar(head[at + 16])) | (quint32(uchar(head[at + 17])) << 8) | (quint32(uchar(head[at + 18])) << 16)
                    | (quint32(uchar(head[at + 19])) << 24);
            }
            if (id == "data")
            {
                return byteRate ? double(size) / double(byteRate) : 0.0;
            }
            at += 8 + int(size) + int(size & 1);
        }
        return 0.0;
    }

    //! The words a file is referred to by inside prefabs and materials ("/level0_wallpaper." or "\"level0_wallpaper.").
    QStringList BrowserNeedles(const HearthAssetFile& file)
    {
        const QString stem = file.name.toLower();
        return { QLatin1Char('/') + stem + QLatin1Char('.'), QLatin1Char('"') + stem + QLatin1Char('.') };
    }

    //! Swatch colours per kind, as in the mockup.
    QBrush BrowserKindBrush(const QString& kind, const QRectF& rect)
    {
        QLinearGradient gradient(rect.topLeft(), rect.bottomRight());
        if (kind == QLatin1String("MATERIAL"))
        {
            gradient.setColorAt(0, QColor(0x5a, 0x54, 0x44));
            gradient.setColorAt(1, QColor(0x2a, 0x26, 0x20));
        }
        else
        {
            gradient.setColorAt(0, kind == QLatin1String("MODEL") ? QColor(0x3a, 0x35, 0x30) : QColor(0x2c, 0x28, 0x24));
            gradient.setColorAt(1, kind == QLatin1String("MODEL") ? QColor(0x24, 0x21, 0x1e) : QColor(0x1f, 0x1c, 0x19));
        }
        return gradient;
    }

    void BrowserPaintSwatch(QPainter& painter, const QRectF& rect, const HearthAssetFile& file, const QPixmap& thumb, bool failed)
    {
        painter.save();
        painter.setClipRect(rect);
        if (failed)
        {
            QLinearGradient gradient(rect.topLeft(), rect.bottomRight());
            gradient.setColorAt(0, QColor(0x3a, 0x1d, 0x14));
            gradient.setColorAt(1, QColor(0x24, 0x21, 0x1e));
            painter.fillRect(rect, gradient);
        }
        else if (!thumb.isNull())
        {
            const QPixmap scaled = thumb.scaled(rect.size().toSize() * painter.device()->devicePixelRatioF(), Qt::KeepAspectRatioByExpanding,
                                                Qt::SmoothTransformation);
            const QSizeF size = QSizeF(scaled.size()) / painter.device()->devicePixelRatioF();
            painter.drawPixmap(QRectF(rect.center() - QPointF(size.width() / 2, size.height() / 2), size), scaled, scaled.rect());
        }
        else if (file.kind == QLatin1String("SOUND"))
        {
            painter.fillRect(rect, QColor(0x3a, 0x35, 0x30));
            // A little waveform drawn from the file's own bytes, so each sound looks different.
            const uint seed = qHash(file.relative);
            painter.setPen(Qt::NoPen);
            painter.setBrush(Hearth::Ember());
            uint step = 0;
            for (qreal x = rect.left() + 6; x < rect.right() - 6; x += 4, ++step)
            {
                const uint noise = (seed ^ (step * 2654435761u)) * 2246822519u;
                const qreal h = rect.height() * (0.15 + 0.6 * double((noise >> 8) % 97) / 97.0);
                painter.drawRoundedRect(QRectF(x, rect.center().y() - h / 2, 2, h), 1, 1);
            }
        }
        else
        {
            painter.fillRect(rect, BrowserKindBrush(file.kind, rect));
            if (file.kind == QLatin1String("PREFAB") || file.kind == QLatin1String("MODEL"))
            {
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(QPen(QColor(255, 255, 255, 40), 1.4));
                painter.setBrush(Qt::NoBrush);
                const QPointF c = rect.center();
                const qreal s = qMin(rect.width(), rect.height()) * 0.18;
                QPolygonF top({ c + QPointF(0, -s), c + QPointF(s, -s / 2), c, c + QPointF(-s, -s / 2) });
                painter.drawPolygon(top);
                painter.drawLine(c + QPointF(-s, -s / 2), c + QPointF(-s, s / 2));
                painter.drawLine(c + QPointF(s, -s / 2), c + QPointF(s, s / 2));
                painter.drawLine(c, c + QPointF(0, s));
                painter.drawLine(c + QPointF(-s, s / 2), c + QPointF(0, s));
                painter.drawLine(c + QPointF(s, s / 2), c + QPointF(0, s));
            }
        }
        painter.restore();
    }

    void BrowserPaintKindPill(QPainter& painter, const QRectF& swatch, const QString& kind)
    {
        QFont font = painter.font();
        font.setPixelSize(10);
        font.setWeight(QFont::DemiBold);
        font.setLetterSpacing(QFont::PercentageSpacing, 104);
        painter.setFont(font);
        const qreal w = QFontMetricsF(font).horizontalAdvance(kind) + 14;
        const QRectF pill(swatch.right() - 8 - w, swatch.bottom() - 8 - 18, w, 18);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(13, 12, 11, 191));
        painter.drawRoundedRect(pill, 9, 9);
        painter.setPen(Hearth::Text());
        painter.drawText(pill, Qt::AlignCenter, kind);
    }

    class HearthAssetTileDelegate : public QStyledItemDelegate
    {
    public:
        HearthAssetTileDelegate(QObject* parent, std::function<const HearthAssetFile*(int)> file, std::function<QPixmap(int)> thumb,
                                std::function<QString(int)> meta, std::function<bool(int)> failed, std::function<bool()> grid)
            : QStyledItemDelegate(parent)
            , m_file(std::move(file))
            , m_thumb(std::move(thumb))
            , m_meta(std::move(meta))
            , m_failed(std::move(failed))
            , m_grid(std::move(grid))
        {
        }

        QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override
        {
            return m_grid() ? QSize(168, 162) : QSize(option.rect.width() > 0 ? option.rect.width() : 400, 44);
        }

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            const int i = index.data(BrowserIndexRole).toInt();
            const HearthAssetFile* file = m_file(i);
            if (!file)
            {
                return;
            }
            const bool failed = m_failed(i);
            const bool selected = option.state & QStyle::State_Selected;
            const bool hover = option.state & QStyle::State_MouseOver;
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            QFont nameFont = option.font;
            nameFont.setPixelSize(13);
            nameFont.setWeight(QFont::Medium);
            QFont metaFont = option.font;
            metaFont.setPixelSize(11);
            const QColor border = selected ? Hearth::Ember() : (failed ? QColor(255, 122, 102, 128) : QColor(255, 255, 255, hover ? 30 : 15));
            if (m_grid())
            {
                const QRectF card = QRectF(option.rect).adjusted(5, 5, -5, -5);
                QPainterPath path;
                path.addRoundedRect(card, 12, 12);
                painter->fillPath(path, QColor(0x1f, 0x1c, 0x19));
                painter->save();
                painter->setClipPath(path);
                const QRectF swatch(card.left(), card.top(), card.width(), 104);
                BrowserPaintSwatch(*painter, swatch, *file, m_thumb(i), failed);
                BrowserPaintKindPill(*painter, swatch, file->kind);
                painter->restore();
                painter->setPen(QPen(border, selected ? 1.5 : 1.0));
                painter->setBrush(Qt::NoBrush);
                painter->drawPath(path);
                const QRectF text = QRectF(card.left() + 10, swatch.bottom() + 8, card.width() - 20, 18);
                painter->setFont(nameFont);
                painter->setPen(Hearth::Text());
                painter->drawText(text, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(nameFont).elidedText(file->name, Qt::ElideRight, int(text.width())));
                painter->setFont(metaFont);
                painter->setPen(failed ? QColor(0xff, 0xb3, 0x8f) : Hearth::Dim());
                painter->drawText(text.translated(0, 17), Qt::AlignLeft | Qt::AlignVCenter,
                                  QFontMetrics(metaFont).elidedText(m_meta(i), Qt::ElideRight, int(text.width())));
            }
            else
            {
                const QRectF row = QRectF(option.rect).adjusted(2, 2, -2, -2);
                if (selected || hover)
                {
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(selected ? QColor(232, 116, 59, 31) : QColor(255, 255, 255, 8));
                    painter->drawRoundedRect(row, 8, 8);
                }
                QPainterPath swatchPath;
                const QRectF swatch(row.left() + 6, row.top() + 5, 30, row.height() - 10);
                swatchPath.addRoundedRect(swatch, 6, 6);
                painter->save();
                painter->setClipPath(swatchPath);
                BrowserPaintSwatch(*painter, swatch, *file, m_thumb(i), failed);
                painter->restore();
                painter->setFont(nameFont);
                painter->setPen(Hearth::Text());
                painter->drawText(QRectF(swatch.right() + 12, row.top(), row.width() * 0.4, row.height()), Qt::AlignVCenter | Qt::AlignLeft, file->name);
                painter->setFont(metaFont);
                painter->setPen(failed ? QColor(0xff, 0xb3, 0x8f) : Hearth::Dim());
                painter->drawText(QRectF(row.left() + row.width() * 0.45, row.top(), row.width() * 0.35, row.height()), Qt::AlignVCenter | Qt::AlignLeft, m_meta(i));
                painter->drawText(QRectF(row.right() - 110, row.top(), 100, row.height()), Qt::AlignVCenter | Qt::AlignRight, file->kind);
            }
            painter->restore();
        }

    private:
        std::function<const HearthAssetFile*(int)> m_file;
        std::function<QPixmap(int)> m_thumb;
        std::function<QString(int)> m_meta;
        std::function<bool(int)> m_failed;
        std::function<bool()> m_grid;
    };
} // namespace

void HearthAssetBrowser::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(90, 60, 1440, 900);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthAssetBrowser>("Browse assets", "Tools", options);
}

HearthAssetBrowser::HearthAssetBrowser(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthAssetBrowser"));
    setStyleSheet(QStringLiteral("QWidget#HearthAssetBrowser { background-color: #0D0C0B; }"));
    setAcceptDrops(true);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    // Header: flame, title, search, Import, New asset.
    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Assets"), tr("Looking for assets…"));
    if (auto* titles = header->itemAt(1) ? header->itemAt(1)->layout() : nullptr)
    {
        if (titles->count() > 1)
        {
            m_subtitle = qobject_cast<QLabel*>(titles->itemAt(1)->widget());
        }
    }
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search assets"));
    m_search->setClearButtonEnabled(true);
    m_search->setFixedWidth(320);
    m_search->setStyleSheet(QStringLiteral("QLineEdit { background-color: #1C1917; border: 1px solid rgba(255, 255, 255, 15); border-radius: 15px; padding: 5px 12px; color: #ECE6DE; }"));
    connect(m_search, &QLineEdit::textChanged, this, &HearthAssetBrowser::FillTiles);
    header->addStretch(1);
    header->addWidget(m_search);
    QPushButton* import = Hearth::MakeButton(tr("Import"), false, this);
    connect(import, &QPushButton::clicked, this, []() { QtViewPaneManager::instance()->OpenPane(QStringLiteral("Import")); });
    header->addWidget(import);
    QPushButton* newAsset = Hearth::MakeButton(tr("New asset"), true, this);
    connect(newAsset, &QPushButton::clicked, this, &HearthAssetBrowser::NewAsset);
    header->addWidget(newAsset);
    root->addLayout(header);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);

    // Folders.
    QVBoxLayout* foldersLayout = nullptr;
    QFrame* foldersCard = Hearth::MakeCard(this, &foldersLayout);
    foldersCard->setFixedWidth(240);
    foldersLayout->addWidget(Hearth::MakeLabel(tr("Folders"), QStringLiteral("small"), foldersCard));
    m_folders = new QTreeWidget(foldersCard);
    m_folders->setHeaderHidden(true);
    m_folders->setColumnCount(2);
    m_folders->setRootIsDecorated(false);
    m_folders->setIndentation(14);
    m_folders->header()->setStretchLastSection(false);
    m_folders->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_folders->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_folders->setStyleSheet(QStringLiteral("QTreeWidget { background: transparent; border: none; outline: none; color: #ECE6DE; }"
                                            "QTreeWidget::item { padding: 5px 4px; border-radius: 7px; }"
                                            "QTreeWidget::item:selected { background-color: rgba(232, 116, 59, 41); color: #FFCFB4; }"
                                            "QTreeWidget::item:hover:!selected { background-color: rgba(255, 255, 255, 8); }"));
    connect(m_folders, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item)
    {
        if (!item)
        {
            return;
        }
        m_folderArea = item->data(0, FolderAreaRole).toString();
        m_folderKind = item->data(0, FolderKindRole).toString();
        FillTiles();
    });
    foldersLayout->addWidget(m_folders, 1);
    body->addWidget(foldersCard);

    // Tiles.
    auto* middle = new QVBoxLayout();
    middle->setSpacing(8);
    auto* chipsRow = new QHBoxLayout();
    chipsRow->setSpacing(6);
    const QString chipStyle = QStringLiteral("QPushButton { background-color: rgba(255, 255, 255, 10); border: 1px solid rgba(255, 255, 255, 15); border-radius: 13px; padding: 4px 12px; color: #ECE6DE; }"
                                             "QPushButton:checked { background-color: #ECE6DE; color: #1A0F08; border-color: #ECE6DE; }");
    const QStringList chipKinds = { QString(), QStringLiteral("MATERIAL"), QStringLiteral("TEXTURE"), QStringLiteral("MODEL"), QStringLiteral("SOUND"), QStringLiteral("PREFAB") };
    for (const QString& kind : chipKinds)
    {
        auto* chip = new QPushButton(kind.isEmpty() ? tr("All") : BrowserKindPlural(kind), this);
        chip->setCheckable(true);
        chip->setChecked(kind.isEmpty());
        chip->setStyleSheet(chipStyle);
        chip->setCursor(Qt::PointingHandCursor);
        connect(chip, &QPushButton::clicked, this, [this, chip, kind]()
        {
            for (QPushButton* other : m_chips)
            {
                other->setChecked(other == chip);
            }
            m_typeFilter = kind;
            FillTiles();
        });
        m_chips << chip;
        chipsRow->addWidget(chip);
    }
    chipsRow->addStretch(1);
    const QString viewStyle = QStringLiteral("QPushButton { background: transparent; border: none; border-radius: 7px; padding: 4px; }"
                                             "QPushButton:checked { background-color: rgba(255, 255, 255, 20); }");
    auto viewIcon = [](bool grid)
    {
        QPixmap pixmap(32, 32);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(Hearth::Text());
        if (grid)
        {
            for (int x : { 4, 18 })
            {
                for (int y : { 4, 18 })
                {
                    p.drawRoundedRect(QRectF(x, y, 10, 10), 2, 2);
                }
            }
        }
        else
        {
            for (int y : { 6, 14, 22 })
            {
                p.drawRoundedRect(QRectF(4, y, 24, 4), 2, 2);
            }
        }
        return QIcon(pixmap);
    };
    m_gridButton = new QPushButton(viewIcon(true), QString(), this);
    m_listButton = new QPushButton(viewIcon(false), QString(), this);
    for (QPushButton* button : { m_gridButton, m_listButton })
    {
        button->setCheckable(true);
        button->setIconSize(QSize(16, 16));
        button->setStyleSheet(viewStyle);
        chipsRow->addWidget(button);
    }
    m_gridButton->setToolTip(tr("Grid view"));
    m_listButton->setToolTip(tr("List view"));
    m_gridButton->setChecked(true);
    middle->addLayout(chipsRow);

    m_tiles = new QListWidget(this);
    m_tiles->setFrameShape(QFrame::NoFrame);
    m_tiles->setMouseTracking(true);
    m_tiles->setUniformItemSizes(true);
    m_tiles->setStyleSheet(QStringLiteral("QListWidget { background: transparent; outline: none; } QListWidget::item { background: transparent; border: none; }"));
    m_tiles->setItemDelegate(new HearthAssetTileDelegate(
        m_tiles, [this](int i) { return i >= 0 && i < m_files.size() ? &m_files[i] : nullptr; }, [this](int i) { return Thumbnail(i); },
        [this](int i) { return Meta(m_files[i]); }, [this](int i) { return m_failed.contains(m_files[i].path); }, [this]() { return m_grid; }));
    auto setGrid = [this](bool grid)
    {
        m_grid = grid;
        m_gridButton->setChecked(grid);
        m_listButton->setChecked(!grid);
        m_tiles->setViewMode(grid ? QListView::IconMode : QListView::ListMode);
        m_tiles->setResizeMode(QListView::Adjust);
        m_tiles->setMovement(QListView::Static);
        m_tiles->setSpacing(grid ? 2 : 0);
        m_tiles->setWrapping(grid);
        m_tiles->doItemsLayout();
    };
    connect(m_gridButton, &QPushButton::clicked, this, [setGrid]() { setGrid(true); });
    connect(m_listButton, &QPushButton::clicked, this, [setGrid]() { setGrid(false); });
    setGrid(true);
    connect(m_tiles, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item)
    {
        ShowDetails(item ? item->data(BrowserIndexRole).toInt() : -1);
    });
    connect(m_tiles, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item)
    {
        OpenInEditor(m_files[item->data(BrowserIndexRole).toInt()]);
    });
    middle->addWidget(m_tiles, 1);
    body->addLayout(middle, 1);

    // Selected asset.
    m_details = Hearth::MakeCard(this, &m_detailsLayout);
    m_details->setFixedWidth(320);
    body->addWidget(m_details);
    root->addLayout(body, 1);

    m_problemTimer = new QTimer(this);
    m_problemTimer->setInterval(60);
    connect(m_problemTimer, &QTimer::timeout, this, &HearthAssetBrowser::CheckNextForProblems);

    ShowDetails(-1);
}

void HearthAssetBrowser::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_files.isEmpty() && !m_scanning)
    {
        Scan();
    }
}

void HearthAssetBrowser::Scan()
{
    m_scanning = true;
    const QString project = QString::fromUtf8(AZ::Utils::GetProjectPath().c_str());
    const QString engine = Hearth::EngineRoot();
    QPointer<HearthAssetBrowser> self(this);
    QThreadPool::globalInstance()->start([self, project, engine]()
    {
        static const QStringList skip = { QStringLiteral("build"), QStringLiteral("Cache"), QStringLiteral("user"), QStringLiteral("Gem"),
                                          QStringLiteral("Code"), QStringLiteral("Platform"), QStringLiteral("Registry"), QStringLiteral("cmake"),
                                          QStringLiteral("Tools"), QStringLiteral("Editor"), QStringLiteral("Config"), QStringLiteral(".git"),
                                          QStringLiteral("AssetBundling"), QStringLiteral("ShaderLib") };
        static const QStringList typeFolders = { QStringLiteral("materials"), QStringLiteral("textures"), QStringLiteral("models"),
                                                 QStringLiteral("sounds"), QStringLiteral("prefabs"), QStringLiteral("levels") };
        QVector<HearthAssetFile> files;
        QSet<QString> areasWithFiles;
        QStringList allAreas;
        // Walks the folders itself so build/, Cache/ and the like are never entered.
        std::function<void(const QString&, const QString&, bool)> walk;
        walk = [&](const QString& base, const QString& folder, bool isEngine)
        {
            const QFileInfoList entries = QDir(folder).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
            for (const QFileInfo& info : entries)
            {
                const QString path = info.absoluteFilePath();
                const QString relative = QDir(base).relativeFilePath(path);
                const QStringList parts = relative.split(QLatin1Char('/'));
                if (info.isDir() && ((!isEngine && parts.size() == 1 && skip.contains(parts.first())) || info.fileName() == QLatin1String("Cache")
                                     || info.fileName() == QLatin1String("build") || info.fileName().startsWith(QLatin1Char('.'))))
                {
                    continue;
                }
                // Area = the folder right under a type folder (Assets/Materials/Level0/..., Prefabs/Player/..., Levels/Level0/...).
                QString area;
                for (int i = 0; i + 1 < parts.size(); ++i)
                {
                    if (typeFolders.contains(parts[i].toLower()))
                    {
                        if (info.isDir() && i + 2 == parts.size() && !isEngine)
                        {
                            allAreas << parts[i + 1];
                        }
                        if (i + 2 < parts.size())
                        {
                            area = parts[i + 1];
                        }
                        break;
                    }
                }
                if (info.isDir())
                {
                    walk(base, path, isEngine);
                    continue;
                }
                const QString kind = BrowserKindFor(info.suffix());
                if (kind.isEmpty())
                {
                    continue;
                }
                HearthAssetFile file;
                file.path = path;
                file.relative = relative;
                file.name = info.completeBaseName();
                file.kind = kind;
                file.area = isEngine ? QString() : area;
                file.engine = isEngine;
                file.bytes = info.size();
                file.changed = info.lastModified();
                files << file;
                if (!file.area.isEmpty())
                {
                    areasWithFiles.insert(file.area);
                }
            }
        };
        auto collect = [&](const QString& base, bool isEngine) { walk(base, base, isEngine); };
        collect(project, false);
        if (!engine.isEmpty() && QFileInfo(QDir(engine).filePath(QStringLiteral("Assets"))).isDir())
        {
            collect(QDir(engine).filePath(QStringLiteral("Assets")), true);
        }
        std::sort(files.begin(), files.end(), [](const HearthAssetFile& a, const HearthAssetFile& b)
        {
            return a.kind != b.kind ? BrowserKinds().indexOf(a.kind) < BrowserKinds().indexOf(b.kind) : a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });
        allAreas.removeDuplicates();
        QStringList empty;
        for (const QString& area : allAreas)
        {
            if (!areasWithFiles.contains(area))
            {
                empty << area;
            }
        }
        QMetaObject::invokeMethod(qApp, [self, files, empty]()
        {
            if (self)
            {
                self->ScanFinished(files, empty);
            }
        }, Qt::QueuedConnection);
    });
}

void HearthAssetBrowser::ScanFinished(const QVector<HearthAssetFile>& files, const QStringList& emptyAreas)
{
    m_scanning = false;
    m_files = files;
    m_metaCache.clear();
    m_thumbs.clear();
    m_emptyAreas = emptyAreas;
    int projectCount = 0;
    for (const HearthAssetFile& file : m_files)
    {
        projectCount += file.engine ? 0 : 1;
    }
    if (m_subtitle)
    {
        const QString project = QFileInfo(QString::fromUtf8(AZ::Utils::GetProjectPath().c_str())).fileName();
        QString name = project;
        const QJsonObject json = BrowserReadJson(QDir(QString::fromUtf8(AZ::Utils::GetProjectPath().c_str())).filePath(QStringLiteral("project.json")));
        if (json.contains(QStringLiteral("display_name")))
        {
            name = json.value(QStringLiteral("display_name")).toString();
        }
        m_subtitle->setText(tr("%1 · %n item(s)", nullptr, projectCount).arg(name));
    }
    FillFolders();
    m_problemCursor = 0;
    m_problemTimer->start();
    if (!m_showAfterScan.isEmpty())
    {
        const QString path = m_showAfterScan;
        m_showAfterScan.clear();
        ShowFile(path);
    }
}

void HearthAssetBrowser::Rescan(const QString& thenShow)
{
    m_showAfterScan = thenShow;
    if (!m_scanning)
    {
        Scan();
    }
}

void HearthAssetBrowser::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
    {
        event->acceptProposedAction();
    }
}

void HearthAssetBrowser::dropEvent(QDropEvent* event)
{
    // Files dropped onto Assets go through Import, so they're checked before they go in.
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls())
    {
        if (url.isLocalFile())
        {
            paths << url.toLocalFile();
        }
    }
    event->acceptProposedAction();
    if (const QtViewPane* pane = QtViewPaneManager::instance()->OpenPane(QStringLiteral("Import")))
    {
        if (auto* import = qobject_cast<HearthImportPane*>(pane->Widget()))
        {
            import->AddFiles(paths, true);
        }
    }
}

void HearthAssetBrowser::FillFolders()
{
    const QString keepArea = m_folderArea;
    const QString keepKind = m_folderKind;
    m_folders->blockSignals(true);
    m_folders->clear();
    auto add = [this](QTreeWidgetItem* parent, const QString& title, int count, const QString& area, const QString& kind)
    {
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_folders);
        item->setText(0, title);
        item->setText(1, QString::number(count));
        item->setForeground(1, Hearth::Dim());
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setData(0, FolderAreaRole, area);
        item->setData(0, FolderKindRole, kind);
        return item;
    };
    QMap<QString, QMap<QString, int>> areas;
    int all = 0;
    int engine = 0;
    int recent = 0;
    const QDateTime week = QDateTime::currentDateTime().addDays(-7);
    for (const HearthAssetFile& file : m_files)
    {
        if (file.engine)
        {
            ++engine;
            continue;
        }
        ++all;
        recent += file.changed > week ? 1 : 0;
        if (!file.area.isEmpty())
        {
            areas[file.area][file.kind] += 1;
        }
    }
    for (const QString& area : m_emptyAreas)
    {
        areas[area];
    }
    QTreeWidgetItem* current = add(nullptr, tr("All assets"), all, QString(), QString());
    for (auto it = areas.begin(); it != areas.end(); ++it)
    {
        int total = 0;
        for (int n : it.value())
        {
            total += n;
        }
        QTreeWidgetItem* area = add(nullptr, BrowserAreaTitle(it.key()), total, it.key(), QString());
        if (it.key() == keepArea && keepKind.isEmpty())
        {
            current = area;
        }
        for (const QString& kind : BrowserKinds())
        {
            if (it.value().value(kind) > 0)
            {
                QTreeWidgetItem* child = add(area, BrowserKindPlural(kind), it.value().value(kind), it.key(), kind);
                if (it.key() == keepArea && kind == keepKind)
                {
                    current = child;
                }
            }
        }
        area->setExpanded(true);
    }
    if (engine > 0)
    {
        QTreeWidgetItem* item = add(nullptr, tr("Engine"), engine, QStringLiteral("*engine"), QString());
        current = keepArea == QLatin1String("*engine") ? item : current;
    }
    auto* line = new QTreeWidgetItem(m_folders);
    line->setFlags(Qt::NoItemFlags);
    line->setSizeHint(0, QSize(10, 8));
    QTreeWidgetItem* recentItem = add(nullptr, tr("Recently changed"), recent, QStringLiteral("*recent"), QString());
    QTreeWidgetItem* problems = add(nullptr, tr("Problems"), m_failed.size(), QStringLiteral("*problems"), QString());
    problems->setForeground(1, m_failed.isEmpty() ? Hearth::Dim() : Hearth::Bad());
    current = keepArea == QLatin1String("*recent") ? recentItem : (keepArea == QLatin1String("*problems") ? problems : current);
    m_folders->setCurrentItem(current);
    m_folders->blockSignals(false);
    m_folderArea = current->data(0, FolderAreaRole).toString();
    m_folderKind = current->data(0, FolderKindRole).toString();
    FillTiles();
}

void HearthAssetBrowser::FillTiles()
{
    const QString keep = m_selected >= 0 && m_selected < m_files.size() ? m_files[m_selected].path : QString();
    const QString search = m_search->text().trimmed().toLower();
    const QDateTime week = QDateTime::currentDateTime().addDays(-7);
    m_tiles->blockSignals(true);
    m_tiles->clear();
    QListWidgetItem* again = nullptr;
    for (int i = 0; i < m_files.size(); ++i)
    {
        const HearthAssetFile& file = m_files[i];
        if (m_folderArea == QLatin1String("*engine"))
        {
            if (!file.engine) continue;
        }
        else if (file.engine)
        {
            continue;
        }
        else if (m_folderArea == QLatin1String("*recent"))
        {
            if (file.changed <= week) continue;
        }
        else if (m_folderArea == QLatin1String("*problems"))
        {
            if (!m_failed.contains(file.path)) continue;
        }
        else if (!m_folderArea.isEmpty() && file.area != m_folderArea)
        {
            continue;
        }
        if (!m_folderKind.isEmpty() && file.kind != m_folderKind) continue;
        if (!m_typeFilter.isEmpty() && file.kind != m_typeFilter) continue;
        if (!search.isEmpty() && !file.relative.toLower().contains(search)) continue;
        auto* item = new QListWidgetItem(m_tiles);
        item->setData(BrowserIndexRole, i);
        item->setToolTip(file.relative);
        if (file.path == keep)
        {
            again = item;
        }
    }
    m_tiles->blockSignals(false);
    if (again)
    {
        m_tiles->setCurrentItem(again);
    }
    else if (m_tiles->count() > 0)
    {
        m_tiles->setCurrentRow(0);
    }
    else
    {
        ShowDetails(-1);
    }
}

QString HearthAssetBrowser::Meta(const HearthAssetFile& file) const
{
    if (m_failed.contains(file.path))
    {
        return tr("Couldn't process");
    }
    // Painting asks for this often: work it out once per scan.
    auto cached = m_metaCache.constFind(file.path);
    if (cached != m_metaCache.constEnd())
    {
        return cached.value();
    }
    const QString meta = WorkOutMeta(file);
    m_metaCache.insert(file.path, meta);
    return meta;
}

QString HearthAssetBrowser::WorkOutMeta(const HearthAssetFile& file) const
{
    if (file.kind == QLatin1String("TEXTURE"))
    {
        const QSize size = QImageReader(file.path).size();
        return size.isValid() ? QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()) : file.relative.section(QLatin1Char('.'), -1).toUpper();
    }
    if (file.kind == QLatin1String("SOUND"))
    {
        const double seconds = BrowserWavSeconds(file.path);
        const bool loop = file.name.contains(QLatin1String("loop"), Qt::CaseInsensitive) || file.name.contains(QLatin1String("hum"), Qt::CaseInsensitive);
        if (seconds > 0)
        {
            return seconds >= 60 ? tr("%1 min %2 s").arg(int(seconds) / 60).arg(int(seconds) % 60) + (loop ? tr(" · loop") : QString())
                                 : tr("%1 s").arg(seconds, 0, 'f', 1) + (loop ? tr(" · loop") : QString());
        }
        return QLocale().formattedDataSize(file.bytes);
    }
    if (file.kind == QLatin1String("PREFAB"))
    {
        const QJsonObject json = BrowserReadJson(file.path);
        const int entities = json.value(QStringLiteral("Entities")).toObject().size() + (json.contains(QStringLiteral("ContainerEntity")) ? 1 : 0);
        return tr("%n entities", nullptr, entities);
    }
    if (file.kind == QLatin1String("MATERIAL"))
    {
        const QVariantMap values = BrowserMaterialValues(BrowserReadJson(file.path));
        for (auto it = values.begin(); it != values.end(); ++it)
        {
            if (it.key().endsWith(QLatin1String("blendMode")) || it.key().startsWith(QLatin1String("decal")))
            {
                return tr("Blended decal");
            }
            if (it.key().startsWith(QLatin1String("emissive.enable")) && it.value().toBool())
            {
                return tr("Emissive");
            }
        }
        return BrowserMaterialTextures(values).isEmpty() ? tr("Colour only") : tr("%n texture(s)", nullptr, BrowserMaterialTextures(values).size());
    }
    return QLocale().formattedDataSize(file.bytes);
}

QPixmap HearthAssetBrowser::Thumbnail(int index)
{
    if (index < 0 || index >= m_files.size())
    {
        return {};
    }
    const HearthAssetFile& file = m_files[index];
    if (file.kind != QLatin1String("TEXTURE") && file.kind != QLatin1String("MATERIAL"))
    {
        return {};
    }
    auto found = m_thumbs.constFind(file.path);
    if (found != m_thumbs.constEnd())
    {
        return found.value();
    }
    if (m_thumbsLoading.contains(file.path))
    {
        return {};
    }
    m_thumbsLoading.insert(file.path);
    const QString project = QString::fromUtf8(AZ::Utils::GetProjectPath().c_str());
    QPointer<HearthAssetBrowser> self(this);
    const HearthAssetFile copy = file;
    QThreadPool::globalInstance()->start([self, copy, project]()
    {
        QImage image;
        QString source = copy.path;
        QColor colour;
        if (copy.kind == QLatin1String("MATERIAL"))
        {
            const QVariantMap values = BrowserMaterialValues(BrowserReadJson(copy.path));
            source = BrowserResolveTexture(values.value(QStringLiteral("baseColor.textureMap")).toString(), copy.path, project);
            if (values.value(QStringLiteral("baseColor.textureMap")).toString().isEmpty())
            {
                source.clear();
            }
            const QVariantList rgb = values.value(QStringLiteral("baseColor.color")).toList();
            if (rgb.size() >= 3)
            {
                colour = QColor::fromRgbF(qBound(0.0, rgb[0].toDouble(), 1.0), qBound(0.0, rgb[1].toDouble(), 1.0), qBound(0.0, rgb[2].toDouble(), 1.0));
            }
        }
        if (!source.isEmpty())
        {
            QImageReader reader(source);
            const QSize size = reader.size();
            if (size.isValid())
            {
                reader.setScaledSize(size.scaled(320, 320, Qt::KeepAspectRatioByExpanding));
            }
            image = reader.read();
        }
        if (image.isNull() && colour.isValid())
        {
            image = QImage(64, 64, QImage::Format_RGB32);
            QPainter p(&image);
            QLinearGradient g(0, 0, 64, 64);
            g.setColorAt(0, colour.lighter(112));
            g.setColorAt(1, colour.darker(125));
            p.fillRect(image.rect(), g);
        }
        QMetaObject::invokeMethod(qApp, [self, image, copy]()
        {
            if (!self)
            {
                return;
            }
            self->m_thumbs.insert(copy.path, QPixmap::fromImage(image));
            self->m_thumbsLoading.remove(copy.path);
            self->m_tiles->viewport()->update();
            if (self->m_selected >= 0 && self->m_selected < self->m_files.size() && self->m_files[self->m_selected].path == copy.path)
            {
                self->ShowDetails(self->m_selected);
            }
        }, Qt::QueuedConnection);
    });
    return {};
}

QString HearthAssetBrowser::StatusOf(const HearthAssetFile& file, bool* failed) const
{
    using namespace AzToolsFramework::AssetSystem;
    AZ::Outcome<JobInfoContainer> jobs = AZ::Failure();
    AzToolsFramework::AssetSystemJobRequestBus::BroadcastResult(jobs, &AzToolsFramework::AssetSystemJobRequestBus::Events::GetAssetJobsInfo,
                                                                AZStd::string(file.path.toUtf8().constData()), false);
    if (failed)
    {
        *failed = false;
    }
    if (!jobs.IsSuccess() || jobs.GetValue().empty())
    {
        return file.kind == QLatin1String("PREFAB") && file.relative.startsWith(QLatin1String("Levels/")) ? tr("Level") : tr("Not processed yet");
    }
    bool anyFailed = false;
    bool anyWorking = false;
    bool anyWaiting = false;
    for (const JobInfo& job : jobs.GetValue())
    {
        anyFailed |= job.m_status == JobStatus::Failed || job.m_status == JobStatus::Failed_InvalidSourceNameExceedsMaxLimit;
        anyWorking |= job.m_status == JobStatus::InProgress;
        anyWaiting |= job.m_status == JobStatus::Queued;
    }
    if (failed)
    {
        *failed = anyFailed;
    }
    return anyFailed ? tr("Failed") : (anyWorking ? tr("Processing") : (anyWaiting ? tr("Waiting") : tr("Processed")));
}

void HearthAssetBrowser::CheckNextForProblems()
{
    // A few files per tick, so the window never stalls while the Asset Processor answers.
    int checked = 0;
    bool changed = false;
    while (m_problemCursor < m_files.size() && checked < 6)
    {
        const HearthAssetFile& file = m_files[m_problemCursor++];
        if (file.engine)
        {
            continue;
        }
        ++checked;
        bool failed = false;
        StatusOf(file, &failed);
        if (failed != m_failed.contains(file.path))
        {
            changed = true;
            failed ? (void)m_failed.insert(file.path) : (void)m_failed.remove(file.path);
        }
    }
    if (changed)
    {
        FillFolders();
    }
    if (m_problemCursor >= m_files.size())
    {
        m_problemTimer->stop();
        // Look again in a minute: files change and get processed while the editor is open.
        QTimer::singleShot(60000, this, [this]()
        {
            if (isVisible() && !m_scanning)
            {
                m_problemCursor = 0;
                m_problemTimer->start();
            }
        });
    }
}

QStringList HearthAssetBrowser::UsersOf(const HearthAssetFile& file) const
{
    // Prefabs, levels and materials that name this file.
    QStringList users;
    const QStringList needles = BrowserNeedles(file);
    for (const HearthAssetFile& other : m_files)
    {
        if (other.engine || other.path == file.path || (other.kind != QLatin1String("PREFAB") && other.kind != QLatin1String("MATERIAL")))
        {
            continue;
        }
        QFile text(other.path);
        if (!text.open(QIODevice::ReadOnly))
        {
            continue;
        }
        const QString body = QString::fromUtf8(text.readAll()).toLower();
        for (const QString& needle : needles)
        {
            if (body.contains(needle))
            {
                users << other.path;
                break;
            }
        }
    }
    return users;
}

void HearthAssetBrowser::ShowDetails(int index)
{
    m_selected = index;
    while (QLayoutItem* item = m_detailsLayout->takeAt(0))
    {
        if (item->widget())
        {
            item->widget()->deleteLater();
        }
        else if (item->layout())
        {
            QLayout* layout = item->layout();
            while (QLayoutItem* inner = layout->takeAt(0))
            {
                if (inner->widget())
                {
                    inner->widget()->deleteLater();
                }
                delete inner;
            }
        }
        delete item;
    }
    if (index < 0 || index >= m_files.size())
    {
        m_detailsLayout->addWidget(Hearth::MakeLabel(m_scanning ? tr("Looking for assets…") : tr("Pick an asset to see where it's used."), QStringLiteral("dim"), m_details));
        m_detailsLayout->addStretch(1);
        return;
    }
    const HearthAssetFile file = m_files[index];
    const QString project = QString::fromUtf8(AZ::Utils::GetProjectPath().c_str());

    // Big swatch.
    auto* swatch = new QLabel(m_details);
    swatch->setFixedHeight(170);
    {
        const qreal ratio = devicePixelRatioF();
        QPixmap pixmap(QSize(296, 170) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(0, 0, 296, 170), 12, 12);
        painter.setClipPath(clip);
        BrowserPaintSwatch(painter, QRectF(0, 0, 296, 170), file, Thumbnail(index), m_failed.contains(file.path));
        BrowserPaintKindPill(painter, QRectF(0, 0, 296, 170), file.kind);
        swatch->setPixmap(pixmap);
    }
    m_detailsLayout->addWidget(swatch);

    QString where = QFileInfo(file.relative).path();
    if (file.kind == QLatin1String("MATERIAL"))
    {
        const QString type = BrowserReadJson(file.path).value(QStringLiteral("materialType")).toString();
        if (!type.isEmpty())
        {
            where += QStringLiteral(" · ") + QFileInfo(type).completeBaseName();
        }
    }
    QLabel* name = Hearth::MakeLabel(file.name, QStringLiteral("title"), m_details);
    name->setWordWrap(true);
    m_detailsLayout->addWidget(name);
    QLabel* folder = Hearth::MakeLabel(where, QStringLiteral("small"), m_details);
    folder->setWordWrap(true);
    m_detailsLayout->addWidget(folder);

    // Facts.
    const QStringList users = file.engine ? QStringList() : UsersOf(file);
    int prefabs = 0;
    int levels = 0;
    int materials = 0;
    for (const QString& user : users)
    {
        if (user.endsWith(QLatin1String(".material")))
        {
            ++materials;
        }
        else if (QDir(project).relativeFilePath(user).startsWith(QLatin1String("Levels/")))
        {
            ++levels;
        }
        else
        {
            ++prefabs;
        }
    }
    QStringList usedIn;
    if (prefabs) usedIn << tr("%n prefab(s)", nullptr, prefabs);
    if (materials) usedIn << tr("%n material(s)", nullptr, materials);
    if (levels) usedIn << tr("%n level(s)", nullptr, levels);
    bool failed = false;
    const QString status = StatusOf(file, &failed);
    if (failed != m_failed.contains(file.path))
    {
        failed ? (void)m_failed.insert(file.path) : (void)m_failed.remove(file.path);
    }
    QString thirdTitle = tr("Size");
    QString thirdValue = QLocale().formattedDataSize(file.bytes);
    if (file.kind == QLatin1String("MATERIAL"))
    {
        const QStringList textures = BrowserMaterialTextures(BrowserMaterialValues(BrowserReadJson(file.path)));
        int largest = 0;
        for (const QString& texture : textures)
        {
            const QSize size = QImageReader(BrowserResolveTexture(texture, file.path, project)).size();
            largest = qMax(largest, qMax(size.width(), size.height()));
        }
        thirdTitle = tr("Textures");
        thirdValue = textures.isEmpty() ? tr("None") : (largest > 0 ? tr("%1 · %2 px").arg(textures.size()).arg(largest) : QString::number(textures.size()));
    }
    else if (file.kind == QLatin1String("TEXTURE"))
    {
        const QSize size = QImageReader(file.path).size();
        thirdValue = size.isValid() ? QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()) : thirdValue;
    }
    else if (file.kind == QLatin1String("SOUND"))
    {
        thirdTitle = tr("Length");
        thirdValue = Meta(file);
    }
    else if (file.kind == QLatin1String("PREFAB"))
    {
        thirdTitle = tr("Entities");
        thirdValue = Meta(file).section(QLatin1Char(' '), 0, 0);
    }
    auto* facts = new QGridLayout();
    facts->setHorizontalSpacing(8);
    facts->setVerticalSpacing(8);
    auto fact = [this, facts](int at, const QString& title, const QString& value, const QColor& colour)
    {
        auto* box = new QFrame(m_details);
        box->setStyleSheet(QStringLiteral("QFrame { background-color: rgba(255, 255, 255, 8); border-radius: 10px; }"));
        auto* layout = new QVBoxLayout(box);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(2);
        layout->addWidget(Hearth::MakeLabel(title, QStringLiteral("small"), box));
        QLabel* label = Hearth::MakeLabel(value, QStringLiteral("body"), box);
        label->setStyleSheet(QStringLiteral("background: transparent; color: %1; font-weight: 500;").arg(colour.name()));
        layout->addWidget(label);
        facts->addWidget(box, at / 2, at % 2);
    };
    fact(0, tr("Used in"), file.engine ? tr("Engine asset") : (usedIn.isEmpty() ? tr("Nothing yet") : usedIn.join(QStringLiteral(" · "))), Hearth::Text());
    fact(1, tr("Status"), status, failed ? Hearth::Bad() : (status == tr("Processed") ? Hearth::Good() : Hearth::Text()));
    fact(2, thirdTitle, thirdValue, Hearth::Text());
    fact(3, tr("Changed"), BrowserAgo(file.changed), Hearth::Text());
    m_detailsLayout->addLayout(facts);

    // Used by.
    m_detailsLayout->addWidget(Hearth::MakeLabel(tr("Used by"), QStringLiteral("small"), m_details));
    if (users.isEmpty())
    {
        m_detailsLayout->addWidget(Hearth::MakeLabel(file.engine ? tr("Comes with the engine.") : tr("Nothing uses it yet."), QStringLiteral("dim"), m_details));
    }
    for (const QString& user : users.mid(0, 8))
    {
        auto* row = new QPushButton(QFileInfo(user).completeBaseName(), m_details);
        row->setCursor(Qt::PointingHandCursor);
        row->setStyleSheet(QStringLiteral("QPushButton { text-align: left; background-color: rgba(255, 255, 255, 8); border: none; border-radius: 8px; padding: 6px 10px; color: #ECE6DE; }"
                                          "QPushButton:hover { background-color: rgba(255, 255, 255, 16); }"));
        connect(row, &QPushButton::clicked, this, [this, user]() { ShowFile(user); });
        m_detailsLayout->addWidget(row);
    }
    if (users.size() > 8)
    {
        m_detailsLayout->addWidget(Hearth::MakeLabel(tr("and %n more", nullptr, int(users.size() - 8)), QStringLiteral("dim"), m_details));
    }
    m_detailsLayout->addStretch(1);

    // Actions.
    const bool material = file.kind == QLatin1String("MATERIAL");
    QPushButton* open = Hearth::MakeButton(material ? tr("Open in material editor") : tr("Open"), true, m_details);
    connect(open, &QPushButton::clicked, this, [this, file]() { OpenInEditor(file); });
    m_detailsLayout->addWidget(open);
    QPushButton* show = Hearth::MakeButton(tr("Show in level"), false, m_details);
    show->setEnabled(!file.engine && GetIEditor() && GetIEditor()->GetDocument() && GetIEditor()->GetDocument()->IsDocumentReady());
    connect(show, &QPushButton::clicked, this, [this, file]() { ShowInLevel(file); });
    m_detailsLayout->addWidget(show);
    m_tiles->viewport()->update();
}

void HearthAssetBrowser::ShowFile(const QString& path)
{
    for (int i = 0; i < m_files.size(); ++i)
    {
        if (m_files[i].path == path)
        {
            // Make sure it's in the list: show its folder, all types, no search.
            m_search->clear();
            m_typeFilter.clear();
            for (QPushButton* chip : m_chips)
            {
                chip->setChecked(chip == m_chips.first());
            }
            m_folderArea = m_files[i].engine ? QStringLiteral("*engine") : QString();
            m_folderKind.clear();
            m_selected = i;
            FillFolders();
            return;
        }
    }
}

void HearthAssetBrowser::ShowKind(const QString& kind)
{
    static const QStringList order = { QString(), QStringLiteral("MATERIAL"), QStringLiteral("TEXTURE"), QStringLiteral("MODEL"), QStringLiteral("SOUND"), QStringLiteral("PREFAB") };
    m_typeFilter = kind;
    for (int i = 0; i < m_chips.size(); ++i)
    {
        m_chips[i]->setChecked(order.value(i) == kind);
    }
    m_search->clear();
    m_folderArea.clear();
    m_folderKind.clear();
    if (!m_files.isEmpty())
    {
        FillFolders();
    }
}

void HearthAssetBrowser::OpenInEditor(const HearthAssetFile& file)
{
    // Materials open in the Hearth material editor, live in the level.
    if (file.kind == QLatin1String("MATERIAL") && !file.engine)
    {
        if (const QtViewPane* pane = QtViewPaneManager::instance()->OpenPane(QStringLiteral("Material editor")))
        {
            if (auto* editor = qobject_cast<HearthMaterialPane*>(pane->Widget()))
            {
                editor->OpenMaterial(file.path);
                return;
            }
        }
    }
    AZ::Data::AssetInfo info;
    AZStd::string watchFolder;
    bool found = false;
    AzToolsFramework::AssetSystemRequestBus::BroadcastResult(found, &AzToolsFramework::AssetSystemRequestBus::Events::GetSourceInfoBySourcePath,
                                                             file.path.toUtf8().constData(), info, watchFolder);
    if (found && info.m_assetId.IsValid())
    {
        // The same openers the Asset Browser uses (Material Editor for .material, the prefab opener, ...).
        bool handled = false;
        AzToolsFramework::AssetBrowser::AssetBrowserInteractionNotificationBus::Broadcast(
            &AzToolsFramework::AssetBrowser::AssetBrowserInteractionNotifications::OpenAssetInAssociatedEditor,
            AZ::Data::AssetId(info.m_assetId.m_guid, 0), handled);
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(file.path));
}

void HearthAssetBrowser::ShowInLevel(const HearthAssetFile& file)
{
    // Every editor entity whose saved form names this file (or, for a prefab, every instance of it).
    const QStringList needles = BrowserNeedles(file);
    const bool prefab = file.kind == QLatin1String("PREFAB");
    const QString fileName = QFileInfo(file.path).fileName().toLower();
    auto* mapper = AZ::Interface<AzToolsFramework::Prefab::InstanceEntityMapperInterface>::Get();
    AzToolsFramework::EntityIdList matches;
    AZ::ComponentApplicationBus::Broadcast(&AZ::ComponentApplicationRequests::EnumerateEntities, [&](AZ::Entity* entity)
    {
        if (!entity)
        {
            return;
        }
        bool editorEntity = false;
        AzToolsFramework::EditorEntityContextRequestBus::BroadcastResult(editorEntity, &AzToolsFramework::EditorEntityContextRequests::IsEditorEntity, entity->GetId());
        if (!editorEntity)
        {
            return;
        }
        if (prefab && mapper)
        {
            auto owner = mapper->FindOwningInstance(entity->GetId());
            if (owner.has_value() && owner->get().GetContainerEntityId() == entity->GetId()
                && QString::fromUtf8(owner->get().GetTemplateSourcePath().Native().c_str()).toLower().endsWith(fileName))
            {
                matches.push_back(entity->GetId());
            }
            return;
        }
        rapidjson::Document document;
        if (AZ::JsonSerialization::Store(document, document.GetAllocator(), *entity).GetProcessing() == AZ::JsonSerializationResult::Processing::Halted)
        {
            return;
        }
        AZStd::string text;
        if (!AZ::JsonSerializationUtils::WriteJsonString(document, text).IsSuccess())
        {
            return;
        }
        const QString body = QString::fromUtf8(text.c_str()).toLower();
        for (const QString& needle : needles)
        {
            if (body.contains(needle))
            {
                matches.push_back(entity->GetId());
                return;
            }
        }
    });
    if (matches.empty())
    {
        QMessageBox::information(this, tr("Show in level"), tr("%1 isn't used in the open level.").arg(file.name));
        return;
    }
    AzToolsFramework::ToolsApplicationRequestBus::Broadcast(&AzToolsFramework::ToolsApplicationRequests::SetSelectedEntities, matches);
    if (auto* actions = AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get())
    {
        actions->TriggerAction("o3de.action.view.centerOnSelection");
    }
}

void HearthAssetBrowser::NewAsset()
{
    QMenu menu(this);
    const QString project = QString::fromUtf8(AZ::Utils::GetProjectPath().c_str());
    QString area = m_folderArea.startsWith(QLatin1Char('*')) ? QString() : m_folderArea;
    menu.addAction(tr("Material"), this, [this, project, area]()
    {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New material"), tr("Name"), QLineEdit::Normal, QStringLiteral("new_material"), &ok).trimmed();
        if (!ok || name.isEmpty())
        {
            return;
        }
        QDir folder(QDir(project).filePath(QStringLiteral("Assets/Materials") + (area.isEmpty() ? QString() : QLatin1Char('/') + area)));
        folder.mkpath(QStringLiteral("."));
        const QString path = folder.filePath(QString(name).replace(QLatin1Char(' '), QLatin1Char('_')).toLower() + QStringLiteral(".material"));
        if (QFileInfo::exists(path))
        {
            QMessageBox::warning(this, tr("New material"), tr("%1 already exists.").arg(QFileInfo(path).fileName()));
            return;
        }
        QJsonObject material;
        material.insert(QStringLiteral("materialType"), QStringLiteral("@gemroot:Atom_Feature_Common@/Assets/Materials/Types/StandardPBR.materialtype"));
        material.insert(QStringLiteral("materialTypeVersion"), 5);
        material.insert(QStringLiteral("propertyValues"), QJsonObject{ { QStringLiteral("baseColor.color"), QJsonArray{ 0.8, 0.8, 0.8 } } });
        QFile out(path);
        if (out.open(QIODevice::WriteOnly))
        {
            out.write(QJsonDocument(material).toJson());
            out.close();
        }
        Rescan(path);
    });
    menu.addAction(tr("Folder for a new area"), this, [this, project]()
    {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New area"), tr("Name (for example FoundationSite)"), QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || name.isEmpty())
        {
            return;
        }
        const QString clean = QString(name).remove(QLatin1Char(' '));
        for (const QString& kind : { QStringLiteral("Materials"), QStringLiteral("Textures"), QStringLiteral("Models"), QStringLiteral("Sounds") })
        {
            QDir(project).mkpath(QStringLiteral("Assets/%1/%2").arg(kind, clean));
        }
        QDir(project).mkpath(QStringLiteral("Prefabs/%1").arg(clean));
        Rescan();
    });
    menu.addSeparator();
    menu.addAction(tr("Voice line…"), this, []() { QtViewPaneManager::instance()->OpenPane(QStringLiteral("Voice Studio")); });
    menu.exec(QCursor::pos());
}
