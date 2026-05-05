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
        QVERIFY(toolbar.findChild<QToolButton *>("aspectRatioButton"));
        QVERIFY(toolbar.findChild<QToolButton *>("settingsButton"));
    }

    void exposesAspectRatioButton() {
        ToolbarWidget toolbar;
        auto *button = toolbar.findChild<QToolButton *>("aspectRatioButton");
        QVERIFY(button != nullptr);
        QVERIFY(button->isCheckable());
        QVERIFY(!button->isChecked());
    }

    void aspectRatioButtonTogglesSignal() {
        ToolbarWidget toolbar;
        QSignalSpy spy(&toolbar, &ToolbarWidget::aspectRatioToggled);
        auto *button = toolbar.findChild<QToolButton *>("aspectRatioButton");
        QVERIFY(button != nullptr);
        button->setChecked(true);
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.takeFirst().at(0).toBool());
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
