#include <QtTest/QtTest>
#include "app/AppSettings.h"
#include "app/AppSettingsStore.h"
#include "app/MainWindow.h"
#include "app/SettingsDialog.h"
#include "app/ShortcutAction.h"
#include "app/VideoSurfaceWidget.h"
#include "app/WindowStateStore.h"
#include "backend/FakeAirPlayReceiver.h"
#include "backend/ReceiverState.h"
#include "platform/FakeHotkeyService.h"
#include "platform/RecordingPathActions.h"

#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QImage>
#include <QIcon>
#include <QFileInfo>
#include <QKeySequenceEdit>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <cmath>
#include <memory>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

class WindowVisibilityEventCounter final : public QObject {
public:
    int hideEvents = 0;
    int showEvents = 0;

    void reset() {
        hideEvents = 0;
        showEvents = 0;
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::Hide) {
            ++hideEvents;
        } else if (event->type() == QEvent::Show) {
            ++showEvents;
        }
        return QObject::eventFilter(watched, event);
    }
};

bool windowHasNativeTopmostState(const QWidget &widget) {
    const HWND window = reinterpret_cast<HWND>(widget.winId());
    return (GetWindowLongPtr(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

RECT nativeClientRectFor(HWND hwnd);

HWND windowFromWidgetCenter(const QWidget &widget) {
    const QWidget *window = widget.window();
    if (window == nullptr || window->width() <= 0 || window->height() <= 0) {
        const QPoint globalCenter = widget.mapToGlobal(widget.rect().center());
        return WindowFromPoint(POINT{globalCenter.x(), globalCenter.y()});
    }

    const HWND windowHwnd = reinterpret_cast<HWND>(window->winId());
    const RECT client = nativeClientRectFor(windowHwnd);
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    if (clientWidth <= 0 || clientHeight <= 0) {
        const QPoint globalCenter = widget.mapToGlobal(widget.rect().center());
        return WindowFromPoint(POINT{globalCenter.x(), globalCenter.y()});
    }

    const QPoint logicalCenter = widget.mapTo(window, widget.rect().center());
    const POINT nativeCenter{
        client.left + static_cast<LONG>(std::lround(logicalCenter.x() * static_cast<double>(clientWidth) / window->width())),
        client.top + static_cast<LONG>(std::lround(logicalCenter.y() * static_cast<double>(clientHeight) / window->height()))};
    return WindowFromPoint(nativeCenter);
}

RECT nativeClientRectFor(HWND hwnd) {
    RECT localClient{};
    if (!GetClientRect(hwnd, &localClient)) {
        return RECT{};
    }

    POINT clientPoints[2] = {{localClient.left, localClient.top}, {localClient.right, localClient.bottom}};
    if (MapWindowPoints(hwnd, nullptr, clientPoints, 2) == 0 && GetLastError() != 0) {
        return RECT{};
    }
    return RECT{clientPoints[0].x, clientPoints[0].y, clientPoints[1].x, clientPoints[1].y};
}

double windowAspectRatio(const QWidget &widget) {
    return static_cast<double>(widget.width()) / widget.height();
}

void verifyWindowAspectRatio(const QWidget &widget, double expectedRatio) {
    const double diff = qAbs(windowAspectRatio(widget) - expectedRatio);
    QVERIFY2(diff < 0.01,
             qPrintable(QStringLiteral("actual=%1 expected=%2")
                            .arg(windowAspectRatio(widget))
                            .arg(expectedRatio)));
}

class RejectingRecordingReceiver final : public FakeAirPlayReceiver {
public:
    RecordingStartResult startRecording(const RecordingOptions &options) override {
        lastRecordingOptions = options;
        ++startRecordingCount;
        return {false, QStringLiteral("Output folder unavailable")};
    }
};

class FakeRecordingPathActions final : public RecordingPathActions {
public:
    QString chooseExistingDirectory(QWidget *, const QString &) override {
        return chosenDirectory;
    }

    QString ensureAndOpenDirectory(const QString &directory) override {
        openedDirectory = directory;
        return openError;
    }

    QString revealFile(const QString &filePath) override {
        revealedFile = filePath;
        return revealError;
    }

    QString chosenDirectory;
    QString openedDirectory;
    QString revealedFile;
    QString openError;
    QString revealError;
};

class ExitRaceRecordingReceiver final : public FakeAirPlayReceiver {
public:
    void discardRecording() override {
        ++discardCallsIncludingIdle;
        FakeAirPlayReceiver::discardRecording();
    }

    int discardCallsIncludingIdle = 0;
};

class MainWindowSmokeTest : public QObject {
    Q_OBJECT

private slots:
    void constructsWithExpectedTitle() {
        MainWindow window;
        QCOMPARE(window.windowTitle(), QString("AirPlay Receiver"));
    }

    void constructsWithApplicationIcon() {
        MainWindow window;
        QVERIFY(!window.windowIcon().isNull());
        QVERIFY(!window.windowIcon().availableSizes().isEmpty());
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

    void visibleAlwaysOnTopToggleDoesNotHideOrShowWindow() {
        if (QGuiApplication::platformName().compare("windows", Qt::CaseInsensitive) != 0) {
            QSKIP("Requires the Windows QPA platform");
        }

        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        WindowVisibilityEventCounter events;
        window.installEventFilter(&events);

        window.setAlwaysOnTopEnabled(true);
        QCoreApplication::processEvents();

        QCOMPARE(events.hideEvents, 0);
        QCOMPARE(events.showEvents, 0);
        QVERIFY(window.isAlwaysOnTopEnabled());
        QVERIFY(windowHasNativeTopmostState(window));

        events.reset();

        window.setAlwaysOnTopEnabled(false);
        QCoreApplication::processEvents();

        QCOMPARE(events.hideEvents, 0);
        QCOMPARE(events.showEvents, 0);
        QVERIFY(!window.isAlwaysOnTopEnabled());
        QVERIFY(!windowHasNativeTopmostState(window));
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

    void recordingUiQueriesReceiverInsteadOfSignalArguments() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        auto *button = window.findChild<QToolButton *>("recordingButton");
        QVERIFY(button != nullptr);
        QVERIFY(!button->isEnabled());

        receiver.setRecordingAvailableForTest(true);
        QVERIFY(button->isEnabled());
        QCOMPARE(button->text(), QString("Record"));

        emit receiver.recordingAvailabilityChanged(false);
        QVERIFY(button->isEnabled());
        emit receiver.recordingStateChanged(RecordingState::Finalizing);
        QCOMPARE(button->text(), QString("Record"));
        QVERIFY(!button->isChecked());
    }

    void recordingButtonStartsWithSettingsSnapshot() {
        AppSettings settings = AppSettings::defaults();
        settings.setRecordingOutputDirectory("C:/recording-snapshot");
        settings.setRecordingFormat(RecordingFormat::Mp4);
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        MainWindow window(settings, nullptr, &receiver);
        auto *button = window.findChild<QToolButton *>("recordingButton");
        QVERIFY(button != nullptr);

        button->click();

        QCOMPARE(receiver.startRecordingCount, 1);
        QCOMPARE(receiver.lastRecordingOptions.outputDirectory,
                 settings.recordingOutputDirectory());
        QCOMPARE(receiver.lastRecordingOptions.format, RecordingFormat::Mp4);
        QCOMPARE(receiver.recordingState(), RecordingState::Recording);
        QCOMPARE(button->text(), QString("Stop"));
        QVERIFY(button->isChecked());
    }

    void recordingRemainsStoppableWhenAvailabilityDrops() {
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        auto *button = window.findChild<QToolButton *>("recordingButton");
        QVERIFY(button != nullptr);
        button->click();
        QCOMPARE(receiver.recordingState(), RecordingState::Recording);

        receiver.setRecordingAvailableForTest(false);

        QVERIFY(button->isEnabled());
        QCOMPARE(button->text(), QString("Stop"));
        button->click();
        QCOMPARE(receiver.stopRecordingCount, 1);
        QCOMPARE(receiver.recordingState(), RecordingState::Finalizing);
    }

    void rapidRecordingStopClicksCallReceiverOnce() {
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        auto *button = window.findChild<QToolButton *>("recordingButton");
        QVERIFY(button != nullptr);
        button->click();

        button->click();
        button->click();

        QCOMPARE(receiver.stopRecordingCount, 1);
        QCOMPARE(receiver.recordingState(), RecordingState::Finalizing);
        QVERIFY(!button->isEnabled());
        QCOMPARE(button->text(), QString("Saving..."));
    }

    void unavailableRecordingShortcutShowsExactStatus() {
        FakeAirPlayReceiver receiver;
        FakeHotkeyService hotkeys;
        MainWindow window(AppSettings::defaults(), &hotkeys, &receiver);
        auto *status = window.findChild<QLabel *>("receiverStatusLabel");
        QVERIFY(status != nullptr);

        emit hotkeys.activated(ShortcutAction::ToggleRecording);

        QCOMPARE(status->text(), QString("No recordable mirrored content"));
        QCOMPARE(receiver.startRecordingCount, 0);
        QCOMPARE(receiver.recordingState(), RecordingState::Idle);
    }

    void recordingShortcutWithoutReceiverShowsExactStatus() {
        FakeHotkeyService hotkeys;
        MainWindow window(AppSettings::defaults(), &hotkeys);
        auto *status = window.findChild<QLabel *>("receiverStatusLabel");
        QVERIFY(status != nullptr);

        emit hotkeys.activated(ShortcutAction::ToggleRecording);

        QCOMPARE(status->text(), QString("No recordable mirrored content"));
    }

    void recordingShortcutAfterReceiverDestructionIsSafe() {
        auto receiver = std::make_unique<FakeAirPlayReceiver>();
        receiver->setRecordingAvailableForTest(true);
        FakeHotkeyService hotkeys;
        MainWindow window(AppSettings::defaults(), &hotkeys, receiver.get());
        auto *status = window.findChild<QLabel *>("receiverStatusLabel");
        auto *button = window.findChild<QToolButton *>("recordingButton");
        QVERIFY(status != nullptr);
        QVERIFY(button != nullptr);
        QVERIFY(button->isEnabled());
        receiver.reset();

        QVERIFY(!button->isEnabled());
        QVERIFY(!button->isChecked());
        QCOMPARE(button->text(), QString("Record"));

        emit hotkeys.activated(ShortcutAction::ToggleRecording);

        QCOMPARE(status->text(), QString("No recordable mirrored content"));
        QVERIFY(!button->isEnabled());
        QVERIFY(!button->isChecked());
    }

    void recordingTooltipUsesConfiguredNativeShortcut() {
        AppSettings defaults = AppSettings::defaults();
        MainWindow defaultWindow(defaults, nullptr);
        auto *defaultButton = defaultWindow.findChild<QToolButton *>("recordingButton");
        QVERIFY(defaultButton != nullptr);
        QCOMPARE(defaultButton->toolTip(), QString("Record: %1").arg(
            defaults.shortcutFor(ShortcutAction::ToggleRecording)
                .toString(QKeySequence::NativeText)));

        AppSettings custom = AppSettings::defaults();
        custom.setShortcut(ShortcutAction::ToggleRecording,
                           QKeySequence("Ctrl+Shift+R"));
        MainWindow customWindow(custom, nullptr);
        auto *customButton = customWindow.findChild<QToolButton *>("recordingButton");
        QVERIFY(customButton != nullptr);
        QCOMPARE(customButton->toolTip(), QString("Record: %1").arg(
            custom.shortcutFor(ShortcutAction::ToggleRecording)
                .toString(QKeySequence::NativeText)));
    }

    void recordingStartRejectionShowsSpecificError() {
        RejectingRecordingReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        auto *button = window.findChild<QToolButton *>("recordingButton");
        auto *status = window.findChild<QLabel *>("receiverStatusLabel");
        QVERIFY(button != nullptr);
        QVERIFY(status != nullptr);

        button->click();

        QCOMPARE(receiver.startRecordingCount, 1);
        QCOMPARE(receiver.recordingState(), RecordingState::Idle);
        QCOMPARE(status->text(), QString("Output folder unavailable"));
        QCOMPARE(button->text(), QString("Record"));
    }

    void rejectedRecordingStartDoesNotCaptureCompletionPreference() {
        AppSettings settings = AppSettings::defaults();
        settings.setShowRecordingCompletionMessage(true);
        RejectingRecordingReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        MainWindow window(settings, nullptr, &receiver);
        window.findChild<QToolButton *>("recordingButton")->click();

        emit receiver.recordingFinished(
            RecordingResult{"C:/recordings/never-started.mp4", {}});

        QVERIFY(QApplication::activeModalWidget() == nullptr);
    }

    void recordingShortcutStopsButDoesNothingWhileFinalizing() {
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        FakeHotkeyService hotkeys;
        MainWindow window(AppSettings::defaults(), &hotkeys, &receiver);

        emit hotkeys.activated(ShortcutAction::ToggleRecording);
        QCOMPARE(receiver.startRecordingCount, 1);
        emit hotkeys.activated(ShortcutAction::ToggleRecording);
        QCOMPARE(receiver.stopRecordingCount, 1);
        QCOMPARE(receiver.recordingState(), RecordingState::Finalizing);
        emit hotkeys.activated(ShortcutAction::ToggleRecording);
        QCOMPARE(receiver.stopRecordingCount, 1);
    }

    void recordingActionsDoNotForceToolbarVisibility() {
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        receiver.forceState(ReceiverState::Connected);
        FakeHotkeyService hotkeys;
        MainWindow window(AppSettings::defaults(), &hotkeys, &receiver);
        QVERIFY(!window.isToolbarVisible());

        emit hotkeys.activated(ShortcutAction::ToggleToolbar);
        QVERIFY(window.isToolbarVisible());
        emit hotkeys.activated(ShortcutAction::ToggleRecording);
        QVERIFY(window.isToolbarVisible());

        emit hotkeys.activated(ShortcutAction::ToggleToolbar);
        QVERIFY(!window.isToolbarVisible());
        emit hotkeys.activated(ShortcutAction::ToggleRecording);
        QVERIFY(!window.isToolbarVisible());
    }

    void cleanRecordingCompletionUsesStartSnapshotAndRevealsActualFile() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AppSettings settings = AppSettings::defaults();
        settings.setRecordingOutputDirectory(dir.filePath("folder-a"));
        settings.setShowRecordingCompletionMessage(true);
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        FakeRecordingPathActions pathActions;
        MainWindow window(settings, nullptr, &receiver, QString(), &pathActions);
        auto *recordButton = window.findChild<QToolButton *>("recordingButton");
        QVERIFY(recordButton != nullptr);

        recordButton->click();
        recordButton->click();
        const QString actualPath = QFileInfo(dir.filePath("folder-a/actual.mp4"))
                                       .absoluteFilePath();
        bool sawInformation = false;
        QTimer::singleShot(0, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box != nullptr);
            sawInformation = box->icon() == QMessageBox::Information;
            QVERIFY(box->text().contains(QDir::toNativeSeparators(actualPath)));
            QPushButton *openFolder = nullptr;
            for (auto *button : box->buttons()) {
                if (button->text() == "Open Folder") {
                    openFolder = qobject_cast<QPushButton *>(button);
                }
            }
            QVERIFY(openFolder != nullptr);
            QVERIFY(box->button(QMessageBox::Ok) != nullptr);
            openFolder->click();
        });

        receiver.completeRecordingForTest({actualPath, {}});

        QVERIFY(sawInformation);
        QCOMPARE(pathActions.revealedFile, actualPath);
    }

    void revealFailureAfterCompletionIsAlwaysVisible() {
        AppSettings settings = AppSettings::defaults();
        settings.setShowRecordingCompletionMessage(true);
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        FakeRecordingPathActions pathActions;
        pathActions.revealError = "Explorer could not reveal recording";
        MainWindow window(settings, nullptr, &receiver, QString(), &pathActions);
        auto *recordButton = window.findChild<QToolButton *>("recordingButton");
        recordButton->click();
        recordButton->click();

        bool sawActionFailure = false;
        QTimer::singleShot(0, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box != nullptr);
            for (auto *button : box->buttons()) {
                if (button->text() == "Open Folder") {
                    button->click();
                    QTimer::singleShot(0, [&] {
                        auto *failure = qobject_cast<QMessageBox *>(
                            QApplication::activeModalWidget());
                        QVERIFY(failure != nullptr);
                        sawActionFailure = failure->icon() == QMessageBox::Warning &&
                                           failure->text().contains(pathActions.revealError);
                        failure->button(QMessageBox::Ok)->click();
                    });
                    return;
                }
            }
            QFAIL("Open Folder button missing");
        });

        receiver.completeRecordingForTest({"C:/recordings/action-error.mp4", {}});
        QVERIFY(sawActionFailure);
    }

    void completionPreferenceOnlySuppressesCleanSuccess() {
        AppSettings settings = AppSettings::defaults();
        settings.setShowRecordingCompletionMessage(false);
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        MainWindow window(settings, nullptr, &receiver);
        auto *recordButton = window.findChild<QToolButton *>("recordingButton");
        QVERIFY(recordButton != nullptr);

        recordButton->click();
        recordButton->click();
        receiver.completeRecordingForTest({"C:/recordings/quiet.mp4", {}});
        QVERIFY(QApplication::activeModalWidget() == nullptr);

        recordButton->click();
        recordButton->click();
        bool sawWarning = false;
        QTimer::singleShot(0, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box != nullptr);
            sawWarning = box->icon() == QMessageBox::Warning;
            QVERIFY(box->text().contains("dropped frames"));
            box->button(QMessageBox::Ok)->click();
        });
        receiver.completeRecordingForTest(
            {"C:/recordings/warn.mp4", "Recording completed with dropped frames"});
        QVERIFY(sawWarning);

        recordButton->click();
        bool sawCritical = false;
        QTimer::singleShot(0, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box != nullptr);
            sawCritical = box->icon() == QMessageBox::Critical;
            QVERIFY(box->text().contains("muxer failed"));
            box->button(QMessageBox::Ok)->click();
        });
        receiver.failRecordingForTest("Runtime muxer failed");
        QVERIFY(sawCritical);
    }

    void recordingSettingsChangedMidSessionOnlyAffectNextStart() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString settingsPath = dir.filePath("settings.json");
        const QString folderA = QFileInfo(dir.filePath("folder-a")).absoluteFilePath();
        const QString folderB = QFileInfo(dir.filePath("folder-b")).absoluteFilePath();
        AppSettings settings = AppSettings::defaults();
        settings.setRecordingOutputDirectory(folderA);
        settings.setShowRecordingCompletionMessage(true);
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        FakeRecordingPathActions pathActions;
        MainWindow window(settings, nullptr, &receiver, settingsPath, &pathActions);
        auto *recordButton = window.findChild<QToolButton *>("recordingButton");
        auto *settingsButton = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(recordButton != nullptr);
        QVERIFY(settingsButton != nullptr);

        recordButton->click();
        QCOMPARE(receiver.lastRecordingOptions.outputDirectory, folderA);
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *folderEdit = dialog->findChild<QLineEdit *>("recordingOutputDirectoryEdit");
            auto *completion = dialog->findChild<QCheckBox *>(
                "showRecordingCompletionMessageCheckBox");
            QVERIFY(folderEdit != nullptr);
            QVERIFY(completion != nullptr);
            folderEdit->setText(QDir::toNativeSeparators(folderB));
            completion->setChecked(false);
            dialog->accept();
        });
        settingsButton->click();

        recordButton->click();
        bool sawDialog = false;
        const QString actualA = QFileInfo(dir.filePath("folder-a/actual.mp4"))
                                    .absoluteFilePath();
        QTimer::singleShot(0, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box != nullptr);
            sawDialog = true;
            QVERIFY(box->text().contains(QDir::toNativeSeparators(actualA)));
            box->button(QMessageBox::Ok)->click();
        });
        receiver.completeRecordingForTest({actualA, {}});
        QVERIFY(sawDialog);

        recordButton->click();
        QCOMPARE(receiver.lastRecordingOptions.outputDirectory, folderB);
    }

    void acceptedReceiverRestartWaitsForRecordingSave() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString settingsPath = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        settings.setShowRecordingCompletionMessage(false);
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        receiver.forceState(ReceiverState::Connected);
        MainWindow window(settings, nullptr, &receiver, settingsPath);
        auto *recordButton = window.findChild<QToolButton *>("recordingButton");
        auto *settingsButton = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(recordButton != nullptr);
        QVERIFY(settingsButton != nullptr);
        recordButton->click();

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            dialog->findChild<QLineEdit *>("receiverNameEdit")->setText("Desk Receiver");
            QTimer::singleShot(0, [] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(box != nullptr);
                box->button(QMessageBox::Yes)->click();
            });
            dialog->accept();
        });
        settingsButton->click();

        QCOMPARE(AppSettingsStore(settingsPath).loadOrDefaults().receiverName(),
                 QString("Desk Receiver"));
        QCOMPARE(receiver.receiverName(), QString("AirPlay Receiver"));
        QCOMPARE(receiver.stopRecordingCount, 1);
        QCOMPARE(receiver.recordingState(), RecordingState::Finalizing);

        receiver.completeRecordingForTest({"C:/recordings/saved.mp4", {}});

        QCOMPARE(receiver.receiverName(), QString("Desk Receiver"));
        QCOMPARE(receiver.stopCount, 1);
        QCOMPARE(receiver.startCount, 1);
    }

    void acceptedVideoRestartRunsAfterFinalizeFailure() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString settingsPath = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        settings.setShowRecordingCompletionMessage(false);
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        receiver.forceState(ReceiverState::Connected);
        MainWindow window(settings, nullptr, &receiver, settingsPath);
        auto *recordButton = window.findChild<QToolButton *>("recordingButton");
        auto *settingsButton = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(recordButton != nullptr);
        QVERIFY(settingsButton != nullptr);
        recordButton->click();
        recordButton->click();

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *combo = dialog->findChild<QComboBox *>("videoResolutionCombo");
            combo->setCurrentIndex(combo->findData(static_cast<int>(VideoResolution::P720)));
            QTimer::singleShot(0, [] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(box != nullptr);
                box->button(QMessageBox::Yes)->click();
            });
            dialog->accept();
        });
        settingsButton->click();

        QCOMPARE(receiver.stopRecordingCount, 1);
        QCOMPARE(receiver.lastAppliedVideoQuality, AppSettings::defaults().videoQuality());
        bool sawFailure = false;
        QTimer::singleShot(0, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box != nullptr);
            sawFailure = box->icon() == QMessageBox::Critical;
            box->button(QMessageBox::Ok)->click();
        });
        receiver.failRecordingForTest("Could not rename recording");

        QVERIFY(sawFailure);
        QCOMPARE(receiver.lastAppliedVideoQuality.resolution, VideoResolution::P720);
        QCOMPARE(receiver.stopCount, 1);
        QCOMPARE(receiver.startCount, 1);
    }

    void closeCancelLeavesRecordingUntouched() {
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        window.findChild<QToolButton *>("recordingButton")->click();
        QTimer::singleShot(0, [] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box != nullptr);
            QCOMPARE(box->defaultButton(), box->button(QMessageBox::Cancel));
            box->button(QMessageBox::Cancel)->click();
        });

        QVERIFY(!window.close());
        QCOMPARE(receiver.recordingState(), RecordingState::Recording);
        QCOMPARE(receiver.discardRecordingCount, 0);
    }

    void closeCancelLeavesFinalizingUntouched() {
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        auto *recordButton = window.findChild<QToolButton *>("recordingButton");
        recordButton->click();
        recordButton->click();
        QTimer::singleShot(0, [] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box != nullptr);
            QCOMPARE(box->defaultButton(), box->button(QMessageBox::Cancel));
            box->button(QMessageBox::Cancel)->click();
        });

        QVERIFY(!window.close());
        QCOMPARE(receiver.recordingState(), RecordingState::Finalizing);
        QCOMPARE(receiver.discardRecordingCount, 0);
    }

    void finalizingCompletionDuringExitConfirmIsSuppressedBeforeDiscard() {
        ExitRaceRecordingReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        auto *recordButton = window.findChild<QToolButton *>("recordingButton");
        recordButton->click();
        recordButton->click();
        bool sawCompletionDialog = false;

        QTimer::singleShot(0, [&] {
            receiver.completeRecordingForTest(
                {"C:/recordings/committed-during-confirm.mp4", {}});
        });
        QTimer::singleShot(1, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box != nullptr);
            if (box->windowTitle() == "Recording saved") {
                sawCompletionDialog = true;
                box->button(QMessageBox::Ok)->click();
                QTimer::singleShot(0, [] {
                    auto *confirmation = qobject_cast<QMessageBox *>(
                        QApplication::activeModalWidget());
                    QVERIFY(confirmation != nullptr);
                    confirmation->button(QMessageBox::Discard)->click();
                });
                return;
            }
            box->button(QMessageBox::Discard)->click();
        });

        QVERIFY(window.close());
        QVERIFY(!sawCompletionDialog);
        QCOMPARE(receiver.discardCallsIncludingIdle, 1);
    }

    void finalizingCompletionDuringExitCancelShowsSavedResultAfterPrompt() {
        ExitRaceRecordingReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        auto *recordButton = window.findChild<QToolButton *>("recordingButton");
        recordButton->click();
        recordButton->click();
        bool sawCompletionAfterCancel = false;

        QTimer::singleShot(0, [&] {
            receiver.completeRecordingForTest(
                {"C:/recordings/finished-before-cancel.mp4", {}});
        });
        QTimer::singleShot(1, [&] {
            auto *confirmation = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget());
            QVERIFY(confirmation != nullptr);
            QCOMPARE(confirmation->windowTitle(), QString("Discard recording?"));
            confirmation->button(QMessageBox::Cancel)->click();
            QTimer::singleShot(0, [&] {
                auto *completion = qobject_cast<QMessageBox *>(
                    QApplication::activeModalWidget());
                QVERIFY(completion != nullptr);
                sawCompletionAfterCancel = completion->windowTitle() == "Recording saved";
                completion->button(QMessageBox::Ok)->click();
            });
        });

        QVERIFY(!window.close());
        QVERIFY(sawCompletionAfterCancel);
        QCOMPARE(receiver.discardCallsIncludingIdle, 0);
        QCOMPARE(receiver.recordingState(), RecordingState::Idle);
    }

    void closeConfirmSynchronouslyDiscardsRecordingAndFinalizing() {
        for (const bool finalizing : {false, true}) {
            FakeAirPlayReceiver receiver;
            receiver.setRecordingAvailableForTest(true);
            MainWindow window(AppSettings::defaults(), nullptr, &receiver);
            auto *recordButton = window.findChild<QToolButton *>("recordingButton");
            recordButton->click();
            if (finalizing) {
                recordButton->click();
            }
            QTimer::singleShot(0, [] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(box != nullptr);
                box->button(QMessageBox::Discard)->click();
            });

            QVERIFY(window.close());
            QCOMPARE(receiver.discardRecordingCount, 1);
            QCOMPARE(receiver.recordingState(), RecordingState::Idle);
            QVERIFY(QApplication::activeModalWidget() == nullptr);
        }
    }


    void leavingConnectedStateClearsVideoSurface() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        window.resize(120, 80);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *surface = window.findChild<VideoSurfaceWidget *>();
        QVERIFY(surface != nullptr);

        emit receiver.stateChanged(ReceiverState::Connected);
        QVERIFY(receiver.frameCallback() != nullptr);

        QImage redFrame(32, 32, QImage::Format_RGBA8888);
        redFrame.fill(Qt::red);
        receiver.frameCallback()(redFrame);
        QCoreApplication::processEvents();
        surface->repaint();

        QImage frameCapture = surface->grab().toImage();
        if (frameCapture.isNull())
            QSKIP("Video surface capture unavailable");
        QCOMPARE(frameCapture.pixelColor(frameCapture.width() / 2, frameCapture.height() / 2), QColor(Qt::red));

        emit receiver.stateChanged(ReceiverState::Discoverable);
        QCoreApplication::processEvents();
        surface->repaint();

        QImage resetCapture = surface->grab().toImage();
        if (resetCapture.isNull())
            QSKIP("Video surface capture unavailable");

        QVERIFY(window.isVisible());
        QVERIFY(surface->isVisible());
        QVERIFY(window.isToolbarVisible());
        QCOMPARE(resetCapture.pixelColor(resetCapture.width() / 2, resetCapture.height() / 2), QColor(Qt::white));
    }

    void stoppingConnectedReceiverClearsVideoSurface() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        window.resize(120, 80);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *surface = window.findChild<VideoSurfaceWidget *>();
        QVERIFY(surface != nullptr);

        emit receiver.stateChanged(ReceiverState::Connected);
        QVERIFY(receiver.frameCallback() != nullptr);

        QImage redFrame(32, 32, QImage::Format_RGBA8888);
        redFrame.fill(Qt::red);
        receiver.frameCallback()(redFrame);
        QCoreApplication::processEvents();
        surface->repaint();

        QImage frameCapture = surface->grab().toImage();
        if (frameCapture.isNull())
            QSKIP("Video surface capture unavailable");
        QCOMPARE(frameCapture.pixelColor(frameCapture.width() / 2, frameCapture.height() / 2), QColor(Qt::red));

        emit receiver.stateChanged(ReceiverState::Idle);
        QCoreApplication::processEvents();
        surface->repaint();

        QImage resetCapture = surface->grab().toImage();
        if (resetCapture.isNull())
            QSKIP("Video surface capture unavailable");

        QVERIFY(window.isVisible());
        QVERIFY(surface->isVisible());
        QVERIFY(window.isToolbarVisible());
        QCOMPARE(resetCapture.pixelColor(resetCapture.width() / 2, resetCapture.height() / 2), QColor(Qt::white));
    }

    void videoSurfaceDoesNotCoverVisibleOverlays() {
        if (QGuiApplication::platformName().compare("windows", Qt::CaseInsensitive) != 0) {
            QSKIP("Requires the Windows QPA platform");
        }

        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        window.resize(320, 180);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto *surface = window.findChild<VideoSurfaceWidget *>();
        auto *status = window.findChild<QLabel *>("receiverStatusLabel");
        auto *settings = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(surface != nullptr);
        QVERIFY(status != nullptr);
        QVERIFY(settings != nullptr);

        emit receiver.stateChanged(ReceiverState::Connected);
        window.toggleToolbarVisibility();
        QCoreApplication::processEvents();

        QVERIFY(window.isToolbarVisible());
        QVERIFY(status->isHidden());
        QVERIFY(settings->isVisible());

        const HWND surfaceHwnd = reinterpret_cast<HWND>(surface->winId());
        QVERIFY(surfaceHwnd != nullptr);
        QVERIFY(IsWindow(surfaceHwnd));

        const HWND toolbarTop = windowFromWidgetCenter(*settings);
        QVERIFY(toolbarTop != nullptr);
        QVERIFY(toolbarTop != surfaceHwnd);
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
        QVERIFY(label->isHidden());
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

    void failedInitialHotkeyRegistrationShowsError() {
        FakeHotkeyService hotkeys;
        hotkeys.rejectedRegistrations.append({ShortcutAction::ToggleToolbar, AppSettings::defaults().shortcutFor(ShortcutAction::ToggleToolbar)});
        MainWindow window(AppSettings::defaults(), &hotkeys);
        auto *label = window.findChild<QLabel *>("receiverStatusLabel");
        QVERIFY(label != nullptr);
        QCOMPARE(label->text(), QString("Could not register one or more shortcuts"));
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

    void shortcutAspectRatioTogglesButton() {
        FakeHotkeyService hotkeys;
        MainWindow window(AppSettings::defaults(), &hotkeys);
        auto *button = window.findChild<QToolButton *>("aspectRatioButton");
        QVERIFY(button != nullptr);
        QVERIFY(!button->isChecked());

        emit hotkeys.activated(ShortcutAction::ToggleAspectRatio);

        QVERIFY(button->isChecked());
    }

    void toolbarButtonsShowShortcutTooltips() {
        AppSettings settings = AppSettings::defaults();
        settings.setShortcut(ShortcutAction::VolumeUp, QKeySequence("Ctrl+Shift+U"));
        settings.setShortcut(ShortcutAction::VolumeDown, QKeySequence("Ctrl+Shift+D"));
        settings.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Shift+P"));
        settings.setShortcut(ShortcutAction::ToggleAspectRatio, QKeySequence("Ctrl+Shift+A"));

        MainWindow window(settings, nullptr);
        auto *volumeButton = window.findChild<QToolButton *>("volumeButton");
        auto *pinButton = window.findChild<QToolButton *>("alwaysOnTopButton");
        auto *aspectButton = window.findChild<QToolButton *>("aspectRatioButton");
        QVERIFY(volumeButton != nullptr);
        QVERIFY(pinButton != nullptr);
        QVERIFY(aspectButton != nullptr);

        const QString volumeUp = settings.shortcutFor(ShortcutAction::VolumeUp).toString(QKeySequence::NativeText);
        const QString volumeDown = settings.shortcutFor(ShortcutAction::VolumeDown).toString(QKeySequence::NativeText);
        const QString pin = settings.shortcutFor(ShortcutAction::ToggleAlwaysOnTop).toString(QKeySequence::NativeText);
        const QString aspect = settings.shortcutFor(ShortcutAction::ToggleAspectRatio).toString(QKeySequence::NativeText);

        QCOMPARE(volumeButton->toolTip(), QString("Volume: %1 / %2").arg(volumeUp, volumeDown));
        QCOMPARE(pinButton->toolTip(), QString("Pin: %1").arg(pin));
        QCOMPARE(aspectButton->toolTip(), QString("Aspect: %1").arg(aspect));
    }

    void volumeSliderUpdatesReceiver() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        auto *slider = window.findChild<QSlider *>("volumeSlider");
        QVERIFY(slider != nullptr);

        slider->setValue(40);

        QVERIFY(std::abs(receiver.volume() - std::pow(10.0, 0.05 * -18.0)) < 0.000001);
    }

    void receiverVolumeChangeUpdatesSliderWithoutFeedback() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        auto *slider = window.findChild<QSlider *>("volumeSlider");
        QVERIFY(slider != nullptr);
        QCOMPARE(receiver.volume(), 1.0);

        emit receiver.volumeChanged(std::pow(10.0, 0.05 * -15.0));

        QCOMPARE(slider->value(), 50);
        QCOMPARE(receiver.volume(), 1.0);
    }

    void receiverVolumeChangeSavesSettings() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, path);

        emit receiver.volumeChanged(std::pow(10.0, 0.05 * -15.0));

        AppSettingsStore store(path);
        QCOMPARE(store.loadOrDefaults().volume(), 50);
    }

    void appliesLoadedVolume() {
        AppSettings settings = AppSettings::defaults();
        settings.setVolume(45);
        FakeAirPlayReceiver receiver;

        MainWindow window(settings, nullptr, &receiver);

        auto *slider = window.findChild<QSlider *>("volumeSlider");
        QVERIFY(slider != nullptr);
        QCOMPARE(slider->value(), 45);
        QVERIFY(std::abs(receiver.volume() - std::pow(10.0, 0.05 * -16.5)) < 0.000001);
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

    void startupWithDefaultVolumeDoesNotOverwriteFile() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(R"({"receiverName":"AirPlay Receiver","shortcuts":{"toggleAlwaysOnTop":"Ctrl+Shift+P","volumeUp":"Ctrl+Shift+Up","volumeDown":"Ctrl+Shift+Down","toggleToolbar":"Ctrl+Shift+T","toggleAspectRatio":"Ctrl+Shift+A"},"volume":100,"aspectRatioLock":false})");
            file.close();
        }

        AppSettings settings = AppSettings::defaults();
        MainWindow window(settings, nullptr, nullptr, path);

        const AppSettings loaded = AppSettingsStore(path).loadOrDefaults();
        QCOMPARE(loaded.volume(), 100);
        QCOMPARE(loaded.receiverName(), QString("AirPlay Receiver"));
        QVERIFY(!loaded.aspectRatioLock());
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
        QVERIFY(receiver.frameCallback() != nullptr);
    }

    void videoSurfaceWidgetExists() {
        MainWindow window;
        auto *surface = window.findChild<VideoSurfaceWidget *>();
        QVERIFY(surface != nullptr);
        QCOMPARE(surface->objectName(), QString("videoSurface"));
    }

    void startsWithAspectRatioLockDisabled() {
        MainWindow window;
        auto *button = window.findChild<QToolButton *>("aspectRatioButton");
        QVERIFY(button != nullptr);
        QVERIFY(!button->isChecked());
    }

    void videoSizeStoredOnSignal() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);

        receiver.emitVideoSize(1170, 2532);

        auto *button = window.findChild<QToolButton *>("aspectRatioButton");
        button->setChecked(true);

        double expectedRatio = 1170.0 / 2532.0;
        double actualRatio = static_cast<double>(window.width()) / window.height();
        double diff = qAbs(actualRatio - expectedRatio);
        QVERIFY(diff < 0.01);
    }

    void aspectRatioLockRoundsToNearestWidth() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);

        receiver.emitVideoSize(1170, 2532);
        auto *button = window.findChild<QToolButton *>("aspectRatioButton");
        button->setChecked(true);

        QCOMPARE(window.width(), 250);
    }

    void decodedFrameSizeOverridesReportedVideoSizeForAspectLock() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);

        receiver.emitVideoSize(1170, 2532);
        auto *button = window.findChild<QToolButton *>("aspectRatioButton");
        button->setChecked(true);

        QImage frame(1920, 1080, QImage::Format_RGBA8888);
        receiver.frameCallback()(frame);

        verifyWindowAspectRatio(window, 16.0 / 9.0);
    }

    void reportedVideoSizeDoesNotOverrideKnownDecodedFrameSize() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);

        auto *button = window.findChild<QToolButton *>("aspectRatioButton");
        button->setChecked(true);

        QImage frame(1920, 1080, QImage::Format_RGBA8888);
        receiver.frameCallback()(frame);
        receiver.emitVideoSize(1170, 2532);

        verifyWindowAspectRatio(window, 16.0 / 9.0);
    }

    void decodedFrameSizeChangeReappliesAspectLock() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);

        auto *button = window.findChild<QToolButton *>("aspectRatioButton");
        button->setChecked(true);

        QImage firstFrame(1920, 1080, QImage::Format_RGBA8888);
        receiver.frameCallback()(firstFrame);

        QImage rotatedFrame(1170, 2532, QImage::Format_RGBA8888);
        receiver.frameCallback()(rotatedFrame);

        verifyWindowAspectRatio(window, 1170.0 / 2532.0);
    }

    void decodedFrameSizeClearsWhenReceiverReturnsToDiscoverable() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);

        auto *button = window.findChild<QToolButton *>("aspectRatioButton");
        button->setChecked(true);

        QImage frame(1170, 2532, QImage::Format_RGBA8888);
        receiver.frameCallback()(frame);
        verifyWindowAspectRatio(window, 1170.0 / 2532.0);

        emit receiver.stateChanged(ReceiverState::Discoverable);
        receiver.emitVideoSize(1920, 1080);

        verifyWindowAspectRatio(window, 16.0 / 9.0);
    }

    void nativeAspectSizingAdjustsPendingRightEdgeRect() {
        if (QGuiApplication::platformName().compare("windows", Qt::CaseInsensitive) != 0) {
            QSKIP("Requires the Windows QPA platform");
        }

        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        receiver.emitVideoSize(1920, 1080);
        auto *button = window.findChild<QToolButton *>("aspectRatioButton");
        button->setChecked(true);
        QCoreApplication::processEvents();

        const HWND hwnd = reinterpret_cast<HWND>(window.winId());
        RECT rect{};
        QVERIFY(GetWindowRect(hwnd, &rect));

        const LONG originalLeft = rect.left;
        const LONG originalRight = rect.right;
        const LONG originalTop = rect.top;
        const LONG originalBottom = rect.bottom;
        rect.right += 160;

        SendMessage(hwnd, WM_SIZING, WMSZ_RIGHT, reinterpret_cast<LPARAM>(&rect));

        const RECT nativeClient = nativeClientRectFor(hwnd);
        const int frameWidth = static_cast<int>(originalRight - originalLeft) - static_cast<int>(nativeClient.right - nativeClient.left);
        const int frameHeight = static_cast<int>(originalBottom - originalTop) - static_cast<int>(nativeClient.bottom - nativeClient.top);
        const int clientWidth = static_cast<int>(rect.right - rect.left) - frameWidth;
        const int clientHeight = static_cast<int>(rect.bottom - rect.top) - frameHeight;
        const double actualRatio = static_cast<double>(clientWidth) / clientHeight;

        QCOMPARE(rect.left, originalLeft);
        QCOMPARE(rect.right, originalRight + 160);
        QCOMPARE(rect.top, originalTop);
        QVERIFY(qAbs(actualRatio - (16.0 / 9.0)) < 0.01);
    }

    void nativeAspectSizingAdjustsPendingLeftEdgeRect() {
        if (QGuiApplication::platformName().compare("windows", Qt::CaseInsensitive) != 0) {
            QSKIP("Requires the Windows QPA platform");
        }

        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        receiver.emitVideoSize(1920, 1080);
        auto *button = window.findChild<QToolButton *>("aspectRatioButton");
        button->setChecked(true);
        QCoreApplication::processEvents();

        const HWND hwnd = reinterpret_cast<HWND>(window.winId());
        RECT rect{};
        QVERIFY(GetWindowRect(hwnd, &rect));

        const LONG originalLeft = rect.left;
        const LONG originalRight = rect.right;
        const LONG originalTop = rect.top;
        const LONG originalBottom = rect.bottom;
        rect.left -= 160;

        SendMessage(hwnd, WM_SIZING, WMSZ_LEFT, reinterpret_cast<LPARAM>(&rect));

        const RECT nativeClient = nativeClientRectFor(hwnd);
        const int frameWidth = static_cast<int>(originalRight - originalLeft) - static_cast<int>(nativeClient.right - nativeClient.left);
        const int frameHeight = static_cast<int>(originalBottom - originalTop) - static_cast<int>(nativeClient.bottom - nativeClient.top);
        const int clientWidth = static_cast<int>(rect.right - rect.left) - frameWidth;
        const int clientHeight = static_cast<int>(rect.bottom - rect.top) - frameHeight;
        const double actualRatio = static_cast<double>(clientWidth) / clientHeight;

        QCOMPARE(rect.left, originalLeft - 160);
        QCOMPARE(rect.right, originalRight);
        QCOMPARE(rect.top, originalTop);
        QVERIFY(qAbs(actualRatio - (16.0 / 9.0)) < 0.01);
    }

    void disablingAspectRatioLockKeepsCurrentSize() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        receiver.emitVideoSize(1170, 2532);

        auto *button = window.findChild<QToolButton *>("aspectRatioButton");
        button->setChecked(true);
        QSize lockedSize = window.size();

        button->setChecked(false);
        QCOMPARE(window.size(), lockedSize);
    }

    void aspectRatioLockDoesNotCorrectOrdinaryResizeEvents() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        receiver.emitVideoSize(1920, 1080);

        auto *button = window.findChild<QToolButton *>("aspectRatioButton");
        button->setChecked(true);

        window.resize(640, 640);
        QCoreApplication::processEvents();

        QCOMPARE(window.size(), QSize(640, 640));
    }

    void loadsAspectRatioLockFromSettings() {
        AppSettings settings = AppSettings::defaults();
        settings.setAspectRatioLock(true);
        MainWindow window(settings, nullptr);
        auto *button = window.findChild<QToolButton *>("aspectRatioButton");
        QVERIFY(button->isChecked());
    }

    void aspectRatioLockTogglePersistsToSettings() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        MainWindow window(AppSettings::defaults(), nullptr, nullptr, path);
        auto *button = window.findChild<QToolButton *>("aspectRatioButton");
        QVERIFY(button != nullptr);

        button->setChecked(true);

        AppSettingsStore store(path);
        QVERIFY(store.loadOrDefaults().aspectRatioLock());
    }

    void shortcutVideoFitTogglesButtonAndReceiver() {
        FakeHotkeyService hotkeys;
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), &hotkeys, &receiver);
        auto *button = window.findChild<QToolButton *>("videoFitButton");
        QVERIFY(button != nullptr);
        QVERIFY(!button->isChecked());
        QVERIFY(!receiver.lastVideoFitMode());

        emit hotkeys.activated(ShortcutAction::ToggleVideoFit);

        QVERIFY(button->isChecked());
        QVERIFY(receiver.lastVideoFitMode());
    }

    void toolbarVideoFitTooltipUsesDefaultShortcut() {
        MainWindow window(AppSettings::defaults(), nullptr);
        auto *button = window.findChild<QToolButton *>("videoFitButton");
        QVERIFY(button != nullptr);

        const QString shortcut = AppSettings::defaults().shortcutFor(ShortcutAction::ToggleVideoFit).toString(QKeySequence::NativeText);
        QCOMPARE(button->toolTip(), QString("Fit: %1").arg(shortcut));
    }

    void toolbarVideoFitTooltipUsesCustomizedShortcut() {
        AppSettings settings = AppSettings::defaults();
        settings.setShortcut(ShortcutAction::ToggleVideoFit, QKeySequence("Ctrl+Alt+V"));
        MainWindow window(settings, nullptr);
        auto *button = window.findChild<QToolButton *>("videoFitButton");
        QVERIFY(button != nullptr);

        const QString shortcut = settings.shortcutFor(ShortcutAction::ToggleVideoFit).toString(QKeySequence::NativeText);
        QCOMPARE(button->toolTip(), QString("Fit: %1").arg(shortcut));
    }

    void clickingVideoFitButtonCallsReceiverSetTrue() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        auto *button = window.findChild<QToolButton *>("videoFitButton");
        QVERIFY(button != nullptr);

        button->setChecked(true);

        QVERIFY(receiver.lastVideoFitMode());
    }

    void clickingVideoFitButtonCallsReceiverSetFalse() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        auto *button = window.findChild<QToolButton *>("videoFitButton");
        QVERIFY(button != nullptr);

        button->setChecked(true);
        QVERIFY(receiver.lastVideoFitMode());

        button->setChecked(false);
        QVERIFY(!receiver.lastVideoFitMode());
    }

    void loadedVideoFitModeInitializesButtonAndReceiver() {
        AppSettings settings = AppSettings::defaults();
        settings.setVideoFitMode(true);
        FakeAirPlayReceiver receiver;

        MainWindow window(settings, nullptr, &receiver);
        auto *button = window.findChild<QToolButton *>("videoFitButton");
        QVERIFY(button != nullptr);
        QVERIFY(button->isChecked());
        QVERIFY(receiver.lastVideoFitMode());
    }

    void videoFitModeTogglePersistsToSettings() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        MainWindow window(AppSettings::defaults(), nullptr, nullptr, path);
        auto *button = window.findChild<QToolButton *>("videoFitButton");
        QVERIFY(button != nullptr);

        button->setChecked(true);

        AppSettingsStore store(path);
        QVERIFY(store.loadOrDefaults().videoFitMode());
    }

    void videoFitModeToggleOffPersistsToSettings() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        AppSettings settings = AppSettings::defaults();
        settings.setVideoFitMode(true);
        MainWindow window(settings, nullptr, nullptr, path);
        auto *button = window.findChild<QToolButton *>("videoFitButton");
        QVERIFY(button != nullptr);
        QVERIFY(button->isChecked());

        button->setChecked(false);

        AppSettingsStore store(path);
        QVERIFY(!store.loadOrDefaults().videoFitMode());
    }

    void videoQualityChangedWhileIdleAppliesImmediatelyAndPersists() {
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
            auto *resCombo = dialog->findChild<QComboBox *>("videoResolutionCombo");
            QVERIFY(resCombo != nullptr);
            resCombo->setCurrentIndex(resCombo->findData(static_cast<int>(VideoResolution::P720)));
            auto *fpsCombo = dialog->findChild<QComboBox *>("videoFrameRateCombo");
            QVERIFY(fpsCombo != nullptr);
            fpsCombo->setCurrentIndex(fpsCombo->findData(static_cast<int>(VideoFrameRate::Fps60)));
            dialog->accept();
        });

        button->click();

        const VideoQualitySettings expected{VideoResolution::P720, VideoFrameRate::Fps60};
        QCOMPARE(receiver.lastAppliedVideoQuality, expected);
        QCOMPARE(AppSettingsStore(path).loadOrDefaults().videoQuality(), expected);
    }

    void videoQualityChangedWhileConnectedCanBeDeferred() {
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
            auto *resCombo = dialog->findChild<QComboBox *>("videoResolutionCombo");
            QVERIFY(resCombo != nullptr);
            resCombo->setCurrentIndex(resCombo->findData(static_cast<int>(VideoResolution::P540)));
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

        const VideoQualitySettings newQuality{VideoResolution::P540, VideoFrameRate::Fps30};
        QCOMPARE(AppSettingsStore(path).loadOrDefaults().videoQuality(), newQuality);
        QCOMPARE(receiver.lastAppliedVideoQuality, AppSettings::defaults().videoQuality());

        receiver.forceState(ReceiverState::Discoverable);

        QCOMPARE(receiver.lastAppliedVideoQuality, newQuality);
    }

    void videoQualityChangedWhileConnectedAppliesImmediatelyWithYes() {
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
            auto *fpsCombo = dialog->findChild<QComboBox *>("videoFrameRateCombo");
            QVERIFY(fpsCombo != nullptr);
            fpsCombo->setCurrentIndex(fpsCombo->findData(static_cast<int>(VideoFrameRate::Fps15)));
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

        const VideoQualitySettings expected{VideoResolution::P1080, VideoFrameRate::Fps15};
        QCOMPARE(receiver.lastAppliedVideoQuality, expected);
        QCOMPARE(receiver.stopCount, 1);
        QCOMPARE(receiver.startCount, 1);
        QCOMPARE(AppSettingsStore(path).loadOrDefaults().videoQuality(), expected);
    }

    void videoQualityApplyFailureSetsPendingAndShowsStatus() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        FakeAirPlayReceiver receiver;
        const VideoQualitySettings rejected{VideoResolution::P540, VideoFrameRate::Fps30};
        receiver.rejectedVideoQualities.append(rejected);

        MainWindow window(AppSettings::defaults(), nullptr, &receiver, path);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *resCombo = dialog->findChild<QComboBox *>("videoResolutionCombo");
            QVERIFY(resCombo != nullptr);
            resCombo->setCurrentIndex(resCombo->findData(static_cast<int>(VideoResolution::P540)));
            dialog->accept();
        });

        button->click();

        QCOMPARE(AppSettingsStore(path).loadOrDefaults().videoQuality(), rejected);
        QCOMPARE(receiver.lastAppliedVideoQuality, AppSettings::defaults().videoQuality());

        auto *label = window.findChild<QLabel *>("receiverStatusLabel");
        QVERIFY(label != nullptr);
        QCOMPARE(label->text(), QString("Could not apply video quality; will retry"));

        receiver.forceState(ReceiverState::Discoverable);

        QCOMPARE(receiver.lastAppliedVideoQuality, AppSettings::defaults().videoQuality());
        QCOMPARE(label->text(), QString("Could not apply video quality; will retry"));

        receiver.forceState(ReceiverState::Error);

        QCOMPARE(receiver.lastAppliedVideoQuality, AppSettings::defaults().videoQuality());
        QCOMPARE(label->text(), QString("Could not apply video quality; will retry"));
    }

    void appliesLoadedVideoQualityToReceiver() {
        AppSettings settings = AppSettings::defaults();
        const VideoQualitySettings customQuality{VideoResolution::P720, VideoFrameRate::Fps60};
        settings.setVideoQuality(customQuality);
        FakeAirPlayReceiver receiver;

        MainWindow window(settings, nullptr, &receiver);

        QCOMPARE(receiver.lastAppliedVideoQuality, customQuality);
    }

    void startupWithVideoFitModeTrueDoesNotSaveUnchangedSettings() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(R"({"videoFitMode":true})");
            file.close();
        }

        AppSettings settings = AppSettingsStore(path).loadOrDefaults();
        QVERIFY(settings.videoFitMode());

        QFile::remove(path);
        QVERIFY(!QFile::exists(path));

        MainWindow window(settings, nullptr, nullptr, path);

        QVERIFY(!QFile::exists(path));
    }

    void closePersistsWindowStateBesideSettingsFile() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString settingsPath = dir.filePath("airplay-settings.json");
        const QString windowStatePath = dir.filePath("airplay-window-state.dat");
        MainWindow window(AppSettings::defaults(), nullptr, nullptr, settingsPath);
        window.resize(640, 360);

        QVERIFY(window.close());

        const std::optional<WindowStateSnapshot> saved = WindowStateStore(windowStatePath).load();
        QVERIFY(saved.has_value());
        QVERIFY(!saved->geometry.isEmpty());
    }

    void constructionRestoresSavedWindowGeometry() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString settingsPath = dir.filePath("airplay-settings.json");
        const QString windowStatePath = dir.filePath("airplay-window-state.dat");
        {
            MainWindow source(AppSettings::defaults(), nullptr, nullptr, settingsPath);
            source.resize(640, 360);
            QVERIFY(WindowStateStore(windowStatePath).save({source.saveGeometry(), source.saveState()}));
        }

        MainWindow restored(AppSettings::defaults(), nullptr, nullptr, settingsPath);

        QCOMPARE(restored.size(), QSize(640, 360));
    }
};

QTEST_MAIN(MainWindowSmokeTest)
#include "MainWindowSmokeTest.moc"
