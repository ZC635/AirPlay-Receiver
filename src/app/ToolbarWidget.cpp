#include "app/ToolbarWidget.h"

#include <QHBoxLayout>
#include <QSlider>
#include <QToolButton>

ToolbarWidget::ToolbarWidget(QWidget *parent)
    : QWidget(parent),
      volumeButton_(new QToolButton(this)),
      volumeSlider_(new QSlider(Qt::Horizontal, this)),
      alwaysOnTopButton_(new QToolButton(this)),
      aspectRatioButton_(new QToolButton(this)),
      videoFitButton_(new QToolButton(this)),
      recordingButton_(new QToolButton(this)),
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

    settingsButton_->setObjectName("settingsButton");

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(volumeButton_);
    layout->addWidget(volumeSlider_);
    layout->addWidget(alwaysOnTopButton_);
    layout->addWidget(aspectRatioButton_);
    layout->addWidget(videoFitButton_);
    layout->addWidget(recordingButton_);
    layout->addWidget(settingsButton_);

    connect(volumeButton_, &QToolButton::toggled, volumeSlider_, &QSlider::setVisible);
    connect(volumeSlider_, &QSlider::valueChanged, this, &ToolbarWidget::volumeChanged);
    connect(alwaysOnTopButton_, &QToolButton::toggled, this, &ToolbarWidget::alwaysOnTopToggled);
    connect(aspectRatioButton_, &QToolButton::toggled, this, &ToolbarWidget::aspectRatioToggled);
    connect(videoFitButton_, &QToolButton::toggled, this, &ToolbarWidget::videoFitToggled);
    connect(recordingButton_, &QToolButton::clicked, this, &ToolbarWidget::recordingToggledRequested);
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

void ToolbarWidget::setVolumeShortcutTooltip(const QString &tooltip) {
    volumeButton_->setToolTip(tooltip);
}

void ToolbarWidget::setAlwaysOnTopShortcutTooltip(const QString &tooltip) {
    alwaysOnTopButton_->setToolTip(tooltip);
}

void ToolbarWidget::setAspectRatioChecked(bool checked) {
    aspectRatioButton_->setChecked(checked);
}

void ToolbarWidget::setAspectRatioShortcutTooltip(const QString &tooltip) {
    aspectRatioButton_->setToolTip(tooltip);
}

void ToolbarWidget::setVideoFitChecked(bool checked) {
    videoFitButton_->setChecked(checked);
}

void ToolbarWidget::setRecordingUi(RecordingState state, bool available) {
    recordingState_ = state;
    recordingAvailable_ = available;
    switch (recordingState_) {
    case RecordingState::Idle:
        recordingButton_->setText(tr("Record"));
        recordingButton_->setChecked(false);
        recordingButton_->setEnabled(recordingAvailable_);
        break;
    case RecordingState::Recording:
        recordingButton_->setText(tr("Stop"));
        recordingButton_->setChecked(true);
        recordingButton_->setEnabled(true);
        break;
    case RecordingState::Finalizing:
        recordingButton_->setText(tr("Saving..."));
        recordingButton_->setChecked(true);
        recordingButton_->setEnabled(false);
        break;
    }
}

void ToolbarWidget::setVideoFitShortcutTooltip(const QString &tooltip) {
    videoFitButton_->setToolTip(tooltip);
}

void ToolbarWidget::setRecordingShortcutTooltip(const QString &tooltip) {
    recordingButton_->setToolTip(tooltip);
}

void ToolbarWidget::changeEvent(QEvent *event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QWidget::changeEvent(event);
}

void ToolbarWidget::retranslateUi() {
    volumeButton_->setText(tr("Volume"));
    alwaysOnTopButton_->setText(tr("Pin"));
    aspectRatioButton_->setText(tr("Aspect"));
    videoFitButton_->setText(tr("Fit"));
    settingsButton_->setText(tr("Settings"));

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
}
