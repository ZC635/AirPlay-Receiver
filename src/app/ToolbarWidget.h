#pragma once

#include "backend/RecordingTypes.h"

#include <QString>
#include <QEvent>
#include <QSlider>
#include <QToolButton>
#include <QWidget>

class ToolbarWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ToolbarWidget(QWidget *parent = nullptr);
    int volume() const;
    void setVolume(int value);
    void setAlwaysOnTopChecked(bool checked);
    void setFullscreenChecked(bool checked);
    void setAspectRatioChecked(bool checked);
    void setVideoFitChecked(bool checked);
    void setRecordingUi(RecordingState state, bool available);
    void setVolumeShortcutTooltip(const QString &tooltip);
    void setAlwaysOnTopShortcutTooltip(const QString &tooltip);
    void setAspectRatioShortcutTooltip(const QString &tooltip);
    void setVideoFitShortcutTooltip(const QString &tooltip);
    void setRecordingShortcutTooltip(const QString &tooltip);

signals:
    void volumeChanged(int value);
    void alwaysOnTopToggled(bool enabled);
    void fullscreenToggled(bool enabled);
    void aspectRatioToggled(bool enabled);
    void videoFitToggled(bool enabled);
    void recordingToggledRequested();
    void settingsRequested();

private:
    void changeEvent(QEvent *event) override;
    void retranslateUi();
    void updateFullscreenText();

    QToolButton *volumeButton_;
    QSlider *volumeSlider_;
    QToolButton *alwaysOnTopButton_;
    QToolButton *aspectRatioButton_;
    QToolButton *videoFitButton_;
    QToolButton *recordingButton_;
    QToolButton *fullscreenButton_;
    QToolButton *settingsButton_;
    RecordingState recordingState_ = RecordingState::Idle;
    bool recordingAvailable_ = false;
};
