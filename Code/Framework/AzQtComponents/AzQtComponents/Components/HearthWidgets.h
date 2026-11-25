/*
 * Hearth Engine shared look and helpers (header only, no moc needed).
 * Built on O3DE (Apache-2.0 OR MIT).
 *
 * Colours, the flame icon, cards, toggles, chips, pill buttons and the Hearth paths/version, used by the editor,
 * the Hub (Project Manager) and the tools, so every screen matches the approved mockups.
 */
#pragma once

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>
#include <QUrlQuery>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

namespace AzQtComponents
{
    namespace Hearth
    {
        // ---------- Colours (from the approved mockups) ----------
        inline QColor Background() { return QColor(0x0D, 0x0C, 0x0B); }
        inline QColor Card() { return QColor(0x17, 0x15, 0x13); }
        inline QColor Input() { return QColor(0x1C, 0x19, 0x17); }
        inline QColor Ember() { return QColor(0xE8, 0x74, 0x3B); }
        inline QColor EmberLight() { return QColor(0xFF, 0xCF, 0xB4); }
        inline QColor Ink() { return QColor(0x1A, 0x0F, 0x08); }
        inline QColor Text() { return QColor(0xEC, 0xE6, 0xDE); }
        inline QColor Dim() { return QColor(0x8D, 0x85, 0x7B); }
        inline QColor Good() { return QColor(0xA6, 0xDD, 0x97); }
        inline QColor Warn() { return QColor(0xF0, 0xD1, 0x7A); }
        inline QColor Bad() { return QColor(0xFF, 0x9F, 0x8F); }

        inline const char* CardStyle()
        {
            return "background-color: #171513; border: 1px solid rgba(255, 255, 255, 15); border-radius: 14px;";
        }

        // ---------- Paths and version ----------

        //! Engine root: HEARTH_ENGINE_ROOT (set by the hearth-engine launcher), else four folders above the binaries
        //! (bin/Linux/<config>/Default).
        inline QString EngineRoot()
        {
            const QString fromEnv = qEnvironmentVariable("HEARTH_ENGINE_ROOT");
            if (!fromEnv.isEmpty())
            {
                return fromEnv;
            }
            QDir dir(QCoreApplication::applicationDirPath());
            for (int i = 0; i < 4; ++i)
            {
                if (QFile::exists(dir.filePath("HEARTH_VERSION")) || QFile::exists(dir.filePath("engine.json")))
                {
                    return dir.absolutePath();
                }
                dir.cdUp();
            }
            return dir.absolutePath();
        }

        inline QString ReadFirstLine(const QString& path)
        {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly | QIODevice::Text))
            {
                return QString::fromUtf8(file.readLine()).trimmed();
            }
            return {};
        }

        //! Hearth Engine version (Dragongamer9 picks it; the .deb writes it to HEARTH_VERSION).
        inline QString Version()
        {
            const QString fromEnv = qEnvironmentVariable("HEARTH_ENGINE_VERSION");
            if (!fromEnv.isEmpty())
            {
                return fromEnv;
            }
            const QString fromFile = ReadFirstLine(QDir(EngineRoot()).filePath("HEARTH_VERSION"));
            return fromFile.isEmpty() ? QStringLiteral("dev") : fromFile;
        }

        //! GitHub build number the .deb came from (HEARTH_BUILD), empty for local builds.
        inline QString Build()
        {
            return ReadFirstLine(QDir(EngineRoot()).filePath("HEARTH_BUILD"));
        }

        inline QString HomeFolder()
        {
            return QDir(QDir::homePath()).filePath(QStringLiteral("O3DE/HearthEngine"));
        }

        //! Every run's log goes here (the launcher and the editor), so Claude can read all errors.
        inline QString LogsFolder()
        {
            return QDir(HomeFolder()).filePath(QStringLiteral("logs"));
        }

        //! Hearth Engine settings (first-run choices, laptop preset, ...). Never changed by the engine on its own.
        inline QString SettingsPath()
        {
            const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
            return QDir(config).filePath(QStringLiteral("hearth-engine/settings.ini"));
        }

        inline QSettings* NewSettings(QObject* parent = nullptr)
        {
            return new QSettings(SettingsPath(), QSettings::IniFormat, parent);
        }

        // ---------- Claude ----------

        //! Hand a problem to Claude: the full report goes to ~/O3DE/HearthEngine/claude-requests/<time>.md, a short
        //! message about it goes on the clipboard and claude.ai opens with it filled in. When Claude is done it leaves
        //! a note in ~/O3DE/HearthEngine/claude-results/ (the Hub shows it as "Claude fixed it").
        inline QString AskClaude(const QString& title, const QString& details, const QString& projectPath = {})
        {
            QDir requests(QDir(HomeFolder()).filePath(QStringLiteral("claude-requests")));
            requests.mkpath(QStringLiteral("."));
            const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
            const QString path = requests.filePath(stamp + QStringLiteral(".md"));
            QFile file(path);
            if (file.open(QIODevice::WriteOnly | QIODevice::Text))
            {
                const QString text = QStringLiteral("# %1\n\n- Time: %2\n- Hearth Engine: %3\n- Project: %4\n- Logs: %5\n\n```\n%6\n```\n")
                                         .arg(title, QDateTime::currentDateTime().toString(Qt::ISODate), Version(), projectPath,
                                              LogsFolder(), details);
                file.write(text.toUtf8());
            }
            const QString message = QCoreApplication::translate("Hearth",
                "%1. The report is in %2 and the logs in %3. Please read them, fix the cause, and leave a note in %4 when it's done.")
                                        .arg(title, path, LogsFolder(), QDir(HomeFolder()).filePath(QStringLiteral("claude-results")));
            QApplication::clipboard()->setText(message);
            QUrl url(QStringLiteral("https://claude.ai/new"));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("q"), message);
            url.setQuery(query);
            QDesktopServices::openUrl(url);
            return path;
        }

        // ---------- The flame ----------

        //! The Hearth flame path (18 x 18 box): M9 1 C12 5 14 7 14 11 A5 5 0 0 1 4 11 C4 8 6 7 7 4 C8 6 9 6 9 1 Z
        inline QPainterPath FlamePath()
        {
            QPainterPath path;
            path.moveTo(9, 1);
            path.cubicTo(12, 5, 14, 7, 14, 11);
            path.arcTo(QRectF(4, 6, 10, 10), 0, -180); // half circle under the flame, from (14,11) to (4,11)
            path.cubicTo(4, 8, 6, 7, 7, 4);
            path.cubicTo(8, 6, 9, 6, 9, 1);
            path.closeSubpath();
            return path;
        }

        //! Ember rounded tile with the ink flame (the app icon / header tile). size is in pixels.
        inline QPixmap FlameTile(int size, qreal devicePixelRatio = 1.0)
        {
            QPixmap pixmap(QSize(size, size) * devicePixelRatio);
            pixmap.setDevicePixelRatio(devicePixelRatio);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            const qreal unit = size / 30.0; // tile is 30 units, radius 9
            painter.setPen(Qt::NoPen);
            painter.setBrush(Ember());
            painter.drawRoundedRect(QRectF(0, 0, size, size), 9 * unit, 9 * unit);
            painter.translate(6 * unit, 6 * unit);
            painter.scale(unit, unit);
            painter.setBrush(Ink());
            painter.drawPath(FlamePath());
            return pixmap;
        }

        //! Just the flame in a colour (small icons).
        inline QPixmap Flame(int size, const QColor& color, qreal devicePixelRatio = 1.0)
        {
            QPixmap pixmap(QSize(size, size) * devicePixelRatio);
            pixmap.setDevicePixelRatio(devicePixelRatio);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.scale(size / 18.0, size / 18.0);
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawPath(FlamePath());
            return pixmap;
        }

        inline QIcon AppIcon()
        {
            QIcon icon;
            for (int size : { 16, 24, 32, 48, 64, 128, 256 })
            {
                icon.addPixmap(FlameTile(size));
            }
            return icon;
        }

        // ---------- Widgets ----------

        //! Label with Hearth text styling. kind: "title", "big", "dim", "small", "mono", "body".
        inline QLabel* MakeLabel(const QString& text, const QString& kind = QStringLiteral("body"), QWidget* parent = nullptr)
        {
            auto* label = new QLabel(text, parent);
            label->setWordWrap(kind == QLatin1String("body") || kind == QLatin1String("dim"));
            QString style = QStringLiteral("background: transparent; border: none; ");
            if (kind == QLatin1String("title"))
            {
                style += QStringLiteral("color: #ECE6DE; font-size: 13px; font-weight: 600;");
            }
            else if (kind == QLatin1String("big"))
            {
                style += QStringLiteral("color: #ECE6DE; font-size: 26px; font-weight: 600;");
            }
            else if (kind == QLatin1String("dim"))
            {
                style += QStringLiteral("color: #8D857B; font-size: 12px;");
            }
            else if (kind == QLatin1String("small"))
            {
                style += QStringLiteral("color: #8D857B; font-size: 11px;");
            }
            else if (kind == QLatin1String("mono"))
            {
                style += QStringLiteral("color: #ECE6DE; font-family: \"Geist Mono\"; font-size: 12px;");
            }
            else
            {
                style += QStringLiteral("color: #ECE6DE; font-size: 13px;");
            }
            label->setStyleSheet(style);
            return label;
        }

        //! Pill button: primary (ember with ink text) or ghost (outline).
        inline QPushButton* MakeButton(const QString& text, bool primary = false, QWidget* parent = nullptr)
        {
            auto* button = new QPushButton(text, parent);
            button->setCursor(Qt::PointingHandCursor);
            button->setMinimumHeight(30);
            if (primary)
            {
                button->setStyleSheet(QStringLiteral(
                    "QPushButton { background-color: #E8743B; color: #1A0F08; font-weight: 600; border: none;"
                    " border-radius: 15px; padding: 0px 18px; }"
                    "QPushButton:hover { background-color: #F08A55; }"
                    "QPushButton:pressed { background-color: #C9602C; }"
                    "QPushButton:disabled { background-color: #4A2A1A; color: #8D857B; }"));
            }
            else
            {
                button->setStyleSheet(QStringLiteral(
                    "QPushButton { background-color: transparent; color: #ECE6DE; border: 1px solid rgba(255, 255, 255, 26);"
                    " border-radius: 15px; padding: 0px 16px; }"
                    "QPushButton:hover { background-color: rgba(255, 255, 255, 15); }"
                    "QPushButton:pressed { background-color: rgba(255, 255, 255, 8); }"
                    "QPushButton:disabled { color: #6E675E; }"));
            }
            return button;
        }

        //! Small rounded status chip. tone: "good", "warn", "bad", "ember", "plain".
        inline QLabel* MakeChip(const QString& text, const QString& tone = QStringLiteral("plain"), QWidget* parent = nullptr)
        {
            auto* chip = new QLabel(text, parent);
            QString colors = QStringLiteral("background-color: rgba(255, 255, 255, 15); color: #8D857B;");
            if (tone == QLatin1String("good"))
            {
                colors = QStringLiteral("background-color: rgba(127, 207, 106, 31); color: #A6DD97;");
            }
            else if (tone == QLatin1String("warn"))
            {
                colors = QStringLiteral("background-color: rgba(232, 190, 59, 36); color: #F0D17A;");
            }
            else if (tone == QLatin1String("bad"))
            {
                colors = QStringLiteral("background-color: rgba(232, 80, 59, 41); color: #FF9F8F;");
            }
            else if (tone == QLatin1String("ember"))
            {
                colors = QStringLiteral("background-color: rgba(232, 116, 59, 41); color: #FFCFB4;");
            }
            chip->setStyleSheet(colors + QStringLiteral(" border-radius: 10px; padding: 3px 10px; font-size: 11px; border: none;"));
            return chip;
        }

        //! A Hearth card (rounded panel) with a vertical layout ready to fill.
        inline QFrame* MakeCard(QWidget* parent = nullptr, QVBoxLayout** layoutOut = nullptr)
        {
            auto* card = new QFrame(parent);
            card->setObjectName(QStringLiteral("HearthCard"));
            card->setStyleSheet(QStringLiteral("QFrame#HearthCard { ") + QLatin1String(CardStyle()) + QStringLiteral(" }"));
            auto* layout = new QVBoxLayout(card);
            layout->setContentsMargins(14, 14, 14, 14);
            layout->setSpacing(10);
            if (layoutOut)
            {
                *layoutOut = layout;
            }
            return card;
        }

        //! The header row every Hearth screen starts with: flame tile, title, subtitle. Returns the row layout so
        //! callers can add buttons on the right (after a stretch).
        inline QHBoxLayout* MakeHeader(QWidget* parent, const QString& title, const QString& subtitle)
        {
            auto* row = new QHBoxLayout();
            row->setContentsMargins(8, 6, 8, 6);
            row->setSpacing(14);
            auto* tile = new QLabel(parent);
            tile->setPixmap(FlameTile(30, parent ? parent->devicePixelRatioF() : 1.0));
            tile->setFixedSize(30, 30);
            tile->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
            row->addWidget(tile);
            auto* titles = new QVBoxLayout();
            titles->setSpacing(0);
            titles->addWidget(MakeLabel(title, QStringLiteral("title"), parent));
            if (!subtitle.isEmpty())
            {
                titles->addWidget(MakeLabel(subtitle, QStringLiteral("small"), parent));
            }
            row->addLayout(titles);
            row->addStretch(1);
            return row;
        }

        //! Hearth on/off switch (34 x 20, ember when on). It is a checkable QAbstractButton: use toggled(bool).
        class Toggle : public QAbstractButton
        {
        public:
            explicit Toggle(QWidget* parent = nullptr)
                : QAbstractButton(parent)
            {
                setCheckable(true);
                setCursor(Qt::PointingHandCursor);
                setFixedSize(34, 20);
            }

            QSize sizeHint() const override
            {
                return QSize(34, 20);
            }

        protected:
            void paintEvent(QPaintEvent*) override
            {
                QPainter painter(this);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(Qt::NoPen);
                const bool on = isChecked();
                QColor track = on ? Ember() : QColor(255, 255, 255, 26);
                if (!isEnabled())
                {
                    track.setAlpha(90);
                }
                painter.setBrush(track);
                painter.drawRoundedRect(QRectF(0, 0, 34, 20), 10, 10);
                painter.setBrush(on ? Ink() : Dim());
                painter.drawEllipse(QRectF(on ? 17 : 3, 3, 14, 14));
            }
        };

        //! One settings row: label and hint on the left, a control on the right, a thin line above.
        inline QWidget* MakeSettingRow(const QString& label, const QString& hint, QWidget* control, QWidget* parent = nullptr)
        {
            auto* row = new QWidget(parent);
            row->setObjectName(QStringLiteral("HearthSettingRow"));
            row->setStyleSheet(QStringLiteral(
                "QWidget#HearthSettingRow { border-top: 1px solid rgba(255, 255, 255, 15); background: transparent; }"));
            auto* layout = new QHBoxLayout(row);
            layout->setContentsMargins(4, 9, 4, 9);
            layout->setSpacing(12);
            auto* texts = new QVBoxLayout();
            texts->setSpacing(2);
            texts->addWidget(MakeLabel(label, QStringLiteral("body"), row));
            if (!hint.isEmpty())
            {
                texts->addWidget(MakeLabel(hint, QStringLiteral("small"), row));
            }
            layout->addLayout(texts, 1);
            if (control)
            {
                layout->addWidget(control, 0, Qt::AlignVCenter);
            }
            return row;
        }
    } // namespace Hearth
} // namespace AzQtComponents
