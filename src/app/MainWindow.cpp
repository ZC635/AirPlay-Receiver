#include "app/MainWindow.h"

#include "app/ToolbarWidget.h"

#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      toolbar_(new ToolbarWidget(this)) {
    setWindowTitle("AirPlay Receiver");
    resize(960, 540);

    auto *placeholder = new QWidget(this);
    placeholder->setStyleSheet("background-color: black;");

    auto *layout = new QVBoxLayout(placeholder);
    layout->setAlignment(Qt::AlignTop | Qt::AlignRight);
    layout->addWidget(toolbar_);

    setCentralWidget(placeholder);

    connect(toolbar_, &ToolbarWidget::alwaysOnTopToggled, this, &MainWindow::setAlwaysOnTopEnabled);
}

bool MainWindow::isToolbarVisible() const {
    return !toolbar_->isHidden();
}

void MainWindow::toggleToolbarVisibility() {
    toolbar_->setVisible(!isToolbarVisible());
}

bool MainWindow::isAlwaysOnTopEnabled() const {
    return windowFlags().testFlag(Qt::WindowStaysOnTopHint);
}

void MainWindow::setAlwaysOnTopEnabled(bool enabled) {
    const bool wasVisible = isVisible();
    setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
    if (wasVisible) {
        show();
    }
}
