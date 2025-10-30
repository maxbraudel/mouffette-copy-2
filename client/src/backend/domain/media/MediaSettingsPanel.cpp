#include "backend/domain/media/MediaSettingsPanel.h"
#include "backend/files/Theme.h"
#include "frontend/ui/theme/AppColors.h"
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QPushButton>
#include <QFont>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QSpacerItem>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>
#include "backend/domain/media/MediaItems.h" // for ResizableMediaBase

namespace {

constexpr int kOptionVerticalSpacing = 10; // Global vertical spacing between settings rows
constexpr int kHeaderVerticalSpacing = 35; // Space between headers and the next option row
constexpr int kHeaderFirstRowTopMargin = kHeaderVerticalSpacing - kOptionVerticalSpacing; // Supplemental space we insert after each header
constexpr int kOptionRowHeight = 20; // Baseline height for every option row (user-tunable)

QString tabButtonStyle(bool active, const QString& overlayTextCss) {
    const QString fontCss = AppColors::canvasButtonFontCss();
    if (active) {
        return QString(
            "QPushButton {"
            " padding: 8px 0px;"
            " %1 "
            " color: white;"
            " background: rgba(255,255,255,0.1);"
            " border: none;"
            " border-radius: 0px;"
            " margin: 0px;"
            "}"
            "QPushButton:hover {"
            " color: white;"
            " background: rgba(255,255,255,0.15);"
            "}"
        ).arg(fontCss);
    }

    return QString(
        "QPushButton {"
        " padding: 8px 0px;"
        " %1 "
        " color: %2;"
        " background: transparent;"
        " border: none;"
        " border-radius: 0px;"
        " margin: 0px;"
        "}"
        "QPushButton:hover {"
        " color: white;"
        " background: rgba(255,255,255,0.05);"
        "}"
        "QPushButton:pressed {"
        " color: white;"
        " background: rgba(255,255,255,0.1);"
        "}"
    ).arg(fontCss, overlayTextCss);
}

} // anonymous namespace

MediaSettingsPanel::MediaSettingsPanel(QWidget* parentWidget)
    : QObject(parentWidget)
{
    buildUi(parentWidget);
}

void MediaSettingsPanel::buildUi(QWidget* parentWidget) {
    m_widget = new QWidget(parentWidget);
    m_widget->setObjectName("MediaSettingsPanelWidget");

    // Enable styled background so the QWidget can paint the overlay chrome itself
    m_widget->setAttribute(Qt::WA_StyledBackground, true);
    // Ensure overlay blocks mouse events to canvas behind it (like media overlay)
    m_widget->setAttribute(Qt::WA_NoMousePropagation, true);
    // Apply the same background, border, and typography styling as the media list overlay
    const QString overlayTextCss = AppColors::colorToCss(AppColors::gOverlayTextColor);
    const QString overlayBorderCss = AppColors::colorToCss(AppColors::gOverlayBorderColor);
    const QString overlayTextStyle = QStringLiteral("color: %1;").arg(overlayTextCss);
    const QString widgetStyle = QString(
        "#MediaSettingsPanelWidget {"
        " background-color: %1;"
        " border: 1px solid %2;"
        " border-radius: %3px;"
        " color: %4;"
        " %5 "
        "}"
        " #MediaSettingsPanelWidget * {"
        " background-color: transparent;"
        "}"
    )
    .arg(AppColors::colorToCss(AppColors::gOverlayBackgroundColor))
    .arg(overlayBorderCss)
    .arg(QString::number(gOverlayCornerRadiusPx))
    .arg(overlayTextCss)
    .arg(AppColors::canvasMediaSettingsOptionsFontCss());
    m_widget->setStyleSheet(widgetStyle);
    m_widget->setAutoFillBackground(true);

    // Root layout on widget
    m_rootLayout = new QVBoxLayout(m_widget);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(0);
    m_rootLayout->setSizeConstraint(QLayout::SetNoConstraint);

    // Create fused double-button tab switcher at the top
    m_tabSwitcherContainer = new QWidget(m_widget);
    m_tabSwitcherContainer->setFixedHeight(40);
    m_tabSwitcherContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* tabSwitcherLayout = new QHBoxLayout(m_tabSwitcherContainer);
    tabSwitcherLayout->setContentsMargins(0, 0, 0, 0);
    tabSwitcherLayout->setSpacing(0);
    
    m_sceneTabButton = new QPushButton("Scene", m_tabSwitcherContainer);
    m_elementTabButton = new QPushButton("Element", m_tabSwitcherContainer);
    
    // Apply bold font to tab buttons to match section headers
    QFont tabFont = m_sceneTabButton->font();
    AppColors::applyCanvasButtonFont(tabFont);
    m_sceneTabButton->setFont(tabFont);
    m_elementTabButton->setFont(tabFont);
    
    m_sceneTabButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_elementTabButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_sceneTabButton->setFixedHeight(40);
    m_elementTabButton->setFixedHeight(40);
    
    tabSwitcherLayout->addWidget(m_sceneTabButton);
    // Add vertical separator line between buttons
    auto* separator = new QWidget(m_tabSwitcherContainer);
    separator->setFixedWidth(1);
    separator->setStyleSheet(QStringLiteral("background-color: %1;").arg(overlayBorderCss));
    tabSwitcherLayout->addWidget(separator);
    tabSwitcherLayout->addWidget(m_elementTabButton);
    
    m_rootLayout->addWidget(m_tabSwitcherContainer);
    
    // Add bottom separator for the tab switcher
    m_tabSwitcherSeparator = new QWidget(m_widget);
    m_tabSwitcherSeparator->setFixedHeight(1);
    m_tabSwitcherSeparator->setStyleSheet(QStringLiteral("background-color: %1;").arg(overlayBorderCss));
    m_rootLayout->addWidget(m_tabSwitcherSeparator);
    
    // Connect tab buttons
    QObject::connect(m_sceneTabButton, &QPushButton::clicked, this, &MediaSettingsPanel::onSceneTabClicked);
    QObject::connect(m_elementTabButton, &QPushButton::clicked, this, &MediaSettingsPanel::onElementTabClicked);

    // Container that holds the scrollable content
    m_scrollContainer = new QWidget(m_widget);
    m_scrollContainer->setObjectName("MediaSettingsScrollContainer");
    m_scrollContainer->setAttribute(Qt::WA_StyledBackground, true);
    m_scrollContainer->setAttribute(Qt::WA_NoMousePropagation, true);
    m_scrollContainer->setAutoFillBackground(false);
    m_scrollContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_scrollContainer->installEventFilter(this);
    auto* scrollContainerLayout = new QVBoxLayout(m_scrollContainer);
    scrollContainerLayout->setContentsMargins(0, 0, 0, 0);
    scrollContainerLayout->setSpacing(0);

    // Scroll area for inner content (mirror media overlay)
    m_scrollArea = new QScrollArea(m_scrollContainer);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setAlignment(Qt::AlignTop);
    // Ensure scroll area blocks mouse propagation
    m_scrollArea->setAttribute(Qt::WA_NoMousePropagation, true);
    if (auto* hBar = m_scrollArea->horizontalScrollBar()) { hBar->setEnabled(false); hBar->hide(); }
    if (m_scrollArea->viewport()) {
        m_scrollArea->viewport()->setAutoFillBackground(false);
        // Block mouse propagation on viewport too
        m_scrollArea->viewport()->setAttribute(Qt::WA_NoMousePropagation, true);
    }
    if (auto* vBar = m_scrollArea->verticalScrollBar()) vBar->hide();
    m_scrollArea->setStyleSheet(
        "QAbstractScrollArea { background: transparent; border: none; }"
        " QAbstractScrollArea > QWidget#qt_scrollarea_viewport { background: transparent; margin: 0; }"
        " QAbstractScrollArea::corner { background: transparent; }"
        " QScrollArea QScrollBar:vertical { width: 0px; margin: 0; background: transparent; }"
    );
    scrollContainerLayout->addWidget(m_scrollArea);
    m_rootLayout->addWidget(m_scrollContainer);
    m_scrollContainer->setVisible(true);

    // Inner content widget inside scroll area (with margins like before)
    m_innerContent = new QWidget(m_scrollArea);
    m_innerContent->setAttribute(Qt::WA_StyledBackground, true);
    m_innerContent->setAttribute(Qt::WA_NoMousePropagation, true);
    m_innerContent->setStyleSheet(QString(
        "background-color: transparent; color: %1; %2"
    ).arg(overlayTextCss, AppColors::canvasMediaSettingsOptionsFontCss()));
    m_scrollArea->setWidget(m_innerContent);

    // Content layout
    m_contentLayout = new QVBoxLayout(m_innerContent);
    m_contentLayout->setContentsMargins(15, 10, 15, 10);
    m_contentLayout->setSpacing(kOptionVerticalSpacing);
    m_contentLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    m_contentLayout->setAlignment(Qt::AlignTop);

    auto configureRow = [](QWidget* row) {
        if (!row) return;
        row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        row->setMinimumHeight(kOptionRowHeight);
        row->setMaximumHeight(kOptionRowHeight);
        row->setVisible(true);
    };

    auto configureRowLayout = [](QHBoxLayout* layout) {
        if (!layout) return;
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->setAlignment(Qt::AlignVCenter);
    };
    
    // Create Scene Options container grouped by category
    m_sceneOptionsContainer = new QWidget(m_innerContent);
    m_sceneOptionsLayout = new QVBoxLayout(m_sceneOptionsContainer);
    m_sceneOptionsLayout->setContentsMargins(0, 0, 0, 0);
    m_sceneOptionsLayout->setSpacing(kOptionVerticalSpacing);
    m_sceneOptionsLayout->setAlignment(Qt::AlignTop);
    m_contentLayout->addWidget(m_sceneOptionsContainer);

    bool sceneFirstSection = true;
    auto addSceneSectionHeader = [&](const QString& text, QSpacerItem*& headerGap) -> std::pair<QSpacerItem*, QLabel*> {
        QSpacerItem* spacer = nullptr;
        if (!sceneFirstSection) {
            spacer = new QSpacerItem(0, kHeaderFirstRowTopMargin, QSizePolicy::Minimum, QSizePolicy::Fixed);
            m_sceneOptionsLayout->addItem(spacer);
        }
        sceneFirstSection = false;

        auto* header = new QLabel(text, m_sceneOptionsContainer);
        QFont font = header->font();
        AppColors::applyCanvasMediaSettingsSectionHeadersFont(font);
        header->setFont(font);
        header->setStyleSheet(QString("%1 %2").arg(overlayTextStyle, AppColors::canvasMediaSettingsSectionHeadersFontCss()));
        header->setContentsMargins(0, 0, 0, 0);

        m_sceneOptionsLayout->addWidget(header);
        headerGap = new QSpacerItem(0, kHeaderFirstRowTopMargin, QSizePolicy::Minimum, QSizePolicy::Fixed);
        m_sceneOptionsLayout->addItem(headerGap);
        return {spacer, header};
    };

    // Helper to create a small value box label like [1]
    auto makeValueBox = [&](const QString& text = QStringLiteral("1")) {
        auto* box = new QLabel(text);
        box->setAlignment(Qt::AlignCenter);
        // Make clickable and install event filter for click handling
        box->setAttribute(Qt::WA_Hover, true);
        box->setFocusPolicy(Qt::ClickFocus); // allow focus for key events
        box->installEventFilter(this);
        setBoxActive(box, false); // set initial inactive style
        box->setMinimumWidth(28);
        return box;
    };

    {
        QSpacerItem* gap = nullptr;
        auto headerInfo = addSceneSectionHeader("Image", gap);
        m_sceneImageHeader = headerInfo.second;
        m_sceneImageHeaderGap = gap;
    }

    // Display automatically + Display delay controls
    {
        auto* autoRow = new QWidget(m_sceneOptionsContainer);
        configureRow(autoRow);
    auto* autoLayout = new QHBoxLayout(autoRow);
    configureRowLayout(autoLayout);
        m_displayAfterCheck = new QCheckBox("Display automatically", autoRow);
        m_displayAfterCheck->setStyleSheet(overlayTextStyle);
        m_displayAfterCheck->installEventFilter(this);
        autoLayout->addWidget(m_displayAfterCheck);
        autoLayout->addStretch();
        m_sceneOptionsLayout->addWidget(autoRow);

        auto* delayRow = new QWidget(m_sceneOptionsContainer);
        configureRow(delayRow);
    auto* h = new QHBoxLayout(delayRow);
    configureRowLayout(h);
        m_displayDelayCheck = new QCheckBox("Display delay: ", delayRow);
        m_displayDelayCheck->setStyleSheet(overlayTextStyle);
        m_displayDelayCheck->installEventFilter(this);
        QObject::connect(m_displayDelayCheck, &QCheckBox::toggled, this, [this](bool){ if (!m_updatingFromMedia) pushSettingsToMedia(); });
        m_displayAfterBox = makeValueBox();
        m_displayAfterSecondsLabel = new QLabel("s", delayRow);
        m_displayAfterSecondsLabel->setStyleSheet(overlayTextStyle);
        h->addWidget(m_displayDelayCheck);
        h->addWidget(m_displayAfterBox);
        h->addWidget(m_displayAfterSecondsLabel);
        h->addStretch();
        m_sceneOptionsLayout->addWidget(delayRow);
    }

    // Hide delay
    {
        m_hideDelayRow = new QWidget(m_sceneOptionsContainer);
        configureRow(m_hideDelayRow);
    auto* h = new QHBoxLayout(m_hideDelayRow);
    configureRowLayout(h);
        m_hideDelayCheck = new QCheckBox("Hide delay: ", m_hideDelayRow);
        m_hideDelayCheck->setStyleSheet(overlayTextStyle);
        m_hideDelayCheck->installEventFilter(this);
        QObject::connect(m_hideDelayCheck, &QCheckBox::toggled, this, &MediaSettingsPanel::onHideDelayToggled);
        m_hideDelayBox = makeValueBox();
        m_hideDelaySecondsLabel = new QLabel("s", m_hideDelayRow);
        m_hideDelaySecondsLabel->setStyleSheet(overlayTextStyle);
        h->addWidget(m_hideDelayCheck);
        h->addWidget(m_hideDelayBox);
        h->addWidget(m_hideDelaySecondsLabel);
        h->addStretch();
        m_sceneOptionsLayout->addWidget(m_hideDelayRow);
    }

    // Hide when video ends (video only)
    {
        m_hideWhenEndsRow = new QWidget(m_sceneOptionsContainer);
        configureRow(m_hideWhenEndsRow);
    auto* h = new QHBoxLayout(m_hideWhenEndsRow);
    configureRowLayout(h);
        m_hideWhenVideoEndsCheck = new QCheckBox("Hide when video ends", m_hideWhenEndsRow);
        m_hideWhenVideoEndsCheck->setStyleSheet(overlayTextStyle);
        m_hideWhenVideoEndsCheck->installEventFilter(this);
        QObject::connect(m_hideWhenVideoEndsCheck, &QCheckBox::toggled, this, [this](bool){ if (!m_updatingFromMedia) pushSettingsToMedia(); });
        h->addWidget(m_hideWhenVideoEndsCheck);
        h->addStretch();
        m_sceneOptionsLayout->addWidget(m_hideWhenEndsRow);
    }

    {
        QSpacerItem* gap = nullptr;
        auto [spacer, header] = addSceneSectionHeader("Audio", gap);
        m_sceneAudioSpacer = spacer;
        m_sceneAudioHeader = header;
        m_sceneAudioHeaderGap = gap;
    }

    // Unmute automatically (video only)
    {
        m_unmuteRow = new QWidget(m_sceneOptionsContainer);
        configureRow(m_unmuteRow);
    auto* h = new QHBoxLayout(m_unmuteRow);
    configureRowLayout(h);
        m_unmuteCheck = new QCheckBox("Unmute automatically", m_unmuteRow);
        m_unmuteCheck->setStyleSheet(overlayTextStyle);
        m_unmuteCheck->installEventFilter(this);
        h->addWidget(m_unmuteCheck);
        h->addStretch();
        m_sceneOptionsLayout->addWidget(m_unmuteRow);
    }

    // Unmute delay (video only)
    {
        m_unmuteDelayRow = new QWidget(m_sceneOptionsContainer);
        configureRow(m_unmuteDelayRow);
    auto* h = new QHBoxLayout(m_unmuteDelayRow);
    configureRowLayout(h);
        m_unmuteDelayCheck = new QCheckBox("Unmute delay: ", m_unmuteDelayRow);
        m_unmuteDelayCheck->setStyleSheet(overlayTextStyle);
        m_unmuteDelayCheck->installEventFilter(this);
        QObject::connect(m_unmuteDelayCheck, &QCheckBox::toggled, this, [this](bool){ if (!m_updatingFromMedia) pushSettingsToMedia(); });
        m_unmuteDelayBox = makeValueBox(QStringLiteral("0"));
        m_unmuteDelaySecondsLabel = new QLabel("s", m_unmuteDelayRow);
        m_unmuteDelaySecondsLabel->setStyleSheet(overlayTextStyle);
        h->addWidget(m_unmuteDelayCheck);
        h->addWidget(m_unmuteDelayBox);
        h->addWidget(m_unmuteDelaySecondsLabel);
        h->addStretch();
        m_sceneOptionsLayout->addWidget(m_unmuteDelayRow);
    }

    // Mute delay (video only)
    {
        m_muteDelayRow = new QWidget(m_sceneOptionsContainer);
        configureRow(m_muteDelayRow);
    auto* h = new QHBoxLayout(m_muteDelayRow);
    configureRowLayout(h);
        m_muteDelayCheck = new QCheckBox("Mute delay: ", m_muteDelayRow);
        m_muteDelayCheck->setStyleSheet(overlayTextStyle);
        m_muteDelayCheck->installEventFilter(this);
        QObject::connect(m_muteDelayCheck, &QCheckBox::toggled, this, &MediaSettingsPanel::onMuteDelayToggled);
        m_muteDelayBox = makeValueBox(QStringLiteral("1"));
        m_muteDelaySecondsLabel = new QLabel("s", m_muteDelayRow);
        m_muteDelaySecondsLabel->setStyleSheet(overlayTextStyle);
        h->addWidget(m_muteDelayCheck);
        h->addWidget(m_muteDelayBox);
        h->addWidget(m_muteDelaySecondsLabel);
        h->addStretch();
        m_sceneOptionsLayout->addWidget(m_muteDelayRow);
    }

    // Mute when video ends (video only)
    {
        m_muteWhenEndsRow = new QWidget(m_sceneOptionsContainer);
        configureRow(m_muteWhenEndsRow);
    auto* h = new QHBoxLayout(m_muteWhenEndsRow);
    configureRowLayout(h);
        m_muteWhenVideoEndsCheck = new QCheckBox("Mute when video ends", m_muteWhenEndsRow);
        m_muteWhenVideoEndsCheck->setStyleSheet(overlayTextStyle);
        m_muteWhenVideoEndsCheck->installEventFilter(this);
        QObject::connect(m_muteWhenVideoEndsCheck, &QCheckBox::toggled, this, [this](bool){ if (!m_updatingFromMedia) pushSettingsToMedia(); });
        h->addWidget(m_muteWhenVideoEndsCheck);
        h->addStretch();
        m_sceneOptionsLayout->addWidget(m_muteWhenEndsRow);
    }

    {
        QSpacerItem* gap = nullptr;
        auto [spacer, header] = addSceneSectionHeader("Video", gap);
        m_sceneVideoSpacer = spacer;
        m_sceneVideoHeader = header;
        m_sceneVideoHeaderGap = gap;
    }

    // Play automatically (video only)
    {
        m_autoPlayRow = new QWidget(m_sceneOptionsContainer);
        configureRow(m_autoPlayRow);
    auto* autoLayout = new QHBoxLayout(m_autoPlayRow);
    configureRowLayout(autoLayout);
        m_autoPlayCheck = new QCheckBox("Play automatically", m_autoPlayRow);
        m_autoPlayCheck->setStyleSheet(overlayTextStyle);
        m_autoPlayCheck->installEventFilter(this);
        autoLayout->addWidget(m_autoPlayCheck);
        autoLayout->addStretch();
        m_sceneOptionsLayout->addWidget(m_autoPlayRow);
    }
    
    // Play delay (video only)
    {
        m_playDelayRow = new QWidget(m_sceneOptionsContainer);
        configureRow(m_playDelayRow);
    auto* h = new QHBoxLayout(m_playDelayRow);
    configureRowLayout(h);
        m_playDelayCheck = new QCheckBox("Play delay: ", m_playDelayRow);
        m_playDelayCheck->setStyleSheet(overlayTextStyle);
        m_playDelayCheck->installEventFilter(this);
        QObject::connect(m_playDelayCheck, &QCheckBox::toggled, this, [this](bool){ if (!m_updatingFromMedia) pushSettingsToMedia(); });
        m_autoPlayBox = makeValueBox();
        m_autoPlaySecondsLabel = new QLabel("s", m_playDelayRow);
        m_autoPlaySecondsLabel->setStyleSheet(overlayTextStyle);
        h->addWidget(m_playDelayCheck);
        h->addWidget(m_autoPlayBox);
        h->addWidget(m_autoPlaySecondsLabel);
        h->addStretch();
        m_sceneOptionsLayout->addWidget(m_playDelayRow);
    }

    // Pause delay (video only)
    {
        m_pauseDelayRow = new QWidget(m_sceneOptionsContainer);
        configureRow(m_pauseDelayRow);
    auto* h = new QHBoxLayout(m_pauseDelayRow);
    configureRowLayout(h);
        m_pauseDelayCheck = new QCheckBox("Pause delay: ", m_pauseDelayRow);
        m_pauseDelayCheck->setStyleSheet(overlayTextStyle);
        m_pauseDelayCheck->installEventFilter(this);
        QObject::connect(m_pauseDelayCheck, &QCheckBox::toggled, this, &MediaSettingsPanel::onPauseDelayToggled);
        m_pauseDelayBox = makeValueBox();
        m_pauseDelaySecondsLabel = new QLabel("s", m_pauseDelayRow);
        m_pauseDelaySecondsLabel->setStyleSheet(overlayTextStyle);
        h->addWidget(m_pauseDelayCheck);
        h->addWidget(m_pauseDelayBox);
        h->addWidget(m_pauseDelaySecondsLabel);
        h->addStretch();
        m_sceneOptionsLayout->addWidget(m_pauseDelayRow);
    }

    // Repeat (video only)
    {
        m_repeatRow = new QWidget(m_sceneOptionsContainer);
        configureRow(m_repeatRow);
    auto* h = new QHBoxLayout(m_repeatRow);
    configureRowLayout(h);
        m_repeatCheck = new QCheckBox("Repeat ", m_repeatRow);
        m_repeatCheck->setStyleSheet(overlayTextStyle);
        m_repeatCheck->installEventFilter(this);
        QObject::connect(m_repeatCheck, &QCheckBox::toggled, this, [this](bool){ if (!m_updatingFromMedia) pushSettingsToMedia(); });
        m_repeatBox = makeValueBox();
        auto* suffix = new QLabel(" times", m_repeatRow);
        suffix->setStyleSheet(overlayTextStyle);
        h->addWidget(m_repeatCheck);
        h->addWidget(m_repeatBox);
        h->addWidget(suffix);
        h->addStretch();
        m_sceneOptionsLayout->addWidget(m_repeatRow);
    }

    // Create Element Properties container grouped by category
    m_elementPropertiesContainer = new QWidget(m_innerContent);
    m_elementPropertiesLayout = new QVBoxLayout(m_elementPropertiesContainer);
    m_elementPropertiesLayout->setContentsMargins(0, 0, 0, 0);
    m_elementPropertiesLayout->setSpacing(kOptionVerticalSpacing);
    m_elementPropertiesLayout->setAlignment(Qt::AlignTop);
    m_contentLayout->addWidget(m_elementPropertiesContainer);
    m_elementPropertiesContainer->setVisible(false);

    bool elementFirstSection = true;
    auto addElementSectionHeader = [&](const QString& text, QSpacerItem*& headerGap) -> std::pair<QSpacerItem*, QLabel*> {
        QSpacerItem* spacer = nullptr;
        if (!elementFirstSection) {
            spacer = new QSpacerItem(0, kHeaderFirstRowTopMargin, QSizePolicy::Minimum, QSizePolicy::Fixed);
            m_elementPropertiesLayout->addItem(spacer);
        }
        elementFirstSection = false;

        auto* header = new QLabel(text, m_elementPropertiesContainer);
        QFont font = header->font();
        AppColors::applyCanvasMediaSettingsSectionHeadersFont(font);
        header->setFont(font);
        header->setStyleSheet(QString("%1 %2").arg(overlayTextStyle, AppColors::canvasMediaSettingsSectionHeadersFontCss()));
        header->setContentsMargins(0, 0, 0, 0);

        m_elementPropertiesLayout->addWidget(header);
        headerGap = new QSpacerItem(0, kHeaderFirstRowTopMargin, QSizePolicy::Minimum, QSizePolicy::Fixed);
        m_elementPropertiesLayout->addItem(headerGap);
        return {spacer, header};
    };

    {
        QSpacerItem* gap = nullptr;
        auto headerInfo = addElementSectionHeader("Image", gap);
        m_elementImageHeader = headerInfo.second;
        m_elementImageHeaderGap = gap;
    }

    // Image fade in
    {
        auto* row = new QWidget(m_elementPropertiesContainer);
        configureRow(row);
    auto* h = new QHBoxLayout(row);
    configureRowLayout(h);
        m_fadeInCheck = new QCheckBox("Image fade in: ", row);
        m_fadeInCheck->setStyleSheet(overlayTextStyle);
        m_fadeInCheck->installEventFilter(this);
        QObject::connect(m_fadeInCheck, &QCheckBox::toggled, this, [this](bool){ if (!m_updatingFromMedia) pushSettingsToMedia(); });
        m_fadeInBox = makeValueBox();
        auto* suffix = new QLabel("s", row);
        suffix->setStyleSheet(overlayTextStyle);
        h->addWidget(m_fadeInCheck);
        h->addWidget(m_fadeInBox);
        h->addWidget(suffix);
        h->addStretch();
        m_elementPropertiesLayout->addWidget(row);
    }

    // Image fade out
    {
        auto* row = new QWidget(m_elementPropertiesContainer);
        configureRow(row);
    auto* h = new QHBoxLayout(row);
    configureRowLayout(h);
        m_fadeOutCheck = new QCheckBox("Image fade out: ", row);
        m_fadeOutCheck->setStyleSheet(overlayTextStyle);
        m_fadeOutCheck->installEventFilter(this);
        QObject::connect(m_fadeOutCheck, &QCheckBox::toggled, this, [this](bool){ if (!m_updatingFromMedia) pushSettingsToMedia(); });
        m_fadeOutBox = makeValueBox();
        auto* suffix = new QLabel("s", row);
        suffix->setStyleSheet(overlayTextStyle);
        h->addWidget(m_fadeOutCheck);
        h->addWidget(m_fadeOutBox);
        h->addWidget(suffix);
        h->addStretch();
        m_elementPropertiesLayout->addWidget(row);
    }

    // Opacity
    {
        auto* row = new QWidget(m_elementPropertiesContainer);
        configureRow(row);
    auto* h = new QHBoxLayout(row);
    configureRowLayout(h);
        m_opacityCheck = new QCheckBox("Opacity: ", row);
        m_opacityCheck->setStyleSheet(overlayTextStyle);
        m_opacityCheck->installEventFilter(this);
        m_opacityBox = makeValueBox(QStringLiteral("100")); // Default to 100%
        auto* suffix = new QLabel("%", row);
        suffix->setStyleSheet(overlayTextStyle);
        h->addWidget(m_opacityCheck);
        h->addWidget(m_opacityBox);
        h->addWidget(suffix);
        h->addStretch();
        QObject::connect(m_opacityCheck, &QCheckBox::toggled, this, &MediaSettingsPanel::onOpacityToggled);
        m_elementPropertiesLayout->addWidget(row);
    }

    {
        QSpacerItem* gap = nullptr;
        auto [spacer, header] = addElementSectionHeader("Audio", gap);
        m_elementAudioSpacer = spacer;
        m_elementAudioHeader = header;
        m_elementAudioHeaderGap = gap;
    }

    // Volume (video only)
    {
        m_volumeRow = new QWidget(m_elementPropertiesContainer);
        configureRow(m_volumeRow);
    auto* h = new QHBoxLayout(m_volumeRow);
    configureRowLayout(h);
        m_volumeCheck = new QCheckBox("Volume: ", m_volumeRow);
        m_volumeCheck->setStyleSheet(overlayTextStyle);
        m_volumeCheck->installEventFilter(this);
        m_volumeBox = makeValueBox(QStringLiteral("100"));
        auto* suffix = new QLabel("%", m_volumeRow);
        suffix->setStyleSheet(overlayTextStyle);
        h->addWidget(m_volumeCheck);
        h->addWidget(m_volumeBox);
        h->addWidget(suffix);
        h->addStretch();
        QObject::connect(m_volumeCheck, &QCheckBox::toggled, this, &MediaSettingsPanel::onVolumeToggled);
        m_elementPropertiesLayout->addWidget(m_volumeRow);
    }

    // Audio fade in (video only)
    {
        m_audioFadeInRow = new QWidget(m_elementPropertiesContainer);
        configureRow(m_audioFadeInRow);
    auto* h = new QHBoxLayout(m_audioFadeInRow);
    configureRowLayout(h);
        m_audioFadeInCheck = new QCheckBox("Audio fade in: ", m_audioFadeInRow);
        m_audioFadeInCheck->setStyleSheet(overlayTextStyle);
        m_audioFadeInCheck->installEventFilter(this);
        QObject::connect(m_audioFadeInCheck, &QCheckBox::toggled, this, [this](bool){ if (!m_updatingFromMedia) pushSettingsToMedia(); });
        m_audioFadeInBox = makeValueBox();
        m_audioFadeInSecondsLabel = new QLabel("s", m_audioFadeInRow);
        m_audioFadeInSecondsLabel->setStyleSheet(overlayTextStyle);
        h->addWidget(m_audioFadeInCheck);
        h->addWidget(m_audioFadeInBox);
        h->addWidget(m_audioFadeInSecondsLabel);
        h->addStretch();
        m_elementPropertiesLayout->addWidget(m_audioFadeInRow);
    }

    // Audio fade out (video only)
    {
        m_audioFadeOutRow = new QWidget(m_elementPropertiesContainer);
        configureRow(m_audioFadeOutRow);
    auto* h = new QHBoxLayout(m_audioFadeOutRow);
    configureRowLayout(h);
        m_audioFadeOutCheck = new QCheckBox("Audio fade out: ", m_audioFadeOutRow);
        m_audioFadeOutCheck->setStyleSheet(overlayTextStyle);
        m_audioFadeOutCheck->installEventFilter(this);
        QObject::connect(m_audioFadeOutCheck, &QCheckBox::toggled, this, [this](bool){ if (!m_updatingFromMedia) pushSettingsToMedia(); });
        m_audioFadeOutBox = makeValueBox();
        m_audioFadeOutSecondsLabel = new QLabel("s", m_audioFadeOutRow);
        m_audioFadeOutSecondsLabel->setStyleSheet(overlayTextStyle);
        h->addWidget(m_audioFadeOutCheck);
        h->addWidget(m_audioFadeOutBox);
        h->addWidget(m_audioFadeOutSecondsLabel);
        h->addStretch();
        m_elementPropertiesLayout->addWidget(m_audioFadeOutRow);
    }
    
    // Configure widget dimensions and event handling
    m_widget->setMouseTracking(true);
    m_widget->setFixedWidth(m_panelWidthPx);
    
    // Install event filter to catch clicks and wheel events
    m_widget->installEventFilter(this);
    if (m_innerContent) m_innerContent->installEventFilter(this);
    
    // Create floating overlay scrollbar synced with scroll area
    if (!m_overlayVScroll) {
        m_overlayVScroll = new QScrollBar(Qt::Vertical, m_widget);
        m_overlayVScroll->setObjectName("settingsOverlayVScroll");
        m_overlayVScroll->setAutoFillBackground(false);
        m_overlayVScroll->setAttribute(Qt::WA_TranslucentBackground, true);
        m_overlayVScroll->setCursor(Qt::ArrowCursor);
        // Hide scrollbar by default - only show on scroll interaction
        m_overlayVScroll->hide();
        m_overlayVScroll->setStyleSheet(
            "QScrollBar#settingsOverlayVScroll { background: transparent; border: none; width: 8px; margin: 0px; }"
            " QScrollBar#settingsOverlayVScroll::groove:vertical { background: transparent; border: none; margin: 0px; }"
            " QScrollBar#settingsOverlayVScroll::handle:vertical { background: rgba(255,255,255,0.35); min-height: 24px; border-radius: 4px; }"
            " QScrollBar#settingsOverlayVScroll::handle:vertical:hover { background: rgba(255,255,255,0.55); }"
            " QScrollBar#settingsOverlayVScroll::handle:vertical:pressed { background: rgba(255,255,255,0.7); }"
            " QScrollBar#settingsOverlayVScroll::add-line:vertical, QScrollBar#settingsOverlayVScroll::sub-line:vertical { height: 0px; width: 0px; background: transparent; border: none; }"
            " QScrollBar#settingsOverlayVScroll::add-page:vertical, QScrollBar#settingsOverlayVScroll::sub-page:vertical { background: transparent; }"
        );
        // Auto-hide timer
        if (!m_scrollbarHideTimer) {
            m_scrollbarHideTimer = new QTimer(this);
            m_scrollbarHideTimer->setSingleShot(true);
            m_scrollbarHideTimer->setInterval(500);
            QObject::connect(m_scrollbarHideTimer, &QTimer::timeout, this, [this]() {
                if (m_overlayVScroll) m_overlayVScroll->hide();
            });
        }
        // Sync with scroll area's vertical scrollbar
        QScrollBar* src = m_scrollArea->verticalScrollBar();
        QObject::connect(m_overlayVScroll, &QScrollBar::valueChanged, src, &QScrollBar::setValue);
        QObject::connect(src, &QScrollBar::rangeChanged, this, [this](int min, int max){
            if (m_overlayVScroll) {
                m_overlayVScroll->setRange(min, max);
                // Sync page step for proper thumb sizing
                m_overlayVScroll->setPageStep(m_scrollArea->verticalScrollBar()->pageStep());
            }
            updateScrollbarGeometry();
        });
        QObject::connect(src, &QScrollBar::valueChanged, this, [this](int v){ if (m_overlayVScroll) m_overlayVScroll->setValue(v); });
        auto showScrollbarAndRestartTimer = [this]() {
            if (m_overlayVScroll && m_scrollbarHideTimer) {
                m_overlayVScroll->show();
                m_scrollbarHideTimer->start();
            }
        };
        QObject::connect(m_overlayVScroll, &QScrollBar::valueChanged, this, showScrollbarAndRestartTimer);
        QObject::connect(src, &QScrollBar::valueChanged, this, showScrollbarAndRestartTimer);
        
        // Initialize current values immediately (including pageStep for proper thumb sizing)
        m_overlayVScroll->setRange(src->minimum(), src->maximum());
        m_overlayVScroll->setPageStep(src->pageStep());
        m_overlayVScroll->setValue(src->value());
    }

    // Connect display automatically checkbox to enable/disable display delay controls
    if (m_displayAfterCheck) {
        QObject::connect(m_displayAfterCheck, &QCheckBox::toggled, this, &MediaSettingsPanel::onDisplayAutomaticallyToggled);
        // Set initial state (display delay disabled by default since display automatically is unchecked)
        onDisplayAutomaticallyToggled(m_displayAfterCheck->isChecked());
    }

    if (m_unmuteCheck) {
        QObject::connect(m_unmuteCheck, &QCheckBox::toggled, this, &MediaSettingsPanel::onUnmuteAutomaticallyToggled);
        onUnmuteAutomaticallyToggled(m_unmuteCheck->isChecked());
    }
    
    // Connect play automatically checkbox to enable/disable play delay controls
    if (m_autoPlayCheck) {
        QObject::connect(m_autoPlayCheck, &QCheckBox::toggled, this, &MediaSettingsPanel::onPlayAutomaticallyToggled);
        // Set initial state (play delay disabled by default since play automatically is unchecked)
        onPlayAutomaticallyToggled(m_autoPlayCheck->isChecked());
    }

    if (m_hideDelayCheck) {
        onHideDelayToggled(m_hideDelayCheck->isChecked());
    }
    if (m_muteDelayCheck) {
        onMuteDelayToggled(m_muteDelayCheck->isChecked());
    }
    if (m_pauseDelayCheck) {
        onPauseDelayToggled(m_pauseDelayCheck->isChecked());
    }
    if (m_hideWhenEndsRow) {
        m_hideWhenEndsRow->setVisible(false); // default hidden until media type provided
    }
    if (m_muteDelayRow) {
        m_muteDelayRow->setVisible(false);
    }
    if (m_muteWhenEndsRow) {
        m_muteWhenEndsRow->setVisible(false);
    }
    updateSectionHeaderVisibility();
    updateActiveTabUi();
    updatePosition();
}

void MediaSettingsPanel::setVisible(bool visible) {
    if (!m_widget) return;
    if (visible) {
        // Finalize all geometry BEFORE making widget visible to prevent first-frame flicker
        
        // Force complete layout calculation while widget is hidden
        if (m_rootLayout) {
            m_rootLayout->invalidate();
            m_rootLayout->activate();
        }
        if (m_contentLayout) {
            m_contentLayout->invalidate();
            m_contentLayout->activate();
        }
        
        // Ensure Qt has computed final sizes for all widgets
        if (m_tabSwitcherContainer) {
            m_tabSwitcherContainer->ensurePolished();
        }
        if (m_tabSwitcherSeparator) {
            m_tabSwitcherSeparator->ensurePolished();
        }
        if (m_innerContent) {
            m_innerContent->ensurePolished();
        }
        if (m_scrollContainer) {
            m_scrollContainer->ensurePolished();
        }
        if (m_scrollArea) {
            m_scrollArea->ensurePolished();
        }
        m_widget->ensurePolished();
        
        // Compute final geometry
        updatePosition();
        
        // Force geometry updates to take effect
        if (m_widget) {
            m_widget->updateGeometry();
        }
        if (m_scrollContainer) {
            m_scrollContainer->updateGeometry();
        }
        if (m_scrollArea) {
            m_scrollArea->updateGeometry();
        }
        
        // Process pending layout events to ensure everything is finalized
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        
        // NOW make widget visible with geometry 100% finalized
        m_widget->setVisible(true);
        
        // Final position update for any post-visibility adjustments
        updatePosition();
        return;
    }

    m_widget->setVisible(false);
    clearActiveBox();
}

bool MediaSettingsPanel::isVisible() const {
    return m_widget && m_widget->isVisible();
}

void MediaSettingsPanel::updateSectionHeaderVisibility() {
    auto updateSection = [&](QLabel* header, QSpacerItem* spacer, std::initializer_list<QWidget*> rows) {
        if (!header) return;
        bool anyVisible = false;
        for (QWidget* row : rows) {
            if (row && !row->isHidden()) {
                anyVisible = true;
                break;
            }
        }
        header->setVisible(anyVisible);
        if (spacer) {
            spacer->changeSize(0, anyVisible ? kHeaderFirstRowTopMargin : 0, QSizePolicy::Minimum, QSizePolicy::Fixed);
        }
    };

    updateSection(m_sceneAudioHeader, m_sceneAudioSpacer, {m_unmuteRow, m_unmuteDelayRow, m_muteDelayRow, m_muteWhenEndsRow});
    updateSection(m_sceneVideoHeader, m_sceneVideoSpacer, {m_autoPlayRow, m_playDelayRow, m_pauseDelayRow, m_repeatRow});
    updateSection(m_elementAudioHeader, m_elementAudioSpacer, {m_volumeRow, m_audioFadeInRow, m_audioFadeOutRow});

    if (m_sceneImageHeader) {
        m_sceneImageHeader->setVisible(true);
    }
    if (m_elementImageHeader) {
        m_elementImageHeader->setVisible(true);
    }
}

void MediaSettingsPanel::setMediaType(bool isVideo) {
    // Show/hide video-only options (this changes layout significantly for video media)
    if (m_autoPlayRow) {
        m_autoPlayRow->setVisible(isVideo);
    }
    if (m_playDelayRow) {
        m_playDelayRow->setVisible(isVideo);
    }
    if (m_pauseDelayRow) {
        m_pauseDelayRow->setVisible(isVideo);
    }
    if (m_repeatRow) {
        m_repeatRow->setVisible(isVideo);
    }
    if (m_audioFadeInRow) {
        m_audioFadeInRow->setVisible(isVideo);
    }
    if (m_audioFadeOutRow) {
        m_audioFadeOutRow->setVisible(isVideo);
    }
    if (m_unmuteRow) {
        m_unmuteRow->setVisible(isVideo);
    }
    if (m_unmuteDelayRow) {
        m_unmuteDelayRow->setVisible(isVideo);
    }
    if (m_hideWhenEndsRow) {
        m_hideWhenEndsRow->setVisible(isVideo);
    }
    if (m_muteDelayRow) {
        m_muteDelayRow->setVisible(isVideo);
    }
    if (m_muteWhenEndsRow) {
        m_muteWhenEndsRow->setVisible(isVideo);
    }
    if (m_volumeRow) {
        m_volumeRow->setVisible(isVideo);
    }
    
    // Reset video-only checkboxes when switching to image
    if (!isVideo && m_hideWhenVideoEndsCheck) {
        const bool prev = m_hideWhenVideoEndsCheck->blockSignals(true);
        m_hideWhenVideoEndsCheck->setChecked(false);
        m_hideWhenVideoEndsCheck->blockSignals(prev);
    }
    if (!isVideo && m_muteDelayCheck) {
        const bool prev = m_muteDelayCheck->blockSignals(true);
        m_muteDelayCheck->setChecked(false);
        m_muteDelayCheck->blockSignals(prev);
    }
    if (!isVideo && m_muteWhenVideoEndsCheck) {
        const bool prev = m_muteWhenVideoEndsCheck->blockSignals(true);
        m_muteWhenVideoEndsCheck->setChecked(false);
        m_muteWhenVideoEndsCheck->blockSignals(prev);
    }
    if (!isVideo && m_unmuteCheck) {
        const bool prev = m_unmuteCheck->blockSignals(true);
        m_unmuteCheck->setChecked(false);
        m_unmuteCheck->blockSignals(prev);
        onUnmuteAutomaticallyToggled(false);
    }
    if (!isVideo && m_unmuteDelayCheck) {
        const bool prev = m_unmuteDelayCheck->blockSignals(true);
        m_unmuteDelayCheck->setChecked(false);
        m_unmuteDelayCheck->blockSignals(prev);
    }
    if (!isVideo && m_volumeCheck) {
        const bool prev = m_volumeCheck->blockSignals(true);
        m_volumeCheck->setChecked(false);
        m_volumeCheck->blockSignals(prev);
    }
    if (!isVideo && m_audioFadeInCheck) {
        const bool prev = m_audioFadeInCheck->blockSignals(true);
        m_audioFadeInCheck->setChecked(false);
        m_audioFadeInCheck->blockSignals(prev);
    }
    if (!isVideo && m_audioFadeOutCheck) {
        const bool prev = m_audioFadeOutCheck->blockSignals(true);
        m_audioFadeOutCheck->setChecked(false);
        m_audioFadeOutCheck->blockSignals(prev);
    }
    
    // Clear active box if it belongs to a hidden video-only option
    if (!isVideo && m_activeBox && (m_activeBox == m_autoPlayBox || m_activeBox == m_pauseDelayBox || m_activeBox == m_repeatBox || m_activeBox == m_volumeBox || m_activeBox == m_unmuteDelayBox || m_activeBox == m_audioFadeInBox || m_activeBox == m_audioFadeOutBox || m_activeBox == m_muteDelayBox)) {
        clearActiveBox();
    }
    
    // Force layout recalculation after visibility changes
    updateSectionHeaderVisibility();

    if (m_contentLayout) {
        m_contentLayout->invalidate();
        m_contentLayout->activate();
    }
    if (m_sceneOptionsLayout) {
        m_sceneOptionsLayout->invalidate();
        m_sceneOptionsLayout->activate();
    }
    if (m_elementPropertiesLayout) {
        m_elementPropertiesLayout->invalidate();
        m_elementPropertiesLayout->activate();
    }
    
    // Polish widgets with new layout
    if (m_innerContent) {
        m_innerContent->ensurePolished();
    }
    if (m_widget) {
        m_widget->ensurePolished();
    }
}

void MediaSettingsPanel::updatePosition() {
    if (!m_widget) return;
    
    QWidget* viewport = m_widget->parentWidget();
    if (!viewport) return;
    
    const int viewportHeight = viewport->height();
    const int rawAvailableHeight = viewportHeight - m_anchorTopMargin - m_anchorBottomMargin;
    const int availableHeight = std::max(0, rawAvailableHeight);

    if (rawAvailableHeight <= 0) {
        m_widget->setMaximumHeight(QWIDGETSIZE_MAX);
        m_widget->setMinimumHeight(0);
        if (m_scrollContainer) {
            m_scrollContainer->setMinimumHeight(0);
            m_scrollContainer->setMaximumHeight(QWIDGETSIZE_MAX);
            m_scrollContainer->updateGeometry();
        }
        if (m_scrollArea) {
            m_scrollArea->setMinimumHeight(0);
            m_scrollArea->setMaximumHeight(QWIDGETSIZE_MAX);
            m_scrollArea->updateGeometry();
        }
        updateScrollbarGeometry();
        return;
    }

    m_widget->setMaximumHeight(std::max(50, availableHeight));
    m_widget->setMinimumHeight(0);

    // Calculate chrome height (tab + separator)
    int chromeHeight = 0;
    if (m_tabSwitcherContainer) {
        chromeHeight += m_tabSwitcherContainer->height();
    }
    if (m_tabSwitcherSeparator) {
        chromeHeight += m_tabSwitcherSeparator->height();
    }

    int contentHeight = 0;
    if (m_innerContent) {
        contentHeight = m_innerContent->sizeHint().height();
    }
    if (m_scrollArea) {
        contentHeight += m_scrollArea->frameWidth() * 2;
    }
    if (m_rootLayout) {
        const QMargins margins = m_rootLayout->contentsMargins();
        contentHeight += margins.top() + margins.bottom();
    }

    int desiredHeight = chromeHeight + contentHeight;
    if (desiredHeight <= 0) {
        desiredHeight = m_widget->sizeHint().height();
    }

    int boundedHeight = std::min(availableHeight, desiredHeight);
    boundedHeight = std::max(1, boundedHeight);

    const QSize newSize(m_panelWidthPx, boundedHeight);
    if (m_widget->size() != newSize) {
        m_widget->resize(newSize);
    }

    const int viewportTarget = std::max(0, m_widget->height() - chromeHeight);
    if (m_scrollContainer) {
        m_scrollContainer->setMinimumHeight(viewportTarget);
        m_scrollContainer->setMaximumHeight(viewportTarget);
        m_scrollContainer->updateGeometry();
    }
    if (m_scrollArea) {
        m_scrollArea->setMinimumHeight(viewportTarget);
        m_scrollArea->setMaximumHeight(viewportTarget);
        m_scrollArea->updateGeometry();
    }

    m_widget->move(m_anchorLeftMargin, m_anchorTopMargin);

    updateScrollbarGeometry();
}

void MediaSettingsPanel::setAnchorMargins(int left, int top, int bottom) {
    m_anchorLeftMargin = std::max(0, left);
    m_anchorTopMargin = std::max(0, top);
    m_anchorBottomMargin = std::max(0, bottom);
    updatePosition();
}

void MediaSettingsPanel::setBoxActive(QLabel* box, bool active) {
    if (!box) return;

    if (active) {
        box->setStyleSheet(
            QString("QLabel {"
            "  background-color: %1;"
            "  border: 1px solid %1;"
            "  border-radius: 6px;"
            "  padding: 2px 10px;"
            "  margin-left: 4px;"
            "  margin-right: 0px;"
            "  color: white;"
            "}").arg(AppColors::gMediaPanelActiveBg.name())
        );
    } else {
        box->setStyleSheet(
            QString("QLabel {"
            "  background-color: %1;"
            "  border: 1px solid %2;"
            "  border-radius: 6px;"
            "  padding: 2px 10px;"
            "  margin-left: 4px;"
            "  margin-right: 0px;"
            "  color: white;"
            "}").arg(AppColors::gMediaPanelInactiveBg.name()).arg(AppColors::gMediaPanelInactiveBorder.name())
        );
    }
}

void MediaSettingsPanel::clearActiveBox() {
    if (!m_activeBox) return;

    QLabel* previous = m_activeBox;
    m_activeBox = nullptr;

    const bool wasOpacityBox = (previous == m_opacityBox);
    const bool wasVolumeBox = (previous == m_volumeBox);

    setBoxActive(previous, false);
    previous->clearFocus();

    m_clearOnFirstType = false;
    m_pendingDecimalInsertion = false;

    if (wasOpacityBox && m_opacityCheck && m_opacityCheck->isChecked()) {
        applyOpacityFromUi();
    }
    if (wasVolumeBox && m_volumeCheck && m_volumeCheck->isChecked()) {
        applyVolumeFromUi();
    }
}

bool MediaSettingsPanel::eventFilter(QObject* obj, QEvent* event) {
    if ((obj == m_scrollContainer || obj == m_widget) && event->type() == QEvent::Resize) {
        updateScrollbarGeometry();
    }

    const bool withinPanelHierarchy = m_widget && obj && obj->isWidgetType() &&
        (obj == m_widget || m_widget->isAncestorOf(static_cast<QWidget*>(obj)));

    // Handle clicks on value boxes FIRST (before general mouse blocking)
    if (event->type() == QEvent::MouseButtonPress) {
        QLabel* box = qobject_cast<QLabel*>(obj);
    if (box && (box == m_displayAfterBox || box == m_unmuteDelayBox || box == m_autoPlayBox || box == m_pauseDelayBox || box == m_repeatBox || 
        box == m_fadeInBox || box == m_fadeOutBox || box == m_audioFadeInBox || box == m_audioFadeOutBox || box == m_hideDelayBox || box == m_muteDelayBox || box == m_opacityBox || box == m_volumeBox)) {
            // Don't allow interaction with disabled boxes
            if (!box->isEnabled()) {
                return true; // consume the event but don't activate
            }
            
            // Clear previous active box
            clearActiveBox();
            // Set this box as active
            m_activeBox = box;
            setBoxActive(box, true);
            // Give focus to the box so it can receive key events
            box->setFocus();
            // Enable one-shot clear-on-type behavior
            m_clearOnFirstType = true;
            m_pendingDecimalInsertion = false;
            return true; // consume the event
        }
    }

    // Block all mouse interactions from reaching canvas when over settings panel
    if (event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonRelease ||
        event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonDblClick) {
        if (withinPanelHierarchy || obj == m_scrollArea || obj == m_scrollContainer) {
            if (event->type() == QEvent::MouseButtonPress) {
                clearActiveBox();
            }
            return false; // Allow widget to process; WA_NoMousePropagation stops propagation
        }
    }
    // Handle key presses when a box is active
    else if (event->type() == QEvent::KeyPress && m_activeBox) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        
        // Enter key deactivates
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            clearActiveBox();
            return true;
        }
        // Backspace clears the box and shows "..."
        else if (keyEvent->key() == Qt::Key_Backspace) {
            m_activeBox->setText("...");
            m_pendingDecimalInsertion = false;
            if (!m_updatingFromMedia) {
                pushSettingsToMedia();
            }
            return true;
        }
        // 'i' key sets infinity symbol (only for repeat box)
        else if (keyEvent->key() == Qt::Key_I) {
            // Infinity is only allowed for repeat box
            if (m_activeBox == m_repeatBox) {
                m_activeBox->setText("∞");
                m_pendingDecimalInsertion = false;
                if (!m_updatingFromMedia) {
                    pushSettingsToMedia();
                }
                return true;
            } else {
                // Block infinity for display delay, play delay, fade in, fade out, and opacity
                return true; // consume the event but don't apply
            }
        }
        // Handle input based on which box is active
        else {
            QString text = keyEvent->text();
            if (!text.isEmpty() && isValidInputForBox(m_activeBox, text[0])) {
                const QChar ch = text[0];

                // Handle decimal separator explicitly
                if (ch == '.') {
                    if (!boxSupportsDecimal(m_activeBox)) {
                        return true;
                    }
                    QString currentText = m_activeBox->text();
                    QString effectiveText = currentText;
                    const bool replaceAll = m_clearOnFirstType || effectiveText == "..." || effectiveText == "∞";
                    if (replaceAll) {
                        effectiveText.clear();
                        m_clearOnFirstType = false;
                    }
                    if (effectiveText.isEmpty()) {
                        return true; // dot cannot be first character
                    }
                    if (effectiveText.contains('.')) {
                        return true; // only one dot allowed
                    }
                    if (m_pendingDecimalInsertion) {
                        return true;
                    }
                    // Append decimal point immediately so the user sees it, but remember that a digit must follow
                    effectiveText.append('.');
                    m_activeBox->setText(effectiveText);
                    m_pendingDecimalInsertion = true;
                    if (!m_updatingFromMedia) {
                        // Avoid propagating a trailing decimal to the media; wait until a digit is entered
                        // If the user finishes editing without adding a digit, the later validation will reset the value
                    }
                    return true;
                }

                if (ch == '-' && (m_activeBox == m_hideDelayBox || m_activeBox == m_muteDelayBox)) {
                    QString currentText = m_activeBox->text();
                    QString baseText = (m_clearOnFirstType || currentText == "..." || currentText == "∞")
                        ? QString()
                        : currentText;

                    if (m_clearOnFirstType) {
                        m_clearOnFirstType = false;
                    }

                    if (baseText.startsWith('-')) {
                        return true;
                    }

                    if (m_pendingDecimalInsertion) {
                        m_pendingDecimalInsertion = false;
                    }

                    baseText.remove('-');
                    baseText.prepend('-');

                    if (baseText.isEmpty()) {
                        baseText = QStringLiteral("-");
                    }

                    m_activeBox->setText(baseText);
                    if (!m_updatingFromMedia) {
                        pushSettingsToMedia();
                    }
                    return true;
                }

                if (ch.isDigit()) {
                    QString currentText = m_activeBox->text();
                    const bool replaceAll = m_clearOnFirstType || currentText == "..." || currentText == "∞";
                    QString baseText = replaceAll ? QString() : currentText;
                    if (replaceAll) {
                        m_clearOnFirstType = false;
                    }

                    if (m_pendingDecimalInsertion) {
                        if (boxSupportsDecimal(m_activeBox) && !baseText.contains('.') && !baseText.isEmpty()) {
                            baseText.append('.');
                        }
                        m_pendingDecimalInsertion = false;
                    }

                    QString newText = baseText + ch;

                    int digitCount = 0;
                    for (QChar c : newText) {
                        if (c.isDigit()) {
                            ++digitCount;
                        }
                    }

                    if (m_activeBox == m_opacityBox || m_activeBox == m_volumeBox) {
                        bool ok = false;
                        int val = newText.toInt(&ok);
                        if (ok && val > 100) {
                            newText = QStringLiteral("100");
                        }
                        m_activeBox->setText(newText);
                    } else if (m_activeBox == m_repeatBox) {
                        if (digitCount > 5) {
                            m_activeBox->setText(QStringLiteral("∞"));
                        } else {
                            m_activeBox->setText(newText);
                        }
                    } else if (boxSupportsDecimal(m_activeBox)) {
                        if (digitCount > 5) {
                            return true;
                        }
                        m_activeBox->setText(newText);
                    } else {
                        m_activeBox->setText(newText);
                    }

                    if (!m_updatingFromMedia) {
                        pushSettingsToMedia();
                    }
                    return true;
                }
            }
        }
    }
    // Consume wheel events over the settings panel so canvas doesn't zoom/scroll
    else if (event->type() == QEvent::Wheel) {
        // Only handle if event is within our widget tree
        if (withinPanelHierarchy || obj == m_scrollContainer || obj == m_scrollArea) {
            // Route to scroll area
            if (m_scrollArea) {
                QCoreApplication::sendEvent(m_scrollArea->viewport(), event);
                return true;
            }
        }
    }
    
    return QObject::eventFilter(obj, event);
}

bool MediaSettingsPanel::boxSupportsDecimal(QLabel* box) const {
    return box == m_displayAfterBox || box == m_unmuteDelayBox || box == m_autoPlayBox || box == m_fadeInBox ||
           box == m_fadeOutBox || box == m_audioFadeInBox || box == m_audioFadeOutBox || box == m_hideDelayBox || box == m_pauseDelayBox ||
           box == m_muteDelayBox;
}

bool MediaSettingsPanel::isValidInputForBox(QLabel* box, QChar character) {
    if (!box) return false;

    if (box == m_repeatBox) {
        // Repeat: numbers only (excluding 0)
        return character.isDigit() && character != '0';
    }

    if (box == m_opacityBox || box == m_volumeBox) {
        return character.isDigit();
    }

    if (boxSupportsDecimal(box)) {
        if (character == '.') {
            return true;
        }
        if ((box == m_hideDelayBox || box == m_muteDelayBox) && character == '-') {
            const QString current = box->text();
            return !current.contains('-');
        }
        return character.isDigit();
    }

    return character.isDigit();
}

void MediaSettingsPanel::onOpacityToggled(bool checked) {
    Q_UNUSED(checked);
    applyOpacityFromUi();
    if (!m_updatingFromMedia) {
        pushSettingsToMedia();
    }
}

void MediaSettingsPanel::applyOpacityFromUi() {
    if (!m_mediaItem) return;

    if (m_updatingFromMedia) {
        const auto state = m_mediaItem->mediaSettingsState();
        if (state.opacityOverrideEnabled) {
            bool ok = false;
            int val = state.opacityText.trimmed().toInt(&ok);
            if (!ok) val = 100;
            val = std::clamp(val, 0, 100);
            m_mediaItem->setContentOpacity(static_cast<qreal>(val) / 100.0);
        } else {
            m_mediaItem->setContentOpacity(1.0);
        }
        return;
    }

    if (!m_opacityCheck || !m_opacityBox) return;
    pushSettingsToMedia();
}

void MediaSettingsPanel::onVolumeToggled(bool checked) {
    Q_UNUSED(checked);
    applyVolumeFromUi();
    if (!m_updatingFromMedia) {
        pushSettingsToMedia();
    }
}

void MediaSettingsPanel::applyVolumeFromUi() {
    if (!m_mediaItem) return;
    auto* videoItem = dynamic_cast<ResizableVideoItem*>(m_mediaItem);
    if (!videoItem) return;

    if (m_updatingFromMedia) {
        videoItem->applyVolumeOverrideFromState();
        return;
    }

    if (!m_volumeCheck || !m_volumeBox) return;
    pushSettingsToMedia();
}

double MediaSettingsPanel::fadeInSeconds() const {
    if (!m_fadeInCheck || !m_fadeInBox) return 0.0;
    if (!m_fadeInCheck->isChecked()) return 0.0;
    QString t = m_fadeInBox->text().trimmed();
    if (t == "∞" || t.isEmpty() || t == "...") return 0.0; // treat infinity / empty as instant for now
    // Replace comma with dot for parsing
    t.replace(',', '.');
    bool ok=false; double v = t.toDouble(&ok); if (!ok) return 0.0; return std::clamp(v, 0.0, 3600.0); // clamp to 1 hour max
}

bool MediaSettingsPanel::displayAutomaticallyEnabled() const {
    return m_displayAfterCheck && m_displayAfterCheck->isChecked();
}

int MediaSettingsPanel::displayDelayMillis() const {
    if (!m_displayDelayCheck || !m_displayAfterCheck) return 0;
    if (!m_displayDelayCheck->isChecked()) return 0;
    if (!m_displayAfterBox) return 0;
    QString text = m_displayAfterBox->text().trimmed();
    if (text.isEmpty() || text == QStringLiteral("...")) return 0;
    text.replace(',', '.');
    bool ok = false;
    const double seconds = text.toDouble(&ok);
    if (!ok || seconds < 0.0) return 0;
    return static_cast<int>(std::lround(seconds * 1000.0));
}

bool MediaSettingsPanel::playAutomaticallyEnabled() const {
    return m_autoPlayCheck && m_autoPlayCheck->isChecked();
}

int MediaSettingsPanel::playDelayMillis() const {
    if (!m_playDelayCheck || !m_autoPlayCheck) return 0;
    if (!m_playDelayCheck->isChecked()) return 0;
    if (!m_autoPlayBox) return 0;
    QString text = m_autoPlayBox->text().trimmed();
    if (text.isEmpty() || text == QStringLiteral("...")) return 0;
    text.replace(',', '.');
    bool ok = false;
    const double seconds = text.toDouble(&ok);
    if (!ok || seconds < 0.0) return 0;
    return static_cast<int>(std::lround(seconds * 1000.0));
}

double MediaSettingsPanel::fadeOutSeconds() const {
    if (!m_fadeOutCheck || !m_fadeOutBox) return 0.0;
    if (!m_fadeOutCheck->isChecked()) return 0.0;
    QString t = m_fadeOutBox->text().trimmed();
    if (t == "∞" || t.isEmpty() || t == "...") return 0.0;
    t.replace(',', '.');
    bool ok=false; double v = t.toDouble(&ok); if (!ok) return 0.0; return std::clamp(v, 0.0, 3600.0);
}

void MediaSettingsPanel::updateScrollbarGeometry() {
    if (!m_overlayVScroll || !m_widget || !m_scrollArea || !m_scrollContainer) return;

    // Hide scrollbar entirely when collapsed or no scrollable content is visible
    if (!m_scrollContainer->isVisible()) {
        m_overlayVScroll->hide();
        return;
    }

    QScrollBar* sourceScrollBar = m_scrollArea->verticalScrollBar();
    if (!sourceScrollBar) {
        m_overlayVScroll->hide();
        return;
    }

    const bool needed = sourceScrollBar->maximum() > sourceScrollBar->minimum();
    if (!needed) {
        m_overlayVScroll->hide();
        return;
    }

    // Align overlay scrollbar with the scroll container so it doesn't overlap the header button
    const QRect scrollRect = m_scrollContainer->geometry();
    const int margin = 6; // inset from right edge for visual parity with other overlays
    const int topMargin = 6;
    const int bottomMargin = 6;
    const int width = 8;
    const int height = qMax(0, scrollRect.height() - topMargin - bottomMargin);
    if (height <= 0) {
        m_overlayVScroll->hide();
        return;
    }
    const int x = scrollRect.x() + scrollRect.width() - width - margin;
    const int y = scrollRect.y() + topMargin;

    m_overlayVScroll->setRange(sourceScrollBar->minimum(), sourceScrollBar->maximum());
    m_overlayVScroll->setPageStep(sourceScrollBar->pageStep());
    m_overlayVScroll->setValue(sourceScrollBar->value());
    m_overlayVScroll->setGeometry(x, y, width, height);

    // Respect auto-hide timing; only force visibility if timer is already keeping it shown
    if (!m_scrollbarHideTimer || m_scrollbarHideTimer->isActive()) {
        m_overlayVScroll->show();
    }
}

void MediaSettingsPanel::onDisplayAutomaticallyToggled(bool checked) {
    // Enable/disable display delay checkbox and input box based on display automatically state
    const QString activeTextStyle = QStringLiteral("color: %1;")
        .arg(AppColors::colorToCss(AppColors::gOverlayTextColor));
    const QString disabledTextStyle = QStringLiteral("color: #808080;");
    
    if (m_displayDelayCheck) {
        m_displayDelayCheck->setEnabled(checked);
        
        // Update visual styling for disabled state
        if (checked) {
            m_displayDelayCheck->setStyleSheet(activeTextStyle);
        } else {
            m_displayDelayCheck->setStyleSheet(disabledTextStyle); // Gray color for disabled
            // Also uncheck the display delay checkbox when disabled
            m_displayDelayCheck->setChecked(false);
        }
    }
    
    if (m_displayAfterBox) {
        m_displayAfterBox->setEnabled(checked);
        
        // Update visual styling for the input box
        if (checked) {
            // Reset to normal styling when enabled
            setBoxActive(m_displayAfterBox, m_activeBox == m_displayAfterBox);
        } else {
            // Apply disabled styling
            m_displayAfterBox->setStyleSheet(
                "QLabel {"
                "  background-color: #404040;"
                "  border: 1px solid #606060;"
                "  border-radius: 6px;"
                "  padding: 2px 10px;"
                "  margin-left: 4px;"
                "  margin-right: 0px;"
                "  color: #808080;"
                "}"
            );
            
            // Clear active state if this box was active
            if (m_activeBox == m_displayAfterBox) {
                clearActiveBox();
            }
        }
    }
    
    // Also update the "seconds" label styling
    if (m_displayAfterSecondsLabel) {
        if (checked) {
            m_displayAfterSecondsLabel->setStyleSheet(activeTextStyle);
        } else {
            m_displayAfterSecondsLabel->setStyleSheet(disabledTextStyle); // Gray color for disabled
        }
    }

    if (!m_updatingFromMedia) {
        pushSettingsToMedia();
    }
}

void MediaSettingsPanel::onUnmuteAutomaticallyToggled(bool checked) {
    // Enable/disable unmute delay checkbox and input box based on unmute automatically state
    const QString activeTextStyle = QStringLiteral("color: %1;")
        .arg(AppColors::colorToCss(AppColors::gOverlayTextColor));
    const QString disabledTextStyle = QStringLiteral("color: #808080;");

    if (m_unmuteDelayCheck) {
        m_unmuteDelayCheck->setEnabled(checked);
        
        // Update visual styling for disabled state
        if (checked) {
            m_unmuteDelayCheck->setStyleSheet(activeTextStyle);
        } else {
            m_unmuteDelayCheck->setStyleSheet(disabledTextStyle); // Gray color for disabled
            // Also uncheck the unmute delay checkbox when disabled
            m_unmuteDelayCheck->setChecked(false);
        }
    }
    
    if (m_unmuteDelayBox) {
        m_unmuteDelayBox->setEnabled(checked);
        
        // Update visual styling for the input box
        if (checked) {
            // Reset to normal styling when enabled
            setBoxActive(m_unmuteDelayBox, m_activeBox == m_unmuteDelayBox);
        } else {
            // Apply disabled styling
            m_unmuteDelayBox->setStyleSheet(
                "QLabel {"
                "  background-color: #404040;"
                "  border: 1px solid #606060;"
                "  border-radius: 6px;"
                "  padding: 2px 10px;"
                "  margin-left: 4px;"
                "  margin-right: 0px;"
                "  color: #808080;"
                "}"
            );
            
            // Clear active state if this box was active
            if (m_activeBox == m_unmuteDelayBox) {
                clearActiveBox();
            }
        }
    }
    
    // Also update the "seconds" label styling
    if (m_unmuteDelaySecondsLabel) {
        if (checked) {
            m_unmuteDelaySecondsLabel->setStyleSheet(activeTextStyle);
        } else {
            m_unmuteDelaySecondsLabel->setStyleSheet(disabledTextStyle); // Gray color for disabled
        }
    }

    if (!m_updatingFromMedia) {
        pushSettingsToMedia();
    }
}

void MediaSettingsPanel::onPlayAutomaticallyToggled(bool checked) {
    // Enable/disable play delay checkbox and input box based on play automatically state
    const QString activeTextStyle = QStringLiteral("color: %1;")
        .arg(AppColors::colorToCss(AppColors::gOverlayTextColor));
    const QString disabledTextStyle = QStringLiteral("color: #808080;");

    if (m_playDelayCheck) {
        m_playDelayCheck->setEnabled(checked);
        
        // Update visual styling for disabled state
        if (checked) {
            m_playDelayCheck->setStyleSheet(activeTextStyle);
        } else {
            m_playDelayCheck->setStyleSheet(disabledTextStyle); // Gray color for disabled
            // Also uncheck the play delay checkbox when disabled
            m_playDelayCheck->setChecked(false);
        }
    }
    
    if (m_autoPlayBox) {
        m_autoPlayBox->setEnabled(checked);
        
        // Update visual styling for the input box
        if (checked) {
            // Reset to normal styling when enabled
            setBoxActive(m_autoPlayBox, m_activeBox == m_autoPlayBox);
        } else {
            // Apply disabled styling
            m_autoPlayBox->setStyleSheet(
                "QLabel {"
                "  background-color: #404040;"
                "  border: 1px solid #606060;"
                "  border-radius: 6px;"
                "  padding: 2px 10px;"
                "  margin-left: 4px;"
                "  margin-right: 0px;"
                "  color: #808080;"
                "}"
            );
            
            // Clear active state if this box was active
            if (m_activeBox == m_autoPlayBox) {
                clearActiveBox();
            }
        }
    }
    
    // Also update the "seconds" label styling
    if (m_autoPlaySecondsLabel) {
        if (checked) {
            m_autoPlaySecondsLabel->setStyleSheet(activeTextStyle);
        } else {
            m_autoPlaySecondsLabel->setStyleSheet(disabledTextStyle); // Gray color for disabled
        }
    }

    if (!m_updatingFromMedia) {
        pushSettingsToMedia();
    }
}

void MediaSettingsPanel::onHideDelayToggled(bool checked) {
    const QString textStyle = QStringLiteral("color: %1;")
        .arg(AppColors::colorToCss(AppColors::gOverlayTextColor));

    if (m_hideDelayCheck) {
        m_hideDelayCheck->setStyleSheet(textStyle);
    }

    if (m_hideDelayBox) {
        if (!checked && m_activeBox == m_hideDelayBox) {
            clearActiveBox();
        }
        setBoxActive(m_hideDelayBox, m_activeBox == m_hideDelayBox);
    }

    if (m_hideDelaySecondsLabel) {
        m_hideDelaySecondsLabel->setStyleSheet(textStyle);
    }

    if (!m_updatingFromMedia) {
        pushSettingsToMedia();
    }
}

void MediaSettingsPanel::onMuteDelayToggled(bool checked) {
    const QString textStyle = QStringLiteral("color: %1;")
        .arg(AppColors::colorToCss(AppColors::gOverlayTextColor));

    if (m_muteDelayCheck) {
        m_muteDelayCheck->setStyleSheet(textStyle);
    }

    if (m_muteDelayBox) {
        if (!checked && m_activeBox == m_muteDelayBox) {
            clearActiveBox();
        }
        setBoxActive(m_muteDelayBox, m_activeBox == m_muteDelayBox);
    }

    if (m_muteDelaySecondsLabel) {
        m_muteDelaySecondsLabel->setStyleSheet(textStyle);
    }

    if (!m_updatingFromMedia) {
        pushSettingsToMedia();
    }
}

void MediaSettingsPanel::onPauseDelayToggled(bool checked) {
    const QString textStyle = QStringLiteral("color: %1;")
        .arg(AppColors::colorToCss(AppColors::gOverlayTextColor));

    if (m_pauseDelayCheck) {
        m_pauseDelayCheck->setStyleSheet(textStyle);
    }

    if (m_pauseDelayBox) {
        if (!checked && m_activeBox == m_pauseDelayBox) {
            clearActiveBox();
        }
        setBoxActive(m_pauseDelayBox, m_activeBox == m_pauseDelayBox);
    }

    if (m_pauseDelaySecondsLabel) {
        m_pauseDelaySecondsLabel->setStyleSheet(textStyle);
    }

    if (!m_updatingFromMedia) {
        pushSettingsToMedia();
    }
}

void MediaSettingsPanel::setMediaItem(ResizableMediaBase* item) {
    clearActiveBox();
    if (m_mediaItem == item) {
        if (m_mediaItem) {
            pullSettingsFromMedia();
        }
        return;
    }
    m_mediaItem = item;
    pullSettingsFromMedia();
}

void MediaSettingsPanel::pullSettingsFromMedia() {
    m_updatingFromMedia = true;
    if (!m_mediaItem) {
        m_updatingFromMedia = false;
        return;
    }

    const auto state = m_mediaItem->mediaSettingsState();

    auto applyCheckState = [](QCheckBox* box, bool checked) {
        if (!box) return;
        const bool prev = box->blockSignals(true);
        box->setChecked(checked);
        box->blockSignals(prev);
    };

    auto applyBoxText = [&](QLabel* label, const QString& text, const QString& fallback, bool normalizeDecimal = false) {
        if (!label) return;
        QString value = text.isEmpty() ? fallback : text;
        if (normalizeDecimal) {
            bool negative = value.startsWith('-');
            if (negative) {
                value.remove(0, 1);
            }
            value.replace(',', '.');
            if (value.startsWith('.')) {
                value.remove(0, 1);
            }
            if (value.endsWith('.')) {
                value.chop(1);
            }
            if (value.count('.') > 1) {
                // retain only the first decimal point
                int first = value.indexOf('.');
                QString digitsOnly;
                digitsOnly.reserve(value.size());
                for (int i = 0; i < value.size(); ++i) {
                    if (value[i].isDigit()) {
                        digitsOnly.append(value[i]);
                    } else if (value[i] == '.' && i == first) {
                        digitsOnly.append('.');
                    }
                }
                value = digitsOnly;
            }
            if (value.isEmpty()) {
                value = fallback;
            }
            if (negative && value != QStringLiteral("...")) {
                value.prepend('-');
            }
        }
        label->setText(value);
    };

    applyCheckState(m_displayAfterCheck, state.displayAutomatically);
    applyCheckState(m_displayDelayCheck, state.displayDelayEnabled);
    applyCheckState(m_autoPlayCheck, state.playAutomatically);
    applyCheckState(m_playDelayCheck, state.playDelayEnabled);
    applyCheckState(m_pauseDelayCheck, state.pauseDelayEnabled);
    applyCheckState(m_repeatCheck, state.repeatEnabled);
    applyCheckState(m_fadeInCheck, state.fadeInEnabled);
    applyCheckState(m_fadeOutCheck, state.fadeOutEnabled);
    applyCheckState(m_audioFadeInCheck, state.audioFadeInEnabled);
    applyCheckState(m_audioFadeOutCheck, state.audioFadeOutEnabled);
    applyCheckState(m_opacityCheck, state.opacityOverrideEnabled);
    applyCheckState(m_volumeCheck, state.volumeOverrideEnabled);
    applyCheckState(m_unmuteCheck, state.unmuteAutomatically);
    applyCheckState(m_unmuteDelayCheck, state.unmuteDelayEnabled);
    applyCheckState(m_hideDelayCheck, state.hideDelayEnabled);
    applyCheckState(m_hideWhenVideoEndsCheck, state.hideWhenVideoEnds);
    applyCheckState(m_muteDelayCheck, state.muteDelayEnabled);
    applyCheckState(m_muteWhenVideoEndsCheck, state.muteWhenVideoEnds);

    applyBoxText(m_displayAfterBox, state.displayDelayText, QStringLiteral("1"), true);
    applyBoxText(m_autoPlayBox, state.playDelayText, QStringLiteral("1"), true);
    applyBoxText(m_pauseDelayBox, state.pauseDelayText, QStringLiteral("1"), true);
    applyBoxText(m_repeatBox, state.repeatCountText, QStringLiteral("1"));
    applyBoxText(m_fadeInBox, state.fadeInText, QStringLiteral("1"), true);
    applyBoxText(m_fadeOutBox, state.fadeOutText, QStringLiteral("1"), true);
    applyBoxText(m_audioFadeInBox, state.audioFadeInText, QStringLiteral("1"), true);
    applyBoxText(m_audioFadeOutBox, state.audioFadeOutText, QStringLiteral("1"), true);
    applyBoxText(m_hideDelayBox, state.hideDelayText, QStringLiteral("1"), true);
    applyBoxText(m_muteDelayBox, state.muteDelayText, QStringLiteral("1"), true);
    applyBoxText(m_opacityBox, state.opacityText, QStringLiteral("100"));
    applyBoxText(m_volumeBox, state.volumeText, QStringLiteral("100"));
    applyBoxText(m_unmuteDelayBox, state.unmuteDelayText, QStringLiteral("1"), true);

    // Re-run UI interlock logic without persisting back to the media item
    onDisplayAutomaticallyToggled(m_displayAfterCheck ? m_displayAfterCheck->isChecked() : false);
    onPlayAutomaticallyToggled(m_autoPlayCheck ? m_autoPlayCheck->isChecked() : false);
    onUnmuteAutomaticallyToggled(m_unmuteCheck ? m_unmuteCheck->isChecked() : false);
    onOpacityToggled(m_opacityCheck ? m_opacityCheck->isChecked() : false);
    onVolumeToggled(m_volumeCheck ? m_volumeCheck->isChecked() : false);
    onHideDelayToggled(m_hideDelayCheck ? m_hideDelayCheck->isChecked() : false);
    onMuteDelayToggled(m_muteDelayCheck ? m_muteDelayCheck->isChecked() : false);
    onPauseDelayToggled(m_pauseDelayCheck ? m_pauseDelayCheck->isChecked() : false);

    m_updatingFromMedia = false;

    // Ensure opacity is immediately applied (uses stored state when guard is false)
    applyOpacityFromUi();
    applyVolumeFromUi();

    if (m_widget && m_contentLayout) {
        m_contentLayout->invalidate();
        m_contentLayout->activate();
    }
    if (m_widget) {
        m_widget->ensurePolished();
        m_widget->updateGeometry();
        m_widget->adjustSize();
    }
    updatePosition();
}

void MediaSettingsPanel::refreshVolumeDisplay() {
    if (!m_mediaItem || !m_volumeBox || !m_volumeCheck) {
        return;
    }

    const auto state = m_mediaItem->mediaSettingsState();
    const QString stored = state.volumeText.isEmpty() ? QStringLiteral("100") : state.volumeText;
    const QString displayText = state.volumeOverrideEnabled ? stored : QStringLiteral("100");

    QSignalBlocker checkBlocker(m_volumeCheck);
    QSignalBlocker boxBlocker(m_volumeBox);

    const bool previousGuard = m_updatingFromMedia;
    m_updatingFromMedia = true;

    m_volumeCheck->setChecked(state.volumeOverrideEnabled);
    m_volumeBox->setText(displayText);
    const bool volumeBoxManuallyActive = (m_activeBox == m_volumeBox); // Keep highlight only for explicit user focus
    setBoxActive(m_volumeBox, volumeBoxManuallyActive);

    applyVolumeFromUi();

    m_updatingFromMedia = previousGuard;
}

void MediaSettingsPanel::pushSettingsToMedia() {
    if (m_updatingFromMedia) return;
    if (!m_mediaItem) return;

    auto trimmedText = [](QLabel* label, const QString& fallback) {
        if (!label) return fallback;
        const QString text = label->text().trimmed();
        return text.isEmpty() ? fallback : text;
    };

    auto trimmedDecimalText = [&](QLabel* label, const QString& fallback) {
        QString fallbackValue = fallback;
        fallbackValue.replace(',', '.');
        QString value = trimmedText(label, fallbackValue);
        value.replace(',', '.');
        bool negative = value.startsWith('-');
        if (negative) {
            value.remove(0, 1);
        }
        if (value.startsWith('.')) {
            value.remove(0, 1);
        }
        if (value.endsWith('.')) {
            value.chop(1);
        }
        if (value.count('.') > 1) {
            int first = value.indexOf('.');
            QString digitsOnly;
            digitsOnly.reserve(value.size());
            for (int i = 0; i < value.size(); ++i) {
                if (value[i].isDigit()) {
                    digitsOnly.append(value[i]);
                } else if (value[i] == '.' && i == first) {
                    digitsOnly.append('.');
                }
            }
            value = digitsOnly;
        }
        if (value.isEmpty()) {
            value = fallbackValue;
        }
        if (negative && value != QStringLiteral("...")) {
            value.prepend('-');
        }
        return value;
    };

    auto trimmedPercentText = [&](QLabel* label, const QString& fallback) {
        QString value = trimmedText(label, fallback);
        bool ok = false;
        int percent = value.toInt(&ok);
        if (!ok) {
            percent = fallback.toInt(&ok) ? fallback.toInt(&ok) : 100;
        }
        percent = std::clamp(percent, 0, 100);
        return QString::number(percent);
    };

    ResizableMediaBase::MediaSettingsState state = m_mediaItem->mediaSettingsState();
    state.displayAutomatically = m_displayAfterCheck && m_displayAfterCheck->isChecked();
    state.displayDelayEnabled = m_displayDelayCheck && m_displayDelayCheck->isChecked();
    state.displayDelayText = trimmedDecimalText(m_displayAfterBox, state.displayDelayText);
    state.playAutomatically = m_autoPlayCheck && m_autoPlayCheck->isChecked();
    state.playDelayEnabled = m_playDelayCheck && m_playDelayCheck->isChecked();
    state.playDelayText = trimmedDecimalText(m_autoPlayBox, state.playDelayText);
    state.pauseDelayEnabled = m_pauseDelayCheck && m_pauseDelayCheck->isChecked();
    state.pauseDelayText = trimmedDecimalText(m_pauseDelayBox, state.pauseDelayText);
    state.repeatEnabled = m_repeatCheck && m_repeatCheck->isChecked();
    state.repeatCountText = trimmedText(m_repeatBox, state.repeatCountText);
    state.fadeInEnabled = m_fadeInCheck && m_fadeInCheck->isChecked();
    state.fadeInText = trimmedDecimalText(m_fadeInBox, state.fadeInText);
    state.fadeOutEnabled = m_fadeOutCheck && m_fadeOutCheck->isChecked();
    state.fadeOutText = trimmedDecimalText(m_fadeOutBox, state.fadeOutText);
    state.audioFadeInEnabled = m_audioFadeInCheck && m_audioFadeInCheck->isChecked();
    state.audioFadeInText = trimmedDecimalText(m_audioFadeInBox, state.audioFadeInText);
    state.audioFadeOutEnabled = m_audioFadeOutCheck && m_audioFadeOutCheck->isChecked();
    state.audioFadeOutText = trimmedDecimalText(m_audioFadeOutBox, state.audioFadeOutText);
    state.opacityOverrideEnabled = m_opacityCheck && m_opacityCheck->isChecked();
    state.opacityText = trimmedText(m_opacityBox, state.opacityText);
    const QString volumeFallback = state.volumeText.isEmpty() ? QStringLiteral("100") : state.volumeText;
    state.volumeOverrideEnabled = m_volumeCheck && m_volumeCheck->isChecked();
    if (state.volumeOverrideEnabled) {
        state.volumeText = trimmedPercentText(m_volumeBox, volumeFallback);
    } else {
        state.volumeText = QStringLiteral("100");
    }
    state.unmuteAutomatically = m_unmuteCheck && m_unmuteCheck->isChecked();
    state.unmuteDelayEnabled = m_unmuteDelayCheck && m_unmuteDelayCheck->isChecked();
    state.unmuteDelayText = trimmedDecimalText(m_unmuteDelayBox, state.unmuteDelayText);
    state.hideDelayEnabled = m_hideDelayCheck && m_hideDelayCheck->isChecked();
    state.hideDelayText = trimmedDecimalText(m_hideDelayBox, state.hideDelayText);
    state.hideWhenVideoEnds = m_hideWhenVideoEndsCheck && m_hideWhenVideoEndsCheck->isChecked();
    state.muteDelayEnabled = m_muteDelayCheck && m_muteDelayCheck->isChecked();
    state.muteDelayText = trimmedDecimalText(m_muteDelayBox, state.muteDelayText);
    state.muteWhenVideoEnds = m_muteWhenVideoEndsCheck && m_muteWhenVideoEndsCheck->isChecked();

    m_mediaItem->setMediaSettingsState(state);
}

void MediaSettingsPanel::updateActiveTabUi() {
    const QString overlayTextCss = AppColors::colorToCss(AppColors::gOverlayTextColor);
    const bool sceneActive = (m_activeTab == ActiveTab::Scene);

    if (m_sceneTabButton) {
        m_sceneTabButton->setStyleSheet(tabButtonStyle(sceneActive, overlayTextCss));
        // Reapply bold font after stylesheet to ensure font size inherits from system default
        QFont tabFont = m_sceneTabButton->font();
        tabFont.setBold(true);
        m_sceneTabButton->setFont(tabFont);
    }
    if (m_elementTabButton) {
        m_elementTabButton->setStyleSheet(tabButtonStyle(!sceneActive, overlayTextCss));
        // Reapply bold font after stylesheet to ensure font size inherits from system default
        QFont tabFont = m_elementTabButton->font();
        tabFont.setBold(true);
        m_elementTabButton->setFont(tabFont);
    }
    if (m_sceneOptionsContainer) {
        m_sceneOptionsContainer->setVisible(sceneActive);
    }
    if (m_elementPropertiesContainer) {
        m_elementPropertiesContainer->setVisible(!sceneActive);
    }

    if (m_contentLayout) {
        m_contentLayout->invalidate();
        m_contentLayout->activate();
    }
    if (m_innerContent) {
        m_innerContent->adjustSize();
    }
    updatePosition();
}

void MediaSettingsPanel::onSceneTabClicked() {
    if (m_activeTab == ActiveTab::Scene) return; // Already on this tab
    
    m_activeTab = ActiveTab::Scene;
    updateActiveTabUi();
}

void MediaSettingsPanel::onElementTabClicked() {
    if (m_activeTab == ActiveTab::Element) return; // Already on this tab
    
    m_activeTab = ActiveTab::Element;
    updateActiveTabUi();
}
