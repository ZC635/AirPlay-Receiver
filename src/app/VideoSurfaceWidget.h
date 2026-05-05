#pragma once

#include <QWidget>

class VideoSurfaceWidget final : public QWidget {
    Q_OBJECT

public:
    explicit VideoSurfaceWidget(QWidget *parent = nullptr);
    void reset();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    bool requestClear_ = false;
    QColor defaultWindowColor_;
};
