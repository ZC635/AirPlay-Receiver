#include <QtTest/QtTest>
#include "app/ToolbarWidget.h"

class ToolbarWidgetTest : public QObject {
    Q_OBJECT

private slots:
    void exposesRequiredControls() {
        ToolbarWidget toolbar;
        QVERIFY(toolbar.findChild<QToolButton *>("volumeButton"));
        QVERIFY(toolbar.findChild<QSlider *>("volumeSlider"));
        QVERIFY(toolbar.findChild<QToolButton *>("alwaysOnTopButton"));
        QVERIFY(toolbar.findChild<QToolButton *>("settingsButton"));
    }
};

QTEST_MAIN(ToolbarWidgetTest)
#include "ToolbarWidgetTest.moc"
