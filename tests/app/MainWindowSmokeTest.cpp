#include <QtTest/QtTest>
#include "app/AppSettings.h"
#include "app/AppSettingsStore.h"
#include "app/DiagnosticRestartCoordinator.h"
#include "app/LanguageManager.h"
#include "app/MainWindow.h"
#include "app/ToolbarVisibilityController.h"
#include <QCursor>
#include "app/SettingsDialog.h"
#include "app/ShortcutAction.h"
#include "app/VideoSurfaceWidget.h"
#include "app/WindowStateStore.h"
#include "backend/FakeAirPlayReceiver.h"
#include "backend/ReceiverState.h"
#include "platform/FakeHotkeyService.h"
#include "platform/RecordingPathActions.h"
#include "platform/DiagnosticLogFolderActions.h"

#include <QComboBox>
#include <QCheckBox>
#include <QAbstractButton>
#include <QDir>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QRegularExpression>
#include <QSignalSpy>
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
#include <QTranslator>
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

QAbstractButton *messageButton(QMessageBox *box, const QString &text) {
    for (QAbstractButton *button : box->buttons()) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

bool isAscii(const QString &text) {
    return std::all_of(text.cbegin(), text.cend(), [](QChar character) {
        return character.unicode() <= 0x7f;
    });
}

class MainWindowTranslator final : public QTranslator {
public:
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override {
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        return translations.value(QString::fromLatin1(context) + QChar('\x1f')
                                      + QString::fromLatin1(sourceText));
    }

    QHash<QString, QString> translations = {
        {QStringLiteral("MainWindow\u001fAirPlay Receiver"), QStringLiteral("AirPlay 接收器")},
        {QStringLiteral("MainWindow\u001fAirPlay Receiver [Diagnostic Logging]"), QStringLiteral("AirPlay 接收器 [诊断日志]")},
        {QStringLiteral("MainWindow\u001fReady for AirPlay"), QStringLiteral("等待 AirPlay")},
        {QStringLiteral("MainWindow\u001fConnecting"), QStringLiteral("正在连接")},
        {QStringLiteral("MainWindow\u001fConnected"), QStringLiteral("已连接")},
        {QStringLiteral("MainWindow\u001fReceiver error: %1"), QStringLiteral("接收器错误：%1")},
        {QStringLiteral("MainWindow\u001fVolume: %1 / %2"), QStringLiteral("音量：%1 / %2")},
        {QStringLiteral("MainWindow\u001fPin: %1"), QStringLiteral("置顶：%1")},
        {QStringLiteral("MainWindow\u001fAspect: %1"), QStringLiteral("比例：%1")},
        {QStringLiteral("MainWindow\u001fFit: %1"), QStringLiteral("适应：%1")},
        {QStringLiteral("MainWindow\u001fRecord: %1"), QStringLiteral("录制：%1")},
        {QStringLiteral("ToolbarWidget\u001fVolume"), QStringLiteral("音量")},
        {QStringLiteral("ToolbarWidget\u001fPin"), QStringLiteral("置顶")},
        {QStringLiteral("ToolbarWidget\u001fAspect"), QStringLiteral("比例")},
        {QStringLiteral("ToolbarWidget\u001fFit"), QStringLiteral("适应")},
        {QStringLiteral("ToolbarWidget\u001fSettings"), QStringLiteral("设置")},
        {QStringLiteral("ToolbarWidget\u001fRecord"), QStringLiteral("录制")},
        {QStringLiteral("ToolbarWidget\u001fStop"), QStringLiteral("停止")},
        {QStringLiteral("ToolbarWidget\u001fSaving..."), QStringLiteral("正在保存…")},
    };
};

class InstalledTranslator final {
public:
    explicit InstalledTranslator(QTranslator *translator)
        : translator_(translator) {
        QCoreApplication::installTranslator(translator_);
    }

    ~InstalledTranslator() {
        QCoreApplication::removeTranslator(translator_);
    }

private:
    QTranslator *translator_;
};

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

class FakeDiagnosticUiPrompts final : public DiagnosticUiPrompts {
public:
    bool confirmPrivacy(QWidget *, const QString &message) override {
        privacyMessages.append(message);
        return privacyResult;
    }

    bool confirmDiscardDraft(QWidget *, const QString &message) override {
        discardMessages.append(message);
        return discardResult;
    }

    bool confirmDisconnectMirroring(QWidget *, const QString &message) override {
        disconnectMessages.append(message);
        return disconnectResult;
    }

    void showRestartUnavailable(QWidget *, const QString &message) override {
        unavailableMessages.append(message);
    }

    void showRestartFailure(QWidget *, const QString &message) override {
        failureMessages.append(message);
    }

    bool privacyResult = true;
    bool discardResult = true;
    bool disconnectResult = true;
    QStringList privacyMessages;
    QStringList discardMessages;
    QStringList disconnectMessages;
    QStringList unavailableMessages;
    QStringList failureMessages;
};

class FakeDiagnosticLogFolderOperations {
public:
    DiagnosticLogFolderOperations operation() {
        return {
            [this](const QString &path) { createdTargets.append(path); return createError; },
            [this](const QUrl &url) { openedTargets.append(url.toLocalFile()); return openError; },
        };
    }

    QStringList createdTargets;
    QStringList openedTargets;
    QString createError;
    QString openError;
};

class DiagnosticOrderingReceiver final : public FakeAirPlayReceiver {
public:
    void stop() override {
        if (order != nullptr) {
            order->append("receiver_stop");
        }
        FakeAirPlayReceiver::stop();
    }

    QStringList *order = nullptr;
};

class AcknowledgementOrderingReceiver final : public FakeAirPlayReceiver {
public:
    RecordingStartResult startRecording(const RecordingOptions &options) override {
        events.append("start");
        return FakeAirPlayReceiver::startRecording(options);
    }

    void acknowledgeRecordingResult() override {
        events.append("ack");
        FakeAirPlayReceiver::acknowledgeRecordingResult();
    }

    QStringList events;
};

class MainWindowSmokeTest : public QObject {
    Q_OBJECT

private slots:
    void constructsWithExpectedTitle() {
        MainWindow window;
        QCOMPARE(window.windowTitle(), QString("AirPlay Receiver"));
    }

    void diagnosticLoggingTitleAndFailureAreIdempotent() {
        MainWindow window;
        QCOMPARE(window.windowTitle(), QString("AirPlay Receiver"));

        window.setDiagnosticLoggingActive(true);
        QCOMPARE(window.windowTitle(), QString("AirPlay Receiver [Diagnostic Logging]"));

        QSignalSpy stopped(&window, &MainWindow::diagnosticLoggingStopped);
        window.handleDiagnosticWriteFailure("disk full");
        window.handleDiagnosticWriteFailure("disk full");

        QCOMPARE(window.windowTitle(), QString("AirPlay Receiver"));
        QCOMPARE(stopped.count(), 1);
    }

    void diagnosticRestartPrivacyDeclineDoesNotStartChild() {
        FakeAirPlayReceiver receiver;
        FakeDiagnosticUiPrompts prompts;
        prompts.privacyResult = false;
        int starts = 0;
        DiagnosticRestartOperations operations;
        operations.startDetached = [&starts](const QString &, const QStringList &, QString *) {
            ++starts;
            return true;
        };
        operations.createToken = [] { return QStringLiteral("diagnostic-privacy-decline"); };
        DiagnosticRestartCoordinator coordinator(operations);
        MainWindowRuntimeServices services;
        services.diagnosticRestartCoordinator = &coordinator;
        services.diagnosticPrompts = &prompts;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, QString(), nullptr, nullptr, services);
        auto *settings = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(settings != nullptr);

        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            dialog->findChild<QPushButton *>("restartWithDiagnosticLoggingButton")->click();
            QTimer::singleShot(0, dialog, &QDialog::reject);
        });
        settings->click();

        QCOMPARE(prompts.privacyMessages, QStringList({
            "Diagnostic logging records application, Windows, and privacy-filtered network information. Logs stay on this computer and are never uploaded automatically."}));
        QVERIFY(isAscii(prompts.privacyMessages.front()));
        QCOMPARE(starts, 0);
        QCOMPARE(receiver.stopCount, 0);
    }

    void diagnosticRestartHandlesDraftSessionFailureAndLogFolderErrors() {
        FakeAirPlayReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        FakeDiagnosticUiPrompts prompts;
        prompts.discardResult = false;
        int starts = 0;
        DiagnosticRestartOperations operations;
        operations.startDetached = [&starts](const QString &, const QStringList &, QString *) {
            ++starts;
            return false;
        };
        operations.createToken = [] { return QStringLiteral("diagnostic-confirmations"); };
        DiagnosticRestartCoordinator coordinator(operations);
        FakeDiagnosticLogFolderOperations folderOperations;
        folderOperations.openError = "access denied";
        DiagnosticLogFolderActions folders("C:/package", folderOperations.operation());
        MainWindowRuntimeServices services;
        services.diagnosticRestartCoordinator = &coordinator;
        services.diagnosticLogFolderActions = &folders;
        services.diagnosticPrompts = &prompts;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, QString(), nullptr, nullptr, services);
        auto *settings = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(settings != nullptr);

        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            dialog->findChild<QLineEdit *>("receiverNameEdit")->setText("Unapplied draft");
            dialog->findChild<QPushButton *>("restartWithDiagnosticLoggingButton")->click();
            QVERIFY(dialog->hasUnappliedChanges());
            prompts.discardResult = true;
            dialog->findChild<QPushButton *>("restartWithDiagnosticLoggingButton")->click();
            dialog->findChild<QPushButton *>("openDiagnosticLogFolderButton")->click();
            auto *summary = dialog->findChild<QLabel *>("settingsApplySummary");
            QVERIFY(summary != nullptr);
            QVERIFY(summary->text().contains("access denied"));
            QTimer::singleShot(0, dialog, &QDialog::reject);
        });
        settings->click();

        QCOMPARE(prompts.discardMessages, QStringList({
            "Settings has unapplied changes. Restarting will discard them. Continue?",
            "Settings has unapplied changes. Restarting will discard them. Continue?"}));
        QCOMPARE(prompts.disconnectMessages, QStringList({
            "Restarting disconnects the current mirroring session. Continue?"}));
        QCOMPARE(starts, 1);
        QCOMPARE(prompts.failureMessages, QStringList({
            "Diagnostic restart could not start the child process."}));
        QCOMPARE(receiver.stopCount, 0);
        QCOMPARE(folderOperations.createdTargets, QStringList({"C:/package/logs"}));
        QCOMPARE(folderOperations.openedTargets, QStringList({"C:/package/logs"}));
    }

    void diagnosticRestartRejectsActiveRecordingAndDisablesActiveDiagnosticMode() {
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        QVERIFY(receiver.startRecording({"C:/recordings", RecordingFormat::Mp4}).accepted);
        FakeDiagnosticUiPrompts prompts;
        int starts = 0;
        DiagnosticRestartOperations operations;
        operations.startDetached = [&starts](const QString &, const QStringList &, QString *) {
            ++starts;
            return true;
        };
        operations.createToken = [] { return QStringLiteral("diagnostic-recording-active"); };
        DiagnosticRestartCoordinator coordinator(operations);
        MainWindowRuntimeServices services;
        services.diagnosticRestartCoordinator = &coordinator;
        services.diagnosticPrompts = &prompts;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, QString(), nullptr, nullptr, services);
        auto *settings = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(settings != nullptr);

        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            dialog->findChild<QPushButton *>("restartWithDiagnosticLoggingButton")->click();
            QTimer::singleShot(0, dialog, &QDialog::reject);
        });
        settings->click();

        QCOMPARE(prompts.unavailableMessages, QStringList({
            "Diagnostic restart is unavailable while a recording is active or being finalized. Finish or discard the recording, then try again."}));
        QCOMPARE(starts, 0);

        window.setDiagnosticLoggingActive(true);
        QCOMPARE(window.windowTitle(), QString("AirPlay Receiver [Diagnostic Logging]"));
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            QVERIFY(!dialog->findChild<QPushButton *>("restartWithDiagnosticLoggingButton")->isEnabled());
            dialog->reject();
        });
        settings->click();
    }

    void diagnosticRestartRejectsFinalizingRecordingBeforePrivacy() {
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        QVERIFY(receiver.startRecording({"C:/recordings", RecordingFormat::Mp4}).accepted);
        receiver.stopRecording();
        QCOMPARE(receiver.recordingState(), RecordingState::Finalizing);
        FakeDiagnosticUiPrompts prompts;
        int starts = 0;
        DiagnosticRestartOperations operations;
        operations.startDetached = [&starts](const QString &, const QStringList &, QString *) {
            ++starts;
            return true;
        };
        operations.createToken = [] { return QStringLiteral("diagnostic-finalizing"); };
        DiagnosticRestartCoordinator coordinator(operations);
        MainWindowRuntimeServices services;
        services.diagnosticRestartCoordinator = &coordinator;
        services.diagnosticPrompts = &prompts;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, QString(), nullptr, nullptr, services);
        auto *settings = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(settings != nullptr);

        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            dialog->findChild<QPushButton *>("restartWithDiagnosticLoggingButton")->click();
            QTimer::singleShot(0, dialog, &QDialog::reject);
        });
        settings->click();

        QCOMPARE(starts, 0);
        QCOMPARE(prompts.privacyMessages.count(), 0);
        QCOMPARE(prompts.unavailableMessages, QStringList({
            "Diagnostic restart is unavailable while a recording is active or being finalized. Finish or discard the recording, then try again."}));
        QVERIFY(isAscii(prompts.unavailableMessages.front()));
    }

    void diagnosticRestartMirrorDeclineKeepsDraftAndParentOpen() {
        FakeAirPlayReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        FakeDiagnosticUiPrompts prompts;
        prompts.disconnectResult = false;
        int starts = 0;
        DiagnosticRestartOperations operations;
        operations.startDetached = [&starts](const QString &, const QStringList &, QString *) {
            ++starts;
            return true;
        };
        operations.createToken = [] { return QStringLiteral("diagnostic-mirror-decline"); };
        DiagnosticRestartCoordinator coordinator(operations);
        MainWindowRuntimeServices services;
        services.diagnosticRestartCoordinator = &coordinator;
        services.diagnosticPrompts = &prompts;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, QString(), nullptr, nullptr, services);
        window.show();
        QCoreApplication::processEvents();
        auto *settings = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(settings != nullptr);

        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            dialog->findChild<QLineEdit *>("receiverNameEdit")->setText("Keep this draft");
            dialog->findChild<QPushButton *>("restartWithDiagnosticLoggingButton")->click();
            QVERIFY(!dialog->isHidden());
            QVERIFY(dialog->hasUnappliedChanges());
            QTimer::singleShot(0, dialog, &QDialog::reject);
        });
        settings->click();

        QCOMPARE(starts, 0);
        QCOMPARE(receiver.stopCount, 0);
        QVERIFY(window.isVisible());
        QCOMPARE(prompts.disconnectMessages, QStringList({
            "Restarting disconnects the current mirroring session. Continue?"}));
        QVERIFY(isAscii(prompts.disconnectMessages.front()));
    }

    void diagnosticChildStartFailureKeepsVisibleParentAndReportsExactError() {
        FakeAirPlayReceiver receiver;
        FakeDiagnosticUiPrompts prompts;
        DiagnosticRestartOperations operations;
        operations.startDetached = [](const QString &, const QStringList &, QString *) { return false; };
        operations.createToken = [] { return QStringLiteral("diagnostic-start-failure"); };
        DiagnosticRestartCoordinator coordinator(operations);
        MainWindowRuntimeServices services;
        services.diagnosticRestartCoordinator = &coordinator;
        services.diagnosticPrompts = &prompts;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, QString(), nullptr, nullptr, services);
        window.show();
        QCoreApplication::processEvents();
        auto *settings = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(settings != nullptr);

        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            dialog->findChild<QPushButton *>("restartWithDiagnosticLoggingButton")->click();
            QTimer::singleShot(0, dialog, &QDialog::reject);
        });
        settings->click();

        QCOMPARE(prompts.failureMessages, QStringList({
            "Diagnostic restart could not start the child process."}));
        QVERIFY(isAscii(prompts.failureMessages.front()));
        QCOMPARE(receiver.stopCount, 0);
        QVERIFY(window.isVisible());
    }

    void diagnosticChildReadyStopsReceiverAfterReady() {
        DiagnosticOrderingReceiver receiver;
        FakeDiagnosticUiPrompts prompts;
        int starts = 0;
        QStringList order;
        receiver.order = &order;
        DiagnosticRestartOperations operations;
        operations.startDetached = [&starts](const QString &, const QStringList &, QString *) {
            ++starts;
            return true;
        };
        operations.createToken = [] { return QStringLiteral("diagnostic-child-ready"); };
        DiagnosticRestartCoordinator coordinator(operations);
        connect(&coordinator, &DiagnosticRestartCoordinator::childReady,
                [&order] { order.append("child_ready"); });
        MainWindowRuntimeServices services;
        services.diagnosticRestartCoordinator = &coordinator;
        services.diagnosticPrompts = &prompts;
        services.quitApplication = [&order] { order.append("quit"); };
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, QString(), nullptr, nullptr, services);
        auto *settings = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(settings != nullptr);

        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            dialog->findChild<QPushButton *>("restartWithDiagnosticLoggingButton")->click();
            coordinator.childReady();
        });
        settings->click();

        QCOMPARE(starts, 1);
        QCOMPARE(receiver.stopCount, 1);
        QCOMPARE(order, QStringList({"child_ready", "receiver_stop", "quit"}));
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
        QCOMPARE(status->text(), QString("Could not start recording: Output folder unavailable"));
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
        QCOMPARE(receiver.acknowledgeRecordingResultCount, 1);
    }

    void completionAcknowledgesOldResultBeforeNestedHotkeyStartsNextSession() {
        AppSettings settings = AppSettings::defaults();
        settings.setShowRecordingCompletionMessage(true);
        AcknowledgementOrderingReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        FakeHotkeyService hotkeys;
        MainWindow window(settings, &hotkeys, &receiver);

        emit hotkeys.activated(ShortcutAction::ToggleRecording);
        emit hotkeys.activated(ShortcutAction::ToggleRecording);
        QTimer::singleShot(0, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(box != nullptr);
            emit hotkeys.activated(ShortcutAction::ToggleRecording);
            box->button(QMessageBox::Ok)->click();
        });

        receiver.completeRecordingForTest({"C:/recordings/first.mp4", {}});

        QCOMPARE(receiver.events, QStringList({"start", "ack", "start"}));
        QCOMPARE(receiver.recordingState(), RecordingState::Recording);
        receiver.discardRecording();
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
        QCOMPARE(receiver.acknowledgeRecordingResultCount, 1);

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
        QCOMPARE(receiver.acknowledgeRecordingResultCount, 2);

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
        QCOMPARE(receiver.acknowledgeRecordingResultCount, 2);
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
                messageButton(box, "Disconnect and apply now")->click();
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
                messageButton(box, "Disconnect and apply now")->click();
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
        QCOMPARE(receiver.acknowledgeRecordingResultCount, 0);
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
        QCOMPARE(receiver.acknowledgeRecordingResultCount, 1);
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
        hotkeys.reject(ShortcutAction::ToggleToolbar,
                       AppSettings::defaults().shortcutFor(ShortcutAction::ToggleToolbar),
                       1409, "Hot key is already registered.");
        MainWindow window(AppSettings::defaults(), &hotkeys);
        auto *label = window.findChild<QLabel *>("receiverStatusLabel");
        QVERIFY(label != nullptr);
        QCOMPARE(label->text(), QString("Could not register shortcuts: Toggle toolbar (Hot key is already registered. [1409])"));
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
                auto *yes = messageButton(box, "Disconnect and apply now");
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
                auto *no = messageButton(box, "Apply after disconnect");
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
                auto *no = messageButton(box, "Apply after disconnect");
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
                auto *yes = messageButton(box, "Disconnect and apply now");
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
                auto *no = messageButton(box, "Apply after disconnect");
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
                auto *no = messageButton(box, "Apply after disconnect");
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
                auto *no = messageButton(box, "Apply after disconnect");
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
            QTimer::singleShot(0, [] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(box != nullptr);
                auto *defer = messageButton(box, "Apply after disconnect");
                QVERIFY(defer != nullptr);
                defer->click();
            });
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
            QTimer::singleShot(0, dialog, &QDialog::reject);
        });

        button->click();

        QCOMPARE(receiver.receiverName(), QString("AirPlay Receiver"));
        QCOMPARE(AppSettingsStore(path).loadOrDefaults().receiverName(), QString("AirPlay Receiver"));

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
            QTimer::singleShot(0, dialog, &QDialog::reject);
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

    void shortcutSwapFromSettingsDialogPersistsAndUpdatesTooltips() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("settings.json");
        const AppSettings baseline = AppSettings::defaults();
        FakeHotkeyService hotkeys;
        MainWindow window(baseline, &hotkeys, nullptr, path);
        auto *settingsButton = window.findChild<QToolButton *>("settingsButton");
        auto *volumeButton = window.findChild<QToolButton *>("volumeButton");
        QVERIFY(settingsButton != nullptr);
        QVERIFY(volumeButton != nullptr);

        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *toolbarShortcut = dialog->findChild<QKeySequenceEdit *>(
                "shortcutEdit_toggleToolbar");
            auto *volumeUpShortcut = dialog->findChild<QKeySequenceEdit *>("shortcutEdit_volumeUp");
            QVERIFY(toolbarShortcut != nullptr);
            QVERIFY(volumeUpShortcut != nullptr);
            toolbarShortcut->setKeySequence(baseline.shortcutFor(ShortcutAction::VolumeUp));
            volumeUpShortcut->setKeySequence(baseline.shortcutFor(ShortcutAction::ToggleToolbar));
            dialog->accept();
        });

        settingsButton->click();

        const AppSettings saved = AppSettingsStore(path).loadOrDefaults();
        QCOMPARE(saved.shortcutFor(ShortcutAction::ToggleToolbar),
                 baseline.shortcutFor(ShortcutAction::VolumeUp));
        QCOMPARE(saved.shortcutFor(ShortcutAction::VolumeUp),
                 baseline.shortcutFor(ShortcutAction::ToggleToolbar));
        QCOMPARE(volumeButton->toolTip(), QString("Volume: %1 / %2").arg(
            baseline.shortcutFor(ShortcutAction::ToggleToolbar).toString(QKeySequence::NativeText),
            baseline.shortcutFor(ShortcutAction::VolumeDown).toString(QKeySequence::NativeText)));
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
            QTimer::singleShot(0, dialog, &QDialog::reject);
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
        QCOMPARE(label->text(), QString("Receiver error: Pairing failed"));

        emit receiver.stateChanged(ReceiverState::Error);
        QCOMPARE(label->text(), QString("Receiver error: Pairing failed"));
    }

    void errorStateWithoutMessageShowsReadyStatus() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        auto *label = window.findChild<QLabel *>("receiverStatusLabel");
        QVERIFY(label != nullptr);

        emit receiver.stateChanged(ReceiverState::Error);

        QCOMPARE(label->text(), QString("Ready for AirPlay"));
    }

    void languageChangeRetranslatesStatusAndTooltipsWithoutChangingReceiverState() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        auto *status = window.findChild<QLabel *>("receiverStatusLabel");
        auto *fit = window.findChild<QToolButton *>("videoFitButton");
        auto *recording = window.findChild<QToolButton *>("recordingButton");
        QVERIFY(status != nullptr);
        QVERIFY(fit != nullptr);
        QVERIFY(recording != nullptr);

        receiver.forceState(ReceiverState::Connected);
        receiver.setRecordingAvailableForTest(true);
        recording->click();
        QCOMPARE(receiver.recordingState(), RecordingState::Recording);
        const int startsBeforeLanguageChange = receiver.startCount;
        const int stopsBeforeLanguageChange = receiver.stopCount;
        const QString receiverName = receiver.receiverName();

        emit receiver.errorChanged("Pairing failed");
        MainWindowTranslator translator;
        const InstalledTranslator installedTranslator(&translator);
        QEvent languageChange(QEvent::LanguageChange);
        QCoreApplication::sendEvent(&window, &languageChange);

        QCOMPARE(window.windowTitle(), QString("AirPlay Receiver"));
        QCOMPARE(status->text(), QString("接收器错误：Pairing failed"));
        QVERIFY(fit->toolTip().startsWith(QString("适应：")));
        QCOMPARE(recording->text(), QString("停止"));
        QVERIFY(recording->isChecked());
        QVERIFY(recording->isEnabled());
        QCOMPARE(receiver.recordingState(), RecordingState::Recording);
        QCOMPARE(receiver.startCount, startsBeforeLanguageChange);
        QCOMPARE(receiver.stopCount, stopsBeforeLanguageChange);
        QCOMPARE(receiver.receiverName(), receiverName);
    }

    void applyingLanguageSettingPersistsBeforeChangingLanguageManager() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LanguageManager languageManager(QCoreApplication::instance(), nullptr,
                                        [] { return std::make_unique<QTranslator>(); },
                                        [](QTranslator *, const QString &) { return true; });
        QVERIFY(languageManager.apply("en"));

        MainWindowRuntimeServices services;
        services.languageManager = &languageManager;
        MainWindow window(AppSettings::defaults(), nullptr, nullptr,
                          directory.filePath("settings.json"), nullptr, nullptr, services);
        auto *settingsButton = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(settingsButton != nullptr);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *language = dialog->findChild<QComboBox *>("languageCombo");
            QVERIFY(language != nullptr);
            language->setCurrentIndex(language->findData("zh-CN"));
            dialog->accept();
        });
        settingsButton->click();

        QCOMPARE(languageManager.selection(), QString("zh-CN"));
        QCOMPARE(languageManager.effectiveLanguage(), QString("zh-CN"));
        QCOMPARE(AppSettingsStore(directory.filePath("settings.json")).loadOrDefaults().language(),
                 QString("zh-CN"));
    }

    void failedLanguageSettingSaveDoesNotChangeLanguageManager() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LanguageManager languageManager(QCoreApplication::instance(), nullptr,
                                        [] { return std::make_unique<QTranslator>(); },
                                        [](QTranslator *, const QString &) { return true; });
        QVERIFY(languageManager.apply("en"));

        MainWindowRuntimeServices services;
        services.languageManager = &languageManager;
        MainWindow window(AppSettings::defaults(), nullptr, nullptr, directory.path(),
                          nullptr, nullptr, services);
        auto *settingsButton = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(settingsButton != nullptr);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *language = dialog->findChild<QComboBox *>("languageCombo");
            QVERIFY(language != nullptr);
            language->setCurrentIndex(language->findData("zh-CN"));
            dialog->accept();
            QTimer::singleShot(0, dialog, &QDialog::reject);
        });
        settingsButton->click();

        QCOMPARE(languageManager.selection(), QString("en"));
        QCOMPARE(languageManager.effectiveLanguage(), QString("en"));
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

    void hotkeyFailureDoesNotBlockSixtyFpsApply() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString path = dir.filePath("settings.json");
        FakeHotkeyService hotkeys;
        const QKeySequence rejected = AppSettings::defaults().shortcutFor(ShortcutAction::VolumeUp);
        hotkeys.reject(ShortcutAction::VolumeUp, rejected, 1409,
                       "The requested hotkey is already registered.");
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), &hotkeys, &receiver, path);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *fpsCombo = dialog->findChild<QComboBox *>("videoFrameRateCombo");
            QVERIFY(fpsCombo != nullptr);
            fpsCombo->setCurrentIndex(fpsCombo->findData(static_cast<int>(VideoFrameRate::Fps60)));
            dialog->accept();

            QTimer::singleShot(0, dialog, &QDialog::reject);
            QVERIFY(dialog->isVisible());
            auto *status = dialog->findChild<QLabel *>("shortcutError_volumeUp");
            QVERIFY(status != nullptr);
            QVERIFY(status->text().contains("Volume up"));
            QVERIFY(status->text().contains("Ctrl+Alt+Up"));
            QVERIFY(status->text().contains("1409"));
        });

        button->click();

        QCOMPARE(AppSettingsStore(path).loadOrDefaults().videoQuality().frameRate,
                 VideoFrameRate::Fps60);
        QCOMPARE(receiver.lastAppliedVideoQuality.frameRate, VideoFrameRate::Fps60);
    }

    void activeSessionPromptUsesExplicitActionLabels() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        receiver.forceState(ReceiverState::Connected);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *fpsCombo = dialog->findChild<QComboBox *>("videoFrameRateCombo");
            QVERIFY(fpsCombo != nullptr);
            fpsCombo->setCurrentIndex(fpsCombo->findData(static_cast<int>(VideoFrameRate::Fps15)));
            QTimer::singleShot(0, [dialog] {
                auto *prompt = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(prompt != nullptr);
                QVERIFY(messageButton(prompt, "Disconnect and apply now") != nullptr);
                QVERIFY(messageButton(prompt, "Apply after disconnect") != nullptr);
                auto *cancel = prompt->button(QMessageBox::Cancel);
                QVERIFY(cancel != nullptr);
                QTimer::singleShot(0, dialog, &QDialog::reject);
                cancel->click();
            });
            dialog->accept();
        });

        button->click();

        QCOMPARE(receiver.lastAppliedVideoQuality, AppSettings::defaults().videoQuality());
    }

    void invalidReceiverDraftDoesNotPrompt() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        receiver.forceState(ReceiverState::Connected);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *name = dialog->findChild<QLineEdit *>("receiverNameEdit");
            QVERIFY(name != nullptr);
            name->clear();
            dialog->accept();

            QTimer::singleShot(0, dialog, &QDialog::reject);
            QCOMPARE(QApplication::activeModalWidget(), static_cast<QWidget *>(dialog));
        });

        button->click();
        QCOMPARE(receiver.receiverName(), AppSettings::defaults().receiverName());
    }

    void combinedNameAndQualityApplyRestartsOnce() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FakeAirPlayReceiver receiver;
        receiver.forceState(ReceiverState::Discoverable);
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, dir.filePath("settings.json"));
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            dialog->findChild<QLineEdit *>("receiverNameEdit")->setText("Desk Receiver");
            auto *fpsCombo = dialog->findChild<QComboBox *>("videoFrameRateCombo");
            QVERIFY(fpsCombo != nullptr);
            fpsCombo->setCurrentIndex(fpsCombo->findData(static_cast<int>(VideoFrameRate::Fps60)));
            dialog->accept();
        });

        button->click();

        QCOMPARE(receiver.configurationBatchCount, 1);
        QCOMPARE(receiver.broadcastRestartCount, 1);
        QCOMPARE(receiver.receiverName(), QString("Desk Receiver"));
        QCOMPARE(receiver.lastAppliedVideoQuality.frameRate, VideoFrameRate::Fps60);
    }

    void partialShortcutSuccessUpdatesToolbarTooltipImmediately() {
        FakeHotkeyService hotkeys;
        hotkeys.reject(ShortcutAction::VolumeUp,
                       AppSettings::defaults().shortcutFor(ShortcutAction::VolumeUp));
        MainWindow window(AppSettings::defaults(), &hotkeys);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        auto *fitButton = window.findChild<QToolButton *>("videoFitButton");
        QVERIFY(button != nullptr);
        QVERIFY(fitButton != nullptr);

        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *fitShortcut = dialog->findChild<QKeySequenceEdit *>("shortcutEdit_toggleVideoFit");
            QVERIFY(fitShortcut != nullptr);
            fitShortcut->setKeySequence(QKeySequence("Ctrl+Shift+F"));
            dialog->accept();

            QTimer::singleShot(0, dialog, &QDialog::reject);
            QCOMPARE(fitButton->toolTip(), QString("Fit: %1").arg(
                QKeySequence("Ctrl+Shift+F").toString(QKeySequence::NativeText)));
        });

        button->click();
    }

    void partialSuccessThenCancelKeepsCommittedJsonAndRuntime() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FakeHotkeyService hotkeys;
        hotkeys.reject(ShortcutAction::VolumeUp,
                       AppSettings::defaults().shortcutFor(ShortcutAction::VolumeUp));
        FakeAirPlayReceiver receiver;
        const QString path = dir.filePath("settings.json");
        MainWindow window(AppSettings::defaults(), &hotkeys, &receiver, path);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *fpsCombo = dialog->findChild<QComboBox *>("videoFrameRateCombo");
            QVERIFY(fpsCombo != nullptr);
            fpsCombo->setCurrentIndex(fpsCombo->findData(static_cast<int>(VideoFrameRate::Fps60)));
            dialog->accept();
            QTimer::singleShot(0, dialog, &QDialog::reject);
        });

        button->click();

        QCOMPARE(AppSettingsStore(path).loadOrDefaults().videoQuality().frameRate,
                 VideoFrameRate::Fps60);
        QCOMPARE(receiver.lastAppliedVideoQuality.frameRate, VideoFrameRate::Fps60);
    }

    void settingsApplyIsSafeAfterHotkeyAndReceiverAreDestroyed() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto *hotkeys = new FakeHotkeyService;
        auto *receiver = new FakeAirPlayReceiver;
        MainWindow window(AppSettings::defaults(), hotkeys, receiver, dir.filePath("settings.json"));
        delete hotkeys;
        delete receiver;

        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *shortcut = dialog->findChild<QKeySequenceEdit *>("shortcutEdit_toggleVideoFit");
            auto *frameRate = dialog->findChild<QComboBox *>("videoFrameRateCombo");
            QVERIFY(shortcut != nullptr);
            QVERIFY(frameRate != nullptr);
            shortcut->setKeySequence(QKeySequence("Ctrl+Shift+F"));
            frameRate->setCurrentIndex(frameRate->findData(static_cast<int>(VideoFrameRate::Fps60)));
            dialog->accept();

            QTimer::singleShot(0, dialog, &QDialog::reject);
            auto *summary = dialog->findChild<QLabel *>("settingsApplySummary");
            QVERIFY(summary != nullptr);
            QVERIFY(summary->isVisible());
        });

        button->click();

        QCOMPARE(AppSettingsStore(dir.filePath("settings.json")).loadOrDefaults()
                     .shortcutFor(ShortcutAction::ToggleVideoFit),
                 QKeySequence("Ctrl+Shift+F"));
    }

    void deferredReceiverFailureShowsOneModalAndRollsBackJson() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("settings.json");
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, path);
        receiver.forceState(ReceiverState::Connected);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *resolution = dialog->findChild<QComboBox *>("videoResolutionCombo");
            QVERIFY(resolution != nullptr);
            resolution->setCurrentIndex(resolution->findData(static_cast<int>(VideoResolution::P540)));
            QTimer::singleShot(0, [] {
                auto *prompt = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(prompt != nullptr);
                auto *defer = messageButton(prompt, "Apply after disconnect");
                QVERIFY(defer != nullptr);
                defer->click();
            });
            dialog->accept();
        });
        button->click();

        QCOMPARE(AppSettingsStore(path).loadOrDefaults().videoQuality().resolution, VideoResolution::P540);
        receiver.rejectedVideoQualities.append({VideoResolution::P540, VideoFrameRate::Fps30});
        int modalCount = 0;
        QTimer::singleShot(0, [&] {
            auto *failure = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(failure != nullptr);
            ++modalCount;
            const QString message = failure->text();
            failure->button(QMessageBox::Ok)->click();
            QVERIFY(message.contains("Resolution"));
            QVERIFY(message.contains("540p"));
            QVERIFY(message.contains("Requested video quality is rejected"));
        });
        receiver.forceState(ReceiverState::Discoverable);

        QCOMPARE(modalCount, 1);
        QCOMPARE(AppSettingsStore(path).loadOrDefaults().videoQuality(), AppSettings::defaults().videoQuality());
        QCOMPARE(receiver.lastAppliedVideoQuality, AppSettings::defaults().videoQuality());
    }

    void deferredRecoveryFailureModalDoesNotClaimRollbackSucceeded() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, dir.filePath("settings.json"));
        receiver.forceState(ReceiverState::Connected);
        auto *button = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(button != nullptr);

        QTimer::singleShot(0, [] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog != nullptr);
            auto *resolution = dialog->findChild<QComboBox *>("videoResolutionCombo");
            QVERIFY(resolution != nullptr);
            resolution->setCurrentIndex(resolution->findData(static_cast<int>(VideoResolution::P540)));
            QTimer::singleShot(0, [] {
                auto *prompt = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                QVERIFY(prompt != nullptr);
                messageButton(prompt, "Apply after disconnect")->click();
            });
            dialog->accept();
        });
        button->click();

        receiver.requestedConfigurationRestartError = "requested restart failed";
        receiver.rollbackConfigurationRestartError = "rollback restart failed";
        QTimer::singleShot(0, [&] {
            auto *failure = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            QVERIFY(failure != nullptr);
            const QString message = failure->text();
            failure->button(QMessageBox::Ok)->click();
            QVERIFY(message.contains("Receiver restoration failed: rollback restart failed"));
            QVERIFY(!message.contains("Recovery failed: Receiver restoration failed"));
            QVERIFY(!message.contains("Rollback succeeded"));
        });
        receiver.forceState(ReceiverState::Discoverable);
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
                auto *no = messageButton(box, "Apply after disconnect");
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
                auto *yes = messageButton(box, "Disconnect and apply now");
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
            QTimer::singleShot(0, dialog, &QDialog::reject);
        });

        button->click();

        QCOMPARE(AppSettingsStore(path).loadOrDefaults().videoQuality(), AppSettings::defaults().videoQuality());
        QCOMPARE(receiver.lastAppliedVideoQuality, AppSettings::defaults().videoQuality());
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

    void manualToolbarToggleActsOnVisibleState_data() {
        QTest::addColumn<bool>("temporaryVisible");
        QTest::newRow("hidden-with-cursor-at-top") << false;
        QTest::newRow("temporary-with-cursor-outside") << true;
    }
    void manualToolbarToggleActsOnVisibleState() {
        QFETCH(bool, temporaryVisible);
        MainWindow window;
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        auto *controller = window.findChild<ToolbarVisibilityController *>();
        QVERIFY(controller);
        const QPoint top = window.centralWidget()->mapToGlobal(QPoint(5, 1));
        const QPoint away = window.centralWidget()->mapToGlobal(QPoint(5, 150));
        controller->receiverStateChanged(ReceiverState::Connected);
        if (temporaryVisible) controller->evaluatePointer(top, true);
        QCOMPARE(window.isToolbarVisible(), temporaryVisible);
        // Set the native cursor without dispatching move events before the hotkey.
        QCursor::setPos(temporaryVisible ? away : top);
        window.toggleToolbarVisibility();
        QCOMPARE(window.isToolbarVisible(), !temporaryVisible);
        controller->evaluatePointer(away, true);
        QCOMPARE(window.isToolbarVisible(), !temporaryVisible);
    }
    void toolbarPolicySurvivesFullscreenAndReceiverDuplicates() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        window.show();
        auto *controller = window.findChild<ToolbarVisibilityController *>();
        QVERIFY(controller);
        emit receiver.stateChanged(ReceiverState::Connected);
        QVERIFY(!window.isToolbarVisible());
        const QPoint top = window.centralWidget()->mapToGlobal(QPoint(5, 1));
        QCursor::setPos(top);
        controller->evaluatePointer(top, true);
        QVERIFY(window.isToolbarVisible());
        window.setFullscreenEnabled(true);
        QVERIFY(window.isToolbarVisible());
        window.setFullscreenEnabled(false);
        QVERIFY(window.isToolbarVisible());
        controller->evaluatePointer(window.centralWidget()->mapToGlobal(QPoint(5, 150)), true);
        QVERIFY(!window.isToolbarVisible());
        window.toggleToolbarVisibility();
        emit receiver.stateChanged(ReceiverState::Connected);
        QVERIFY(window.isToolbarVisible());
        window.setFullscreenEnabled(true);
        controller->evaluatePointer(QPoint(-9999, -9999), false);
        QVERIFY(window.isToolbarVisible());
        window.toggleToolbarVisibility();
        emit receiver.stateChanged(ReceiverState::Discoverable);
        QVERIFY(!window.isFullScreen());
        QVERIFY(window.isToolbarVisible());
        QVERIFY(!window.findChild<QLabel *>("receiverStatusLabel")->isHidden());
    }

    void applyingHoverPreferenceClearsTemporaryVisibility() {
        MainWindow window;
        window.show();
        auto *controller = window.findChild<ToolbarVisibilityController *>();
        QVERIFY(controller);
        window.toggleToolbarVisibility();
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<SettingsDialog *>();
            QVERIFY(dialog);
            auto *checkbox = dialog->findChild<QCheckBox *>("toolbarHoverRevealCheckBox");
            QVERIFY(checkbox);
            checkbox->setChecked(false);
            QTimer::singleShot(2000, dialog, &QDialog::reject);
            controller->evaluatePointer(QPoint(-9999, -9999), true);
            // Deterministic seam creates the reveal immediately before committing;
            // modal activation must not mask a missing preference forwarding call.
            controller->evaluatePointer(window.centralWidget()->mapToGlobal(QPoint(5, 1)), true);
            QVERIFY(window.isToolbarVisible());
            AppSettings draft = AppSettings::defaults();
            draft.setToolbarHoverReveal(false);
            emit dialog->applyRequested(draft);
            QVERIFY(!window.isToolbarVisible());
            controller->evaluatePointer(window.centralWidget()->mapToGlobal(QPoint(5, 1)), true);
            QVERIFY(!window.isToolbarVisible());
            dialog->reject();
        });
        window.findChild<QToolButton *>("settingsButton")->click();
    }

    void nativeToolbarCursorFallbackAndPopupFocus() {
        if (QGuiApplication::platformName() != QStringLiteral("windows")) QSKIP("Windows QPA required");
        MainWindow window;
        window.setGeometry(100, 100, 900, 500);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        auto *controller = window.findChild<ToolbarVisibilityController *>();
        QVERIFY(controller);
        auto *surface = window.findChild<VideoSurfaceWidget *>();
        QVERIFY(surface);
        QVERIFY(surface->testAttribute(Qt::WA_NativeWindow));
        QCursor::setPos(window.centralWidget()->mapToGlobal(QPoint(5, 150)));
        window.toggleToolbarVisibility();
        QVERIFY(!window.isToolbarVisible());
        QCursor::setPos(window.centralWidget()->mapToGlobal(QPoint(5, 1)));
        QTRY_VERIFY(window.isToolbarVisible());
        QCursor::setPos(window.centralWidget()->mapToGlobal(QPoint(5, 150)));
        QTRY_VERIFY(!window.isToolbarVisible());
        QCursor::setPos(window.centralWidget()->mapToGlobal(QPoint(5, 1)));
        QTRY_VERIFY(window.isToolbarVisible());
        window.setFullscreenEnabled(true);
        QTest::qWait(70);
        QVERIFY(window.isToolbarVisible());
        window.setFullscreenEnabled(false);
        QTest::qWait(70);
        QVERIFY(window.isToolbarVisible());
        auto *toolbar = window.findChild<QToolButton *>("settingsButton")->parentWidget();
        QMenu popup(toolbar);
        popup.addAction("Owned popup");
        const QPoint popupOrigin = window.centralWidget()->mapToGlobal(QPoint(200, 100));
        // Qt 6.11's QMenu transient-parent setup warns for the intentionally
        // native child overlay. Expect only this diagnostic for this fixture.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(
            R"(^QWidgetWindow\(0x[0-9a-fA-F]+, name="ToolbarWidgetClassWindow"\) must be a top level window\.$)"));
        popup.popup(popupOrigin);
        QCursor::setPos(popupOrigin + QPoint(5, 5));
        controller->evaluatePointer(QCursor::pos(), true);
        QTest::qWait(70);
        QVERIFY(window.isToolbarVisible());
        popup.hide();
        QTRY_VERIFY(!window.isToolbarVisible());
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QCursor::setPos(window.centralWidget()->mapToGlobal(QPoint(5, 1)));
        QTRY_VERIFY(window.isToolbarVisible());
        QWidget foreignWindow;
        foreignWindow.setGeometry(1100, 200, 100, 100);
        foreignWindow.show();
        foreignWindow.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&foreignWindow));
        QTRY_VERIFY(!window.isToolbarVisible());
        QTest::qWait(70);
        QVERIFY(!window.isToolbarVisible());
        window.toggleToolbarVisibility();
        QTest::qWait(70);
        QVERIFY(window.isToolbarVisible());
    }
    void fullscreenRestoresNormalGeometryAndToolbarChoice() {
        MainWindow window;
        window.resize(640, 400);
        window.show();
        QCoreApplication::processEvents();
        const QRect before = window.geometry();
        window.toggleToolbarVisibility();
        const bool toolbarVisible = window.isToolbarVisible();

        window.setFullscreenEnabled(true);
        QVERIFY(window.isFullScreen());
        QCOMPARE(window.isToolbarVisible(), toolbarVisible);
        auto *button = window.findChild<QToolButton *>("fullscreenButton");
        QVERIFY(button != nullptr);
        QVERIFY(button->isChecked());
        window.setFullscreenEnabled(false);
        QVERIFY(!window.isFullScreen());
        QCOMPARE(window.geometry(), before);
        QCOMPARE(window.isToolbarVisible(), toolbarVisible);
        QVERIFY(!button->isChecked());
    }

    void fullscreenRestoresMaximizedWindow() {
        MainWindow window;
        window.showMaximized();
        QCoreApplication::processEvents();
        QVERIFY(window.isMaximized());
        window.setFullscreenEnabled(true);
        QVERIFY(window.isFullScreen());
        window.setFullscreenEnabled(false);
        QVERIFY(window.isMaximized());
    }

    void fullscreenKeepsFitAndAlwaysOnTop() {
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        window.show();
        window.setAlwaysOnTopEnabled(true);
        auto *fit = window.findChild<QToolButton *>("videoFitButton");
        QVERIFY(fit != nullptr);
        const bool previousFit = fit->isChecked();
        window.setFullscreenEnabled(true);
        QCOMPARE(fit->isChecked(), previousFit);
        QVERIFY(window.isAlwaysOnTopEnabled());
        window.setFullscreenEnabled(false);
        QCOMPARE(fit->isChecked(), previousFit);
        QVERIFY(window.isAlwaysOnTopEnabled());
    }

    void fullscreenExitsOnlyAfterActiveReceiverSessionEnds_data() {
        QTest::addColumn<ReceiverState>("endState");
        QTest::newRow("idle") << ReceiverState::Idle;
        QTest::newRow("discoverable") << ReceiverState::Discoverable;
        QTest::newRow("error") << ReceiverState::Error;
    }

    void fullscreenExitsOnlyAfterActiveReceiverSessionEnds() {
        QFETCH(ReceiverState, endState);
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        window.show();
        window.setFullscreenEnabled(true);
        emit receiver.stateChanged(ReceiverState::Discoverable);
        QVERIFY(window.isFullScreen());
        emit receiver.stateChanged(ReceiverState::Connecting);
        QVERIFY(window.isFullScreen());
        emit receiver.stateChanged(endState);
        QVERIFY(!window.isFullScreen());
    }

    void closingFullscreenPersistsPreviousWindowState() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString settingsPath = dir.filePath("settings.json");
        MainWindow window(AppSettings::defaults(), nullptr, nullptr, settingsPath);
        window.resize(640, 400);
        window.show();
        QCoreApplication::processEvents();
        window.setFullscreenEnabled(true);
        QVERIFY(window.close());

        MainWindow restored(AppSettings::defaults(), nullptr, nullptr, settingsPath);
        QVERIFY(!restored.isFullScreen());
        QCOMPARE(restored.size(), QSize(640, 400));
    }

    void closingFullscreenPersistsPreviousMaximizedState() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString settingsPath = dir.filePath("settings.json");
        MainWindow window(AppSettings::defaults(), nullptr, nullptr, settingsPath);
        window.showMaximized();
        QCoreApplication::processEvents();
        window.setFullscreenEnabled(true);
        QVERIFY(window.close());
        MainWindow restored(AppSettings::defaults(), nullptr, nullptr, settingsPath);
        QVERIFY(!restored.isFullScreen());
        QVERIFY(restored.isMaximized());
    }

    void nativeFullscreenSuspendsAspectSizingAndVideoResize() {
        if (QGuiApplication::platformName().compare("windows", Qt::CaseInsensitive) != 0) {
            QSKIP("Requires the Windows QPA platform");
        }
        FakeAirPlayReceiver receiver;
        MainWindow window(AppSettings::defaults(), nullptr, &receiver);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *aspect = window.findChild<QToolButton *>("aspectRatioButton");
        QVERIFY(aspect != nullptr);
        aspect->setChecked(true);
        window.setFullscreenEnabled(true);
        const QSize fullscreenSize = window.size();
        receiver.emitVideoSize(1170, 2532);
        QCOMPARE(window.size(), fullscreenSize);

        const HWND hwnd = reinterpret_cast<HWND>(window.winId());
        RECT pending{};
        QVERIFY(GetWindowRect(hwnd, &pending));
        pending.right += 117;
        const RECT expected = pending;
        SendMessage(hwnd, WM_SIZING, WMSZ_RIGHT, reinterpret_cast<LPARAM>(&pending));
        QCOMPARE(pending.left, expected.left);
        QCOMPARE(pending.top, expected.top);
        QCOMPARE(pending.right, expected.right);
        QCOMPARE(pending.bottom, expected.bottom);
    }

    void nativeFullscreenKeysRespectFocusAndModalControls() {
        if (QGuiApplication::platformName().compare("windows", Qt::CaseInsensitive) != 0) {
            QSKIP("Requires the Windows QPA platform");
        }
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QWidget other;
        other.show();
        other.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&other));
        QTest::keyClick(&other, Qt::Key_F11);
        QVERIFY(!window.isFullScreen());
        other.close();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        auto *settings = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(settings != nullptr);
        settings->setFocus();
        QTest::keyClick(settings, Qt::Key_F11);
        QVERIFY(window.isFullScreen());

        QDialog dialog(&window);
        dialog.open();
        QVERIFY(QTest::qWaitForWindowActive(&dialog));
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QTRY_VERIFY(!dialog.isVisible());
        QVERIFY(window.isFullScreen());

        QMenu popup(&window);
        popup.addAction("Example");
        popup.popup(window.mapToGlobal(QPoint(20, 20)));
        QTRY_VERIFY(popup.isVisible());
        QTest::keyClick(&popup, Qt::Key_Escape);
        QTRY_VERIFY(!popup.isVisible());
        QVERIFY(window.isFullScreen());

        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QTest::keyClick(settings, Qt::Key_Escape);
        QVERIFY(!window.isFullScreen());
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
