/*
 * Hearth Engine asset browser (approved mockup "Asset browser", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * Folders by area (Level 0, Player, ...) and type, picture tiles or a list, the picked asset's details: where it
 * is used, whether it processed, its textures or size, when it changed, what uses it, and Open / Show in level.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QDateTime>
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QListWidget)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QTimer)
QT_FORWARD_DECLARE_CLASS(QTreeWidget)
QT_FORWARD_DECLARE_CLASS(QTreeWidgetItem)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

struct HearthAssetFile
{
    QString path;      //!< full path
    QString relative;  //!< from the project (or engine) folder
    QString name;      //!< file name without extension
    QString kind;      //!< MATERIAL, TEXTURE, MODEL, SOUND, PREFAB
    QString area;      //!< "Level0", "Player", ... (empty when the file isn't in an area folder)
    bool engine = false;
    qint64 bytes = 0;
    QDateTime changed;
};

class HearthAssetBrowser : public QWidget
{
    Q_OBJECT

public:
    explicit HearthAssetBrowser(QWidget* parent = nullptr);
    static void RegisterViewClass();

    //! Picks a file in the browser (full path), as when another Hearth window shows an asset.
    void ShowFile(const QString& path);
    //! Shows all of the project's assets of one kind (MATERIAL, TEXTURE, MODEL, SOUND, PREFAB).
    void ShowKind(const QString& kind);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void Scan();
    void ScanFinished(const QVector<HearthAssetFile>& files, const QStringList& emptyAreas);
    void FillFolders();
    void FillTiles();
    void ShowDetails(int index);
    void CheckNextForProblems();
    QString StatusOf(const HearthAssetFile& file, bool* failed = nullptr) const;
    QStringList UsersOf(const HearthAssetFile& file) const;
    void ShowInLevel(const HearthAssetFile& file);
    void OpenInEditor(const HearthAssetFile& file);
    void NewAsset();
    QPixmap Thumbnail(int index);
    QString Meta(const HearthAssetFile& file) const;
    QString WorkOutMeta(const HearthAssetFile& file) const;

    QVector<HearthAssetFile> m_files;
    QStringList m_emptyAreas;
    QHash<QString, QPixmap> m_thumbs;
    mutable QHash<QString, QString> m_metaCache;
    QSet<QString> m_thumbsLoading;
    QSet<QString> m_failed;
    int m_problemCursor = 0;
    QTimer* m_problemTimer = nullptr;
    bool m_scanning = false;

    QString m_folderArea;   //!< "" = all, "*recent", "*problems", "*engine", or an area
    QString m_folderKind;   //!< kind inside an area, or ""
    QString m_typeFilter;   //!< chip: "" = All
    bool m_grid = true;
    int m_selected = -1;

    QLabel* m_subtitle = nullptr;
    QLineEdit* m_search = nullptr;
    QTreeWidget* m_folders = nullptr;
    QList<QPushButton*> m_chips;
    QPushButton* m_gridButton = nullptr;
    QPushButton* m_listButton = nullptr;
    QListWidget* m_tiles = nullptr;
    QWidget* m_details = nullptr;
    QVBoxLayout* m_detailsLayout = nullptr;
};
