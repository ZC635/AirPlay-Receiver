#pragma once

#include <QWidget>

class VideoSurfaceWidget final : public QWidget {
    Q_OBJECT

public:
    explicit VideoSurfaceWidget(QWidget *parent = nullptr);
};
