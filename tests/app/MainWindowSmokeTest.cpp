#include <QtTest/QtTest>
#include "app/AppSettings.h"
#include "app/MainWindow.h"
#include "app/ShortcutAction.h"
#include "app/VideoSurfaceWidget.h"
#include "backend/FakeAirPlayReceiver.h"
#include "backend/ReceiverState.h"
#include "platform/FakeHotkeyService.h"

#include <QLabel>
#include <QSlider>
#include <QToolButton>
#include <memory>

class MainWindowSmokeTest : public QObject {
    Q_OBJECT

private slots:
    void constructsWithExpectedTitle() {
        MainWindow window;
        QCOMPARE(window.windowTitle(), QString("AirPlay Receiver"));
    }

    void togglesToolbarVisibility() {
        MainWindow window;
        QVERIFY(window.isToolbarVisible());
        window.toggleToolbarVisibility();
        QVERIFY(!window.isToolbarVisible());
    }

    void togglesAlwaysOnTopState() {
        MainWindow window;
        QVERIFY(!window.isAlwaysOnTopEnabled());
        window.setAlwaysOnTopEnabled(true);
        QVERIFY(window.isAlwaysOnTopEnabled());
    }

    void shortcutTogglesToolbar() {
        FakeHotkeyService hotkeys;
        MainWindow window(AppSettings::defaults(), &hotkeys);
        QVERIFY(window.isToolbarVisible());
        emit hotkeys.activated(ShortcutAction::ToggleToolbar);
        QVERIFY(!window.isToolbarVisible());
    }

    void connectedStateHidesToolbar() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        QVERIFY(window.isToolbarVisible());

        emit receiver.stateChanged(ReceiverState::Connected);

        QVERIFY(!window.isToolbarVisible());
    }

    void leavingConnectedStateShowsToolbar() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);

        emit receiver.stateChanged(ReceiverState::Connected);
        QVERIFY(!window.isToolbarVisible());

        emit receiver.stateChanged(ReceiverState::Discoverable);

        QVERIFY(window.isToolbarVisible());
    }

    void shortcutShowsToolbarWhileConnected() {
        FakeHotkeyService hotkeys;
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), &hotkeys, &receiver);

        emit receiver.stateChanged(ReceiverState::Connected);
        QVERIFY(!window.isToolbarVisible());

        emit hotkeys.activated(ShortcutAction::ToggleToolbar);

        QVERIFY(window.isToolbarVisible());
    }

    void connectedStateSyncsStatusLabelWithToolbar() {
        FakeHotkeyService hotkeys;
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), &hotkeys, &receiver);
        auto *label = window.findChild<QLabel *>("receiverStatusLabel");
        QVERIFY(label != nullptr);
        QVERIFY(!label->isHidden());

        emit receiver.stateChanged(ReceiverState::Connected);

        QVERIFY(!window.isToolbarVisible());
        QVERIFY(label->isHidden());

        emit hotkeys.activated(ShortcutAction::ToggleToolbar);

        QVERIFY(window.isToolbarVisible());
        QVERIFY(!label->isHidden());
    }

    void nonConnectedStatusLabelStaysVisibleWhenToolbarToggles() {
        FakeHotkeyService hotkeys;
        MainWindow window(AppSettings::defaults(), &hotkeys);
        auto *label = window.findChild<QLabel *>("receiverStatusLabel");
        QVERIFY(label != nullptr);
        QVERIFY(!label->isHidden());

        emit hotkeys.activated(ShortcutAction::ToggleToolbar);

        QVERIFY(!window.isToolbarVisible());
        QVERIFY(!label->isHidden());
    }

    void statusLabelTextIsBlack() {
        MainWindow window;
        auto *label = window.findChild<QLabel *>("receiverStatusLabel");
        QVERIFY(label != nullptr);

        QVERIFY(label->styleSheet().contains("color: black"));
    }

    void toolbarIsNativeSiblingOverlayForVideoSurface() {
        MainWindow window;
        auto *surface = window.findChild<VideoSurfaceWidget *>();
        auto *volumeButton = window.findChild<QToolButton *>("volumeButton");
        QVERIFY(surface != nullptr);
        QVERIFY(volumeButton != nullptr);

        QWidget *toolbar = volumeButton->parentWidget();
        QVERIFY(toolbar != nullptr);
        QVERIFY(toolbar->parentWidget() != surface);
        QVERIFY(toolbar->testAttribute(Qt::WA_NativeWindow));
    }

    void registersDefaultShortcuts() {
        FakeHotkeyService hotkeys;
        const AppSettings settings = AppSettings::defaults();
        MainWindow window(settings, &hotkeys);

        QCOMPARE(hotkeys.registrations.size(), settings.shortcuts().size());
        const auto shortcuts = settings.shortcuts();
        for (qsizetype i = 0; i < shortcuts.size(); ++i) {
            QCOMPARE(hotkeys.registrations[i].action, shortcuts[i].action);
            QCOMPARE(hotkeys.registrations[i].sequence, shortcuts[i].sequence);
        }
    }

    void shortcutAlwaysOnTopSyncsToolbarButton() {
        FakeHotkeyService hotkeys;
        MainWindow window(AppSettings::defaults(), &hotkeys);
        auto *button = window.findChild<QToolButton *>("alwaysOnTopButton");
        QVERIFY(button != nullptr);
        QVERIFY(!button->isChecked());

        emit hotkeys.activated(ShortcutAction::ToggleAlwaysOnTop);

        QVERIFY(window.isAlwaysOnTopEnabled());
        QVERIFY(button->isChecked());
    }

    void toolbarButtonsShowShortcutTooltips() {
        AppSettings settings = AppSettings::defaults();
        settings.setShortcut(ShortcutAction::VolumeUp, QKeySequence("Ctrl+Shift+U"));
        settings.setShortcut(ShortcutAction::VolumeDown, QKeySequence("Ctrl+Shift+D"));
        settings.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Shift+P"));

        MainWindow window(settings, nullptr);
        auto *volumeButton = window.findChild<QToolButton *>("volumeButton");
        auto *pinButton = window.findChild<QToolButton *>("alwaysOnTopButton");
        QVERIFY(volumeButton != nullptr);
        QVERIFY(pinButton != nullptr);

        const QString volumeUp = settings.shortcutFor(ShortcutAction::VolumeUp).toString(QKeySequence::NativeText);
        const QString volumeDown = settings.shortcutFor(ShortcutAction::VolumeDown).toString(QKeySequence::NativeText);
        const QString pin = settings.shortcutFor(ShortcutAction::ToggleAlwaysOnTop).toString(QKeySequence::NativeText);

        QCOMPARE(volumeButton->toolTip(), QString("Volume: %1 / %2").arg(volumeUp, volumeDown));
        QCOMPARE(pinButton->toolTip(), QString("Pin: %1").arg(pin));
    }

    void volumeSliderUpdatesReceiver() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        auto *slider = window.findChild<QSlider *>("volumeSlider");
        QVERIFY(slider != nullptr);

        slider->setValue(40);

        QCOMPARE(receiver.volume(), 0.40);
    }

    void volumeChangeAfterReceiverDeletedDoesNotCrash() {
        auto receiver = std::make_unique<FakeAirPlayReceiver>();
        MainWindow window(AppSettings::defaults(), nullptr, receiver.get());
        auto *slider = window.findChild<QSlider *>("volumeSlider");
        QVERIFY(slider != nullptr);

        receiver.reset();
        slider->setValue(40);

        QVERIFY(true);
    }

    void receiverStateUpdatesStatusLabel() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        auto *label = window.findChild<QLabel *>("receiverStatusLabel");
        QVERIFY(label != nullptr);
        QCOMPARE(label->text(), QString("Ready for AirPlay"));

        emit receiver.stateChanged(ReceiverState::Connecting);
        QCOMPARE(label->text(), QString("Connecting"));

        emit receiver.stateChanged(ReceiverState::Connected);
        QCOMPARE(label->text(), QString("Connected"));

        emit receiver.errorChanged("Pairing failed");
        QCOMPARE(label->text(), QString("Pairing failed"));

        emit receiver.stateChanged(ReceiverState::Error);
        QCOMPARE(label->text(), QString("Pairing failed"));
    }

    void errorStateWithoutMessageShowsReadyStatus() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        auto *label = window.findChild<QLabel *>("receiverStatusLabel");
        QVERIFY(label != nullptr);

        emit receiver.stateChanged(ReceiverState::Error);

        QCOMPARE(label->text(), QString("Ready for AirPlay"));
    }

    void passesVideoSurfaceToReceiver() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        QVERIFY(receiver.videoSurfaceId() != 0);
    }

    void videoSurfaceWidgetExists() {
        MainWindow window;
        auto *surface = window.findChild<VideoSurfaceWidget *>();
        QVERIFY(surface != nullptr);
        QCOMPARE(surface->objectName(), QString("videoSurface"));
    }
};

QTEST_MAIN(MainWindowSmokeTest)
#include "MainWindowSmokeTest.moc"
