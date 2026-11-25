/*
 * Hearth Engine Import (approved mockup "Import dialog", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * Files dropped onto Assets (or picked) are checked before they go in: what each one is, where it goes, whether it
 * will import, and a scale check for models. Options make materials from textures, prefabs for models, set texture
 * compression and the laptop's max texture size, and convert files the engine can't read.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QStringList>
#include <QVector>
#include <QWidget>
#endif

QT_FORWARD_DECLARE_CLASS(QComboBox)
QT_FORWARD_DECLARE_CLASS(QDoubleSpinBox)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

namespace AzQtComponents
{
    namespace Hearth
    {
        class Toggle;
    }
}

struct HearthImportFile
{
    QString source;     //!< where it is now
    QString kind;       //!< TEXTURE, MODEL, SOUND, MATERIAL, PREFAB, or empty when not supported
    QString about;      //!< "Texture · 2048² · goes to Textures/Level0"
    QString state;      //!< ready, scale, wont, replace
    QString problem;    //!< why it won't import
    bool convertible = false; //!< can be turned into a PNG
    bool failingInProject = false; //!< the project's copy of this file keeps failing in the Asset Processor
    double scaleFix = 1.0;    //!< suggested scale for models that look too big or small
    int scaleTimes = 1;       //!< "looks 100× too big"
    int textureSize = 0;      //!< largest side, for textures
};

class HearthImportPane : public QWidget
{
    Q_OBJECT

public:
    explicit HearthImportPane(QWidget* parent = nullptr);
    static void RegisterViewClass();

    //! Adds files or folders (folders are searched for files the engine can import). fromDrop: they were dropped onto Assets.
    void AddFiles(const QStringList& paths, bool fromDrop = false);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void Check(HearthImportFile& file) const;
    void Fill();
    void FillOptions();
    QString Destination(const HearthImportFile& file) const;
    void Import();

    QVector<HearthImportFile> m_files;
    QStringList m_areas;
    bool m_fromDrop = false;

    QLabel* m_subtitle = nullptr;
    QLabel* m_count = nullptr;
    QVBoxLayout* m_rows = nullptr;
    QComboBox* m_area = nullptr;
    QLabel* m_areaHint = nullptr;
    QVBoxLayout* m_options = nullptr;
    QDoubleSpinBox* m_scale = nullptr;
    AzQtComponents::Hearth::Toggle* m_makeMaterials = nullptr;
    AzQtComponents::Hearth::Toggle* m_makePrefabs = nullptr;
    AzQtComponents::Hearth::Toggle* m_compress = nullptr;
    QComboBox* m_maxSize = nullptr;
    QVector<QPair<QString, AzQtComponents::Hearth::Toggle*>> m_converts;
    QPushButton* m_import = nullptr;
};
