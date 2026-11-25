/*
 * Hearth Engine Game UI editor. Built on O3DE (Apache-2.0 OR MIT). See HearthUiEditor.h.
 */

#include "EditorDefs.h"

#include "HearthUiEditor.h"

#include <AzCore/Interface/Interface.h>
#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/API/ViewPaneOptions.h>
#include <AzToolsFramework/ActionManager/Action/ActionManagerInterface.h>

#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtMath>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <cmath>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    constexpr int UiSnap = 8;

    struct UiDevice
    {
        const char* name;
        int width;
        int height;
    };
    const UiDevice UiDevices[] = { { "1920 × 1080", 1920, 1080 }, { "Steam Deck", 1280, 800 }, { "ROG Ally", 1920, 1080 }, { "Vertical", 1080, 1920 } };

    const char* const UiAnchors[] = { "top-left", "top", "top-right", "left", "center", "right", "bottom-left", "bottom", "bottom-right" };

    //! Game values the HUD can show (what the game's HUD feeds it).
    const char* const UiBindings[] = { "", "player.health", "player.stamina", "player.sanity", "player.flashlight", "weapon.ammo", "weapon.reserve",
                                       "squad.members", "interact.prompt", "toasts", "game.objective" };

    QString UiTypeIcon(const QString& type)
    {
        if (type == QLatin1String("bar")) return QStringLiteral("▭");
        if (type == QLatin1String("meter")) return QStringLiteral("◔");
        if (type == QLatin1String("text")) return QStringLiteral("T");
        if (type == QLatin1String("image")) return QStringLiteral("▣");
        if (type == QLatin1String("button")) return QStringLiteral("⬭");
        if (type == QLatin1String("list")) return QStringLiteral("☰");
        if (type == QLatin1String("crosshair")) return QStringLiteral("+");
        return QStringLiteral("▾");
    }

    QString UiNewId()
    {
        return QStringLiteral("e%1").arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
    }

    int UiAnchorColumn(const QString& anchor)
    {
        if (anchor.endsWith(QLatin1String("left")) || anchor == QLatin1String("left")) return 0;
        if (anchor.endsWith(QLatin1String("right")) || anchor == QLatin1String("right")) return 2;
        return 1;
    }

    int UiAnchorRow(const QString& anchor)
    {
        if (anchor.startsWith(QLatin1String("top"))) return 0;
        if (anchor.startsWith(QLatin1String("bottom"))) return 2;
        return 1;
    }

    //! Sample values so the canvas looks like the game while you lay it out.
    double UiSample(const QString& bind)
    {
        if (bind == QLatin1String("player.health")) return 0.82;
        if (bind == QLatin1String("player.stamina")) return 0.64;
        if (bind == QLatin1String("player.sanity")) return 0.52;
        if (bind == QLatin1String("player.flashlight")) return 0.4;
        return 0.7;
    }

    QString UiSampleText(const QJsonObject& element)
    {
        const QString bind = element.value(QStringLiteral("bind")).toString();
        if (!element.value(QStringLiteral("text")).toString().isEmpty())
        {
            return element.value(QStringLiteral("text")).toString();
        }
        if (bind == QLatin1String("weapon.ammo")) return QStringLiteral("24 / 96");
        if (bind == QLatin1String("interact.prompt")) return QStringLiteral("[E] Open hatch");
        if (bind == QLatin1String("game.objective")) return QObject::tr("Find the exit");
        return element.value(QStringLiteral("name")).toString();
    }
} // namespace

//! The canvas: the layout at the device's size, scaled to fit; drag to move, the corner to resize.
class HearthUiCanvasView : public QWidget
{
public:
    explicit HearthUiCanvasView(HearthUiEditor* editor)
        : QWidget(editor)
        , m_editor(editor)
    {
        setMouseTracking(true);
        setMinimumSize(480, 320);
    }

    double Scale() const
    {
        const QSize canvas = m_editor->CanvasSize();
        return qMin((width() - 24.0) / canvas.width(), (height() - 24.0) / canvas.height());
    }

    QRectF CanvasRect() const
    {
        const QSize canvas = m_editor->CanvasSize();
        const double scale = Scale();
        const QSizeF size(canvas.width() * scale, canvas.height() * scale);
        return QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size);
    }

    QRectF ToView(const QRect& rect) const
    {
        const QRectF canvas = CanvasRect();
        const double scale = Scale();
        return QRectF(canvas.left() + rect.x() * scale, canvas.top() + rect.y() * scale, rect.width() * scale, rect.height() * scale);
    }

    QPoint ToCanvas(const QPointF& point) const
    {
        const QRectF canvas = CanvasRect();
        const double scale = Scale();
        return QPoint(qRound((point.x() - canvas.left()) / scale), qRound((point.y() - canvas.top()) / scale));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(0x12, 0x11, 0x10));
        const QRectF canvas = CanvasRect();
        // A dark game-like backdrop and the snap grid.
        QLinearGradient back(canvas.topLeft(), canvas.bottomLeft());
        back.setColorAt(0, QColor(0x3a, 0x34, 0x22));
        back.setColorAt(1, QColor(0x1f, 0x1b, 0x12));
        p.fillRect(canvas, back);
        p.setPen(QColor(255, 255, 255, 10));
        const double step = UiSnap * 8 * Scale();
        for (double x = canvas.left(); x < canvas.right(); x += step)
        {
            p.drawLine(QPointF(x, canvas.top()), QPointF(x, canvas.bottom()));
        }
        for (double y = canvas.top(); y < canvas.bottom(); y += step)
        {
            p.drawLine(QPointF(canvas.left(), y), QPointF(canvas.right(), y));
        }
        QFont font = p.font();
        for (const QJsonObject& element : m_editor->Elements())
        {
            const QRectF r = ToView(m_editor->ElementRect(element));
            const QString type = element.value(QStringLiteral("type")).toString();
            const QColor fill(element.value(QStringLiteral("fill")).toString(QStringLiteral("#ECE6DE")));
            const double value = UiSample(element.value(QStringLiteral("bind")).toString());
            font.setPixelSize(qMax(8, int(r.height() * (type == QLatin1String("text") ? 0.62 : 0.5))));
            p.setFont(font);
            if (type == QLatin1String("bar"))
            {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0, 0, 0, 120));
                p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
                p.setBrush(fill);
                p.drawRoundedRect(QRectF(r.left(), r.top(), r.width() * value, r.height()), r.height() / 2, r.height() / 2);
            }
            else if (type == QLatin1String("meter"))
            {
                const QRectF ring = r.adjusted(r.width() * 0.08, r.height() * 0.08, -r.width() * 0.08, -r.height() * 0.08);
                p.setPen(QPen(QColor(0, 0, 0, 120), qMax(2.0, r.width() * 0.1)));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(ring);
                p.setPen(QPen(fill, qMax(2.0, r.width() * 0.1), Qt::SolidLine, Qt::RoundCap));
                p.drawArc(ring, 90 * 16, -int(360 * 16 * value));
            }
            else if (type == QLatin1String("crosshair"))
            {
                p.setPen(QPen(fill, qMax(1.0, r.width() * 0.08)));
                const QPointF c = r.center();
                p.drawLine(QPointF(c.x() - r.width() / 2, c.y()), QPointF(c.x() - r.width() / 6, c.y()));
                p.drawLine(QPointF(c.x() + r.width() / 6, c.y()), QPointF(c.x() + r.width() / 2, c.y()));
                p.drawLine(QPointF(c.x(), c.y() - r.height() / 2), QPointF(c.x(), c.y() - r.height() / 6));
                p.drawLine(QPointF(c.x(), c.y() + r.height() / 6), QPointF(c.x(), c.y() + r.height() / 2));
            }
            else if (type == QLatin1String("list"))
            {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0, 0, 0, 90));
                p.drawRoundedRect(r, 6, 6);
            }
            else if (type == QLatin1String("button"))
            {
                p.setPen(Qt::NoPen);
                p.setBrush(fill);
                p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
                p.setPen(Hearth::Ink());
                p.drawText(r, Qt::AlignCenter, UiSampleText(element));
            }
            else if (type == QLatin1String("image"))
            {
                p.setPen(QPen(QColor(255, 255, 255, 60), 1, Qt::DashLine));
                p.setBrush(QColor(255, 255, 255, 12));
                p.drawRoundedRect(r, 4, 4);
            }
            else if (type == QLatin1String("text") || type == QLatin1String("row"))
            {
                p.setPen(fill);
                const QString sample = type == QLatin1String("row") ? element.value(QStringLiteral("text")).toString(QStringLiteral("Chris · 100")) : UiSampleText(element);
                p.drawText(r, (UiAnchorColumn(element.value(QStringLiteral("anchor")).toString()) == 2 ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, sample);
            }
            if (element.value(QStringLiteral("id")).toString() == m_editor->Selected())
            {
                p.setPen(QPen(Hearth::Ember(), 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawRect(r.adjusted(-2, -2, 2, 2));
                p.setBrush(Hearth::Ember());
                p.drawRect(QRectF(r.right() - 3, r.bottom() - 3, 7, 7));
            }
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        m_dragging.clear();
        const QVector<QJsonObject>& elements = m_editor->Elements();
        for (int i = elements.size() - 1; i >= 0; --i)
        {
            const QRectF r = ToView(m_editor->ElementRect(elements[i])).adjusted(-4, -4, 4, 4);
            if (r.contains(event->pos()))
            {
                m_dragging = elements[i].value(QStringLiteral("id")).toString();
                m_resizing = QRectF(r.right() - 10, r.bottom() - 10, 14, 14).contains(event->pos());
                m_start = event->pos();
                m_startRect = m_editor->ElementRect(elements[i]);
                m_editor->Select(m_dragging);
                return;
            }
        }
        m_editor->Select(QString());
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (m_dragging.isEmpty() || !(event->buttons() & Qt::LeftButton))
        {
            return;
        }
        QJsonObject* element = m_editor->Find(m_dragging);
        if (!element)
        {
            return;
        }
        const double scale = Scale();
        const QPointF delta = (event->pos() - m_start) / scale;
        auto snap = [](double v) { return int(std::round(v / UiSnap) * UiSnap); };
        if (m_resizing)
        {
            element->insert(QStringLiteral("w"), qMax(UiSnap, snap(m_startRect.width() + delta.x())));
            element->insert(QStringLiteral("h"), qMax(UiSnap, snap(m_startRect.height() + delta.y())));
            // Keep the top-left where it was (the anchor may be on the right or bottom).
            m_editor->SetCanvasTopLeft(*element, m_startRect.topLeft());
        }
        else
        {
            m_editor->SetCanvasTopLeft(*element, QPoint(snap(m_startRect.x() + delta.x()), snap(m_startRect.y() + delta.y())));
        }
        m_editor->Changed();
    }

private:
    HearthUiEditor* m_editor;
    QString m_dragging;
    bool m_resizing = false;
    QPoint m_start;
    QRect m_startRect;
};

void HearthUiEditor::RegisterViewClass()
{
    AzToolsFramework::ViewPaneOptions options;
    options.preferedDockingArea = Qt::NoDockWidgetArea;
    options.paneRect = QRect(80, 60, 1480, 900);
    options.showInMenu = true;
    AzToolsFramework::RegisterViewPane<HearthUiEditor>("Game UI editor", "Tools", options);
}

HearthUiEditor::HearthUiEditor(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("HearthUi"));
    setStyleSheet(QStringLiteral("QWidget#HearthUi { background-color: #0D0C0B; }"));
    m_path = QDir(QString::fromUtf8(AZ::Utils::GetProjectPath().c_str())).filePath(QStringLiteral("UI/HUD.uicanvas"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    QHBoxLayout* header = Hearth::MakeHeader(this, tr("Game UI editor"), QString());
    if (auto* titles = header->itemAt(1) ? header->itemAt(1)->layout() : nullptr)
    {
        m_subtitle = titles->count() > 1 ? qobject_cast<QLabel*>(titles->itemAt(1)->widget()) : nullptr;
    }
    header->addStretch(1);
    auto* devices = new QWidget(this);
    devices->setObjectName(QStringLiteral("UiDevices"));
    devices->setAttribute(Qt::WA_StyledBackground);
    devices->setStyleSheet(QStringLiteral("QWidget#UiDevices { background-color: rgba(255, 255, 255, 13); border-radius: 13px; }"
                                          "QPushButton { border: none; border-radius: 11px; padding: 3px 12px; color: #ECE6DE; background: transparent; }"
                                          "QPushButton:checked { background-color: #ECE6DE; color: #1A0F08; }"));
    auto* devicesLayout = new QHBoxLayout(devices);
    devicesLayout->setContentsMargins(2, 2, 2, 2);
    devicesLayout->setSpacing(2);
    for (int i = 0; i < 4; ++i)
    {
        auto* button = new QPushButton(tr(UiDevices[i].name), devices);
        button->setCheckable(true);
        button->setChecked(i == 0);
        connect(button, &QPushButton::clicked, this, [this, i]()
        {
            m_device = i;
            for (int j = 0; j < m_devices.size(); ++j)
            {
                m_devices[j]->setChecked(j == i);
            }
            m_view->update();
            FillProperties();
        });
        m_devices << button;
        devicesLayout->addWidget(button);
    }
    header->addWidget(devices);
    QPushButton* preview = Hearth::MakeButton(tr("Preview in game"), false, this);
    connect(preview, &QPushButton::clicked, this, [this]()
    {
        // Save, then play: the game's HUD reads UI/HUD.uicanvas when it starts.
        Save();
        if (auto* actions = AZ::Interface<AzToolsFramework::ActionManagerInterface>::Get())
        {
            actions->TriggerAction("o3de.action.game.play");
        }
    });
    header->addWidget(preview);
    m_save = Hearth::MakeButton(tr("Save"), true, this);
    connect(m_save, &QPushButton::clicked, this, &HearthUiEditor::Save);
    header->addWidget(m_save);
    root->addLayout(header);

    auto* body = new QHBoxLayout();
    body->setSpacing(10);

    // Layers and things to add.
    auto* left = new QVBoxLayout();
    left->setSpacing(10);
    QVBoxLayout* layersLayout = nullptr;
    QFrame* layersCard = Hearth::MakeCard(this, &layersLayout);
    auto* layersTitle = new QHBoxLayout();
    layersTitle->addWidget(Hearth::MakeLabel(tr("Layers"), QStringLiteral("title"), layersCard));
    layersTitle->addStretch(1);
    QPushButton* plus = Hearth::MakeButton(QStringLiteral("+"), false, layersCard);
    plus->setToolTip(tr("Add a group"));
    connect(plus, &QPushButton::clicked, this, [this]() { Add(QStringLiteral("group")); });
    layersTitle->addWidget(plus);
    layersLayout->addLayout(layersTitle);
    m_layers = new QTreeWidget(layersCard);
    m_layers->setHeaderHidden(true);
    m_layers->setStyleSheet(QStringLiteral("QTreeWidget { background: transparent; border: none; outline: none; color: #ECE6DE; }"
                                           "QTreeWidget::item { padding: 4px; border-radius: 6px; }"
                                           "QTreeWidget::item:selected { background-color: rgba(232, 116, 59, 41); color: #FFCFB4; }"));
    connect(m_layers, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item)
    {
        if (item && !m_filling)
        {
            Select(item->data(0, Qt::UserRole).toString());
        }
    });
    layersLayout->addWidget(m_layers, 1);
    left->addWidget(layersCard, 1);
    QVBoxLayout* addLayout = nullptr;
    QFrame* addCard = Hearth::MakeCard(this, &addLayout);
    addLayout->addWidget(Hearth::MakeLabel(tr("Add"), QStringLiteral("title"), addCard));
    auto* addGrid = new QGridLayout();
    const QVector<QPair<QString, QString>> kinds = { { tr("Text"), QStringLiteral("text") }, { tr("Image"), QStringLiteral("image") },
                                                     { tr("Bar"), QStringLiteral("bar") }, { tr("Button"), QStringLiteral("button") },
                                                     { tr("List"), QStringLiteral("list") }, { tr("Meter"), QStringLiteral("meter") } };
    for (int i = 0; i < kinds.size(); ++i)
    {
        QPushButton* button = Hearth::MakeButton(UiTypeIcon(kinds[i].second) + QStringLiteral("  ") + kinds[i].first, false, addCard);
        const QString type = kinds[i].second;
        connect(button, &QPushButton::clicked, this, [this, type]() { Add(type); });
        addGrid->addWidget(button, i / 2, i % 2);
    }
    addLayout->addLayout(addGrid);
    left->addWidget(addCard);
    auto* leftHolder = new QWidget(this);
    leftHolder->setFixedWidth(260);
    leftHolder->setLayout(left);
    body->addWidget(leftHolder);

    // Canvas.
    QVBoxLayout* canvasLayout = nullptr;
    QFrame* canvasCard = Hearth::MakeCard(this, &canvasLayout);
    auto* canvasTitle = new QHBoxLayout();
    canvasTitle->addWidget(Hearth::MakeLabel(tr("Canvas"), QStringLiteral("title"), canvasCard));
    canvasTitle->addStretch(1);
    m_snapLabel = Hearth::MakeLabel(QString(), QStringLiteral("small"), canvasCard);
    canvasTitle->addWidget(m_snapLabel);
    canvasLayout->addLayout(canvasTitle);
    m_view = new HearthUiCanvasView(this);
    canvasLayout->addWidget(m_view, 1);
    body->addWidget(canvasCard, 1);

    // Properties.
    QVBoxLayout* props = nullptr;
    QFrame* propsCard = Hearth::MakeCard(this, &props);
    propsCard->setFixedWidth(320);
    props->addWidget(Hearth::MakeLabel(tr("Selected"), QStringLiteral("small"), propsCard));
    m_selectedLabel = Hearth::MakeLabel(QString(), QStringLiteral("title"), propsCard);
    props->addWidget(m_selectedLabel);
    m_propertiesBody = new QWidget(propsCard);
    auto* body2 = new QVBoxLayout(m_propertiesBody);
    body2->setContentsMargins(0, 0, 0, 0);
    body2->setSpacing(8);
    body2->addWidget(Hearth::MakeLabel(tr("Anchor"), QStringLiteral("body"), m_propertiesBody));
    body2->addWidget(Hearth::MakeLabel(tr("Where it sticks when the screen size changes"), QStringLiteral("small"), m_propertiesBody));
    auto* anchorGrid = new QGridLayout();
    anchorGrid->setSpacing(4);
    for (int i = 0; i < 9; ++i)
    {
        auto* button = new QPushButton(m_propertiesBody);
        button->setCheckable(true);
        button->setFixedSize(26, 26);
        button->setToolTip(QString::fromLatin1(UiAnchors[i]));
        button->setStyleSheet(QStringLiteral("QPushButton { background-color: rgba(255, 255, 255, 10); border: 1px solid rgba(255, 255, 255, 20); border-radius: 6px; }"
                                             "QPushButton:checked { background-color: #E8743B; border-color: #E8743B; }"));
        const QString anchor = QString::fromLatin1(UiAnchors[i]);
        connect(button, &QPushButton::clicked, this, [this, anchor]()
        {
            QJsonObject* element = Find(m_selected);
            if (!element)
            {
                return;
            }
            // Keep it where it is on screen; only what it sticks to changes.
            const QRect before = ElementRect(*element);
            element->insert(QStringLiteral("anchor"), anchor);
            SetCanvasTopLeft(*element, before.topLeft());
            Changed();
            FillProperties();
        });
        m_anchors << button;
        anchorGrid->addWidget(button, i / 3, i % 3);
    }
    auto* anchorRow = new QHBoxLayout();
    anchorRow->addLayout(anchorGrid);
    anchorRow->addStretch(1);
    body2->addLayout(anchorRow);
    auto spin = [this](int low, int high)
    {
        auto* box = new QSpinBox(m_propertiesBody);
        box->setRange(low, high);
        box->setSingleStep(UiSnap);
        box->setFixedWidth(90);
        return box;
    };
    m_x = spin(-4000, 4000);
    m_y = spin(-4000, 4000);
    m_w = spin(1, 4000);
    m_h = spin(1, 4000);
    auto pair = [this, body2](const QString& title, const QString& hint, QSpinBox* a, QSpinBox* b, const QString& sep)
    {
        body2->addWidget(Hearth::MakeLabel(title, QStringLiteral("body"), m_propertiesBody));
        if (!hint.isEmpty())
        {
            body2->addWidget(Hearth::MakeLabel(hint, QStringLiteral("small"), m_propertiesBody));
        }
        auto* row = new QHBoxLayout();
        row->addWidget(a);
        row->addWidget(Hearth::MakeLabel(sep, QStringLiteral("dim"), m_propertiesBody));
        row->addWidget(b);
        row->addStretch(1);
        body2->addLayout(row);
    };
    pair(tr("Position"), tr("From the anchor"), m_x, m_y, QStringLiteral("·"));
    pair(tr("Size"), QString(), m_w, m_h, QStringLiteral("×"));
    m_x->setPrefix(QStringLiteral("x "));
    m_y->setPrefix(QStringLiteral("y "));
    for (QSpinBox* box : { m_x, m_y, m_w, m_h })
    {
        connect(box, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]()
        {
            QJsonObject* element = Find(m_selected);
            if (!element || m_filling)
            {
                return;
            }
            element->insert(QStringLiteral("x"), m_x->value());
            element->insert(QStringLiteral("y"), m_y->value());
            element->insert(QStringLiteral("w"), m_w->value());
            element->insert(QStringLiteral("h"), m_h->value());
            Changed();
            m_selectedLabel->setText(tr("%1 · %2 × %3").arg(element->value(QStringLiteral("name")).toString()).arg(m_w->value()).arg(m_h->value()));
        });
    }
    m_fill = new QPushButton(m_propertiesBody);
    m_fill->setFixedHeight(26);
    connect(m_fill, &QPushButton::clicked, this, [this]()
    {
        QJsonObject* element = Find(m_selected);
        if (!element)
        {
            return;
        }
        const QColor picked = QColorDialog::getColor(QColor(element->value(QStringLiteral("fill")).toString(QStringLiteral("#ECE6DE"))), this, tr("Fill colour"));
        if (picked.isValid())
        {
            element->insert(QStringLiteral("fill"), picked.name().toUpper());
            Changed();
            FillProperties();
        }
    });
    body2->addWidget(Hearth::MakeSettingRow(tr("Fill colour"), QString(), m_fill, m_propertiesBody));
    m_bind = new QComboBox(m_propertiesBody);
    m_bind->setEditable(true);
    for (const char* bind : UiBindings)
    {
        m_bind->addItem(QString::fromLatin1(bind));
    }
    connect(m_bind, &QComboBox::currentTextChanged, this, [this](const QString& text)
    {
        QJsonObject* element = Find(m_selected);
        if (element && !m_filling)
        {
            element->insert(QStringLiteral("bind"), text.trimmed());
            Changed();
        }
    });
    body2->addWidget(Hearth::MakeLabel(tr("Bound to"), QStringLiteral("body"), m_propertiesBody));
    body2->addWidget(Hearth::MakeLabel(tr("Game value that drives it"), QStringLiteral("small"), m_propertiesBody));
    body2->addWidget(m_bind);
    m_text = new QLineEdit(m_propertiesBody);
    m_text->setPlaceholderText(tr("Text (or leave empty to show the game value)"));
    connect(m_text, &QLineEdit::editingFinished, this, [this]()
    {
        QJsonObject* element = Find(m_selected);
        if (element)
        {
            element->insert(QStringLiteral("text"), m_text->text());
            Changed();
        }
    });
    body2->addWidget(m_text);
    m_pulse = new Hearth::Toggle(m_propertiesBody);
    m_pulseBelow = new QSpinBox(m_propertiesBody);
    m_pulseBelow->setRange(1, 99);
    m_pulseBelow->setPrefix(tr("Below "));
    m_pulseBelow->setSuffix(QStringLiteral("%"));
    auto* pulseRow = new QWidget(m_propertiesBody);
    auto* pulseLayout = new QHBoxLayout(pulseRow);
    pulseLayout->setContentsMargins(0, 0, 0, 0);
    pulseLayout->addWidget(m_pulseBelow);
    pulseLayout->addWidget(m_pulse);
    auto pulseChanged = [this]()
    {
        QJsonObject* element = Find(m_selected);
        if (!element || m_filling)
        {
            return;
        }
        if (m_pulse->isChecked())
        {
            element->insert(QStringLiteral("pulseBelow"), m_pulseBelow->value() / 100.0);
        }
        else
        {
            element->remove(QStringLiteral("pulseBelow"));
        }
        m_pulseBelow->setEnabled(m_pulse->isChecked());
        Changed();
    };
    connect(m_pulse, &QAbstractButton::toggled, this, pulseChanged);
    connect(m_pulseBelow, QOverload<int>::of(&QSpinBox::valueChanged), this, pulseChanged);
    body2->addWidget(Hearth::MakeSettingRow(tr("Pulse when low"), QString(), pulseRow, m_propertiesBody));
    m_gamepad = new Hearth::Toggle(m_propertiesBody);
    connect(m_gamepad, &QAbstractButton::toggled, this, [this](bool on)
    {
        QJsonObject* element = Find(m_selected);
        if (element && !m_filling)
        {
            element->insert(QStringLiteral("gamepad"), on);
            Changed();
        }
    });
    body2->addWidget(Hearth::MakeSettingRow(tr("Shows on gamepad"), QString(), m_gamepad, m_propertiesBody));
    QPushButton* remove = Hearth::MakeButton(tr("Remove"), false, m_propertiesBody);
    connect(remove, &QPushButton::clicked, this, [this]()
    {
        // The element and everything inside it.
        QSet<QString> gone{ m_selected };
        bool more = true;
        while (more)
        {
            more = false;
            for (const QJsonObject& element : m_elements)
            {
                if (gone.contains(element.value(QStringLiteral("parent")).toString()) && !gone.contains(element.value(QStringLiteral("id")).toString()))
                {
                    gone.insert(element.value(QStringLiteral("id")).toString());
                    more = true;
                }
            }
        }
        m_elements.erase(std::remove_if(m_elements.begin(), m_elements.end(), [&gone](const QJsonObject& e) { return gone.contains(e.value(QStringLiteral("id")).toString()); }),
                         m_elements.end());
        m_selected.clear();
        Changed(true);
    });
    body2->addWidget(remove, 0, Qt::AlignLeft);
    body2->addStretch(1);
    props->addWidget(m_propertiesBody, 1);
    body->addWidget(propsCard);
    root->addLayout(body, 1);

    Load();
}

QJsonObject* HearthUiEditor::Find(const QString& id)
{
    for (QJsonObject& element : m_elements)
    {
        if (element.value(QStringLiteral("id")).toString() == id)
        {
            return &element;
        }
    }
    return nullptr;
}

QSize HearthUiEditor::CanvasSize() const
{
    return QSize(UiDevices[m_device].width, UiDevices[m_device].height);
}

QRect HearthUiEditor::ParentRect(const QJsonObject& element) const
{
    const QString parent = element.value(QStringLiteral("parent")).toString();
    for (const QJsonObject& other : m_elements)
    {
        if (other.value(QStringLiteral("id")).toString() == parent && !parent.isEmpty())
        {
            // Groups fill their parent; everything else has its own box.
            if (other.value(QStringLiteral("type")).toString() == QLatin1String("group") && !other.contains(QStringLiteral("w")))
            {
                return ParentRect(other);
            }
            return ElementRect(other);
        }
    }
    return QRect(QPoint(0, 0), CanvasSize());
}

QRect HearthUiEditor::ElementRect(const QJsonObject& element) const
{
    const QRect parent = ParentRect(element);
    if (element.value(QStringLiteral("type")).toString() == QLatin1String("group") && !element.contains(QStringLiteral("w")))
    {
        return parent;
    }
    const int w = element.value(QStringLiteral("w")).toInt(100);
    const int h = element.value(QStringLiteral("h")).toInt(24);
    const int x = element.value(QStringLiteral("x")).toInt();
    const int y = element.value(QStringLiteral("y")).toInt();
    const QString anchor = element.value(QStringLiteral("anchor")).toString(QStringLiteral("top-left"));
    int left = parent.left() + x;
    int top = parent.top() + y;
    switch (UiAnchorColumn(anchor))
    {
    case 1: left = parent.left() + parent.width() / 2 - w / 2 + x; break;
    case 2: left = parent.right() + 1 - w - x; break;
    default: break;
    }
    switch (UiAnchorRow(anchor))
    {
    case 1: top = parent.top() + parent.height() / 2 - h / 2 + y; break;
    case 2: top = parent.bottom() + 1 - h - y; break;
    default: break;
    }
    return QRect(left, top, w, h);
}

void HearthUiEditor::SetCanvasTopLeft(QJsonObject& element, const QPoint& topLeft)
{
    const QRect parent = ParentRect(element);
    const int w = element.value(QStringLiteral("w")).toInt(100);
    const int h = element.value(QStringLiteral("h")).toInt(24);
    const QString anchor = element.value(QStringLiteral("anchor")).toString(QStringLiteral("top-left"));
    int x = topLeft.x() - parent.left();
    int y = topLeft.y() - parent.top();
    switch (UiAnchorColumn(anchor))
    {
    case 1: x = topLeft.x() - (parent.left() + parent.width() / 2 - w / 2); break;
    case 2: x = parent.right() + 1 - w - topLeft.x(); break;
    default: break;
    }
    switch (UiAnchorRow(anchor))
    {
    case 1: y = topLeft.y() - (parent.top() + parent.height() / 2 - h / 2); break;
    case 2: y = parent.bottom() + 1 - h - topLeft.y(); break;
    default: break;
    }
    element.insert(QStringLiteral("x"), x);
    element.insert(QStringLiteral("y"), y);
}

void HearthUiEditor::MakeDefault()
{
    // The HUD from the approved mockups, as a starting point.
    m_elements.clear();
    auto add = [this](const QString& id, const QString& parent, const QString& name, const QString& type, const QString& anchor, int x, int y, int w, int h,
                      const QString& fill, const QString& bind, const QString& text = QString(), double pulse = -1.0, bool gamepad = true)
    {
        QJsonObject e{ { QStringLiteral("id"), id }, { QStringLiteral("parent"), parent }, { QStringLiteral("name"), name }, { QStringLiteral("type"), type },
                       { QStringLiteral("anchor"), anchor }, { QStringLiteral("x"), x }, { QStringLiteral("y"), y }, { QStringLiteral("fill"), fill },
                       { QStringLiteral("bind"), bind }, { QStringLiteral("gamepad"), gamepad } };
        if (w > 0)
        {
            e.insert(QStringLiteral("w"), w);
            e.insert(QStringLiteral("h"), h);
        }
        if (!text.isEmpty())
        {
            e.insert(QStringLiteral("text"), text);
        }
        if (pulse >= 0)
        {
            e.insert(QStringLiteral("pulseBelow"), pulse);
        }
        m_elements << e;
    };
    add(QStringLiteral("hud"), QString(), QStringLiteral("HUD"), QStringLiteral("group"), QStringLiteral("top-left"), 0, 0, 0, 0, QStringLiteral("#ECE6DE"), QString());
    add(QStringLiteral("health"), QStringLiteral("hud"), QStringLiteral("Health bar"), QStringLiteral("bar"), QStringLiteral("bottom-left"), 40, 72, 280, 16, QStringLiteral("#E8743B"), QStringLiteral("player.health"), QString(), 0.25);
    add(QStringLiteral("stamina"), QStringLiteral("hud"), QStringLiteral("Stamina bar"), QStringLiteral("bar"), QStringLiteral("bottom-left"), 40, 48, 200, 8, QStringLiteral("#A6DD97"), QStringLiteral("player.stamina"));
    add(QStringLiteral("sanity"), QStringLiteral("hud"), QStringLiteral("Sanity meter"), QStringLiteral("meter"), QStringLiteral("bottom-left"), 280, 14, 52, 52, QStringLiteral("#F0D17A"), QStringLiteral("player.sanity"), QString(), 0.25);
    add(QStringLiteral("crosshair"), QStringLiteral("hud"), QStringLiteral("Crosshair"), QStringLiteral("crosshair"), QStringLiteral("center"), 0, 0, 24, 24, QStringLiteral("#ECE6DE"), QString());
    add(QStringLiteral("ammo"), QStringLiteral("hud"), QStringLiteral("Ammo counter"), QStringLiteral("text"), QStringLiteral("bottom-right"), 40, 48, 160, 40, QStringLiteral("#ECE6DE"), QStringLiteral("weapon.ammo"));
    add(QStringLiteral("interact"), QStringLiteral("hud"), QStringLiteral("Interact prompt"), QStringLiteral("text"), QStringLiteral("center"), 0, 72, 240, 28, QStringLiteral("#FFCFB4"), QStringLiteral("interact.prompt"));
    add(QStringLiteral("squad"), QStringLiteral("hud"), QStringLiteral("Squad list"), QStringLiteral("list"), QStringLiteral("top-left"), 40, 40, 200, 96, QStringLiteral("#ECE6DE"), QStringLiteral("squad.members"));
    add(QStringLiteral("row"), QStringLiteral("squad"), QStringLiteral("Player row"), QStringLiteral("row"), QStringLiteral("top-left"), 12, 8, 176, 24, QStringLiteral("#ECE6DE"), QString(), QStringLiteral("Chris · 100"));
    add(QStringLiteral("toasts"), QStringLiteral("hud"), QStringLiteral("Toasts"), QStringLiteral("list"), QStringLiteral("top-right"), 40, 40, 320, 72, QStringLiteral("#ECE6DE"), QStringLiteral("toasts"));
}

void HearthUiEditor::Load()
{
    QFile file(m_path);
    m_canvas = QJsonObject();
    if (file.open(QIODevice::ReadOnly))
    {
        m_canvas = QJsonDocument::fromJson(file.readAll()).object();
    }
    m_elements.clear();
    if (m_canvas.value(QStringLiteral("hearth_ui")).toInt() >= 1)
    {
        for (const QJsonValue value : m_canvas.value(QStringLiteral("elements")).toArray())
        {
            m_elements << value.toObject();
        }
        m_dirty = false;
    }
    else
    {
        if (QFileInfo::exists(m_path))
        {
            QMessageBox::warning(this, tr("Game UI editor"), tr("%1 isn't a Hearth UI layout (it may be a LyShine canvas). Starting a new layout; saving replaces it.").arg(m_path));
        }
        MakeDefault();
        m_dirty = true;
    }
    const auto projectName = AZ::Utils::GetProjectName();
    if (m_subtitle)
    {
        m_subtitle->setText(tr("%1 · %2").arg(QString::fromUtf8(projectName.c_str(), int(projectName.size())), QFileInfo(m_path).fileName()));
    }
    m_selected = m_elements.size() > 3 ? m_elements[3].value(QStringLiteral("id")).toString() : QString();
    const bool needsSave = m_dirty;
    Changed(true);
    m_dirty = needsSave;
    m_save->setEnabled(m_dirty);
}

void HearthUiEditor::Save()
{
    QJsonArray elements;
    for (const QJsonObject& element : m_elements)
    {
        elements.append(element);
    }
    m_canvas.insert(QStringLiteral("hearth_ui"), 1);
    m_canvas.insert(QStringLiteral("name"), QStringLiteral("HUD"));
    m_canvas.insert(QStringLiteral("designSize"), QJsonArray{ 1920, 1080 });
    m_canvas.insert(QStringLiteral("snap"), UiSnap);
    m_canvas.insert(QStringLiteral("elements"), elements);
    QDir().mkpath(QFileInfo(m_path).path());
    QSaveFile out(m_path);
    if (!out.open(QIODevice::WriteOnly))
    {
        QMessageBox::warning(this, tr("Save"), tr("Couldn't write %1.").arg(m_path));
        return;
    }
    out.write(QJsonDocument(m_canvas).toJson(QJsonDocument::Indented));
    if (out.commit())
    {
        m_dirty = false;
        m_save->setEnabled(false);
    }
}

void HearthUiEditor::Add(const QString& type)
{
    // Into the selected group or list, or the HUD.
    QString parent = QStringLiteral("hud");
    if (QJsonObject* selected = Find(m_selected))
    {
        const QString selectedType = selected->value(QStringLiteral("type")).toString();
        parent = (selectedType == QLatin1String("group") || selectedType == QLatin1String("list")) ? m_selected : selected->value(QStringLiteral("parent")).toString();
    }
    if (!Find(parent))
    {
        parent.clear();
    }
    const QMap<QString, QSize> sizes = { { QStringLiteral("text"), QSize(160, 32) }, { QStringLiteral("image"), QSize(96, 96) },
                                         { QStringLiteral("bar"), QSize(200, 12) }, { QStringLiteral("button"), QSize(160, 40) },
                                         { QStringLiteral("list"), QSize(200, 96) }, { QStringLiteral("meter"), QSize(52, 52) },
                                         { QStringLiteral("group"), QSize(0, 0) } };
    const QMap<QString, QString> names = { { QStringLiteral("text"), tr("Text") }, { QStringLiteral("image"), tr("Image") }, { QStringLiteral("bar"), tr("Bar") },
                                           { QStringLiteral("button"), tr("Button") }, { QStringLiteral("list"), tr("List") }, { QStringLiteral("meter"), tr("Meter") },
                                           { QStringLiteral("group"), tr("Group") } };
    QJsonObject element{ { QStringLiteral("id"), UiNewId() }, { QStringLiteral("parent"), parent }, { QStringLiteral("name"), names.value(type) },
                         { QStringLiteral("type"), type }, { QStringLiteral("anchor"), QStringLiteral("center") }, { QStringLiteral("x"), 0 }, { QStringLiteral("y"), 0 },
                         { QStringLiteral("fill"), type == QLatin1String("button") ? QStringLiteral("#E8743B") : QStringLiteral("#ECE6DE") },
                         { QStringLiteral("bind"), QString() }, { QStringLiteral("gamepad"), true } };
    if (type != QLatin1String("group"))
    {
        element.insert(QStringLiteral("w"), sizes.value(type).width());
        element.insert(QStringLiteral("h"), sizes.value(type).height());
    }
    m_elements << element;
    m_selected = element.value(QStringLiteral("id")).toString();
    Changed(true);
}

void HearthUiEditor::Select(const QString& id)
{
    m_selected = id;
    FillProperties();
    m_view->update();
    m_filling = true;
    QTreeWidgetItemIterator it(m_layers);
    while (*it)
    {
        if ((*it)->data(0, Qt::UserRole).toString() == id)
        {
            m_layers->setCurrentItem(*it);
        }
        ++it;
    }
    m_filling = false;
}

void HearthUiEditor::Changed(bool structure)
{
    m_dirty = true;
    m_save->setEnabled(true);
    if (structure)
    {
        FillLayers();
    }
    FillProperties();
    m_view->update();
}

void HearthUiEditor::FillLayers()
{
    m_filling = true;
    m_layers->clear();
    QMap<QString, QTreeWidgetItem*> items;
    for (const QJsonObject& element : m_elements)
    {
        const QString id = element.value(QStringLiteral("id")).toString();
        QTreeWidgetItem* parent = items.value(element.value(QStringLiteral("parent")).toString());
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_layers);
        item->setText(0, UiTypeIcon(element.value(QStringLiteral("type")).toString()) + QStringLiteral("  ") + element.value(QStringLiteral("name")).toString());
        item->setData(0, Qt::UserRole, id);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        items.insert(id, item);
        if (id == m_selected)
        {
            m_layers->setCurrentItem(item);
        }
    }
    m_layers->expandAll();
    m_filling = false;
    connect(m_layers, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item)
    {
        if (m_filling)
        {
            return;
        }
        QJsonObject* element = Find(item->data(0, Qt::UserRole).toString());
        if (element)
        {
            QString name = item->text(0);
            name.remove(QRegularExpression(QStringLiteral("^\\S+\\s+")));
            element->insert(QStringLiteral("name"), name.trimmed());
            m_dirty = true;
            m_save->setEnabled(true);
            FillProperties();
        }
    }, Qt::UniqueConnection);
}

void HearthUiEditor::FillProperties()
{
    m_snapLabel->setText(tr("Snap %1 px · %2%").arg(UiSnap).arg(qRound(m_view->Scale() * 100)));
    QJsonObject* element = Find(m_selected);
    m_propertiesBody->setEnabled(element != nullptr);
    if (!element)
    {
        m_selectedLabel->setText(tr("Nothing picked"));
        return;
    }
    m_filling = true;
    const QRect rect = ElementRect(*element);
    m_selectedLabel->setText(tr("%1 · %2 × %3").arg(element->value(QStringLiteral("name")).toString()).arg(rect.width()).arg(rect.height()));
    const QString anchor = element->value(QStringLiteral("anchor")).toString(QStringLiteral("top-left"));
    for (int i = 0; i < m_anchors.size(); ++i)
    {
        m_anchors[i]->setChecked(QLatin1String(UiAnchors[i]) == anchor);
    }
    m_x->setValue(element->value(QStringLiteral("x")).toInt());
    m_y->setValue(element->value(QStringLiteral("y")).toInt());
    m_w->setValue(rect.width());
    m_h->setValue(rect.height());
    const QString fill = element->value(QStringLiteral("fill")).toString(QStringLiteral("#ECE6DE"));
    m_fill->setText(fill);
    m_fill->setStyleSheet(QStringLiteral("QPushButton { background-color: %1; color: %2; border: none; border-radius: 8px; font-family: \"Geist Mono\"; }")
                              .arg(fill, QColor(fill).lightness() > 140 ? QStringLiteral("#1A0F08") : QStringLiteral("#ECE6DE")));
    m_bind->setCurrentText(element->value(QStringLiteral("bind")).toString());
    m_text->setText(element->value(QStringLiteral("text")).toString());
    const bool pulse = element->contains(QStringLiteral("pulseBelow"));
    m_pulse->setChecked(pulse);
    m_pulseBelow->setValue(qRound(element->value(QStringLiteral("pulseBelow")).toDouble(0.25) * 100));
    m_pulseBelow->setEnabled(pulse);
    m_gamepad->setChecked(element->value(QStringLiteral("gamepad")).toBool(true));
    m_filling = false;
}
