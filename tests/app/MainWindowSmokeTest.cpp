#include <QtTest/QtTest>
#include "app/AppSettings.h"
#include "app/AppSettingsStore.h"
#include "app/MainWindow.h"
#include "app/SettingsDialog.h"
#include "app/ShortcutAction.h"
#include "app/VideoSurfaceWidget.h"
#include "backend/FakeAirPlayReceiver.h"
#include "backend/ReceiverState.h"
#include "platform/FakeHotkeyService.h"

#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QImage>
#include <QKeySequenceEdit>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
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

    void leavingConnectedStateClearsVideoSurface() {
        const QColor expected = QApplication::palette().color(QPalette::Window);
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        window.resize(120, 80);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *surface = window.findChild<VideoSurfaceWidget *>();
        QVERIFY(surface != nullptr);

        emit receiver.stateChanged(ReceiverState::Connected);
        emit receiver.stateChanged(ReceiverState::Discoverable);

        QImage image(surface->size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        surface->render(&painter);
        painter.end();

        QCOMPARE(image.pixelColor(image.width() / 2, image.height() / 2), expected);
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

    void appliesLoadedVolume() {
        AppSettings settings = AppSettings::defaults();
        settings.setVolume(45);
        FakeAirPlayReceiver receiver;

        MainWindow window(settings, nullptr, &receiver);

        auto *slider = window.findChild<QSlider *>("volumeSlider");
        QVERIFY(slider != nullptr);
        QCOMPARE(slider->value(), 45);
        QCOMPARE(receiver.volume(), 0.45);
    }

    void appliesLoadedReceiverNameToReceiver() {
        AppSettings settings = AppSettings::defaults();
        settings.setReceiverName("Desk Receiver");
        FakeAirPlayReceiver receiver;

        MainWindow window(settings, nullptr, &receiver);

        QCOMPARE(receiver.receiverName(), QString("Desk Receiver"));
    }

    void changedReceiverNameAppliesImmediatelyWhenNotConnected() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, path);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *edit = dialog->findChild<QLineEdit *>("receiverNameEdit");
            QVERIFY(edit != nullptr);
            edit->setText("Desk Receiver");
            dialog->accept();
        });

        button->click();

        QCOMPARE(receiver.receiverName(), QString("Desk Receiver"));
        QCOMPARE(AppSettingsStore(path).loadOrDefaults().receiverName(), QString("Desk Receiver"));
    }

    void changedReceiverNameRestartsBroadcastWhenDiscoverable() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, path);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        receiver.forceState(ReceiverState::Discoverable);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *edit = dialog->findChild<QLineEdit *>("receiverNameEdit");
            QVERIFY(edit != nullptr);
            edit->setText("Desk Receiver");
            dialog->accept();
        });

        button->click();

        QCOMPARE(receiver.receiverName(), QString("Desk Receiver"));
        QCOMPARE(receiver.broadcastRestartCount, 1);
        QCOMPARE(receiver.stopCount, 0);
        QCOMPARE(receiver.startCount, 0);
    }

    void connectedReceiverNameChangeCanDisconnectAndApply() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, path);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        receiver.forceState(ReceiverState::Connected);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *edit = dialog->findChild<QLineEdit *>("receiverNameEdit");
            QVERIFY(edit != nullptr);
            edit->setText("Desk Receiver");
            QTimer::singleShot(0, [] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(box != nullptr);
                auto *yes = box->button(QMessageBox::Yes);
                QVERIFY(yes != nullptr);
                yes->click();
            });
            dialog->accept();
        });

        button->click();

        QCOMPARE(receiver.receiverName(), QString("Desk Receiver"));
        QCOMPARE(receiver.stopCount, 1);
        QCOMPARE(receiver.startCount, 1);
    }

    void connectedReceiverNameChangeCanBeDeferredUntilDisconnect() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, path);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        receiver.forceState(ReceiverState::Connected);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *edit = dialog->findChild<QLineEdit *>("receiverNameEdit");
            QVERIFY(edit != nullptr);
            edit->setText("Desk Receiver");
            QTimer::singleShot(0, [] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(box != nullptr);
                auto *no = box->button(QMessageBox::No);
                QVERIFY(no != nullptr);
                no->click();
            });
            dialog->accept();
        });

        button->click();

        QCOMPARE(AppSettingsStore(path).loadOrDefaults().receiverName(), QString("Desk Receiver"));
        QCOMPARE(receiver.receiverName(), QString("AirPlay Receiver"));

        receiver.forceState(ReceiverState::Discoverable);

        QCOMPARE(receiver.receiverName(), QString("Desk Receiver"));
    }

    void connectingReceiverNameChangeCanBeDeferredUntilDiscoverable() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, path);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        receiver.forceState(ReceiverState::Connecting);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *edit = dialog->findChild<QLineEdit *>("receiverNameEdit");
            QVERIFY(edit != nullptr);
            edit->setText("Desk Receiver");
            QTimer::singleShot(0, [] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(box != nullptr);
                auto *no = box->button(QMessageBox::No);
                QVERIFY(no != nullptr);
                no->click();
            });
            dialog->accept();
        });

        button->click();

        QCOMPARE(AppSettingsStore(path).loadOrDefaults().receiverName(), QString("Desk Receiver"));
        QCOMPARE(receiver.receiverName(), QString("AirPlay Receiver"));

        receiver.forceState(ReceiverState::Discoverable);

        QCOMPARE(receiver.receiverName(), QString("Desk Receiver"));
    }

    void connectingReceiverNameChangeCanDisconnectAndApply() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, path);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        receiver.forceState(ReceiverState::Connecting);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *edit = dialog->findChild<QLineEdit *>("receiverNameEdit");
            QVERIFY(edit != nullptr);
            edit->setText("Desk Receiver");
            QTimer::singleShot(0, [] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(box != nullptr);
                auto *yes = box->button(QMessageBox::Yes);
                QVERIFY(yes != nullptr);
                yes->click();
            });
            dialog->accept();
        });

        button->click();

        QCOMPARE(receiver.receiverName(), QString("Desk Receiver"));
        QCOMPARE(receiver.stopCount, 1);
        QCOMPARE(receiver.startCount, 1);
    }

    void deferredReceiverNameDoesNotPromptAgainForUnchangedPendingName() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, path);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        receiver.forceState(ReceiverState::Connected);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *edit = dialog->findChild<QLineEdit *>("receiverNameEdit");
            QVERIFY(edit != nullptr);
            edit->setText("Desk Receiver");
            QTimer::singleShot(0, [] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(box != nullptr);
                auto *no = box->button(QMessageBox::No);
                QVERIFY(no != nullptr);
                no->click();
            });
            dialog->accept();
        });

        button->click();

        QCOMPARE(AppSettingsStore(path).loadOrDefaults().receiverName(), QString("Desk Receiver"));
        QCOMPARE(receiver.receiverName(), QString("AirPlay Receiver"));

        bool promptedAgain = false;
        QTimer::singleShot(0, [&promptedAgain] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *shortcutEdit = dialog->findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
            QVERIFY(shortcutEdit != nullptr);
            shortcutEdit->setKeySequence(QKeySequence("Ctrl+Shift+H"));
            QTimer::singleShot(0, [&promptedAgain] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (box == nullptr) {
                    return;
                }
                promptedAgain = true;
                auto *no = box->button(QMessageBox::No);
                QVERIFY(no != nullptr);
                no->click();
            });
            dialog->accept();
        });

        button->click();

        QVERIFY(!promptedAgain);
        const AppSettings loaded = AppSettingsStore(path).loadOrDefaults();
        QCOMPARE(loaded.receiverName(), QString("Desk Receiver"));
        QCOMPARE(loaded.shortcutFor(ShortcutAction::ToggleToolbar), QKeySequence("Ctrl+Shift+H"));
        QCOMPARE(receiver.receiverName(), QString("AirPlay Receiver"));

        receiver.forceState(ReceiverState::Discoverable);

        QCOMPARE(receiver.receiverName(), QString("Desk Receiver"));
    }

    void deferredReceiverNameIsClearedWhenChangedBackToActiveName() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, path);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        receiver.forceState(ReceiverState::Connected);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *edit = dialog->findChild<QLineEdit *>("receiverNameEdit");
            QVERIFY(edit != nullptr);
            edit->setText("Desk Receiver");
            QTimer::singleShot(0, [] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(box != nullptr);
                auto *no = box->button(QMessageBox::No);
                QVERIFY(no != nullptr);
                no->click();
            });
            dialog->accept();
        });

        button->click();

        QCOMPARE(AppSettingsStore(path).loadOrDefaults().receiverName(), QString("Desk Receiver"));
        QCOMPARE(receiver.receiverName(), QString("AirPlay Receiver"));

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *edit = dialog->findChild<QLineEdit *>("receiverNameEdit");
            QVERIFY(edit != nullptr);
            edit->setText("AirPlay Receiver");
            dialog->accept();
        });

        button->click();

        QCOMPARE(AppSettingsStore(path).loadOrDefaults().receiverName(), QString("AirPlay Receiver"));
        QCOMPARE(receiver.receiverName(), QString("AirPlay Receiver"));

        receiver.forceState(ReceiverState::Discoverable);

        QCOMPARE(receiver.receiverName(), QString("AirPlay Receiver"));
    }

    void receiverNameApplyFailureRevertsSavedNameToDefault() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        FakeAirPlayReceiver receiver;
        receiver.rejectedReceiverNames.append("Desk Receiver");
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, path);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *edit = dialog->findChild<QLineEdit *>("receiverNameEdit");
            QVERIFY(edit != nullptr);
            edit->setText("Desk Receiver");
            dialog->accept();
        });

        button->click();

        QCOMPARE(receiver.receiverName(), QString("AirPlay Receiver"));
        QCOMPARE(AppSettingsStore(path).loadOrDefaults().receiverName(), QString("AirPlay Receiver"));

        auto *label = window.findChild<QLabel *>("receiverStatusLabel");
        QVERIFY(label != nullptr);
        QVERIFY(label->text().contains("receiver name"));
    }

    void savesVolumeChanges() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        MainWindow window(settings, nullptr, nullptr, path);

        auto *slider = window.findChild<QSlider *>("volumeSlider");
        QVERIFY(slider != nullptr);
        slider->setValue(40);

        AppSettingsStore store(path);
        QCOMPARE(store.loadOrDefaults().volume(), 40);
    }

    void acceptedSettingsDialogUpdatesHotkeysAndSavesShortcuts() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        FakeHotkeyService hotkeys;
        MainWindow window(AppSettings::defaults(), &hotkeys, nullptr, path);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        bool edited = false;
        QTimer::singleShot(0, [&edited] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            if (dialog == nullptr) {
                return;
            }
            auto *edit = dialog->findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
            if (edit == nullptr) {
                return;
            }
            edit->setKeySequence(QKeySequence("Ctrl+Shift+H"));
            edited = true;
            dialog->accept();
        });

        button->click();

        QVERIFY(edited);
        const AppSettings loaded = AppSettingsStore(path).loadOrDefaults();
        QCOMPARE(loaded.shortcutFor(ShortcutAction::ToggleToolbar), QKeySequence("Ctrl+Shift+H"));
        QCOMPARE(hotkeys.registrations.size(), AppSettings::defaults().shortcuts().size());
        const auto toggleToolbar = std::find_if(hotkeys.registrations.cbegin(), hotkeys.registrations.cend(), [](const auto &registration) {
            return registration.action == ShortcutAction::ToggleToolbar;
        });
        QVERIFY(toggleToolbar != hotkeys.registrations.cend());
        QCOMPARE(toggleToolbar->sequence, QKeySequence("Ctrl+Shift+H"));
    }

    void rejectedHotkeyRegistrationDoesNotSaveDialogSettings() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        FakeHotkeyService hotkeys;
        hotkeys.rejectedRegistrations.append({ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+H")});
        MainWindow window(AppSettings::defaults(), &hotkeys, nullptr, path);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        bool edited = false;
        QTimer::singleShot(0, [&edited] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            if (dialog == nullptr) {
                return;
            }
            auto *edit = dialog->findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
            if (edit == nullptr) {
                return;
            }
            edit->setKeySequence(QKeySequence("Ctrl+Shift+H"));
            edited = true;
            dialog->accept();
        });

        button->click();

        QVERIFY(edited);
        const AppSettings defaults = AppSettings::defaults();
        const AppSettings loaded = AppSettingsStore(path).loadOrDefaults();
        QCOMPARE(loaded.shortcutFor(ShortcutAction::ToggleToolbar), defaults.shortcutFor(ShortcutAction::ToggleToolbar));

        const auto toggleToolbar = std::find_if(hotkeys.registrations.cbegin(), hotkeys.registrations.cend(), [](const auto &registration) {
            return registration.action == ShortcutAction::ToggleToolbar;
        });
        QVERIFY(toggleToolbar != hotkeys.registrations.cend());
        QCOMPARE(toggleToolbar->sequence, defaults.shortcutFor(ShortcutAction::ToggleToolbar));

        auto *label = window.findChild<QLabel *>("receiverStatusLabel");
        QVERIFY(label != nullptr);
        QVERIFY(label->text().contains("Could not register"));
    }

    void saveFailureUpdatesStatusLabel() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        MainWindow window(AppSettings::defaults(), nullptr, nullptr, dir.path());
        auto *slider = window.findChild<QSlider *>("volumeSlider");
        QVERIFY(slider != nullptr);
        slider->setValue(40);

        auto *label = window.findChild<QLabel *>("receiverStatusLabel");
        QVERIFY(label != nullptr);
        QVERIFY(label->text().contains("Could not save settings"));
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
