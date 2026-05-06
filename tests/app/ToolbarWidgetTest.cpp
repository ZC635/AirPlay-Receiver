#include <QtTest/QtTest>
#include <QHBoxLayout>
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
        QVERIFY(toolbar.findChild<QToolButton *>("videoFitButton"));
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

    void exposesVideoFitButton() {
        ToolbarWidget toolbar;
        auto *button = toolbar.findChild<QToolButton *>("videoFitButton");
        QVERIFY(button != nullptr);
        QVERIFY(button->isCheckable());
        QVERIFY(!button->isChecked());
    }

    void videoFitButtonTogglesSignal() {
        ToolbarWidget toolbar;
        QSignalSpy spy(&toolbar, &ToolbarWidget::videoFitToggled);
        auto *button = toolbar.findChild<QToolButton *>("videoFitButton");
        QVERIFY(button != nullptr);
        button->setChecked(true);
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.takeFirst().at(0).toBool());
    }

    void videoFitButtonLayoutOrder() {
        ToolbarWidget toolbar;
        auto *layout = toolbar.layout();
        QVERIFY(layout != nullptr);
        auto *aspectBtn = toolbar.findChild<QToolButton *>("aspectRatioButton");
        auto *fitBtn = toolbar.findChild<QToolButton *>("videoFitButton");
        auto *settingsBtn = toolbar.findChild<QToolButton *>("settingsButton");
        int aspectIdx = layout->indexOf(aspectBtn);
        int fitIdx = layout->indexOf(fitBtn);
        int settingsIdx = layout->indexOf(settingsBtn);
        QVERIFY(aspectIdx >= 0);
        QVERIFY(fitIdx >= 0);
        QVERIFY(settingsIdx >= 0);
        QVERIFY(fitIdx == aspectIdx + 1);
        QVERIFY(settingsIdx == fitIdx + 1);
    }

    void storesShortcutTooltips() {
        ToolbarWidget toolbar;
        toolbar.setVolumeShortcutTooltip("Volume: Ctrl+Alt+Up / Ctrl+Alt+Down");
        toolbar.setAlwaysOnTopShortcutTooltip("Pin: Ctrl+Alt+T");
        toolbar.setAspectRatioShortcutTooltip("Aspect: Ctrl+Alt+A");
        toolbar.setVideoFitShortcutTooltip("Fit: Ctrl+Alt+F");

        auto *volumeButton = toolbar.findChild<QToolButton *>("volumeButton");
        auto *pinButton = toolbar.findChild<QToolButton *>("alwaysOnTopButton");
        auto *aspectButton = toolbar.findChild<QToolButton *>("aspectRatioButton");
        auto *fitButton = toolbar.findChild<QToolButton *>("videoFitButton");
        QVERIFY(volumeButton != nullptr);
        QVERIFY(pinButton != nullptr);
        QVERIFY(aspectButton != nullptr);
        QVERIFY(fitButton != nullptr);
        QCOMPARE(volumeButton->toolTip(), QString("Volume: Ctrl+Alt+Up / Ctrl+Alt+Down"));
        QCOMPARE(pinButton->toolTip(), QString("Pin: Ctrl+Alt+T"));
        QCOMPARE(aspectButton->toolTip(), QString("Aspect: Ctrl+Alt+A"));
        QCOMPARE(fitButton->toolTip(), QString("Fit: Ctrl+Alt+F"));
    }
};

QTEST_MAIN(ToolbarWidgetTest)
#include "ToolbarWidgetTest.moc"
