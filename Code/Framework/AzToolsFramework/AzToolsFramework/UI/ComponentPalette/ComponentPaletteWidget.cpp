/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */


#include "ComponentPaletteModel.hxx"
#include "ComponentPaletteUtil.hxx"
#include "ComponentPaletteWidget.hxx"

#include <AzCore/std/containers/unordered_set.h>
#include <AzCore/Debug/Profiler.h>
#include <AzFramework/Components/DeprecatedComponentsBus.h>
#include <AzToolsFramework/API/EntityCompositionRequestBus.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Entity/EditorEntityHelpers.h>

#include <AzQtComponents/Components/Widgets/LineEdit.h>

AZ_PUSH_DISABLE_WARNING(4244 4251, "-Wunknown-warning-option") // 4244: conversion from 'int' to 'float', possible loss of data
                                                               // 4251: class '...' needs to have dll-interface to be used by clients of class '...'
#include <QAction>
#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardItem>
#include <QStandardItem>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QToolButton>
#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QStyledItemDelegate>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
AZ_POP_DISABLE_WARNING

namespace
{
    namespace Hearth = AzQtComponents::Hearth;
    constexpr int ComponentRole = Qt::ItemDataRole::UserRole + 1;
    constexpr int CategoryRole = Qt::ItemDataRole::UserRole + 2;

    //! The project's own components carry its name as their category (Noclip Protocol -> chip "Noclip").
    QString ProjectCategory()
    {
        const auto name = AZ::Utils::GetProjectName();
        const QString raw = QString::fromUtf8(name.c_str(), static_cast<int>(name.size()));
        QString spaced;
        for (int i = 0; i < raw.size(); ++i)
        {
            if (i > 0 && raw[i].isUpper() && raw[i - 1].isLower())
            {
                spaced += QLatin1Char(' ');
            }
            spaced += raw[i];
        }
        return spaced;
    }

    //! Rows drawn like the mockup: icon tile, name, one line of description, the project chip.
    class HearthComponentDelegate : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;

        QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            const bool isComponent = index.data(ComponentRole).toULongLong() != 0;
            return QSize(option.rect.width(), isComponent ? 54 : 26);
        }

        void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
        {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            const QRectF rect = QRectF(option.rect).adjusted(2, 2, -2, -2);
            const auto* classData = reinterpret_cast<const AZ::SerializeContext::ClassData*>(index.data(ComponentRole).toULongLong());
            if (!classData)
            {
                painter->setPen(Hearth::Dim());
                QFont font = option.font;
                font.setPixelSize(11);
                font.setBold(false);
                painter->setFont(font);
                painter->drawText(rect.adjusted(8, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, index.data(Qt::DisplayRole).toString().toUpper());
                painter->restore();
                return;
            }
            const bool selected = option.state & QStyle::State_Selected;
            const bool hovered = option.state & QStyle::State_MouseOver;
            if (selected || hovered)
            {
                painter->setPen(selected ? QPen(QColor(232, 116, 59, 100), 1) : Qt::NoPen);
                painter->setBrush(selected ? QColor(232, 116, 59, 26) : QColor(255, 255, 255, 8));
                painter->drawRoundedRect(rect, 10, 10);
            }
            const QRectF tile(rect.left() + 10, rect.center().y() - 17, 34, 34);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(255, 255, 255, 13));
            painter->drawRoundedRect(tile, 9, 9);
            const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
            if (!icon.isNull())
            {
                icon.paint(painter, tile.adjusted(8, 8, -8, -8).toRect());
            }

            QString chip;
            const QString category = index.data(CategoryRole).toString();
            const QString project = ProjectCategory();
            if (!project.isEmpty() && category.startsWith(project))
            {
                chip = project.section(QLatin1Char(' '), 0, 0);
            }
            qreal right = rect.right() - 10;
            if (!chip.isEmpty())
            {
                QFont chipFont = option.font;
                chipFont.setPixelSize(11);
                const qreal chipWidth = QFontMetricsF(chipFont).horizontalAdvance(chip) + 20;
                const QRectF chipRect(right - chipWidth, rect.center().y() - 10, chipWidth, 20);
                painter->setBrush(QColor(232, 116, 59, 41));
                painter->drawRoundedRect(chipRect, 10, 10);
                painter->setPen(Hearth::EmberLight());
                painter->setFont(chipFont);
                painter->drawText(chipRect, Qt::AlignCenter, chip);
                right = chipRect.left() - 10;
            }

            const QRectF textRect(tile.right() + 12, rect.top() + 7, right - tile.right() - 12, rect.height() - 14);
            QFont nameFont = option.font;
            nameFont.setPixelSize(13);
            nameFont.setWeight(QFont::Medium);
            painter->setFont(nameFont);
            painter->setPen(Hearth::Text());
            painter->drawText(QRectF(textRect.left(), textRect.top(), textRect.width(), textRect.height() / 2),
                              Qt::AlignLeft | Qt::AlignVCenter, index.data(Qt::DisplayRole).toString());
            QFont aboutFont = option.font;
            aboutFont.setPixelSize(11);
            painter->setFont(aboutFont);
            painter->setPen(Hearth::Dim());
            const QString about = classData->m_editData ? QString::fromUtf8(classData->m_editData->m_description) : QString();
            painter->drawText(QRectF(textRect.left(), textRect.center().y(), textRect.width(), textRect.height() / 2),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(aboutFont).elidedText(about, Qt::ElideRight, static_cast<int>(textRect.width())));
            painter->restore();
        }
    };
} // namespace

namespace AzToolsFramework
{
    ComponentPaletteWidget::ComponentPaletteWidget(QWidget* parent, bool enableSearch)
        : QFrame(parent)
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::Popup);
        setFrameShadow(QFrame::Shadow::Raised);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAcceptDrops(false);

        auto outerLayout = new QVBoxLayout(this);
        outerLayout->setSizeConstraint(QLayout::SetNoConstraint);
        outerLayout->setContentsMargins(16, 16, 16, 16);
        outerLayout->setSpacing(12);
        setLayout(outerLayout);
        setObjectName(QStringLiteral("HearthComponentPalette"));
        setStyleSheet(QStringLiteral(
            "QFrame#HearthComponentPalette { background-color: #171513; border: 1px solid rgba(255, 255, 255, 26); border-radius: 16px; }"
            "QListWidget#Categories { background: transparent; border: none; outline: none; }"
            "QListWidget#Categories::item { padding: 7px 10px; border-radius: 8px; color: #ECE6DE; }"
            "QListWidget#Categories::item:selected { background-color: rgba(232, 116, 59, 41); color: #FFCFB4; }"
            "QTreeView#Tree { background: transparent; border: none; outline: none; }"
            "QTreeView#Tree::branch { image: none; border: none; }"));
        setMinimumSize(880, 560);

        // Title row: "Add to <entity>", "Esc to close".
        auto titleRow = new QHBoxLayout();
        m_titleLabel = Hearth::MakeLabel(tr("Add a component"), QStringLiteral("title"), this);
        m_titleLabel->setStyleSheet(QStringLiteral("background: transparent; border: none; color: #ECE6DE; font-size: 15px; font-weight: 600;"));
        titleRow->addWidget(m_titleLabel, 1);
        titleRow->addWidget(Hearth::MakeLabel(tr("Esc to close"), QStringLiteral("dim"), this));
        outerLayout->addLayout(titleRow);

        // Search filter
        m_searchFrame = new QFrame(this);
        m_searchFrame->setObjectName("SearchFrame");
        m_searchFrame->setVisible(enableSearch);

        auto searchLayout = new QHBoxLayout(m_searchFrame);
        searchLayout->setSizeConstraint(QLayout::SetMinimumSize);

        m_searchText = new QLineEdit(m_searchFrame);
        m_searchText->setObjectName("SearchText");
        m_searchText->setText("");
        m_searchText->setPlaceholderText("Search...");
        m_searchText->setClearButtonEnabled(true);
        AzQtComponents::LineEdit::applySearchStyle(m_searchText);

        m_searchRegExp = QRegularExpression("", QRegularExpression::PatternOption::CaseInsensitiveOption);

        searchLayout->addWidget(m_searchText);
        m_resultCount = Hearth::MakeLabel(QString(), QStringLiteral("dim"), m_searchFrame);
        searchLayout->addWidget(m_resultCount);
        searchLayout->setContentsMargins(0, 0, 0, 0);
        m_searchFrame->setLayout(searchLayout);
        outerLayout->addWidget(m_searchFrame);

        m_componentModel = new ComponentPaletteModel(this);

        m_componentTree = new QTreeView(this);
        m_componentTree->setObjectName("Tree");
        m_componentTree->setModel(m_componentModel);
        m_componentTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_componentTree->setItemDelegate(new HearthComponentDelegate(m_componentTree));
        m_componentTree->setIndentation(0);
        m_componentTree->setRootIsDecorated(false);
        m_componentTree->setMouseTracking(true);
        m_componentTree->setIconSize(QSize(18, 18));

        // Body: categories with counts on the left, the components on the right.
        auto bodyRow = new QHBoxLayout();
        bodyRow->setSpacing(12);
        m_categoryList = new QListWidget(this);
        m_categoryList->setObjectName(QStringLiteral("Categories"));
        m_categoryList->setFixedWidth(200);
        m_categoryList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        connect(m_categoryList, &QListWidget::currentTextChanged, this, [this]()
        {
            if (QListWidgetItem* item = m_categoryList->currentItem())
            {
                ShowCategory(item->data(Qt::UserRole).toString());
            }
        });
        bodyRow->addWidget(m_categoryList);
        bodyRow->addWidget(m_componentTree, 1);
        outerLayout->addLayout(bodyRow, 1);

        // Footer: keys, Cancel, Add <name>.
        auto line = new QFrame(this);
        line->setFixedHeight(1);
        line->setStyleSheet(QStringLiteral("background-color: rgba(255, 255, 255, 15); border: none;"));
        outerLayout->addWidget(line);
        auto footer = new QHBoxLayout();
        footer->setSpacing(8);
        footer->addWidget(Hearth::MakeLabel(tr("↑ ↓ to move · Enter to add · Shift+Enter adds and keeps this open"), QStringLiteral("dim"), this), 1);
        QPushButton* cancel = Hearth::MakeButton(tr("Cancel"), false, this);
        connect(cancel, &QPushButton::clicked, this, &ComponentPaletteWidget::OnAddComponentCancel);
        connect(cancel, &QPushButton::clicked, this, &ComponentPaletteWidget::hide);
        footer->addWidget(cancel);
        m_addButton = Hearth::MakeButton(tr("Add"), true, this);
        connect(m_addButton, &QPushButton::clicked, this, [this]() { ActivateSelection(m_componentTree->currentIndex()); });
        footer->addWidget(m_addButton);
        outerLayout->addLayout(footer);

        //hide header for dropdown-style, single-column, tree
        m_componentTree->header()->hide();

        connect(m_searchText, &QLineEdit::textChanged, this, &ComponentPaletteWidget::QueueUpdateSearch);
        QToolButton* clearButton = AzQtComponents::LineEdit::getClearButton(m_searchText);
        assert(clearButton);
        connect(clearButton, &QToolButton::clicked, this, &ComponentPaletteWidget::ClearSearch);

        // Hearth: a click picks a component (the Add button names it); Enter or a double-click adds it.
        connect(m_componentTree, &QTreeView::activated, this, &ComponentPaletteWidget::ActivateSelection);
        connect(m_componentTree, &QTreeView::doubleClicked, this, &ComponentPaletteWidget::ActivateSelection);
        connect(m_componentTree, &QTreeView::expanded, this, &ComponentPaletteWidget::ExpandCategory);
        connect(m_componentTree, &QTreeView::collapsed, this, &ComponentPaletteWidget::CollapseCategory);

        auto actionToHideWindow = new QAction(tr("Hide Window"), this);
        actionToHideWindow->setShortcut(QKeySequence::Cancel);
        actionToHideWindow->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(actionToHideWindow, &QAction::triggered, this, &ComponentPaletteWidget::OnAddComponentCancel);
        connect(actionToHideWindow, &QAction::triggered, this, &ComponentPaletteWidget::hide);
        addAction(actionToHideWindow);

        auto actionToFocusSearchBox = new QAction(tr("Focus Search Box"), this);
        actionToFocusSearchBox->setShortcut(QKeySequence::Find);
        actionToFocusSearchBox->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(actionToFocusSearchBox, &QAction::triggered, this, &ComponentPaletteWidget::FocusSearchBox);
        addAction(actionToFocusSearchBox);

        installEventFilter(this);
        connect(m_componentTree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this]() { UpdateAddButton(); });
    }

    void ComponentPaletteWidget::Populate(
        AZ::SerializeContext* serializeContext,
        const AzToolsFramework::EntityIdList& selectedEntityIds,
        const AzToolsFramework::ComponentFilter& componentFilter,
        AZStd::span<const AZ::ComponentServiceType> serviceFilter,
        AZStd::span<const AZ::ComponentServiceType> incompatibleServiceFilter)
    {
        m_serializeContext = serializeContext;
        m_selectedEntityIds = selectedEntityIds;
        m_componentFilter = componentFilter;
        m_serviceFilter.assign_range(serviceFilter);
        m_incompatibleServiceFilter.assign_range(incompatibleServiceFilter);

        if (m_selectedEntityIds.size() == 1)
        {
            AZStd::string name;
            AZ::ComponentApplicationBus::BroadcastResult(name, &AZ::ComponentApplicationRequests::GetEntityName, m_selectedEntityIds.front());
            m_titleLabel->setText(tr("Add to \"%1\"").arg(QString::fromUtf8(name.c_str())));
        }
        else
        {
            m_titleLabel->setText(tr("Add to %n entities", nullptr, static_cast<int>(m_selectedEntityIds.size())));
        }

        UpdateContent();
        Present();
    }

    void ComponentPaletteWidget::UpdateContent()
    {
        AZ_PROFILE_FUNCTION(AzToolsFramework);
        m_componentModel->clear();

        bool applyRegExFilter = !m_searchRegExp.pattern().isEmpty();

        // Gather all components that match our filter and group by category.
        ComponentPaletteUtil::ComponentDataTable componentDataTable;
        ComponentPaletteUtil::ComponentIconTable componentIconTable;
        ComponentPaletteUtil::BuildComponentTables(
            m_serializeContext, 
            m_componentFilter, 
            m_serviceFilter, 
            m_incompatibleServiceFilter, 
            componentDataTable, 
            componentIconTable);

        AzFramework::Components::DeprecatedComponentsList deprecatedList;
        AzFramework::Components::DeprecatedComponentsRequestBus::Broadcast(&AzFramework::Components::DeprecatedComponentsRequestBus::Events::EnumerateDeprecatedComponents, deprecatedList);

        AZ::Entity::ComponentArrayType componentsOnEntity;

        // Get all components on all selected entities so we can display a count of used components by type
        AZStd::unordered_set<AZ::Component*> allComponentsOnSelectedEntities;
        for (AZ::EntityId entityId : m_selectedEntityIds)
        {
            componentsOnEntity.clear();
            AzToolsFramework::GetAllComponentsForEntity(entityId, componentsOnEntity);
            allComponentsOnSelectedEntities.insert(componentsOnEntity.begin(), componentsOnEntity.end());
        }

        // Populate the context menu.
        AZStd::map<QString, QStandardItem*> categoryItemMap;

        for (const auto& categoryPair : componentDataTable)
        {
            //get the full category name/path and split it by separators for iteration
            const QString& categoryPath = categoryPair.first;
            const QStringList& categoryPathSegments = categoryPath.split('/', Qt::SkipEmptyParts);
            QString categoryPathBuilder;

            //for every segment of the category path, create an expandable header
            auto parentItem = m_componentModel->invisibleRootItem();
            for (const QString& categoryName : categoryPathSegments)
            {
                categoryPathBuilder += categoryName + "/";

                QStandardItem* categoryItem = nullptr;
                auto categoryItemItr = categoryItemMap.find(categoryPathBuilder);
                if (categoryItemItr == categoryItemMap.end())
                {
                    categoryItem = new QStandardItem(categoryName);
                    categoryItem->setCheckable(false);
                    categoryItem->setEditable(false);
                    categoryItem->setSelectable(true);
                    categoryItem->setData((qulonglong)nullptr, Qt::ItemDataRole::UserRole + 1);

                    //make groups bold
                    QFont font = categoryItem->font();
                    font.setBold(true);
                    categoryItem->setFont(font);

                    parentItem->appendRow(categoryItem);

                    categoryItemMap[categoryPathBuilder] = categoryItem;
                }
                else
                {
                    categoryItem = categoryItemItr->second;
                }

                parentItem = categoryItem;
            }
        }

        for (const auto& [categoryName, componentMap] : componentDataTable)
        {
            auto categoryItemItr = categoryItemMap.find(categoryName + "/");
            auto parentItem = categoryItemItr != categoryItemMap.end() ? categoryItemItr->second : m_componentModel->invisibleRootItem();

            for (const auto& componentPair : componentMap)
            {
                auto componentClass = componentPair.second;
                const QString& componentName = componentPair.first;
                const QString& componentIconName = componentIconTable[componentClass];
                auto deprecatedInfo = deprecatedList.find(componentClass->m_typeId);
                bool componentIsDeprecated = deprecatedInfo != deprecatedList.end();
                if ((!applyRegExFilter || categoryName.contains(m_searchRegExp) || componentName.contains(m_searchRegExp))
                    && (!componentIsDeprecated || !deprecatedInfo->second.m_hideComponent))
                {
                    //count the number of components on selected entities that match this type
                    auto componentCount = AZStd::count_if(allComponentsOnSelectedEntities.begin(), allComponentsOnSelectedEntities.end(), [componentClass](const AZ::Component* component) {
                        return componentClass->m_typeId == component->GetUnderlyingComponentType();
                    });

                    //generate the display name for the component
                    QString displayName = componentName;
                    if (componentCount) //<append count if count > 0
                    {
                        displayName += QObject::tr(" (%1)").arg(componentCount);
                    }
                    if (componentIsDeprecated) //< append deprecation strings
                    {
                        displayName += deprecatedInfo->second.m_deprecationString.c_str();
                    }

                    auto componentItem = new QStandardItem(QIcon(componentIconName), displayName);
                    componentItem->setToolTip(componentClass->m_editData->m_description);
                    componentItem->setCheckable(false);
                    componentItem->setEditable(false);
                    componentItem->setSelectable(true);
                    componentItem->setData((qulonglong)componentClass, Qt::ItemDataRole::UserRole + 1);
                    componentItem->setData(categoryName, CategoryRole);
                    parentItem->appendRow(componentItem);
                }
            }
        }

        // If we have removed component items from the visible tree we need to prune the hanging titles.
        // We also need to auto expand all the items that contain children.
        for (int i = m_componentModel->rowCount() - 1; i >= 0; --i)
        {
            if (BranchHasNoChildren(m_componentModel->item(i)))
            {
                m_componentModel->removeRow(i);
            }
            else
            {
                SetExpanded(m_componentModel->index(i, 0));
            }
        }
        m_componentModel->sort(0);

        // Hearth: "Suggested" first: the project's own components and the ones added recently.
        QStringList recent;
        {
            QScopedPointer<QSettings> settings(Hearth::NewSettings());
            recent = settings->value(QStringLiteral("components/recent")).toStringList();
        }
        const QString project = ProjectCategory();
        auto* suggested = new QStandardItem(tr("Suggested"));
        suggested->setEditable(false);
        suggested->setData((qulonglong)nullptr, ComponentRole);
        for (int top = 0; top < m_componentModel->rowCount(); ++top)
        {
            QStandardItem* categoryItem = m_componentModel->item(top);
            QList<QStandardItem*> stack{ categoryItem };
            while (!stack.isEmpty())
            {
                QStandardItem* item = stack.takeLast();
                for (int c = 0; c < item->rowCount(); ++c)
                {
                    QStandardItem* child = item->child(c);
                    if (child->data(ComponentRole).toULongLong() == 0)
                    {
                        stack.push_back(child);
                        continue;
                    }
                    const auto* classData = reinterpret_cast<const AZ::SerializeContext::ClassData*>(child->data(ComponentRole).toULongLong());
                    const bool fromProject = !project.isEmpty() && child->data(CategoryRole).toString().startsWith(project);
                    const bool used = classData && recent.contains(QString::fromUtf8(classData->m_name));
                    if (fromProject || used)
                    {
                        QStandardItem* copy = child->clone();
                        suggested->appendRow(copy);
                    }
                }
            }
        }
        if (suggested->rowCount() > 0)
        {
            m_componentModel->insertRow(0, suggested);
        }
        else
        {
            delete suggested;
        }
        m_componentTree->expandAll();
        RebuildCategories();
        ShowCategory(m_currentCategory);
    }

    void ComponentPaletteWidget::RebuildCategories()
    {
        const QSignalBlocker blocker(m_categoryList);
        m_categoryList->clear();
        int total = 0;
        for (int top = 0; top < m_componentModel->rowCount(); ++top)
        {
            QStandardItem* item = m_componentModel->item(top);
            int count = 0;
            QList<QStandardItem*> stack{ item };
            while (!stack.isEmpty())
            {
                QStandardItem* next = stack.takeLast();
                for (int c = 0; c < next->rowCount(); ++c)
                {
                    QStandardItem* child = next->child(c);
                    if (child->data(ComponentRole).toULongLong() != 0)
                    {
                        ++count;
                    }
                    else
                    {
                        stack.push_back(child);
                    }
                }
            }
            if (item->text() != tr("Suggested"))
            {
                total += count;
            }
            auto* row = new QListWidgetItem(QStringLiteral("%1\t%2").arg(item->text()).arg(count), m_categoryList);
            row->setData(Qt::UserRole, item->text());
            row->setText(item->text() + QStringLiteral("   ") + QString::number(count));
        }
        auto* all = new QListWidgetItem(tr("Everything") + QStringLiteral("   ") + QString::number(total));
        all->setData(Qt::UserRole, QString());
        m_categoryList->insertItem(m_categoryList->count() > 0 && m_componentModel->item(0)->text() == tr("Suggested") ? 1 : 0, all);
        if (m_resultCount)
        {
            m_resultCount->setText(tr("%n result(s)", nullptr, total));
        }
        // Keep the category the user picked; else Suggested (or everything while searching).
        int pick = -1;
        for (int i = 0; i < m_categoryList->count(); ++i)
        {
            if (m_categoryList->item(i)->data(Qt::UserRole).toString() == m_currentCategory)
            {
                pick = i;
            }
        }
        if (pick < 0 || !m_searchRegExp.pattern().isEmpty())
        {
            pick = m_searchRegExp.pattern().isEmpty() ? 0 : m_categoryList->row(all);
            m_currentCategory = m_categoryList->item(pick)->data(Qt::UserRole).toString();
        }
        m_categoryList->setCurrentRow(pick);
    }

    void ComponentPaletteWidget::ShowCategory(const QString& category)
    {
        m_currentCategory = category;
        for (int top = 0; top < m_componentModel->rowCount(); ++top)
        {
            const QString name = m_componentModel->item(top)->text();
            // "Everything" shows every category except the Suggested copies.
            const bool show = category.isEmpty() ? name != tr("Suggested") : name == category;
            m_componentTree->setRowHidden(top, QModelIndex(), !show);
        }
        // Pick the first component that's showing.
        for (int top = 0; top < m_componentModel->rowCount(); ++top)
        {
            if (m_componentTree->isRowHidden(top, QModelIndex()))
            {
                continue;
            }
            QModelIndex index = m_componentModel->index(top, 0);
            while (index.isValid() && m_componentModel->rowCount(index) > 0)
            {
                index = m_componentModel->index(0, 0, index);
            }
            if (index.isValid())
            {
                m_componentTree->setCurrentIndex(index);
            }
            break;
        }
        UpdateAddButton();
    }

    void ComponentPaletteWidget::UpdateAddButton()
    {
        if (!m_addButton)
        {
            return;
        }
        const QModelIndex index = m_componentTree->currentIndex();
        const bool isComponent = index.isValid() && index.data(ComponentRole).toULongLong() != 0;
        m_addButton->setEnabled(isComponent);
        QString name = isComponent ? index.data(Qt::DisplayRole).toString() : QString();
        name = name.section(QStringLiteral(" ("), 0, 0); // drop the "(2)" count
        m_addButton->setText(isComponent ? tr("Add %1").arg(name) : tr("Add"));
    }

    void ComponentPaletteWidget::RememberRecent(const QString& componentName)
    {
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        QStringList recent = settings->value(QStringLiteral("components/recent")).toStringList();
        recent.removeAll(componentName);
        recent.prepend(componentName);
        while (recent.size() > 6)
        {
            recent.removeLast();
        }
        settings->setValue(QStringLiteral("components/recent"), recent);
    }

    bool ComponentPaletteWidget::BranchHasNoChildren(QStandardItem* item)
    {
        bool returnVal = true;

        // We have to check all items to completely remove all non-parent items. Hence no early-exit.
        for (int i = item->rowCount() - 1; i >= 0 ; --i)
        {
            QStandardItem* it = item->child(i);
            // Check in reverse so removing a row doesn't affect remaining items.
            if (it->data(Qt::ItemDataRole::UserRole + 1).toULongLong())
            {
                // if this node has data then the parent has children.
                returnVal = false;
            }
            else
            {
                // If this item has no children, prune it.
                if (BranchHasNoChildren(it))
                {
                    item->removeRow(i);
                }
                else
                {
                    SetExpanded(it->index());
                    returnVal = false;
                }
            }
        }
        return returnVal;
    }

    void ComponentPaletteWidget::SetExpanded(QModelIndex itemIndex)
    {
        auto label = m_componentModel->data(itemIndex, Qt::ItemDataRole::DisplayRole).toString();
        auto stateItr = m_categoryExpandedState.find(label);
        auto expand = stateItr == m_categoryExpandedState.end() || stateItr->second;
        m_componentTree->setExpanded(itemIndex, expand);
    }


    void ComponentPaletteWidget::Present()
    {
        layout()->setEnabled(true);
        layout()->update();
        layout()->activate();

        raise();
        show();

        FocusSearchBox();
    }

    void ComponentPaletteWidget::QueueUpdateSearch()
    {
        QTimer::singleShot(1, this, &ComponentPaletteWidget::UpdateSearch);
    }

    void ComponentPaletteWidget::UpdateSearch()
    {
        AZ_PROFILE_FUNCTION(AzToolsFramework);
        m_searchRegExp = QRegularExpression(m_searchText->text(), QRegularExpression::PatternOption::CaseInsensitiveOption);
        m_searchText->setFocus();
        UpdateContent();
    }

    void ComponentPaletteWidget::ClearSearch()
    {
        m_searchText->setText("");
        QueueUpdateSearch();
    }

    void ComponentPaletteWidget::ActivateSelection(const QModelIndex& index)
    {
        if (index.isValid())
        {
            auto componentClass = reinterpret_cast<const AZ::SerializeContext::ClassData*>(m_componentModel->data(index, Qt::ItemDataRole::UserRole + 1).toULongLong());
            if (componentClass)
            {
                emit OnAddComponentBegin();

                EntityCompositionRequestBus::Broadcast(&EntityCompositionRequests::AddComponentsToEntities, m_selectedEntityIds, AZ::ComponentTypeList{ componentClass->m_typeId });

                emit OnAddComponentEnd();

                RememberRecent(QString::fromUtf8(componentClass->m_name));
                if (QApplication::keyboardModifiers() & Qt::ShiftModifier)
                {
                    UpdateContent(); // Shift+Enter: added; keep the picker open for the next one
                    return;
                }
                ClearSearch();
                hide();
            }
        }
    }

    void ComponentPaletteWidget::ExpandCategory(const QModelIndex& index)
    {
        if (index.isValid())
        {
            auto label = m_componentModel->data(index, Qt::ItemDataRole::DisplayRole).toString();
            m_categoryExpandedState[label] = true;
        }
    }

    void ComponentPaletteWidget::CollapseCategory(const QModelIndex& index)
    {
        if (index.isValid())
        {
            auto label = m_componentModel->data(index, Qt::ItemDataRole::DisplayRole).toString();
            m_categoryExpandedState[label] = false;
        }
    }

    void ComponentPaletteWidget::focusOutEvent(QFocusEvent *event)
    {
        hide();
        QFrame::focusOutEvent(event);
    }

    void ComponentPaletteWidget::FocusSearchBox()
    {
        if (m_searchFrame->isVisible())
        {
            if (!m_searchText->hasFocus())
            {
                m_searchText->setFocus();
            }
        }
        else
        {
            FocusComponentTree();
        }
    }

    void ComponentPaletteWidget::FocusComponentTree()
    {
        if (!m_componentTree->hasFocus())
        {
            m_componentTree->setFocus();
            // Focus the first actual component (leaf node)
            QModelIndex indexToSelect = m_componentModel->index(0, 0);
            while (indexToSelect.isValid() && m_componentModel->rowCount(indexToSelect) > 0)
            {
                indexToSelect = indexToSelect.model()->index(0, 0, indexToSelect);
            }
            m_componentTree->setCurrentIndex(indexToSelect);
        }
    }

    //overridden to intercept key events to move between filter and tree
    bool ComponentPaletteWidget::eventFilter(QObject* object, QEvent* event)
    {
        if (event->type() != QEvent::KeyPress)
        {
            return false;
        }

        if (object != this &&
            object != m_searchText &&
            object != m_componentTree)
        {
            return false;
        }

        if (!hasFocus() &&
            !m_searchText->hasFocus() &&
            !m_componentTree->hasFocus())
        {
            return false;
        }

        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Down ||
            keyEvent->key() == Qt::Key_Return ||
            keyEvent->key() == Qt::Key_Enter)
        {
            FocusComponentTree();
            return false;
        }
        if (keyEvent->key() == Qt::Key_Up)
        {
            if (!m_componentTree->hasFocus() ||
                m_componentTree->selectionModel()->selectedIndexes().empty() ||
                m_componentTree->selectionModel()->selectedIndexes().front().row() == 0)
            {
                FocusSearchBox();
                return false;
            }
        }
        return false;
    }
}

