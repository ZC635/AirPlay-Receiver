#pragma once

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

signals:
    void volumeChanged(int value);
    void alwaysOnTopToggled(bool enabled);
    void settingsRequested();

private:
    QToolButton *volumeButton_;
    QSlider *volumeSlider_;
    QToolButton *alwaysOnTopButton_;
    QToolButton *settingsButton_;
};
