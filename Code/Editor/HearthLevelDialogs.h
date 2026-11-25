/*
 * Hearth Engine level dialogs (approved mockup "New, open and save level", Nov 2026). Built on O3DE (Apache-2.0 OR MIT).
 * New level: a name and what it starts from (empty, the project's templates in Templates/Levels, or a copy of a level).
 * Open level: the project's levels, with their titles and when each was last open.
 * Save first?: what you changed since you last saved, in words, and the backup the editor keeps.
 */
#pragma once

#if !defined(Q_MOC_RUN)
#include <AzCore/base.h>
#include <QDialog>
#endif

QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QListWidget)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QComboBox)

class HearthNewLevelDialog : public QDialog
{
    Q_OBJECT

public:
    explicit HearthNewLevelDialog(QWidget* parent = nullptr);

    QString GetLevel() const;        //!< the new level's name
    QString GetTemplateName() const; //!< the prefab it starts from (full path)
    QString GetLevelsFolder() const; //!< the project's Levels folder

private:
    QLineEdit* m_name = nullptr;
    QList<QPushButton*> m_choices;
    QStringList m_templates;
    QComboBox* m_copyFrom = nullptr;
    QPushButton* m_create = nullptr;
    int m_picked = 0;
};

class HearthOpenLevelDialog : public QDialog
{
    Q_OBJECT

public:
    //! offerNew: also a "New level…" button (the editor's first window); GetFileName() is then "new".
    explicit HearthOpenLevelDialog(QWidget* parent = nullptr, bool offerNew = false);

    QString GetFileName() const; //!< the level's .prefab (full path), or "new"

private:
    QListWidget* m_levels = nullptr;
    bool m_offerNew = false;
    bool m_wantsNew = false;
};

//! The "Save <level> first?" question, in place of the editor's own. Returns what the editor's
//! HandleRootPrefabClosure would: 1 = saved (carry on), 2 = don't save (carry on), 0 = cancel.
int HearthAskToSaveLevel(AZ::u64 rootTemplateId);
