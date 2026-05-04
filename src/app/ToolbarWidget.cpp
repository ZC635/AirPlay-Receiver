#include "app/ToolbarWidget.h"

#include <QHBoxLayout>
#include <QSlider>
#include <QToolButton>

ToolbarWidget::ToolbarWidget(QWidget *parent)
    : QWidget(parent),
      volumeButton_(new QToolButton(this)),
      volumeSlider_(new QSlider(Qt::Horizontal, this)),
      alwaysOnTopButton_(new QToolButton(this)),
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

    settingsButton_->setObjectName("settingsButton");
    settingsButton_->setText("Settings");

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(volumeButton_);
    layout->addWidget(volumeSlider_);
    layout->addWidget(alwaysOnTopButton_);
    layout->addWidget(settingsButton_);

    connect(volumeButton_, &QToolButton::toggled, volumeSlider_, &QSlider::setVisible);
    connect(volumeSlider_, &QSlider::valueChanged, this, &ToolbarWidget::volumeChanged);
    connect(alwaysOnTopButton_, &QToolButton::toggled, this, &ToolbarWidget::alwaysOnTopToggled);
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
