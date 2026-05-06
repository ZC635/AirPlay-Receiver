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
      settingsButton_(new QToolButton(this)) {
    volumeButton_->setObjectName("volumeButton");
    volumeButton_->setText("Volume");
    volumeButton_->setCheckable(true);

    volumeSlider_->setObjectName("volumeSlider");
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setValue(100);
    volumeSlider_->hide();

    alwaysOnTopButton_->setObjectName("alwaysOnTopButton");
    alwaysOnTopButton_->setText("Pin");
    alwaysOnTopButton_->setCheckable(true);

    aspectRatioButton_->setObjectName("aspectRatioButton");
    aspectRatioButton_->setText("Aspect");
    aspectRatioButton_->setCheckable(true);

    videoFitButton_->setObjectName("videoFitButton");
    videoFitButton_->setText("Fit");
    videoFitButton_->setCheckable(true);

    settingsButton_->setObjectName("settingsButton");
    settingsButton_->setText("Settings");

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(volumeButton_);
    layout->addWidget(volumeSlider_);
    layout->addWidget(alwaysOnTopButton_);
    layout->addWidget(aspectRatioButton_);
    layout->addWidget(videoFitButton_);
    layout->addWidget(settingsButton_);

    connect(volumeButton_, &QToolButton::toggled, volumeSlider_, &QSlider::setVisible);
    connect(volumeSlider_, &QSlider::valueChanged, this, &ToolbarWidget::volumeChanged);
    connect(alwaysOnTopButton_, &QToolButton::toggled, this, &ToolbarWidget::alwaysOnTopToggled);
    connect(aspectRatioButton_, &QToolButton::toggled, this, &ToolbarWidget::aspectRatioToggled);
    connect(videoFitButton_, &QToolButton::toggled, this, &ToolbarWidget::videoFitToggled);
    connect(settingsButton_, &QToolButton::clicked, this, &ToolbarWidget::settingsRequested);
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

void ToolbarWidget::setVideoFitShortcutTooltip(const QString &tooltip) {
    videoFitButton_->setToolTip(tooltip);
}
