#pragma once

#include <QString>
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
    void setAspectRatioChecked(bool checked);
    void setVolumeShortcutTooltip(const QString &tooltip);
    void setAlwaysOnTopShortcutTooltip(const QString &tooltip);

signals:
    void volumeChanged(int value);
    void alwaysOnTopToggled(bool enabled);
    void aspectRatioToggled(bool enabled);
    void settingsRequested();

private:
    QToolButton *volumeButton_;
    QSlider *volumeSlider_;
    QToolButton *alwaysOnTopButton_;
    QToolButton *aspectRatioButton_;
    QToolButton *settingsButton_;
};
