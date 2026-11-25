/*
 * Hearth Engine command palette (Ctrl+K), approved mockup "Menus" (Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 *
 * One search box for everything: every menu command, Hearth commands, assets (@), entities in the level (#)
 * and help (?).
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <QDialog>
#include <QString>
#include <QVector>

#include <functional>
#endif

QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QListWidget)
QT_FORWARD_DECLARE_CLASS(QMainWindow)

class HearthCommandPalette : public QDialog
{
    Q_OBJECT

public:
    struct Entry
    {
        QString icon;       //!< one character shown in the tile
        QString name;
        QString category;   //!< menu path, "Asset", "Entity", ...
        QString shortcut;
        std::function<void()> run;
    };

    HearthCommandPalette(QMainWindow* mainWindow, const QString& prefix);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void CollectCommands();
    void CollectAssets();
    void CollectEntities();
    void UpdateResults();
    void RunSelected();
    static int Score(const QString& text, const QString& query);

    QMainWindow* m_mainWindow = nullptr;
    QLineEdit* m_search = nullptr;
    QListWidget* m_list = nullptr;
    QVector<Entry> m_commands;
    QVector<Entry> m_assets;
    QVector<Entry> m_entities;
    bool m_assetsCollected = false;
    bool m_entitiesCollected = false;
    QVector<const Entry*> m_shown;
};
