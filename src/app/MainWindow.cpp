#include "app/MainWindow.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent) {
    setWindowTitle("AirPlay Receiver");
    resize(960, 540);
}
