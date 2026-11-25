/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <AzCore/std/smart_ptr/make_shared.h>
#include <AzFramework/Viewport/ViewportScreen.h>
#include <AzToolsFramework/Viewport/ViewportMessages.h>
#include <AzToolsFramework/ViewportSelection/EditorSelectionUtil.h>
#include <AzToolsFramework/ViewportUi/ViewportUiCluster.h>
#include <AzToolsFramework/ViewportUi/ViewportUiDisplay.h>
#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/Interface/Interface.h>
#include <AzToolsFramework/ActionManager/Action/ActionManagerInterface.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Viewport/ViewportMessages.h>
#include <QHBoxLayout>
#include <QDateTime>
#include <QMenu>
#include <QPainter>
#include <QSlider>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cstdio>
#include <cmath>
#include <functional>
#include <AzToolsFramework/API/SettingsRegistryUtils.h>
#include <AzCore/Settings/SettingsRegistry.h>
#include <AzCore/Console/IConsole.h>
#include <AzCore/Interface/Interface.h>
#include <AzToolsFramework/Viewport/ViewportSettings.h>
#include <AzToolsFramework/ViewportUi/ViewportUiDisplayLayout.h>
#include <AzToolsFramework/ViewportUi/ViewportUiSwitcher.h>
#include <AzToolsFramework/ViewportUi/ViewportUiTextField.h>
#include <QWidget>

namespace AzToolsFramework::ViewportUi::Internal
{
    const static int HighlightBorderSize = ViewportUiLeftRightBottomBorderSize;
    const static char* const HighlightBorderColor = "#E8743B";
    const static int HighlightBorderBackButtonIconSize = 20;
    const static char* const HighlightBorderBackButtonIconFile = "X_axis.svg";

    ViewportUiElementInfo::ViewportUiElementInfo() = default;
    ViewportUiElementInfo::~ViewportUiElementInfo() = default;

    ViewportUiElementInfo::ViewportUiElementInfo(AZStd::shared_ptr<QWidget> widget,
        ViewportUiElementId elementId, bool anchored)
        : m_widget(AZStd::move(widget))
        , m_viewportUiElementId(elementId)
        , m_anchored(anchored)
    {
    }

    static void UnparentWidgets(ViewportUiElementIdInfoLookup& viewportUiElementIdInfoLookup)
    {
        for (auto& element : viewportUiElementIdInfoLookup)
        {
            if (element.second.m_widget)
            {
                element.second.m_widget->setParent(nullptr);
            }
        }
    }

    static Qt::Alignment GetQtAlignment(const Alignment alignment)
    {
        switch (alignment)
        {
        case Alignment::TopRight:
            return Qt::AlignTop | Qt::AlignRight;
        case Alignment::TopLeft:
            return Qt::AlignTop | Qt::AlignLeft;
        case Alignment::BottomRight:
            return Qt::AlignBottom | Qt::AlignRight;
        case Alignment::BottomLeft:
            return Qt::AlignBottom | Qt::AlignLeft;
        case Alignment::Top:
            return Qt::AlignTop;
        case Alignment::Bottom:
            return Qt::AlignBottom;
        }

        AZ_Assert(false, "ViewportUI", "Unhandled ViewportUI Alignment %d", static_cast<int>(alignment));
        return Qt::AlignTop;
    }

    ViewportUiDisplay::ViewportUiDisplay(QWidget* parent, QWidget* renderOverlay)
        : m_renderOverlay(renderOverlay)
        , m_uiMainWindow(parent)
        , m_uiOverlay(parent)
        , m_fullScreenLayout(&m_uiOverlay)
        , m_uiOverlayLayout()
        , m_viewportBorderText(&m_uiOverlay)
        , m_viewportBorderBackButton(&m_uiOverlay)
    {
        AzFramework::ViewportImGuiNotificationBus::Handler::BusConnect();
    }

    ViewportUiDisplay::~ViewportUiDisplay()
    {
        AzFramework::ViewportImGuiNotificationBus::Handler::BusDisconnect();
        UnparentWidgets(m_viewportUiElements);
    }

    void ViewportUiDisplay::AddCluster(AZStd::shared_ptr<ButtonGroup> buttonGroup, const Alignment alignment)
    {
        if (!buttonGroup.get())
        {
            return;
        }

        auto viewportUiCluster = AZStd::make_shared<ViewportUiCluster>(buttonGroup);
        auto id = AddViewportUiElement(viewportUiCluster);
        buttonGroup->SetViewportUiElementId(id);
        PositionViewportUiElementAnchored(id, GetQtAlignment(alignment));
    }

    void ViewportUiDisplay::AddClusterButton(const ViewportUiElementId clusterId, Button* button)
    {
        if (auto viewportUiCluster = qobject_cast<ViewportUiCluster*>(GetViewportUiElement(clusterId).get()))
        {
            viewportUiCluster->RegisterButton(button);
        }
    }

    void ViewportUiDisplay::SetClusterButtonLocked(const ViewportUiElementId clusterId, const ButtonId buttonId, const bool isLocked)
    {
        if (auto viewportUiCluster = qobject_cast<ViewportUiCluster*>(GetViewportUiElement(clusterId).get()))
        {
            viewportUiCluster->SetButtonLocked(buttonId, isLocked);
        }
    }

    void ViewportUiDisplay::SetClusterButtonTooltip(
        const ViewportUiElementId clusterId, const ButtonId buttonId, const AZStd::string& tooltip)
    {
        if (auto viewportUiCluster = qobject_cast<ViewportUiCluster*>(GetViewportUiElement(clusterId).get()))
        {
            viewportUiCluster->SetButtonTooltip(buttonId, tooltip);
        }
    }

    void ViewportUiDisplay::RemoveClusterButton(ViewportUiElementId clusterId, ButtonId buttonId)
    {
        if (auto cluster = qobject_cast<ViewportUiCluster*>(GetViewportUiElement(clusterId).get()))
        {
            cluster->RemoveButton(buttonId);
        }
    }

    void ViewportUiDisplay::UpdateCluster(ViewportUiElementId clusterId)
    {
        if (auto cluster = qobject_cast<ViewportUiCluster*>(GetViewportUiElement(clusterId).get()))
        {
            cluster->Update();
        }
    }

    void ViewportUiDisplay::AddSwitcher(AZStd::shared_ptr<ButtonGroup> buttonGroup, const Alignment alignment)
    {
        if (!buttonGroup.get())
        {
            return;
        }

        auto viewportUiSwitcher = AZStd::make_shared<ViewportUiSwitcher>(buttonGroup);
        auto id = AddViewportUiElement(viewportUiSwitcher);
        buttonGroup->SetViewportUiElementId(id);
        PositionViewportUiElementAnchored(id, GetQtAlignment(alignment));
    }

    void ViewportUiDisplay::AddSwitcherButton(const ViewportUiElementId switcherId, Button* button)
    {
        if (auto switcher = qobject_cast<ViewportUiSwitcher*>(GetViewportUiElement(switcherId).get()))
        {
            switcher->AddButton(button);
        }
    }

    void ViewportUiDisplay::RemoveSwitcherButton(ViewportUiElementId switcherId, ButtonId buttonId)
    {
        if (auto switcher = qobject_cast<ViewportUiSwitcher*>(GetViewportUiElement(switcherId).get()))
        {
            switcher->RemoveButton(buttonId);
        }
    }

    void ViewportUiDisplay::UpdateSwitcher(ViewportUiElementId switcherId)
    {
        if (auto switcher = qobject_cast<ViewportUiSwitcher*>(GetViewportUiElement(switcherId).get()))
        {
            switcher->Update();
        }
    }

    void ViewportUiDisplay::SetSwitcherActiveButton(ViewportUiElementId switcherId, ButtonId buttonId)
    {
        if (auto viewportUiSwitcher = qobject_cast<ViewportUiSwitcher*>(GetViewportUiElement(switcherId).get()))
        {
            viewportUiSwitcher->SetActiveButton(buttonId);
        }
    }

    void ViewportUiDisplay::SetSwitcherButtonTooltip(
        const ViewportUiElementId switcherId, const ButtonId buttonId, const AZStd::string& tooltip)
    {
        if (auto viewportUiSwitcher = qobject_cast<ViewportUiSwitcher*>(GetViewportUiElement(switcherId).get()))
        {
            viewportUiSwitcher->SetButtonTooltip(buttonId, tooltip);
        }
    }

    void ViewportUiDisplay::AddTextField(AZStd::shared_ptr<TextField> textField)
    {
        if (!textField.get())
        {
            return;
        }

        auto viewportUiTextField = AZStd::make_shared<ViewportUiTextField>(textField);
        auto id = AddViewportUiElement(viewportUiTextField);
        textField->m_viewportId = id;
        PositionViewportUiElementAnchored(id, Qt::AlignTop | Qt::AlignLeft);
    }

    void ViewportUiDisplay::UpdateTextField(const ViewportUiElementId textFieldId)
    {
        if (auto textField = qobject_cast<ViewportUiTextField*>(GetViewportUiElement(textFieldId).get()))
        {
            textField->Update();
        }
    }

    void ViewportUiDisplay::Update()
    {
        for (const auto& element : m_viewportUiElements)
        {
            const ViewportUiElementInfo& elementInfo = element.second;
            if (!elementInfo.m_anchored)
            {
                const auto screenPoint =
                    AzFramework::WorldToScreen(elementInfo.m_worldPosition, AzToolsFramework::GetCameraState(m_viewportId));
                elementInfo.m_widget->move(screenPoint.m_x, screenPoint.m_y);
            }
        }

        PositionUiOverlayOverRenderViewport();
        UpdateHearthOverlay();
    }

    ViewportUiElementId ViewportUiDisplay::AddViewportUiElement(AZStd::shared_ptr<QWidget> widget)
    {
        if (!widget)
        {
            return InvalidViewportUiElementId;
        }

        ViewportUiElementId newId = ViewportUiElementId(++m_numViewportElements);
        ViewportUiElementInfo newElement{ widget, newId, true };
        m_viewportUiElements.insert({ newId, newElement });
        return newId;
    }

    AZStd::shared_ptr<QWidget> ViewportUiDisplay::GetViewportUiElement(ViewportUiElementId elementId)
    {
        if (auto element = m_viewportUiElements.find(elementId); element != m_viewportUiElements.end())
        {
            return element->second.m_widget;
        }

        return nullptr;
    }

    ViewportUiElementId ViewportUiDisplay::GetViewportUiElementId(QPointer<QWidget> widget)
    {
        if (auto element = AZStd::find_if(
                m_viewportUiElements.begin(), m_viewportUiElements.end(),
                [widget](const auto& it)
                {
                    return it.second.m_widget.get() == widget;
                });
            element != m_viewportUiElements.end())
        {
            return element->second.m_viewportUiElementId;
        }

        return InvalidViewportUiElementId;
    }

    void ViewportUiDisplay::RemoveViewportUiElement(ViewportUiElementId elementId)
    {
        AZ_Assert(
            elementId != AzToolsFramework::ViewportUi::InvalidViewportUiElementId,
            "Tried to remove a Viewport UI element using an invalid or removed ViewportUiElementId.");

        auto viewportUiMapElement = m_viewportUiElements.find(elementId);
        if (viewportUiMapElement != m_viewportUiElements.end())
        {
            viewportUiMapElement->second.m_widget->hide();
            viewportUiMapElement->second.m_widget->setParent(nullptr);
            m_viewportUiElements.erase(viewportUiMapElement);
        }
    }

    bool ViewportUiDisplay::UiDisplayEnabled() const
    {
        return m_renderOverlay && m_renderOverlay->isVisible();
    }

    void ViewportUiDisplay::ShowViewportUiElement(ViewportUiElementId elementId)
    {
        if (ViewportUiElementInfo element = GetViewportUiElementInfo(elementId); element.m_widget)
        {
            element.m_widget->show();
        }
    }

    void ViewportUiDisplay::HideViewportUiElement(ViewportUiElementId elementId)
    {
        if (ViewportUiElementInfo element = GetViewportUiElementInfo(elementId); element.m_widget)
        {
            element.m_widget->hide();
        }
    }

    bool ViewportUiDisplay::IsViewportUiElementVisible(ViewportUiElementId elementId)
    {
        if (ViewportUiElementInfo element = GetViewportUiElementInfo(elementId); element.m_widget)
        {
            return element.IsValid() && element.m_widget->isVisible();
        }

        return false;
    }

    QMargins ViewportUiDisplay::ViewportElementMargins() const
    {
        if (m_imGuiActive)
        {
            return GetViewportBorderVisible() ? ViewportUiOverlayImGuiBorderMargin : ViewportUiOverlayImGuiMargin;
        }
        else
        {
            return GetViewportBorderVisible() ? ViewportUiOverlayBorderMargin : ViewportUiOverlayDefaultMargin;
        }
    }

    void ViewportUiDisplay::CreateViewportBorder(
        const AZStd::string& borderTitle, AZStd::optional<ViewportUiBackButtonCallback> backButtonCallback)
    {
        m_uiOverlay.setStyleSheet(QString("#ViewportUiOverlay { border: %1px solid %2; border-top: %3px solid %4; }")
                                      .arg(
                                          QString::number(HighlightBorderSize), HighlightBorderColor,
                                          QString::number(ViewportUiTopBorderSize), HighlightBorderColor));

        m_viewportBorderText.setAlignment(Qt::AlignCenter);

        m_viewportBorderText.show();
        m_uiOverlayLayout.setContentsMargins(ViewportElementMargins());
        ChangeViewportBorderText(borderTitle.c_str());

        // only display the back button if a callback was provided
        m_viewportBorderBackButtonCallback = backButtonCallback;
        m_viewportBorderBackButton.setVisible(m_viewportBorderBackButtonCallback.has_value());
    }

    void ViewportUiDisplay::ChangeViewportBorderText(const char* borderTitle)
    {
        // when the text changes if the width is different it will flicker as it changes,
        // this sets the width to the entire overlay to avoid that
        m_viewportBorderText.setFixedWidth(m_uiOverlay.width());
        m_viewportBorderText.setText(borderTitle);
    }

    void ViewportUiDisplay::OnImGuiActivated()
    {
        m_imGuiActive = true;
        m_uiOverlayLayout.setContentsMargins(ViewportElementMargins());
    };

    void ViewportUiDisplay::OnImGuiDeactivated()
    {
        m_imGuiActive = false;
        m_uiOverlayLayout.setContentsMargins(ViewportElementMargins());
    };

    void ViewportUiDisplay::RemoveViewportBorder()
    {
        m_viewportBorderText.hide();
        m_uiOverlay.setStyleSheet("#ViewportUiOverlay { border: none; }");
        m_uiOverlayLayout.setContentsMargins(ViewportElementMargins());
        m_viewportBorderBackButtonCallback.reset();
        m_viewportBorderBackButton.hide();
    }

    bool ViewportUiDisplay::GetViewportBorderVisible() const
    {
        return m_viewportBorderText.isVisible();
    }

    void ViewportUiDisplay::PositionViewportUiElementFromWorldSpace(ViewportUiElementId elementId, const AZ::Vector3& pos)
    {
        auto viewportUiMapElement = m_viewportUiElements.find(elementId);

        if (viewportUiMapElement != m_viewportUiElements.end() && viewportUiMapElement->second.m_widget)
        {
            viewportUiMapElement->second.m_anchored = false;
            viewportUiMapElement->second.m_worldPosition = pos;

            SetUiOverlayContents(viewportUiMapElement->second.m_widget.get());
        }
    }

    void ViewportUiDisplay::PositionViewportUiElementAnchored(ViewportUiElementId elementId, const Qt::Alignment alignment)
    {
        auto viewportUiMapElement = m_viewportUiElements.find(elementId);

        if (viewportUiMapElement != m_viewportUiElements.end() && viewportUiMapElement->second.m_widget)
        {
            viewportUiMapElement->second.m_anchored = true;
            SetUiOverlayContentsAnchored(viewportUiMapElement->second.m_widget.get(), alignment);
        }
    }

    // disables system background for widget and gives a transparent background
    static void ConfigureWindowForViewportUi(QPointer<QMainWindow> mainWindow)
    {
        // no background for the widget else each set of buttons/text-fields/etc would have a black box around them
        SetTransparentBackground(mainWindow);
        mainWindow->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    }

    void ViewportUiDisplay::InitializeUiOverlay()
    {
        m_uiMainWindow.setObjectName(QString("ViewportUiWindow"));
        ConfigureWindowForViewportUi(&m_uiMainWindow);
        m_uiMainWindow.hide();

        m_uiOverlay.setObjectName(QString("ViewportUiOverlay"));
        m_uiMainWindow.setCentralWidget(&m_uiOverlay);
        m_uiOverlay.hide();

        // remove any spacing and margins from the UI Overlay Layout
        m_fullScreenLayout.setSpacing(0);
        m_fullScreenLayout.setContentsMargins(0, 0, 0, 0);
        m_fullScreenLayout.addLayout(&m_uiOverlayLayout, 0, 0, 1, 1);

        // style the label which will appear on top of the highlight border
        m_viewportBorderText.setStyleSheet(QString("background-color: %1; border: none").arg(HighlightBorderColor));
        m_viewportBorderText.setFixedHeight(ViewportUiTopBorderSize);
        m_viewportBorderText.hide();
        m_fullScreenLayout.addWidget(&m_viewportBorderText, 0, 0, Qt::AlignTop | Qt::AlignHCenter);

        m_viewportBorderBackButton.setAutoRaise(true); // hover highlight
        m_viewportBorderBackButton.setFixedHeight(ViewportUiTopBorderSize);
        m_viewportBorderBackButton.hide();

        QIcon backButtonIcon(QString(":/stylesheet/img/UI20/toolbar/%1").arg(HighlightBorderBackButtonIconFile));
        m_viewportBorderBackButton.setIcon(backButtonIcon);
        m_viewportBorderBackButton.setIconSize(QSize(HighlightBorderBackButtonIconSize, HighlightBorderBackButtonIconSize));

        // setup the handler for the back button to call the user provided callback (if any)
        QObject::connect(
            &m_viewportBorderBackButton, &QToolButton::clicked,
            [this]
            {
                if (m_viewportBorderBackButtonCallback.has_value())
                {
                    // we need to swap out the existing back button callback because it will be reset in RemoveViewportBorder()
                    // so preserve the lifetime with this temporary callback until after the call to RemoveViewportBorder()
                    AZStd::optional<ViewportUiBackButtonCallback> backButtonCallback;
                    m_viewportBorderBackButtonCallback.swap(backButtonCallback);
                    RemoveViewportBorder();
                    (*backButtonCallback)();
                }
            });
        m_fullScreenLayout.addWidget(&m_viewportBorderBackButton, 0, 0, Qt::AlignTop | Qt::AlignRight);

        InitializeHearthOverlay();
    }

    namespace
    {
        // Solid colours (no see-through): the overlay is its own window over the render, and see-through parts kept
        // old pixels on some desktops (a second "Lit Wireframe" ghost next to the real buttons).
        const char* HearthPillStyle =
            "QWidget#HearthPill { background-color: #0D0C0B; border: 1px solid #262320; border-radius: 16px; }"
            "QPushButton { background-color: #0D0C0B; color: #ECE6DE; border: 1px solid #262320; border-radius: 13px;"
            " padding: 3px 12px; font-size: 12px; }"
            "QPushButton:hover { background-color: #1C1A17; }"
            "QPushButton:checked { background-color: #3A2216; color: #FFCFB4; border-color: #8A4A2A; }"
            "QPushButton::menu-indicator { image: none; width: 0px; }";

        void TriggerEditorAction(const char* identifier)
        {
            if (auto* actions = AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get())
            {
                actions->TriggerAction(identifier);
            }
        }

        constexpr AZStd::string_view HearthCameraSpeedSetting = "/Amazon/Preferences/Editor/Camera/TranslateSpeed";

        //! Bottom right: which way the camera looks (N up) and how fast it flies; scroll on it to change the speed.
        class HearthCompass : public QWidget
        {
        public:
            explicit HearthCompass(QWidget* parent, AzFramework::ViewportId viewportId)
                : QWidget(parent)
                , m_viewportId(viewportId)
            {
                setFixedSize(46, 46);
                setToolTip(QObject::tr("Which way the camera looks. Scroll here to change the camera speed."));
            }
            float m_yaw = 0.0f;
            std::function<void()> m_speedChanged;

        protected:
            void paintEvent(QPaintEvent*) override
            {
                QPainter p(this);
                p.setRenderHint(QPainter::Antialiasing);
                const QRectF r = QRectF(rect()).adjusted(2, 2, -2, -2);
                p.setPen(QPen(QColor(255, 255, 255, 30), 1));
                p.setBrush(QColor(13, 12, 11, 220));
                p.drawEllipse(r);
                p.translate(r.center());
                p.rotate(-m_yaw * 180.0 / 3.14159265);
                QPolygonF needle({ QPointF(0, -r.height() * 0.38), QPointF(5, 0), QPointF(-5, 0) });
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0xE8, 0x74, 0x3B));
                p.drawPolygon(needle);
                p.setBrush(QColor(0xEC, 0xE6, 0xDE, 120));
                p.drawPolygon(QPolygonF({ QPointF(0, r.height() * 0.38), QPointF(5, 0), QPointF(-5, 0) }));
                p.resetTransform();
                p.setPen(QColor(0xEC, 0xE6, 0xDE));
                QFont font = p.font();
                font.setPixelSize(9);
                p.setFont(font);
                p.drawText(QRectF(r.left(), r.top() + 1, r.width(), 10), Qt::AlignHCenter, QStringLiteral("N"));
            }
            void wheelEvent(QWheelEvent* event) override
            {
                const double speed = AzToolsFramework::GetRegistry(HearthCameraSpeedSetting, 10.0);
                const double next = qBound(0.5, speed * (event->angleDelta().y() > 0 ? 1.25 : 0.8), 200.0);
                AzToolsFramework::SetRegistry(HearthCameraSpeedSetting, next);
                if (m_speedChanged)
                {
                    m_speedChanged();
                }
            }

        private:
            [[maybe_unused]] AzFramework::ViewportId m_viewportId;
        };

        QPushButton* PillButton(QWidget* parent, const QString& text, bool checkable)
        {
            auto* button = new QPushButton(text, parent);
            button->setCheckable(checkable);
            button->setCursor(Qt::PointingHandCursor);
            button->setFocusPolicy(Qt::NoFocus);
            return button;
        }
    } // namespace

    void ViewportUiDisplay::InitializeHearthOverlay()
    {
        // Top centre: snapping and what the viewport shows.
        m_hearthTopBar = new QWidget(&m_uiOverlay);
        m_hearthTopBar->setObjectName(QStringLiteral("HearthPill"));
        m_hearthTopBar->setAttribute(Qt::WA_StyledBackground);
        m_hearthTopBar->setStyleSheet(QLatin1String(HearthPillStyle));
        auto* top = new QHBoxLayout(m_hearthTopBar);
        top->setContentsMargins(6, 4, 6, 4);
        top->setSpacing(4);
        m_hearthGridSnap = PillButton(m_hearthTopBar, QObject::tr("Snap"), true);
        m_hearthAngleSnap = PillButton(m_hearthTopBar, QObject::tr("15°"), true);
        m_hearthGrid = PillButton(m_hearthTopBar, QObject::tr("Grid"), true);
        m_hearthIcons = PillButton(m_hearthTopBar, QObject::tr("Icons"), false);
        QObject::connect(m_hearthGridSnap, &QPushButton::clicked, [] { TriggerEditorAction("o3de.action.edit.snap.toggleGridSnapping"); });
        QObject::connect(m_hearthAngleSnap, &QPushButton::clicked, [] { TriggerEditorAction("o3de.action.edit.snap.toggleAngleSnapping"); });
        QObject::connect(m_hearthGrid, &QPushButton::clicked, [] { TriggerEditorAction("o3de.action.edit.snap.toggleShowingGrid"); });
        QObject::connect(m_hearthIcons, &QPushButton::clicked, [] { TriggerEditorAction("o3de.action.view.toggleIcons"); });
        top->addWidget(m_hearthGridSnap);
        top->addWidget(m_hearthAngleSnap);
        top->addWidget(m_hearthGrid);
        top->addWidget(m_hearthIcons);
        // Chunks: borders of the endless level's chunks while it generates (the game draws them; see /Hearth/Viewport).
        QPushButton* chunks = PillButton(m_hearthTopBar, QObject::tr("Chunks"), true);
        chunks->setToolTip(QObject::tr("Show where each generated chunk starts and ends"));
        {
            bool showChunks = false;
            if (auto* registry = AZ::SettingsRegistry::Get())
            {
                registry->Get(showChunks, "/Hearth/Viewport/ShowChunks");
            }
            chunks->setChecked(showChunks);
        }
        QObject::connect(chunks, &QPushButton::toggled, [](bool on)
        {
            if (auto* registry = AZ::SettingsRegistry::Get())
            {
                registry->Set("/Hearth/Viewport/ShowChunks", on);
            }
        });
        top->addWidget(chunks);
        m_uiOverlayLayout.AddAnchoredWidget(m_hearthTopBar, Qt::AlignTop);

        // Bottom left: what's selected, where, and the frame rate.
        m_hearthStatus = new QLabel(&m_uiOverlay);
        m_hearthStatus->setStyleSheet(QStringLiteral(
            "background-color: #0D0C0B; color: #ECE6DE; border: 1px solid #262320; border-radius: 13px;"
            " padding: 5px 12px; font-family: \"Geist Mono\"; font-size: 12px;"));
        m_uiOverlayLayout.AddAnchoredWidget(m_hearthStatus, Qt::AlignBottom | Qt::AlignLeft);

        // Bottom right: Focus (F) and Play here.
        m_hearthActions = new QWidget(&m_uiOverlay);
        m_hearthActions->setObjectName(QStringLiteral("HearthPill"));
        m_hearthActions->setAttribute(Qt::WA_StyledBackground);
        m_hearthActions->setStyleSheet(QLatin1String(HearthPillStyle) + QStringLiteral(
            "QPushButton#PlayHere { background-color: #E8743B; color: #1A0F08; font-weight: 600; border: none; }"
            "QPushButton#PlayHere:hover { background-color: #F08A55; }"));
        auto* actions = new QHBoxLayout(m_hearthActions);
        actions->setContentsMargins(4, 4, 4, 4);
        actions->setSpacing(4);
        QPushButton* focus = PillButton(m_hearthActions, QObject::tr("Focus  F"), false);
        QObject::connect(focus, &QPushButton::clicked, [] { TriggerEditorAction("o3de.action.view.centerOnSelection"); });
        QPushButton* play = PillButton(m_hearthActions, QObject::tr("Play here"), false);
        play->setObjectName(QStringLiteral("PlayHere"));
        QObject::connect(play, &QPushButton::clicked, [] { TriggerEditorAction("o3de.action.game.play"); });
        actions->addWidget(focus);
        actions->addWidget(play);
        m_uiOverlayLayout.AddAnchoredWidget(m_hearthActions, Qt::AlignBottom | Qt::AlignRight);

        // Top: the view (Perspective / Top / Front / Side) and how it's drawn (Lit / Wireframe).
        m_hearthView = PillButton(m_hearthTopBar, QObject::tr("Perspective  ▾"), false);
        auto* viewMenu = new QMenu(m_hearthView);
        viewMenu->addAction(QObject::tr("Perspective"), [this] { m_hearthView->setText(QObject::tr("Perspective  ▾")); });
        viewMenu->addAction(QObject::tr("Top"), [this] { m_hearthView->setText(QObject::tr("Top  ▾")); TriggerEditorAction("o3de.action.hearth.view.top"); });
        viewMenu->addAction(QObject::tr("Front"), [this] { m_hearthView->setText(QObject::tr("Front  ▾")); TriggerEditorAction("o3de.action.hearth.view.front"); });
        viewMenu->addAction(QObject::tr("Side"), [this] { m_hearthView->setText(QObject::tr("Side  ▾")); TriggerEditorAction("o3de.action.hearth.view.side"); });
        m_hearthView->setMenu(viewMenu);
        top->addWidget(m_hearthView);
        QPushButton* lit = PillButton(m_hearthTopBar, QObject::tr("Lit"), true);
        lit->setChecked(true);
        QPushButton* wire = PillButton(m_hearthTopBar, QObject::tr("Wireframe"), true);
        // Lit / Wireframe: r_wireframe (Atom draws meshes as lines; Hearth Engine addition in MeshDrawPacket).
        auto setWireframe = [](bool on)
        {
            if (auto* console = AZ::Interface<AZ::IConsole>::Get())
            {
                console->PerformCommand(on ? "r_wireframe true" : "r_wireframe false");
            }
        };
        QObject::connect(lit, &QPushButton::clicked, [lit, wire, setWireframe] { lit->setChecked(true); wire->setChecked(false); setWireframe(false); });
        QObject::connect(wire, &QPushButton::clicked, [lit, wire, setWireframe] { wire->setChecked(true); lit->setChecked(false); setWireframe(true); });
        top->addWidget(lit);
        top->addWidget(wire);

        // Left: the tools (select, move, rotate, scale, paint).
        m_hearthTools = new QWidget(&m_uiOverlay);
        m_hearthTools->setObjectName(QStringLiteral("HearthPill"));
        m_hearthTools->setAttribute(Qt::WA_StyledBackground);
        m_hearthTools->setStyleSheet(QLatin1String(HearthPillStyle) + QStringLiteral("QPushButton { padding: 4px 8px; font-size: 14px; min-width: 22px; }"));
        auto* tools = new QHBoxLayout(m_hearthTools); // a row at the top left (mockup), not a column
        tools->setContentsMargins(4, 4, 4, 4);
        tools->setSpacing(4);
        const struct
        {
            const char* glyph;
            const char* tip;
            const char* action;
        } toolList[] = { { "↖", "Select  Q", "o3de.action.componentMode.end" }, { "✥", "Move  W", "o3de.action.edit.transform.move" },
                         { "⟳", "Rotate  E", "o3de.action.edit.transform.rotate" }, { "⤢", "Scale  R", "o3de.action.edit.transform.scale" },
                         { "✎", "Paint and edit", "o3de.action.hearth.edit.paint" } };
        for (const auto& tool : toolList)
        {
            QPushButton* button = PillButton(m_hearthTools, QString::fromUtf8(tool.glyph), false);
            button->setToolTip(QObject::tr(tool.tip));
            const char* action = tool.action;
            QObject::connect(button, &QPushButton::clicked, [action] { TriggerEditorAction(action); });
            tools->addWidget(button);
        }
        // Top left: the overlay layout only has corners and edges (no left-centre), so AlignLeft | AlignVCenter left
        // the pill unplaced as a tiny box.
        m_uiOverlayLayout.AddAnchoredWidget(m_hearthTools, Qt::AlignTop | Qt::AlignLeft);

        // Top right: Split 2, Split 4 and the shortcuts panel.
        m_hearthLayouts = new QWidget(&m_uiOverlay);
        m_hearthLayouts->setObjectName(QStringLiteral("HearthPill"));
        m_hearthLayouts->setAttribute(Qt::WA_StyledBackground);
        m_hearthLayouts->setStyleSheet(QLatin1String(HearthPillStyle));
        auto* layouts = new QHBoxLayout(m_hearthLayouts);
        layouts->setContentsMargins(4, 4, 4, 4);
        layouts->setSpacing(4);
        QPushButton* split2 = PillButton(m_hearthLayouts, QObject::tr("Split 2"), true);
        QPushButton* split4 = PillButton(m_hearthLayouts, QObject::tr("Split 4"), true);
        QObject::connect(split2, &QPushButton::clicked, [split2, split4] { split4->setChecked(false); TriggerEditorAction(split2->isChecked() ? "o3de.action.hearth.view.split2" : "o3de.action.hearth.view.split1"); });
        QObject::connect(split4, &QPushButton::clicked, [split2, split4] { split2->setChecked(false); TriggerEditorAction(split4->isChecked() ? "o3de.action.hearth.view.split4" : "o3de.action.hearth.view.split1"); });
        QPushButton* shortcuts = PillButton(m_hearthLayouts, QObject::tr("?"), true);
        shortcuts->setToolTip(QObject::tr("Shortcuts and gizmo"));
        layouts->addWidget(split2);
        layouts->addWidget(split4);
        layouts->addWidget(shortcuts);
        m_uiOverlayLayout.AddAnchoredWidget(m_hearthLayouts, Qt::AlignTop | Qt::AlignRight);

        // Right: shortcuts, gizmo size, grid and light icons.
        m_hearthPanel = new QWidget(&m_uiOverlay);
        m_hearthPanel->setObjectName(QStringLiteral("HearthPanel"));
        m_hearthPanel->setAttribute(Qt::WA_StyledBackground);
        m_hearthPanel->setStyleSheet(QStringLiteral("QWidget#HearthPanel { background-color: #0D0C0B; border: 1px solid #262320; border-radius: 14px; }"
                                                    "QLabel { color: #ECE6DE; font-size: 12px; background: transparent; } QLabel[dim=\"true\"] { color: #8D857B; }")
                                     + QLatin1String(HearthPillStyle));
        auto* panel = new QVBoxLayout(m_hearthPanel);
        panel->setContentsMargins(12, 10, 12, 10);
        panel->setSpacing(4);
        auto* shortcutsTitle = new QLabel(QObject::tr("Shortcuts"), m_hearthPanel);
        shortcutsTitle->setStyleSheet(QStringLiteral("font-weight: 600;"));
        panel->addWidget(shortcutsTitle);
        const char* keys[][2] = { { "Select", "Q" }, { "Move", "W" }, { "Rotate", "E" }, { "Scale", "R" }, { "World / local", "X" },
                                  { "Snap on / off", "Ctrl" }, { "Fly camera", "Right mouse + WASD" }, { "Focus selection", "F" }, { "Play from camera", "Ctrl+G" } };
        for (const auto& key : keys)
        {
            auto* row = new QHBoxLayout();
            row->addWidget(new QLabel(QObject::tr(key[0]), m_hearthPanel));
            row->addStretch(1);
            auto* value = new QLabel(QString::fromLatin1(key[1]), m_hearthPanel);
            value->setProperty("dim", true);
            value->setStyleSheet(QStringLiteral("color: #8D857B; font-family: \"Geist Mono\";"));
            row->addWidget(value);
            panel->addLayout(row);
        }
        auto* gizmoTitle = new QLabel(QObject::tr("Gizmo"), m_hearthPanel);
        gizmoTitle->setStyleSheet(QStringLiteral("font-weight: 600; padding-top: 6px;"));
        panel->addWidget(gizmoTitle);
        auto* sizeRow = new QHBoxLayout();
        sizeRow->addWidget(new QLabel(QObject::tr("Size"), m_hearthPanel));
        auto* gizmoSize = new QSlider(Qt::Horizontal, m_hearthPanel);
        gizmoSize->setRange(int(AzToolsFramework::MinManipulatorViewBaseScale * 100), int(AzToolsFramework::MaxManipulatorViewBaseScale * 100));
        gizmoSize->setValue(qRound(AzToolsFramework::ManipulatorViewBaseScale() * 100));
        auto* sizeText = new QLabel(QStringLiteral("%1×").arg(AzToolsFramework::ManipulatorViewBaseScale(), 0, 'f', 1), m_hearthPanel);
        QObject::connect(gizmoSize, &QSlider::valueChanged, [sizeText](int value)
        {
            AzToolsFramework::SetManipulatorViewBaseScale(value / 100.0f);
            sizeText->setText(QStringLiteral("%1×").arg(value / 100.0, 0, 'f', 1));
        });
        sizeRow->addWidget(gizmoSize, 1);
        sizeRow->addWidget(sizeText);
        panel->addLayout(sizeRow);
        QPushButton* showGrid = PillButton(m_hearthPanel, QObject::tr("Show grid"), false);
        QObject::connect(showGrid, &QPushButton::clicked, [] { TriggerEditorAction("o3de.action.edit.snap.toggleShowingGrid"); });
        QPushButton* lightIcons = PillButton(m_hearthPanel, QObject::tr("Show light icons"), false);
        QObject::connect(lightIcons, &QPushButton::clicked, [] { TriggerEditorAction("o3de.action.view.toggleIcons"); });
        panel->addWidget(showGrid);
        panel->addWidget(lightIcons);
        m_hearthPanel->setVisible(false);
        QObject::connect(shortcuts, &QPushButton::toggled, m_hearthPanel, &QWidget::setVisible);
        m_uiOverlayLayout.AddAnchoredWidget(m_hearthPanel, Qt::AlignTop | Qt::AlignRight); // under Split 2 / Split 4 / ?

        // Bottom right, above Focus / Play here: compass and camera speed.
        m_hearthCompass = new QWidget(&m_uiOverlay);
        auto* compassRow = new QHBoxLayout(m_hearthCompass);
        compassRow->setContentsMargins(0, 0, 0, 0);
        compassRow->setSpacing(6);
        m_hearthCameraSpeed = new QLabel(m_hearthCompass);
        m_hearthCameraSpeed->setStyleSheet(QStringLiteral(
            "background-color: #0D0C0B; color: #ECE6DE; border: 1px solid #262320; border-radius: 13px;"
            " padding: 5px 12px; font-family: \"Geist Mono\"; font-size: 12px;"));
        auto* compass = new HearthCompass(m_hearthCompass, m_viewportId);
        m_hearthCompassDial = compass;
        compass->m_speedChanged = [this] { UpdateHearthOverlay(); };
        compassRow->addWidget(m_hearthCameraSpeed);
        compassRow->addWidget(compass);
        m_uiOverlayLayout.AddAnchoredWidget(m_hearthCompass, Qt::AlignBottom | Qt::AlignRight);

        m_hearthFrameClock.start();
    }

    void ViewportUiDisplay::UpdateHearthOverlay()
    {
        if (!m_hearthStatus)
        {
            return;
        }
        // Frame rate from how often the viewport asks us to update.
        ++m_hearthFrames;
        const qint64 elapsed = m_hearthFrameClock.elapsed();
        if (elapsed < 500)
        {
            return;
        }
        m_hearthFps = m_hearthFrames * 1000.0f / static_cast<float>(elapsed);
        m_hearthFrames = 0;
        m_hearthFrameClock.restart();

        // Snapping state (the editor owns it; we only show it).
        bool gridSnap = false;
        bool angleSnap = false;
        bool showGrid = false;
        float gridSize = 0.0f;
        float angle = 0.0f;
        AzToolsFramework::ViewportInteraction::ViewportSettingsRequestBus::EventResult(
            gridSnap, m_viewportId, &AzToolsFramework::ViewportInteraction::ViewportSettingsRequestBus::Events::GridSnappingEnabled);
        AzToolsFramework::ViewportInteraction::ViewportSettingsRequestBus::EventResult(
            angleSnap, m_viewportId, &AzToolsFramework::ViewportInteraction::ViewportSettingsRequestBus::Events::AngleSnappingEnabled);
        AzToolsFramework::ViewportInteraction::ViewportSettingsRequestBus::EventResult(
            showGrid, m_viewportId, &AzToolsFramework::ViewportInteraction::ViewportSettingsRequestBus::Events::ShowGrid);
        AzToolsFramework::ViewportInteraction::ViewportSettingsRequestBus::EventResult(
            gridSize, m_viewportId, &AzToolsFramework::ViewportInteraction::ViewportSettingsRequestBus::Events::GridSize);
        AzToolsFramework::ViewportInteraction::ViewportSettingsRequestBus::EventResult(
            angle, m_viewportId, &AzToolsFramework::ViewportInteraction::ViewportSettingsRequestBus::Events::AngleStep);
        m_hearthGridSnap->setChecked(gridSnap);
        m_hearthGridSnap->setText(QObject::tr("Snap %1 m").arg(QString::number(gridSize, 'g', 3)));
        m_hearthAngleSnap->setChecked(angleSnap);
        m_hearthAngleSnap->setText(QStringLiteral("%1°").arg(QString::number(angle, 'g', 3)));
        m_hearthGrid->setChecked(showGrid);

        // Camera: speed (Editor preference) and which way it looks.
        if (m_hearthCameraSpeed)
        {
            const double speed = AzToolsFramework::GetRegistry(HearthCameraSpeedSetting, 10.0);
            m_hearthCameraSpeed->setText(QObject::tr("Camera %1 m/s").arg(QString::number(speed, 'g', 3)));
            m_hearthCameraSpeed->adjustSize();
            AzFramework::CameraState camera;
            AzToolsFramework::ViewportInteraction::ViewportInteractionRequestBus::EventResult(
                camera, m_viewportId, &AzToolsFramework::ViewportInteraction::ViewportInteractionRequestBus::Events::GetCameraState);
            if (auto* compass = static_cast<HearthCompass*>(m_hearthCompassDial))
            {
                compass->m_yaw = std::atan2(camera.m_forward.GetX(), camera.m_forward.GetY());
                compass->update();
            }
        }

        // What's selected and where.
        AzToolsFramework::EntityIdList selected;
        AzToolsFramework::ToolsApplicationRequestBus::BroadcastResult(
            selected, &AzToolsFramework::ToolsApplicationRequests::GetSelectedEntities);
        QString text;
        if (selected.size() == 1)
        {
            AZStd::string name;
            AZ::ComponentApplicationBus::BroadcastResult(name, &AZ::ComponentApplicationRequests::GetEntityName, selected.front());
            AZ::Vector3 position = AZ::Vector3::CreateZero();
            AZ::TransformBus::EventResult(position, selected.front(), &AZ::TransformBus::Events::GetWorldTranslation);
            // While it's being moved: how far, on the axis it moved most ("x +2.50 m").
            QString moved;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (m_hearthDragEntity != selected.front() || now - m_hearthDragSeen > 1500)
            {
                m_hearthDragEntity = selected.front();
                m_hearthDragStart = position;
            }
            const AZ::Vector3 delta = position - m_hearthDragStart;
            if (!delta.IsClose(AZ::Vector3::CreateZero(), 0.001f))
            {
                m_hearthDragSeen = now;
                const float ax = std::fabs(delta.GetX());
                const float ay = std::fabs(delta.GetY());
                const float az = std::fabs(delta.GetZ());
                const char* axis = ax >= ay && ax >= az ? "x" : (ay >= az ? "y" : "z");
                const float amount = ax >= ay && ax >= az ? delta.GetX() : (ay >= az ? delta.GetY() : delta.GetZ());
                moved = QStringLiteral("%1 %2%3 m  ·  ").arg(QLatin1String(axis), amount >= 0 ? QStringLiteral("+") : QString()).arg(amount, 0, 'f', 2);
            }
            text = moved + QStringLiteral("%1  ·  x %2 y %3 z %4  ·  ")
                       .arg(QString::fromUtf8(name.c_str()))
                       .arg(position.GetX(), 0, 'f', 1)
                       .arg(position.GetY(), 0, 'f', 1)
                       .arg(position.GetZ(), 0, 'f', 1);
        }
        else if (selected.size() > 1)
        {
            text = QObject::tr("%1 selected  ·  ").arg(selected.size());
        }
        // Memory the editor uses (Linux: resident pages from /proc/self/statm).
        QString memory;
        if (FILE* statm = fopen("/proc/self/statm", "r"))
        {
            long pages = 0;
            long resident = 0;
            if (fscanf(statm, "%ld %ld", &pages, &resident) == 2)
            {
                memory = QStringLiteral("  ·  %1 MB").arg(resident * 4096 / (1024 * 1024));
            }
            fclose(statm);
        }
        // What the game reports while it runs, for example "9 chunks · 412 pieces" from the endless level.
        QString gameStats;
        if (auto* registry = AZ::SettingsRegistry::Get())
        {
            AZStd::string stats;
            if (registry->Get(stats, "/Hearth/Viewport/Stats") && !stats.empty())
            {
                gameStats = QStringLiteral("  ·  %1").arg(QString::fromUtf8(stats.c_str()).toHtmlEscaped());
            }
        }
        memory = gameStats + memory;
        const QString color = m_hearthFps >= 30.0f ? QStringLiteral("#A6DD97") : (m_hearthFps >= 20.0f ? QStringLiteral("#F0D17A") : QStringLiteral("#FF9F8F"));
        m_hearthStatus->setText(QStringLiteral("%1<span style=\"color:%2\">%3 fps</span>%4").arg(text.toHtmlEscaped(), color).arg(qRound(m_hearthFps)).arg(memory));
        m_hearthStatus->adjustSize();
    }

    void ViewportUiDisplay::PrepareWidgetForViewportUi(QPointer<QWidget> widget)
    {
        widget->setAttribute(Qt::WA_ShowWithoutActivating);
        widget->setParent(&m_uiOverlay);
        widget->setStyleSheet("border: none;");
    }

    void ViewportUiDisplay::SetUiOverlayContents(QPointer<QWidget> widget)
    {
        if (!widget)
        {
            return;
        }

        PrepareWidgetForViewportUi(widget);
    }

    void ViewportUiDisplay::SetUiOverlayContentsAnchored(QPointer<QWidget> widget, const Qt::Alignment alignment)
    {
        if (!widget)
        {
            return;
        }

        PrepareWidgetForViewportUi(widget);
        m_uiOverlayLayout.AddAnchoredWidget(widget, alignment);
    }

    void ViewportUiDisplay::UpdateUiOverlayGeometry()
    {
        // add the viewport border region if visible
        QRegion region;
        if (m_viewportBorderText.isVisible())
        {
            // get the border region by taking the entire region and subtracting the non-border area
            region += m_uiOverlay.rect();
            region -= QRect(
                QPoint(m_uiOverlay.rect().left() + HighlightBorderSize, m_uiOverlay.rect().top() + ViewportUiTopBorderSize),
                QPoint(m_uiOverlay.rect().right() - HighlightBorderSize, m_uiOverlay.rect().bottom() - HighlightBorderSize));

            // if the user changes the size of their window, release the width of the border so the
            // overlay can resize
            if (m_viewportBorderText.width() != m_renderOverlay->width())
            {
                m_viewportBorderText.setMinimumWidth(0);
                m_viewportBorderText.setMaximumWidth(m_renderOverlay->width());
            }
        }

        // add all children widget regions
        region += m_uiOverlay.childrenRegion();

        // set viewport ui visibility depending on if elements are present
        const bool visible = !region.isEmpty() && UiDisplayEnabled();
        m_uiMainWindow.setVisible(visible);
        m_uiOverlay.setVisible(visible);

        m_uiMainWindow.setMask(region);
    }

    void ViewportUiDisplay::PositionUiOverlayOverRenderViewport()
    {
        QPoint offset = m_renderOverlay->mapToGlobal(QPoint());
        m_uiMainWindow.setGeometry(offset.x(), offset.y(), m_renderOverlay->width(), m_renderOverlay->height());
        m_uiOverlay.setGeometry(m_uiMainWindow.rect());
        UpdateUiOverlayGeometry();
    }

    ViewportUiElementInfo ViewportUiDisplay::GetViewportUiElementInfo(const ViewportUiElementId elementId)
    {
        if (auto element = m_viewportUiElements.find(elementId); element != m_viewportUiElements.end())
        {
            return element->second;
        }

        return ViewportUiElementInfo{ nullptr, InvalidViewportUiElementId, false };
    }

    const QMainWindow* ViewportUiDisplay::GetUiMainWindow() const
    {
        return &m_uiMainWindow;
    }

    const QWidget* ViewportUiDisplay::GetUiOverlay() const
    {
        return &m_uiOverlay;
    }

    const QGridLayout* ViewportUiDisplay::GetUiOverlayLayout() const
    {
        return &m_uiOverlayLayout;
    }

    void SetTransparentBackground(QWidget* widget)
    {
        widget->setAttribute(Qt::WA_TranslucentBackground);
        widget->setAutoFillBackground(false);
    }
} // namespace AzToolsFramework::ViewportUi::Internal
