#include <QtTest>

#include "app/AppSettingsStore.h"
#include "app/DiagnosticLifecycle.h"
#include "support/MemoryDiagnosticStorage.h"
#include "app/MainWindow.h"
#include "app/SettingsDialog.h"
#include "app/SettingsApplyCoordinator.h"
#include "backend/FakeAirPlayReceiver.h"
#include "diagnostics/DiagnosticSession.h"

#include <QFile>
#include <QCheckBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QTimer>
#include <QTemporaryDir>
#include <QToolButton>

namespace {
QByteArray readLog(const DiagnosticSession &session) {
    QFile file(session.filePath());
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

class ScriptedPersistence final : public SettingsPersistence {
public:
    AppSettingsSaveResult save(const AppSettings &) const override {
        return results.isEmpty() ? AppSettingsSaveResult{true} : results.takeFirst();
    }
    mutable QList<AppSettingsSaveResult> results;
};
}

class UiDiagnosticLoggingTest final : public QObject {
    Q_OBJECT
private slots:
    void runtimeFailureUsesWindowStatusAndSingleWarningSignal() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto storage = std::make_shared<DiagnosticTestSupport::MemoryStorage>();
        DiagnosticLifecycleDependencies dependencies;
        dependencies.sessionStorage = storage;
        DiagnosticLifecycle diagnostics(qApp, dependencies);
        MainWindow *diagnosticWindow = nullptr;
        int reports = 0;
        DiagnosticLifecycleStart start;
        start.applicationDirectory = directory.path();
        start.activation = {true, DiagnosticActivationSource::CommandArgument};
        start.reportFailure = [&](DiagnosticFailure failure) {
            ++reports;
            QCOMPARE(failure.kind, DiagnosticFailureKind::Write);
            QVERIFY(diagnosticWindow);
            diagnosticWindow->handleDiagnosticWriteFailure(failure.error);
        };
        QVERIFY(diagnostics.start(std::move(start)).creationError.isEmpty());
        MainWindowRuntimeServices services;
        services.diagnosticSink = diagnostics.sink();
        services.diagnosticLoggingActive = diagnostics.loggingActive();
        MainWindow window(AppSettings::defaults(), nullptr, nullptr, {}, nullptr, nullptr, services);
        QSignalSpy warning(&window, &MainWindow::diagnosticLoggingStopped);
        const QString activeTitle = window.windowTitle();
        diagnosticWindow = &window;
        window.show();
        diagnostics.enableFailureReporting();
        storage->file->failWrites = true;
        qWarning("runtime UI failure through the Qt diagnostic bridge");
        QTRY_COMPARE(warning.size(), 1);
        QCOMPARE(warning.front().front().toString(), QStringLiteral("disk full"));
        QCOMPARE(reports, 1);
        QVERIFY(window.windowTitle() != activeTitle);
        QVERIFY(!diagnostics.exitNormally({}).has_value());
        QCoreApplication::processEvents();
        QCOMPARE(warning.size(), 1);
        QCOMPARE(reports, 1);
    }

    void settingsHoverPreferenceChangeIsLoggedEvenWhenVisibilityDoesNotChange() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        DiagnosticSessionOptions options;
        options.applicationDirectory = directory.path();
        auto created = DiagnosticSession::create(options);
        QVERIFY(created.session);
        MainWindowRuntimeServices services;
        services.diagnosticSink = created.session.get();
        MainWindow window(AppSettings::defaults(), nullptr, nullptr, directory.filePath("settings.json"),
                          nullptr, nullptr, services);
        const bool visible = window.isToolbarVisible();
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto close = qScopeGuard([dialog] { dialog->reject(); });
            auto *hover = dialog->findChild<QCheckBox *>("toolbarHoverRevealCheckBox");
            auto *apply = dialog->findChild<QPushButton *>("applySettingsButton");
            QVERIFY(hover);
            QVERIFY(apply);
            hover->setChecked(false);
            apply->click();
        });
        auto *settings = window.findChild<QToolButton *>("settingsButton");
        QVERIFY(settings);
        settings->click();
        QCOMPARE(window.isToolbarVisible(), visible);
        QVERIFY(!AppSettingsStore(directory.filePath("settings.json")).loadOrDefaults().toolbarHoverReveal());
        created.session->closeNormally();
        const auto output = readLog(*created.session);
        QCOMPARE(output.count(" ui display_preference_changed "), 1);
        QVERIFY(output.contains("setting=hover_reveal"));
        QVERIFY(output.contains("persist_result=saved"));
        QVERIFY(!output.contains(" ui toolbar_visibility_changed "));
    }

    void fullscreenTransitionsExplainTriggerAndRestoreWithoutDuplicateEvents() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        DiagnosticSessionOptions options;
        options.applicationDirectory = directory.path();
        auto created = DiagnosticSession::create(options);
        QVERIFY(created.session);
        FakeAirPlayReceiver receiver;
        MainWindowRuntimeServices services;
        services.diagnosticSink = created.session.get();
        MainWindow window(AppSettings::defaults(), nullptr, &receiver, {}, nullptr, nullptr, services);
        window.resize(640, 480);
        window.show();
        QCoreApplication::processEvents();
        window.setFullscreenEnabled(true);
        QVERIFY(window.isFullScreen());
        window.setFullscreenEnabled(true);
        auto *button = window.findChild<QToolButton *>("fullscreenButton");
        QVERIFY(button);
        button->click();
        QVERIFY(!window.isFullScreen());
        receiver.forceState(ReceiverState::Connected);
        window.setFullscreenEnabled(true);
        receiver.forceState(ReceiverState::Discoverable);
        QVERIFY(!window.isFullScreen());
        created.session->closeNormally();
        const auto output = readLog(*created.session);
        QCOMPARE(output.count(" ui fullscreen_changed "), 4);
        QVERIFY(output.contains("trigger=api"));
        QVERIFY(output.contains("trigger=toolbar"));
        QVERIFY(output.contains("trigger=session_inactive"));
        QVERIFY(output.contains("desired=fullscreen"));
        QVERIFY(output.contains("to=windowed"));
        QCOMPARE(output.count(" ui fullscreen_geometry_restored "), 2);
        QVERIFY(output.contains("strategy=original"));
        QVERIFY(output.contains("geometry_result=ok"));
        QVERIFY(output.contains("layout_result=ok"));
        QVERIFY(!output.contains(directory.path().toUtf8()));
    }

    void toolbarSaveFailureReportsStageWithoutPrivateSettings() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile blocker(directory.filePath("blocker"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        DiagnosticSessionOptions options;
        options.applicationDirectory = directory.path();
        auto created = DiagnosticSession::create(options);
        QVERIFY(created.session);
        MainWindowRuntimeServices services;
        services.diagnosticSink = created.session.get();
        AppSettings settings = AppSettings::defaults();
        settings.setReceiverName("Sensitive Receiver");
        MainWindow window(settings, nullptr, nullptr, blocker.fileName() + "/settings.json",
                          nullptr, nullptr, services);
        auto *button = window.findChild<QToolButton *>("aspectRatioButton");
        QVERIFY(button);
        button->click();
        const auto output = readLog(*created.session);
        QVERIFY(output.contains("WARN ui settings_save_failed"));
        QVERIFY(output.contains("stage=open"));
        QVERIFY(output.contains("origin=control"));
        QCOMPARE(output.count(" ui settings_save_failed "), 1);
        QVERIFY(!output.contains("Sensitive"));
        QVERIFY(!output.contains("settings.json"));
        QVERIFY(!output.contains(directory.path().toUtf8()));
        created.session->closeNormally();
        const auto finalOutput = readLog(*created.session);
        QVERIFY(finalOutput.contains("setting=aspect_lock"));
        QVERIFY(finalOutput.contains("persist_result=failed"));
    }

    void settingsApplySummarizesPartialOutcomeWithoutAttemptedValues() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        DiagnosticSessionOptions options;
        options.applicationDirectory = directory.path();
        auto created = DiagnosticSession::create(options);
        QVERIFY(created.session);
        AppSettingsStore store(directory.filePath("private-settings.json"));
        SettingsApplyCoordinator coordinator(nullptr, &store, nullptr, nullptr, created.session.get());
        const auto baseline = AppSettings::defaults();
        auto candidate = baseline;
        candidate.setReceiverName("   ");
        candidate.setLanguage("en");
        candidate.setToolbarHoverReveal(false);
        const auto outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);
        QVERIFY(store.loadOrDefaults().language() == "en");
        QVERIFY(!store.loadOrDefaults().toolbarHoverReveal());
        QVERIFY(!outcome.globalResult);
        created.session->closeNormally();
        const auto output = readLog(*created.session);
        QCOMPARE(output.count(" ui settings_apply_completed "), 1);
        QVERIFY(output.contains("applied_count=2"));
        QVERIFY(output.contains("validation_failed_count=1"));
        QVERIFY(output.contains("timing=immediate"));
        QVERIFY(output.contains("persistence_failed=no"));
        QVERIFY(!output.contains("private-settings.json"));
        QVERIFY(!output.contains(directory.path().toUtf8()));
    }

    void compensationFailureReportsOriginalStage_data() {
        QTest::addColumn<bool>("deferred");
        QTest::newRow("immediate-unavailable") << false;
        QTest::newRow("deferred-unavailable") << true;
    }

    void compensationFailureReportsOriginalStage() {
        QFETCH(bool, deferred);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        DiagnosticSessionOptions options;
        options.applicationDirectory = directory.path();
        auto created = DiagnosticSession::create(options);
        QVERIFY(created.session);
        ScriptedPersistence persistence;
        if (!deferred) persistence.results.append(AppSettingsSaveResult{true});
        persistence.results.append({false, "C:/Users/Private/settings.json", AppSettingsSaveStage::Commit,
                                    QFileDevice::WriteError, "Private receiver and path failure"});
        SettingsApplyCoordinator coordinator(nullptr, &persistence, nullptr, nullptr, created.session.get());
        const auto baseline = AppSettings::defaults();
        auto candidate = baseline;
        candidate.setReceiverName("Private Receiver");
        SettingsApplyOutcome outcome;
        if (deferred) {
            ReceiverConfigurationBatchRequest batch;
            batch.receiverNameChanged = true;
            batch.requestedReceiverName = candidate.receiverName();
            batch.rollbackReceiverName = baseline.receiverName();
            batch.rollbackVideoQuality = baseline.videoQuality();
            outcome = coordinator.completeDeferredReceiverApply(batch, candidate);
        } else {
            outcome = coordinator.execute(
                coordinator.plan(baseline, candidate, false, RecordingState::Idle),
                ReceiverApplyTiming::Immediate);
        }
        QVERIFY(outcome.globalResult);
        const auto output = readLog(*created.session);
        QVERIFY(output.contains("WARN ui settings_save_failed"));
        QVERIFY(output.contains("origin=compensation"));
        QVERIFY(output.contains("stage=commit"));
        QVERIFY(output.contains("io_error_code="));
        QCOMPARE(output.count(" ui settings_save_failed "), 1);
        QVERIFY(output.contains("persistence_failed=yes"));
        QVERIFY(output.contains("recovery_failed_count=1"));
        QVERIFY(!output.contains("Private"));
        QVERIFY(!output.contains("settings.json"));
        QVERIFY(!output.contains(directory.path().toUtf8()));
    }
};

QTEST_MAIN(UiDiagnosticLoggingTest)
#include "UiDiagnosticLoggingTest.moc"
