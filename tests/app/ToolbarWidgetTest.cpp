#include <QtTest/QtTest>
#include <QHBoxLayout>
#include "app/ToolbarWidget.h"
#include "backend/RecordingTypes.h"

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
        QVERIFY(toolbar.findChild<QToolButton *>("recordingButton"));
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
        auto *recordingBtn = toolbar.findChild<QToolButton *>("recordingButton");
        auto *settingsBtn = toolbar.findChild<QToolButton *>("settingsButton");
        int aspectIdx = layout->indexOf(aspectBtn);
        int fitIdx = layout->indexOf(fitBtn);
        int recordingIdx = layout->indexOf(recordingBtn);
        int settingsIdx = layout->indexOf(settingsBtn);
        QVERIFY(aspectIdx >= 0);
        QVERIFY(fitIdx >= 0);
        QVERIFY(recordingIdx >= 0);
        QVERIFY(settingsIdx >= 0);
        QVERIFY(fitIdx == aspectIdx + 1);
        QVERIFY(recordingIdx == fitIdx + 1);
        QVERIFY(settingsIdx == recordingIdx + 1);
    }

    void recordingButtonReflectsAllStates() {
        ToolbarWidget toolbar;
        auto *button = toolbar.findChild<QToolButton *>("recordingButton");
        QVERIFY(button != nullptr);
        QVERIFY(button->isCheckable());
        auto *fitButton = toolbar.findChild<QToolButton *>("videoFitButton");
        QVERIFY(fitButton != nullptr);
        QCOMPARE(button->styleSheet(), fitButton->styleSheet());

        toolbar.setRecordingUi(RecordingState::Idle, false);
        QCOMPARE(button->text(), QString("Record"));
        QVERIFY(!button->isChecked());
        QVERIFY(!button->isEnabled());

        toolbar.setRecordingUi(RecordingState::Idle, true);
        QCOMPARE(button->text(), QString("Record"));
        QVERIFY(!button->isChecked());
        QVERIFY(button->isEnabled());

        toolbar.setRecordingUi(RecordingState::Recording, false);
        QCOMPARE(button->text(), QString("Stop"));
        QVERIFY(button->isChecked());
        QVERIFY(button->isEnabled());

        toolbar.setRecordingUi(RecordingState::Finalizing, true);
        QCOMPARE(button->text(), QString("Saving..."));
        QVERIFY(button->isChecked());
        QVERIFY(!button->isEnabled());
    }

    void recordingSignalOnlyComesFromEnabledClick() {
        ToolbarWidget toolbar;
        QSignalSpy spy(&toolbar, &ToolbarWidget::recordingToggledRequested);
        auto *button = toolbar.findChild<QToolButton *>("recordingButton");
        QVERIFY(button != nullptr);

        toolbar.setRecordingUi(RecordingState::Idle, true);
        QCOMPARE(spy.count(), 0);
        button->click();
        QCOMPARE(spy.count(), 1);

        toolbar.setRecordingUi(RecordingState::Recording, false);
        QCOMPARE(spy.count(), 1);

        toolbar.setRecordingUi(RecordingState::Finalizing, true);
        button->click();
        QCOMPARE(spy.count(), 1);
    }

    void storesShortcutTooltips() {
        ToolbarWidget toolbar;
        toolbar.setVolumeShortcutTooltip("Volume: Ctrl+Alt+Up / Ctrl+Alt+Down");
        toolbar.setAlwaysOnTopShortcutTooltip("Pin: Ctrl+Alt+T");
        toolbar.setAspectRatioShortcutTooltip("Aspect: Ctrl+Alt+A");
        toolbar.setVideoFitShortcutTooltip("Fit: Ctrl+Alt+F");
        toolbar.setRecordingShortcutTooltip("Record: Ctrl+Alt+R");

        auto *volumeButton = toolbar.findChild<QToolButton *>("volumeButton");
        auto *pinButton = toolbar.findChild<QToolButton *>("alwaysOnTopButton");
        auto *aspectButton = toolbar.findChild<QToolButton *>("aspectRatioButton");
        auto *fitButton = toolbar.findChild<QToolButton *>("videoFitButton");
        auto *recordingButton = toolbar.findChild<QToolButton *>("recordingButton");
        QVERIFY(volumeButton != nullptr);
        QVERIFY(pinButton != nullptr);
        QVERIFY(aspectButton != nullptr);
        QVERIFY(fitButton != nullptr);
        QVERIFY(recordingButton != nullptr);
        QCOMPARE(volumeButton->toolTip(), QString("Volume: Ctrl+Alt+Up / Ctrl+Alt+Down"));
        QCOMPARE(pinButton->toolTip(), QString("Pin: Ctrl+Alt+T"));
        QCOMPARE(aspectButton->toolTip(), QString("Aspect: Ctrl+Alt+A"));
        QCOMPARE(fitButton->toolTip(), QString("Fit: Ctrl+Alt+F"));
        QCOMPARE(recordingButton->toolTip(), QString("Record: Ctrl+Alt+R"));
    }
};

QTEST_MAIN(ToolbarWidgetTest)
#include "ToolbarWidgetTest.moc"
