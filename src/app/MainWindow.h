#pragma once

#include <QMainWindow>

class ToolbarWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    ToolbarWidget *toolbar_;
};
