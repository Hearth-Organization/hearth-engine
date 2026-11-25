/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 * Hearth Engine: the splash screen and About box are drawn in the Hearth look (approved mockups
 * "Splash" and "About", Nov 25 2026).
 */

#include "StartupLogoDialog.h"

#include "EditorDefs.h"

#include <AzCore/Utils/Utils.h>
#include <AzQtComponents/Components/HearthWidgets.h>

// Qt
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QRadialGradient>
#include <QSettings>
#include <QSysInfo>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
    namespace Hearth = AzQtComponents::Hearth;

    const char* SourceUrl = "https://github.com/Hearth-Organization/hearth-engine";

    QString ProjectDisplayName()
    {
        const auto name = AZ::Utils::GetProjectName();
        QString project = QString::fromUtf8(name.c_str(), static_cast<int>(name.size()));
        // NoclipProtocol -> Noclip Protocol
        QString spaced;
        for (int i = 0; i < project.size(); ++i)
        {
            if (i > 0 && project[i].isUpper() && project[i - 1].isLower())
            {
                spaced += QLatin1Char(' ');
            }
            spaced += project[i];
        }
        return spaced;
    }

    QString LinkHtml(const QString& href, const QString& text)
    {
        return QStringLiteral("<a href=\"%1\" style=\"color:#FFCFB4;\">%2</a>").arg(href, text);
    }
} // namespace

CStartupLogoDialog* CStartupLogoDialog::s_pLogoWindow = nullptr;

CStartupLogoDialog::CStartupLogoDialog(
    DialogType dialogType, [[maybe_unused]] QString versionText, [[maybe_unused]] QString richTextCopyrightNotice, QWidget* pParent /*=nullptr*/)
    : QDialog(pParent)
    , m_dialogType(dialogType)
{
    s_pLogoWindow = this;
    setAttribute(Qt::WA_TranslucentBackground, true);
    setWindowIcon(Hearth::AppIcon());

    switch (m_dialogType)
    {
    case Loading:
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        setWindowTitle(tr("Starting Hearth Engine"));
        setFixedSize(QSize(EnforcedWidth, EnforcedHeight));
        BuildLoading();
        break;
    case About:
        setWindowFlags(Qt::FramelessWindowHint | Qt::Popup | Qt::NoDropShadowWindowHint);
        setWindowTitle(tr("About Hearth Engine"));
        setFixedSize(QSize(460, 330));
        BuildAbout();
        break;
    }
}

CStartupLogoDialog::~CStartupLogoDialog()
{
    if (m_dialogType == Loading && m_stepsSeen > 5)
    {
        // Remember how many startup steps there were, so next time the progress bar knows how far along it is.
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        settings->setValue(QStringLiteral("splash/steps"), m_stepsSeen);
    }
    s_pLogoWindow = nullptr;
}

void CStartupLogoDialog::BuildLoading()
{
    {
        QScopedPointer<QSettings> settings(Hearth::NewSettings());
        m_stepsExpected = qMax(10, settings->value(QStringLiteral("splash/steps"), 40).toInt());
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 18);
    root->setSpacing(0);
    root->addStretch(3);

    auto* tile = new QLabel(this);
    tile->setPixmap(Hearth::FlameTile(64, devicePixelRatioF()));
    tile->setFixedSize(64, 64);
    tile->setStyleSheet(QStringLiteral("background: transparent;"));
    root->addWidget(tile, 0, Qt::AlignHCenter);
    root->addSpacing(18);

    auto* title = Hearth::MakeLabel(tr("Hearth Engine"), QStringLiteral("big"), this);
    title->setStyleSheet(QStringLiteral("background: transparent; color: #ECE6DE; font-size: 30px; font-weight: 600;"));
    root->addWidget(title, 0, Qt::AlignHCenter);
    root->addSpacing(4);

    QString version = tr("Version %1").arg(Hearth::Version());
    auto* versionLabel = Hearth::MakeLabel(version, QStringLiteral("small"), this);
    versionLabel->setStyleSheet(QStringLiteral("background: transparent; color: #8D857B; font-family: \"Geist Mono\"; font-size: 11px;"));
    root->addWidget(versionLabel, 0, Qt::AlignHCenter);
    root->addSpacing(26);

    auto* progressBox = new QWidget(this);
    progressBox->setFixedWidth(300);
    progressBox->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* progressLayout = new QVBoxLayout(progressBox);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(8);
    m_progress = new QProgressBar(progressBox);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(4);
    m_progress->setStyleSheet(QStringLiteral(
        "QProgressBar { background-color: rgba(255, 255, 255, 20); border: none; border-radius: 2px; }"
        "QProgressBar::chunk { background-color: #E8743B; border-radius: 2px; }"));
    progressLayout->addWidget(m_progress);
    auto* stepRow = new QHBoxLayout();
    stepRow->setSpacing(10);
    m_stepLabel = Hearth::MakeLabel(tr("Starting"), QStringLiteral("small"), progressBox);
    m_stepLabel->setStyleSheet(QStringLiteral("background: transparent; color: #8D857B; font-family: \"Geist Mono\"; font-size: 11px;"));
    m_percentLabel = Hearth::MakeLabel(QStringLiteral("0%"), QStringLiteral("small"), progressBox);
    m_percentLabel->setStyleSheet(m_stepLabel->styleSheet());
    stepRow->addWidget(m_stepLabel, 1);
    stepRow->addWidget(m_percentLabel, 0, Qt::AlignRight);
    progressLayout->addLayout(stepRow);
    root->addWidget(progressBox, 0, Qt::AlignHCenter);

    root->addStretch(4);

    auto* footer = new QHBoxLayout();
    footer->setSpacing(8);
    auto* dot = new QLabel(this);
    dot->setFixedSize(8, 8);
    dot->setStyleSheet(QStringLiteral("background-color: #A6DD97; border-radius: 4px;"));
    footer->addWidget(dot, 0, Qt::AlignVCenter);
    const QString project = ProjectDisplayName();
    footer->addWidget(Hearth::MakeLabel(project.isEmpty() ? tr("Opening the editor") : tr("Opening %1").arg(project),
        QStringLiteral("small"), this));
    footer->addStretch(1);
    footer->addWidget(Hearth::MakeLabel(tr("Built on O3DE (Apache 2.0)"), QStringLiteral("small"), this));
    root->addLayout(footer);
}

void CStartupLogoDialog::BuildAbout()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 26, 28, 22);
    root->setSpacing(6);

    auto* tile = new QLabel(this);
    tile->setPixmap(Hearth::FlameTile(56, devicePixelRatioF()));
    tile->setFixedSize(56, 56);
    tile->setStyleSheet(QStringLiteral("background: transparent;"));
    root->addWidget(tile, 0, Qt::AlignHCenter);
    root->addSpacing(8);

    auto* title = Hearth::MakeLabel(tr("Hearth Engine"), QStringLiteral("big"), this);
    title->setStyleSheet(QStringLiteral("background: transparent; color: #ECE6DE; font-size: 20px; font-weight: 600;"));
    root->addWidget(title, 0, Qt::AlignHCenter);

    QString versionLine = tr("Version %1").arg(Hearth::Version());
    const QString build = Hearth::Build();
    if (!build.isEmpty())
    {
        versionLine += tr(" · build %1").arg(build);
    }
    versionLine += QStringLiteral(" · ") + (QSysInfo::kernelType() == QLatin1String("linux") ? tr("Linux") : QSysInfo::prettyProductName());
    auto* versionLabel = Hearth::MakeLabel(versionLine, QStringLiteral("small"), this);
    versionLabel->setStyleSheet(QStringLiteral("background: transparent; color: #ECE6DE; font-size: 12px;"));
    root->addWidget(versionLabel, 0, Qt::AlignHCenter);
    root->addSpacing(8);

    auto* blurb = Hearth::MakeLabel(
        tr("Made by Dragongamer9 for Hearth OS. Built on O3DE, the Open 3D Engine (Apache 2.0), with Hearth's own look, "
           "tools and laptop tuning."),
        QStringLiteral("dim"), this);
    blurb->setAlignment(Qt::AlignHCenter);
    blurb->setStyleSheet(QStringLiteral("background: transparent; color: #8D857B; font-size: 12px;"));
    root->addWidget(blurb);
    root->addSpacing(8);

    const QDir engineRoot(Hearth::EngineRoot());
    QString licenses = engineRoot.filePath(QStringLiteral("LICENSE.txt"));
    QString changes = engineRoot.filePath(QStringLiteral("HEARTH_CHANGES.md"));
    auto* links = new QLabel(this);
    links->setTextFormat(Qt::RichText);
    links->setTextInteractionFlags(Qt::TextBrowserInteraction);
    links->setOpenExternalLinks(false);
    links->setStyleSheet(QStringLiteral("background: transparent; font-size: 12px;"));
    links->setText(LinkHtml(QLatin1String(SourceUrl), tr("Source code")) + QStringLiteral("&nbsp;&nbsp;&nbsp;&nbsp;") +
                   LinkHtml(QUrl::fromLocalFile(licenses).toString(), tr("Licenses")) + QStringLiteral("&nbsp;&nbsp;&nbsp;&nbsp;") +
                   LinkHtml(QFile::exists(changes) ? QUrl::fromLocalFile(changes).toString() : QString::fromLatin1(SourceUrl) + QStringLiteral("/releases"),
                            tr("What's new")));
    connect(links, &QLabel::linkActivated, this, [](const QString& link) { QDesktopServices::openUrl(QUrl(link)); });
    root->addWidget(links, 0, Qt::AlignHCenter);
    root->addStretch(1);

    auto* close = Hearth::MakeButton(tr("Close"), false, this);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    root->addWidget(close, 0, Qt::AlignHCenter);
}

void CStartupLogoDialog::focusOutEvent([[maybe_unused]] QFocusEvent*)
{
    if (m_dialogType == About)
    {
        accept();
    }
}

void CStartupLogoDialog::SetText(const char* text)
{
    if (s_pLogoWindow)
    {
        s_pLogoWindow->SetInfoText(text);
    }
}

void CStartupLogoDialog::SetInfoText(const char* text)
{
    QMetaObject::invokeMethod(
        this,
        [this, text = QString(text)]()
        {
            if (!m_stepLabel)
            {
                return;
            }
            ++m_stepsSeen;
            const int percent = qBound(1, m_stepsSeen * 100 / qMax(1, m_stepsExpected), 99);
            m_stepLabel->setText(m_stepLabel->fontMetrics().elidedText(text, Qt::ElideRight, 240));
            m_percentLabel->setText(QStringLiteral("%1%").arg(percent));
            m_progress->setValue(percent);
        });
}

void CStartupLogoDialog::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF area = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(QColor(255, 255, 255, 26), 1));
    painter.setBrush(m_dialogType == Loading ? Hearth::Background() : Hearth::Card());
    painter.drawRoundedRect(area, 16, 16);

    if (m_dialogType == Loading)
    {
        // Soft ember glow behind the flame.
        QRadialGradient glow(QPointF(width() / 2.0, height() * 0.38), width() * 0.42);
        glow.setColorAt(0.0, QColor(232, 116, 59, 46));
        glow.setColorAt(1.0, QColor(232, 116, 59, 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(glow);
        painter.drawRoundedRect(area, 16, 16);
    }
}
