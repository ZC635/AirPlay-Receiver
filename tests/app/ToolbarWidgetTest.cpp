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

    void storesShortcutTooltips() {
        ToolbarWidget toolbar;
        toolbar.setVolumeShortcutTooltip("Volume: Ctrl+Alt+Up / Ctrl+Alt+Down");
        toolbar.setAlwaysOnTopShortcutTooltip("Pin: Ctrl+Alt+T");

        auto *volumeButton = toolbar.findChild<QToolButton *>("volumeButton");
        auto *pinButton = toolbar.findChild<QToolButton *>("alwaysOnTopButton");
        QVERIFY(volumeButton != nullptr);
        QVERIFY(pinButton != nullptr);
        QCOMPARE(volumeButton->toolTip(), QString("Volume: Ctrl+Alt+Up / Ctrl+Alt+Down"));
        QCOMPARE(pinButton->toolTip(), QString("Pin: Ctrl+Alt+T"));
    }
};

QTEST_MAIN(ToolbarWidgetTest)
#include "ToolbarWidgetTest.moc"
