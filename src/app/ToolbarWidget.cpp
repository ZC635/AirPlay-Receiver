#include "app/ToolbarWidget.h"
#include "app/ToolbarIcons.h"

#include <QHBoxLayout>
#include <QSlider>
#include <QToolButton>
#include <QStringList>

ToolbarWidget::ToolbarWidget(QWidget *parent)
    : QWidget(parent),
      volumeButton_(new QToolButton(this)),
      volumeSlider_(new QSlider(Qt::Horizontal, this)),
      alwaysOnTopButton_(new QToolButton(this)),
      aspectRatioButton_(new QToolButton(this)),
      videoFitButton_(new QToolButton(this)),
      recordingButton_(new QToolButton(this)),
      fullscreenButton_(new QToolButton(this)),
      settingsButton_(new QToolButton(this)) {
    volumeButton_->setObjectName("volumeButton");
    volumeButton_->setCheckable(true);

    volumeSlider_->setObjectName("volumeSlider");
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(100);
    volumeSlider_->hide();

    alwaysOnTopButton_->setObjectName("alwaysOnTopButton");
    alwaysOnTopButton_->setCheckable(true);

    aspectRatioButton_->setObjectName("aspectRatioButton");
    aspectRatioButton_->setCheckable(true);

    videoFitButton_->setObjectName("videoFitButton");
    videoFitButton_->setCheckable(true);

    recordingButton_->setObjectName("recordingButton");
    recordingButton_->setCheckable(true);
    setRecordingUi(RecordingState::Idle, false);

    fullscreenButton_->setObjectName("fullscreenButton");
    fullscreenButton_->setCheckable(true);

    settingsButton_->setObjectName("settingsButton");

    for (auto *button : {volumeButton_, alwaysOnTopButton_, aspectRatioButton_, videoFitButton_,
                         recordingButton_, fullscreenButton_, settingsButton_}) {
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setIconSize(QSize(20, 20));
        button->setFixedSize(32, 32);
    }
    updateButtonStyles();

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    layout->addWidget(volumeButton_);
    layout->addWidget(volumeSlider_);
    layout->addWidget(alwaysOnTopButton_);
    layout->addWidget(aspectRatioButton_);
    layout->addWidget(videoFitButton_);
    layout->addWidget(recordingButton_);
    layout->addWidget(fullscreenButton_);
    layout->addWidget(settingsButton_);

    connect(volumeButton_, &QToolButton::toggled, volumeSlider_, &QSlider::setVisible);
    connect(volumeSlider_, &QSlider::valueChanged, this, &ToolbarWidget::volumeChanged);
    connect(alwaysOnTopButton_, &QToolButton::toggled, this, &ToolbarWidget::alwaysOnTopToggled);
    connect(aspectRatioButton_, &QToolButton::toggled, this, &ToolbarWidget::aspectRatioToggled);
    connect(videoFitButton_, &QToolButton::toggled, this, &ToolbarWidget::videoFitToggled);
    connect(recordingButton_, &QToolButton::clicked, this, &ToolbarWidget::recordingToggledRequested);
    connect(fullscreenButton_, &QToolButton::toggled, this, [this](bool checked) {
        updateFullscreenText();
        emit fullscreenToggled(checked);
    });
    connect(settingsButton_, &QToolButton::clicked, this, &ToolbarWidget::settingsRequested);

    retranslateUi();
}

int ToolbarWidget::volume() const {
    return volumeSlider_->value();
}

void ToolbarWidget::setVolume(int value) {
    volumeSlider_->setValue(value);
}

void ToolbarWidget::setAlwaysOnTopChecked(bool checked) {
    alwaysOnTopButton_->setChecked(checked);
}

void ToolbarWidget::setFullscreenChecked(bool checked) {
    fullscreenButton_->setChecked(checked);
    updateFullscreenText();
}

void ToolbarWidget::setVolumeShortcuts(const QString &up, const QString &down) {
    volumeUpShortcut_ = up;
    volumeDownShortcut_ = down;
    updateTooltips();
}

void ToolbarWidget::setAlwaysOnTopShortcut(const QString &shortcut) {
    alwaysOnTopShortcut_ = shortcut;
    updateTooltips();
}

void ToolbarWidget::setAspectRatioChecked(bool checked) {
    aspectRatioButton_->setChecked(checked);
}

void ToolbarWidget::setAspectRatioShortcut(const QString &shortcut) {
    aspectRatioShortcut_ = shortcut;
    updateTooltips();
}

void ToolbarWidget::setVideoFitChecked(bool checked) {
    videoFitButton_->setChecked(checked);
}

void ToolbarWidget::setRecordingUi(RecordingState state, bool available) {
    recordingState_ = state;
    recordingAvailable_ = available;
    switch (recordingState_) {
    case RecordingState::Idle:
        recordingButton_->setChecked(false);
        recordingButton_->setEnabled(recordingAvailable_);
        break;
    case RecordingState::Recording:
        recordingButton_->setChecked(true);
        recordingButton_->setEnabled(true);
        break;
    case RecordingState::Finalizing:
        recordingButton_->setChecked(true);
        recordingButton_->setEnabled(false);
        break;
    }
    updateRecordingText();
    updateIcons();
}

void ToolbarWidget::setVideoFitShortcut(const QString &shortcut) {
    videoFitShortcut_ = shortcut;
    updateTooltips();
}

void ToolbarWidget::setRecordingShortcut(const QString &shortcut) {
    recordingShortcut_ = shortcut;
    updateTooltips();
}

void ToolbarWidget::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    } else if (event->type() == QEvent::PaletteChange
               || event->type() == QEvent::ApplicationPaletteChange
               || event->type() == QEvent::StyleChange) {
        updateButtonStyles();
        updateIcons();
    }
    QWidget::changeEvent(event);
}

void ToolbarWidget::retranslateUi() {
    volumeButton_->setText(tr("Volume"));
    alwaysOnTopButton_->setText(tr("Pin"));
    aspectRatioButton_->setText(tr("Aspect"));
    videoFitButton_->setText(tr("Fit"));
    settingsButton_->setText(tr("Settings"));
    for (auto *button : {volumeButton_, alwaysOnTopButton_, aspectRatioButton_, videoFitButton_,
                         settingsButton_}) {
        button->setAccessibleName(button->text());
    }
    updateFullscreenText();
    updateRecordingText();
    updateIcons();
}

void ToolbarWidget::updateRecordingText() {
    switch (recordingState_) {
    case RecordingState::Idle:
        recordingButton_->setText(tr("Record"));
        break;
    case RecordingState::Recording:
        recordingButton_->setText(tr("Stop"));
        break;
    case RecordingState::Finalizing:
        recordingButton_->setText(tr("Saving..."));
        break;
    }
    recordingButton_->setAccessibleName(recordingButton_->text());
    updateTooltips();
}

void ToolbarWidget::updateFullscreenText() {
    const QString label = fullscreenButton_->isChecked() ? tr("Exit Fullscreen") : tr("Fullscreen");
    fullscreenButton_->setText(label);
    fullscreenButton_->setAccessibleName(label);
    fullscreenButton_->setToolTip(label);
    fullscreenButton_->setIcon(ToolbarIcons::create(fullscreenButton_->isChecked()
        ? ToolbarIcons::Glyph::ExitFullscreen : ToolbarIcons::Glyph::Fullscreen, palette()));
}

void ToolbarWidget::updateTooltips() {
    const auto withShortcut = [this](const QString &label, const QString &shortcut) {
        return shortcut.isEmpty() ? label : tr("%1: %2").arg(label, shortcut);
    };
    QStringList volumeShortcuts;
    if (!volumeUpShortcut_.isEmpty()) volumeShortcuts.append(volumeUpShortcut_);
    if (!volumeDownShortcut_.isEmpty()) volumeShortcuts.append(volumeDownShortcut_);
    volumeButton_->setToolTip(withShortcut(tr("Volume"), volumeShortcuts.join(" / ")));
    alwaysOnTopButton_->setToolTip(withShortcut(tr("Pin"), alwaysOnTopShortcut_));
    aspectRatioButton_->setToolTip(withShortcut(tr("Aspect"), aspectRatioShortcut_));
    videoFitButton_->setToolTip(withShortcut(tr("Fit"), videoFitShortcut_));
    recordingButton_->setToolTip(withShortcut(recordingButton_->text(), recordingShortcut_));
    settingsButton_->setToolTip(tr("Settings"));
}

void ToolbarWidget::updateIcons() {
    using Glyph = ToolbarIcons::Glyph;
    volumeButton_->setIcon(ToolbarIcons::create(Glyph::Volume, palette()));
    alwaysOnTopButton_->setIcon(ToolbarIcons::create(Glyph::Pin, palette()));
    aspectRatioButton_->setIcon(ToolbarIcons::create(Glyph::AspectRatio, palette()));
    videoFitButton_->setIcon(ToolbarIcons::create(Glyph::VideoFit, palette()));
    const Glyph recordingGlyph = recordingState_ == RecordingState::Idle ? Glyph::Record
        : recordingState_ == RecordingState::Recording ? Glyph::Stop : Glyph::Saving;
    recordingButton_->setIcon(ToolbarIcons::create(recordingGlyph, palette()));
    fullscreenButton_->setIcon(ToolbarIcons::create(fullscreenButton_->isChecked()
        ? Glyph::ExitFullscreen : Glyph::Fullscreen, palette()));
    settingsButton_->setIcon(ToolbarIcons::create(Glyph::Settings, palette()));
}

void ToolbarWidget::updateButtonStyles() {
    const QColor base = palette().color(QPalette::Button);
    const bool dark = base.lightness() < 128;
    const QColor fill = dark ? base.lighter(130) : base.darker(115);
    const QColor border = dark ? fill.lighter(140) : fill.darker(120);
    const QColor pressed = dark ? fill.lighter(115) : fill.darker(110);
    const QString style = QStringLiteral(
        "QToolButton:checked { background-color: %1; border: 1px solid %2; border-radius: 4px; } "
        "QToolButton:checked:pressed { background-color: %3; }")
        .arg(fill.name(), border.name(), pressed.name());
    for (auto *button : {volumeButton_, alwaysOnTopButton_, aspectRatioButton_, videoFitButton_,
                         recordingButton_, fullscreenButton_, settingsButton_}) {
        if (button->styleSheet() != style) button->setStyleSheet(style);
    }
}
