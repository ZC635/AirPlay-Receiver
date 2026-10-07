#include <QtTest/QtTest>
#include <utility>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include "platform/FakeHotkeyService.h"
#include <QLabel>
#include <QLineEdit>
#include <QJsonDocument>
#include <QJsonObject>
#include "app/SettingsDialog.h"
#include "app/LanguageManager.h"
#include "app/SettingsApplyCoordinator.h"
#include "backend/FakeAirPlayReceiver.h"

class ScriptedPersistence : public SettingsPersistence {
public:
    mutable int count = 0;
    mutable std::function<void()> onSave;
    AppSettingsSaveResult save(const AppSettings &) const override {
        ++count;
        auto callback = std::exchange(onSave, {});
        if (callback) callback();
        return {true};
    }
};
class CallbackReceiver : public FakeAirPlayReceiver {
public:
    std::function<void()> onApply;
    ReceiverConfigurationBatchResult applyConfigurationBatch(const ReceiverConfigurationBatchRequest &batch) override {
        const auto result = FakeAirPlayReceiver::applyConfigurationBatch(batch);
        if (onApply) onApply();
        return result;
    }
};
struct LifecycleFixture {
    AppSettings current = AppSettings::defaults();
    ScriptedPersistence persistence;
    CallbackReceiver receiver;
    SettingsApplyCoordinator coordinator{current, nullptr, &persistence, &receiver};
    QSignalSpy completed{&coordinator, &SettingsApplyCoordinator::deferredApplyFinished};
    LifecycleFixture() {
        coordinator.prepareReceiverStart();
        receiver.start();
        receiver.forceState(ReceiverState::Connected);
        receiver.startCount = 0;
        receiver.configurationBatchCount = 0;
    }
    SettingsSubmitResult submit(QString name, ReceiverApplyTiming timing = ReceiverApplyTiming::AfterDisconnect) {
        auto draft = current;
        draft.setReceiverName(name);
        return coordinator.apply(draft, [timing] { return timing; });
    }
};
class MetaCallCounter : public QObject {
public:
    int deliveries = 0;
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::MetaCall) ++deliveries;
        return false;
    }
};
class DurableCallbackPersistence final : public SettingsPersistence {
public:
    explicit DurableCallbackPersistence(QString path) : store(std::move(path)) {}
    AppSettingsSaveResult save(const AppSettings &settings) const override {
        const int call = ++calls;
        if (call == failAt) { lastFailure = AppSettingsStore(failurePath).save(settings); if (onFailure) onFailure(); return lastFailure; }
        const bool notify = call == callbackAt || (repeatCallback && call == callbackAt + 1);
        if (notify && beforeWrite && callback) callback();
        const auto result = store.save(settings);
        if (notify && !beforeWrite && result.success && callback) callback();
        return result;
    }
    AppSettingsStore store;
    mutable int calls = 0;
    int callbackAt = 1;
    bool beforeWrite = false;
    bool repeatCallback = false;
    int failAt = 0;
    QString failurePath;
    mutable AppSettingsSaveResult lastFailure;
    std::function<void()> onFailure;
    std::function<void()> callback;
};

void retainDurableObservation(const AppSettingsStore &, const QString &path, const char *suffix) {
    const QString root = qEnvironmentVariable("AIRPLAY_FIX_EVIDENCE");
    if (root.isEmpty()) return;
    QDir().mkpath(root);
    const QString target = QDir(root).filePath(QString("%1-%2-%3.json")
        .arg(QTest::currentTestFunction(), QTest::currentDataTag(), suffix));
    QVERIFY(QFile::copy(path, target));
}

void retainSubmitFacts(const SettingsSubmitResult &submit) {
    const QString root = qEnvironmentVariable("AIRPLAY_FIX_EVIDENCE");
    if (root.isEmpty()) return;
    const QString factsRoot = QDir(root).filePath("../facts");
    QVERIFY(QDir().mkpath(factsRoot));
    QJsonObject facts{{"settingsSaved", submit.settingsSaved}, {"backendInvoked", submit.backendInvoked},
        {"status", static_cast<int>(submit.status)}, {"userReason", submit.userReason.render()}};
    if (submit.outcome && submit.outcome->globalResult) {
        const auto &save = submit.outcome->globalResult->persistence;
        facts.insert("saveSuccess", save.success);
        facts.insert("failureStage", save.failureStage ? QJsonValue(static_cast<int>(*save.failureStage)) : QJsonValue());
        facts.insert("fileError", static_cast<int>(save.fileError));
        facts.insert("targetPath", save.targetPath);
        facts.insert("errorString", save.errorString);
        facts.insert("currentVolume", submit.outcome->committedSettings.volume());
        facts.insert("currentReceiverName", submit.outcome->committedSettings.receiverName());
    }
    QFile file(QDir(factsRoot).filePath(QString("%1-%2.json").arg(QTest::currentTestFunction(), QTest::currentDataTag())));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    const auto bytes = QJsonDocument(facts).toJson();
    QCOMPARE(file.write(bytes), static_cast<qint64>(bytes.size()));
}


class DeferredSettingsLifecycleTest : public QObject {
    Q_OBJECT
private slots:
    void interruptedUnknownFirstFailureDoesNotClaimSavingSucceeded() {
        class UnknownFailurePersistence final : public SettingsPersistence {
        public:
            std::function<void()> interrupt;
            AppSettingsSaveResult save(const AppSettings &) const override {
                interrupt();
                return {}; // Supported failure without native details; no actual write.
            }
        } persistence;
        auto current = AppSettings::defaults();
        CallbackReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        SettingsApplyCoordinator coordinator(current, nullptr, &persistence, &receiver);
        persistence.interrupt = [&] { coordinator.endReceiverLifecycle(); };
        auto draft = current;
        draft.setReceiverName("Unsaved target");
        const auto submit = coordinator.apply(draft, [] { return ReceiverApplyTiming::Immediate; });
        QCOMPARE(submit.status, SettingsSubmitStatus::Interrupted);
        QVERIFY(!submit.settingsSaved);
        QVERIFY(!submit.backendInvoked);
        QVERIFY(submit.outcome->globalResult.has_value());
        retainSubmitFacts(submit);
        LanguageManager language(QCoreApplication::instance());
        QVERIFY(language.apply("en"));
        SettingsDialog dialog(current);
        dialog.presentSubmitError(submit.userReason);
        auto *summary = dialog.findChild<QLabel *>("settingsApplySummary");
        QVERIFY(summary);
        QVERIFY2(summary->text().contains("before saving"), qPrintable(summary->text()));
        QVERIFY(!summary->text().contains("Settings were saved"));
        QCOMPARE(current.receiverName(), QString("AirPlay Receiver"));
        QCOMPARE(receiver.configurationBatchCount, 0);
    }


    void unchangedGoalSurvivesUnconfirmedSave_data() {
        QTest::addColumn<int>("failureKind");
        QTest::addColumn<bool>("beforeWrite");
        QTest::addColumn<bool>("invalidName");
        QTest::addColumn<int>("releaseOrder");
        for (int failure = 0; failure < 3; ++failure)
            for (bool before : {true, false})
                for (bool invalid : {false, true})
                    for (int release = 0; release < 3; ++release)
                        QTest::newRow(qPrintable(QString("failure%1-before%2-invalid%3-release%4")
                            .arg(failure).arg(before).arg(invalid).arg(release)))
                            << failure << before << invalid << release;
    }
    void unchangedGoalSurvivesUnconfirmedSave() {
        QFETCH(int, failureKind);
        QFETCH(bool, beforeWrite);
        QFETCH(bool, invalidName);
        QFETCH(int, releaseOrder);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("settings.json");
        auto current = AppSettings::defaults();
        const auto originalQuality = current.videoQuality();
        DurableCallbackPersistence persistence(path);
        QVERIFY(persistence.store.save(current).success);
        CallbackReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        receiver.setRecordingAvailableForTest(true);
        SettingsApplyCoordinator::RecordingPresentationCompletion presentation;
        SettingsApplyCoordinator coordinator(current, nullptr, &persistence, &receiver, nullptr, nullptr,
            [&](auto completion) { presentation = std::move(completion); });
        QSignalSpy finished(&coordinator, &SettingsApplyCoordinator::deferredApplyFinished);
        receiver.startRecording({}); // Fake only; no recording pipeline.
        auto draft = current;
        draft.setReceiverName("Original complete target");
        QVERIFY(coordinator.apply(draft, [] { return ReceiverApplyTiming::AfterDisconnect; }).settingsSaved);
        draft = current;
        draft.setVideoQuality({VideoResolution::P720, VideoFrameRate::Fps60});
        QVERIFY(coordinator.apply(draft, [] { return ReceiverApplyTiming::AfterDisconnect; }).settingsSaved);
        QCOMPARE(receiver.configurationBatchCount, 0);
        persistence.callbackAt = 3;
        persistence.beforeWrite = beforeWrite;
        persistence.repeatCallback = failureKind == 1;
        persistence.failAt = failureKind == 0 ? 4 : failureKind == 2 ? 3 : 0;
        persistence.failurePath = directory.path();
        int independentCommits = 0;
        const auto releaseAll = [&] {
            receiver.forceState(ReceiverState::Discoverable);
            receiver.stopRecording();
            receiver.completeRecordingForTest({});
            QVERIFY(presentation);
            presentation();
            QCoreApplication::sendPostedEvents();
            QCOMPARE(receiver.configurationBatchCount, 0); // Synchronous Apply is still busy.
        };
        persistence.callback = [&] {
            auto independent = persistence.store.loadOrDefaults();
            independent.setVolume(42 + independentCommits++);
            QVERIFY(persistence.store.save(independent).success);
            current = independent;
            if (releaseOrder == 2 && independentCommits == 1) releaseAll();
        };
        if (failureKind == 2 && releaseOrder == 2) persistence.onFailure = releaseAll;
        draft = current;
        draft.setLanguage("zh-CN");
        if (invalidName) draft.setReceiverName("");
        int timingChoices = 0;
        const auto submit = coordinator.apply(draft, [&] { ++timingChoices; return ReceiverApplyTiming::Immediate; });
        QCOMPARE(timingChoices, 0);
        QCOMPARE(submit.settingsSaved, failureKind != 2);
        QVERIFY(!submit.backendInvoked);
        QVERIFY(submit.outcome->globalResult.has_value());
        retainSubmitFacts(submit);
        if (invalidName) QCOMPARE(resultForField(submit.outcome->fieldResults, SettingsFieldId::receiverName())->status,
                                 SettingsFieldStatus::ValidationFailed);
        QCOMPARE(persistence.calls, failureKind == 2 ? 3 : 4);
        QCOMPARE(receiver.configurationBatchCount, 0);
        QCOMPARE(current.receiverName(), QString("Original complete target"));
        QCOMPARE(persistence.store.loadOrDefaults().receiverName(), QString("Original complete target"));
        QCOMPARE(current.volume(), failureKind == 2 ? 100 : failureKind == 1 ? 43 : 42);
        QCOMPARE(persistence.store.loadOrDefaults().volume(), failureKind == 2 ? 100 :
                 beforeWrite ? (failureKind == 1 ? 42 : 100) : (failureKind == 1 ? 43 : 42));
        retainDurableObservation(persistence.store, path, "before-gate-release");
        persistence.callback = {};
        persistence.onFailure = {};
        persistence.failAt = 0;
        // Also prove actual rollback uses C0, not the intermediate name-only saved target.
        const bool rollback = invalidName && releaseOrder == 1;
        if (rollback) receiver.requestedConfigurationRestartError = "Controlled receiver failure";
        if (releaseOrder == 0) {
            receiver.forceState(ReceiverState::Discoverable);
            QCOMPARE(receiver.configurationBatchCount, 0);
            receiver.stopRecording(); receiver.completeRecordingForTest({});
            QCOMPARE(receiver.configurationBatchCount, 0);
            QVERIFY(presentation); presentation();
        } else if (releaseOrder == 1) {
            receiver.stopRecording(); receiver.completeRecordingForTest({});
            QCOMPARE(receiver.configurationBatchCount, 0);
            QVERIFY(presentation); presentation();
            QCOMPARE(receiver.configurationBatchCount, 0);
            receiver.forceState(ReceiverState::Discoverable);
        } else {
            QCoreApplication::sendPostedEvents();
        }
        QCOMPARE(receiver.configurationBatchCount, 1);
        QCOMPARE(finished.count(), 1);
        const auto &request = receiver.configurationBatchRequests.last();
        QCOMPARE(request.requestedReceiverName, QString("Original complete target"));
        QCOMPARE(request.requestedVideoQuality, (VideoQualitySettings{VideoResolution::P720, VideoFrameRate::Fps60}));
        QCOMPARE(request.rollbackReceiverName, QString("AirPlay Receiver"));
        QCOMPARE(request.rollbackVideoQuality, originalQuality);
        QVERIFY(request.receiverNameChanged && request.resolutionChanged && request.frameRateChanged);
        QCOMPARE(receiver.receiverName(), rollback ? QString("AirPlay Receiver") : QString("Original complete target"));
        if (rollback) {
            QCOMPARE(current.receiverName(), QString("AirPlay Receiver"));
            QCOMPARE(current.videoQuality(), originalQuality);
            QCOMPARE(persistence.store.loadOrDefaults().videoQuality(), originalQuality);
            QCOMPARE(persistence.store.loadOrDefaults().volume(), current.volume());
        }
        QCOMPARE(persistence.calls, (failureKind == 2 ? 3 : 4) + (rollback ? 1 : 0));
        retainDurableObservation(persistence.store, path, "after-gate-release");
        receiver.forceState(ReceiverState::Connected);
        receiver.forceState(ReceiverState::Discoverable);
        if (presentation) presentation();
        QCoreApplication::sendPostedEvents();
        QCOMPARE(receiver.configurationBatchCount, 1);
        QCOMPARE(finished.count(), 1);
    }

    void interruptedSavePresentsPersistenceFacts_data() {
        QTest::addColumn<int>("nativeFailureAt");
        QTest::addColumn<bool>("beforeWrite");
        for (int failure : {0, 1, 2})
            for (bool before : {true, false})
                QTest::newRow(qPrintable(QString("failure%1-before%2").arg(failure).arg(before))) << failure << before;
    }
    void interruptedSavePresentsPersistenceFacts() {
        QFETCH(int, nativeFailureAt);
        QFETCH(bool, beforeWrite);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("settings.json");
        auto current = AppSettings::defaults();
        DurableCallbackPersistence persistence(path);
        QVERIFY(persistence.store.save(current).success);
        CallbackReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        SettingsApplyCoordinator coordinator(current, nullptr, &persistence, &receiver);
        persistence.beforeWrite = beforeWrite;
        persistence.failAt = nativeFailureAt;
        persistence.failurePath = directory.path();
        persistence.onFailure = [&] { coordinator.endReceiverLifecycle(); };
        persistence.callback = [&] {
            auto independent = persistence.store.loadOrDefaults();
            independent.setVolume(42);
            QVERIFY(persistence.store.save(independent).success);
            current = independent;
            if (!nativeFailureAt) coordinator.endReceiverLifecycle();
        };
        auto draft = current;
        draft.setReceiverName("Interrupted target");
        const auto submit = coordinator.apply(draft, [] { return ReceiverApplyTiming::Immediate; });
        QCOMPARE(submit.status, SettingsSubmitStatus::Interrupted);
        QCOMPARE(submit.settingsSaved, nativeFailureAt != 1);
        QVERIFY(!submit.backendInvoked);
        QVERIFY(submit.outcome->globalResult.has_value());
        retainSubmitFacts(submit);
        const auto &failure = submit.outcome->globalResult->persistence;
        if (nativeFailureAt) {
            QCOMPARE(failure.failureStage, std::optional<AppSettingsSaveStage>(AppSettingsSaveStage::Open));
            QCOMPARE(failure.fileError, persistence.lastFailure.fileError);
            QCOMPARE(failure.errorString, persistence.lastFailure.errorString);
            QCOMPARE(failure.targetPath, directory.path());
            QVERIFY(!failure.errorString.isEmpty());
        } else {
            QVERIFY(!failure.failureStage);
            QCOMPARE(failure.fileError, QFileDevice::NoError);
            QVERIFY(failure.errorString.isEmpty());
        }
        QCOMPARE(current.volume(), nativeFailureAt == 1 ? 100 : 42);
        QCOMPARE(persistence.store.loadOrDefaults().volume(), nativeFailureAt == 1 || beforeWrite ? 100 : 42);
        retainDurableObservation(persistence.store, path, "interrupted");
        LanguageManager language(QCoreApplication::instance());
        QVERIFY(language.apply("en"));
        SettingsDialog dialog(AppSettings::defaults());
        dialog.findChild<QLineEdit *>("receiverNameEdit")->setText("Interrupted target");
        dialog.presentSubmitError(submit.userReason); // Actual public route used by MainWindow.
        auto *summary = dialog.findChild<QLabel *>("settingsApplySummary");
        QVERIFY(summary);
        QVERIFY(summary->text().contains("interrupted"));
        QVERIFY2(summary->text().contains(nativeFailureAt == 1 ? "could not be saved" : "could not be confirmed"), qPrintable(summary->text()));
        QVERIFY(summary->text().contains("Apply again"));
        if (nativeFailureAt) {
            QVERIFY(summary->text().contains(failure.targetPath));
            QVERIFY(summary->text().contains(failure.errorString));
        }
        QVERIFY(language.apply("zh-CN"));
        QEvent event(QEvent::LanguageChange);
        QCoreApplication::sendEvent(&dialog, &event);
        QVERIFY(summary->text().contains(QStringLiteral("中断")));
        QVERIFY(summary->text().contains(nativeFailureAt == 1 ? QStringLiteral("无法保存") : QStringLiteral("无法确认")));
        QVERIFY(summary->text().contains(QStringLiteral("重新应用")));
        QVERIFY(!summary->text().contains("Settings application was interrupted"));
        QVERIFY(language.apply("en"));
        QCoreApplication::sendEvent(&dialog, &event);
        QVERIFY(summary->text().contains("Apply again"));
        QCOMPARE(dialog.draftSettings().receiverName(), QString("Interrupted target"));
        QCOMPARE(dialog.committedBaseline().receiverName(), QString("AirPlay Receiver"));
        QVERIFY(dialog.hasUnappliedChanges());
        receiver.forceState(ReceiverState::Discoverable);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(receiver.configurationBatchCount, 0);
        QCOMPARE(persistence.calls, nativeFailureAt == 2 ? 2 : 1);
    }


    void unconfirmedSaveNeedsExplicitPrepareForNewReceiverRound_data() {
        QTest::addColumn<bool>("repeatedCommit");
        QTest::newRow("native-reconciliation-failure") << false;
        QTest::newRow("repeated-independent-commit") << true;
    }
    void unconfirmedSaveNeedsExplicitPrepareForNewReceiverRound() {
        QFETCH(bool, repeatedCommit);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto current = AppSettings::defaults();
        DurableCallbackPersistence persistence(directory.filePath("settings.json"));
        QVERIFY(persistence.store.save(current).success);
        persistence.beforeWrite = true;
        persistence.repeatCallback = repeatedCommit;
        persistence.failAt = repeatedCommit ? 0 : 2;
        persistence.failurePath = directory.path();
        int independentCommits = 0;
        persistence.callback = [&] {
            auto independent = persistence.store.loadOrDefaults();
            independent.setVolume(42 + independentCommits++);
            QVERIFY(persistence.store.save(independent).success);
            current = independent;
        };
        CallbackReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        SettingsApplyCoordinator coordinator(current, nullptr, &persistence, &receiver);
        auto draft = current;
        draft.setReceiverName("Next prepared target");
        const auto result = coordinator.apply(draft, [] { return ReceiverApplyTiming::AfterDisconnect; });
        QVERIFY(result.settingsSaved);
        QVERIFY(result.outcome->globalResult.has_value());
        receiver.forceState(ReceiverState::Discoverable);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(receiver.configurationBatchCount, 0);
        coordinator.endReceiverLifecycle();
        receiver.stop();
        const int startsBeforePrepare = receiver.startCount;
        const auto prepared = coordinator.prepareReceiverStart();
        QCOMPARE(prepared.status, ReceiverStartPreparationStatus::Prepared);
        QVERIFY(prepared.backendResult.has_value());
        QCOMPARE(receiver.configurationBatchCount, 1);
        QCOMPARE(receiver.receiverName(), QString("Next prepared target"));
        QCOMPARE(receiver.startCount, startsBeforePrepare);
        QCOMPARE(persistence.calls, 2); // Prepared never claims to repair persistence.
        QCOMPARE(current.volume(), repeatedCommit ? 43 : 42);
        QCOMPARE(persistence.store.loadOrDefaults().volume(), repeatedCommit ? 42 : 100);
        retainDurableObservation(persistence.store, directory.filePath("settings.json"), "prepared-runtime-only");
    }

    void unconfirmedReplacementCancelsOldBatchAndKeepsSavedShortcut_data() {
        QTest::addColumn<bool>("repeatedCommit");
        QTest::newRow("native-reconciliation-failure") << false;
        QTest::newRow("repeated-independent-commit") << true;
    }
    void unconfirmedReplacementCancelsOldBatchAndKeepsSavedShortcut() {
        QFETCH(bool, repeatedCommit);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto current = AppSettings::defaults();
        DurableCallbackPersistence persistence(directory.filePath("settings.json"));
        QVERIFY(persistence.store.save(current).success);
        FakeHotkeyService hotkeys;
        CallbackReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        SettingsApplyCoordinator coordinator(current, &hotkeys, &persistence, &receiver);
        auto draft = current;
        draft.setReceiverName("Old target");
        QVERIFY(coordinator.apply(draft, [] { return ReceiverApplyTiming::AfterDisconnect; }).settingsSaved);
        persistence.callbackAt = 2;
        persistence.beforeWrite = true;
        persistence.failAt = repeatedCommit ? 0 : 3;
        persistence.repeatCallback = repeatedCommit;
        int independentCommits = 0;
        persistence.failurePath = directory.path();
        persistence.callback = [&] {
            auto independent = persistence.store.loadOrDefaults();
            independent.setVolume(42 + independentCommits++);
            QVERIFY(persistence.store.save(independent).success);
            current = independent;
        };
        draft = current;
        draft.setReceiverName("Replacement target");
        draft.setShortcut(ShortcutAction::VolumeUp, QKeySequence("Ctrl+Shift+U"));
        const auto result = coordinator.apply(draft, [] { return ReceiverApplyTiming::AfterDisconnect; });
        QVERIFY(result.settingsSaved);
        QVERIFY(!result.backendInvoked);
        QVERIFY(result.outcome->globalResult.has_value());
        QCOMPARE(resultForField(result.outcome->fieldResults, SettingsFieldId::shortcut(ShortcutAction::VolumeUp))->status, SettingsFieldStatus::Applied);
        QCOMPARE(current.shortcutFor(ShortcutAction::VolumeUp), QKeySequence("Ctrl+Shift+U"));
        QCOMPARE(persistence.store.loadOrDefaults().shortcutFor(ShortcutAction::VolumeUp), QKeySequence("Ctrl+Shift+U"));
        QCOMPARE(hotkeys.activeBindings.value(static_cast<int>(ShortcutAction::VolumeUp)), QKeySequence("Ctrl+Shift+U"));
        receiver.forceState(ReceiverState::Discoverable);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(receiver.configurationBatchCount, 0);
        QCOMPARE(persistence.calls, 3);
        retainDurableObservation(persistence.store, directory.filePath("settings.json"), "unconfirmed");
        persistence.callback = {};
        persistence.failAt = 0;
        receiver.forceState(ReceiverState::Connected);
        int choices = 0;
        const auto retried = coordinator.apply(draft, [&] { ++choices; return ReceiverApplyTiming::AfterDisconnect; });
        QCOMPARE(choices, 1);
        QVERIFY(retried.settingsSaved);
        QVERIFY(!retried.outcome->globalResult.has_value());
        receiver.forceState(ReceiverState::Discoverable);
        QCOMPARE(receiver.configurationBatchCount, 1);
        QCOMPARE(receiver.configurationBatchRequests.last().requestedReceiverName, QString("Replacement target"));
        QCOMPARE(receiver.configurationBatchRequests.last().rollbackReceiverName, QString("AirPlay Receiver"));
        QCOMPARE(persistence.store.loadOrDefaults().volume(), repeatedCommit ? 43 : 42);
        retainDurableObservation(persistence.store, directory.filePath("settings.json"), "explicit-retry");
    }
    void endInsideSaveDoesNotStartReconciliation_data() {
        QTest::addColumn<bool>("beforeWrite");
        QTest::newRow("before-write") << true;
        QTest::newRow("after-write") << false;
    }
    void endInsideSaveDoesNotStartReconciliation() {
        QFETCH(bool, beforeWrite);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto current = AppSettings::defaults();
        DurableCallbackPersistence persistence(directory.filePath("settings.json"));
        QVERIFY(persistence.store.save(current).success);
        CallbackReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        SettingsApplyCoordinator coordinator(current, nullptr, &persistence, &receiver);
        persistence.beforeWrite = beforeWrite;
        persistence.callback = [&] {
            auto independent = persistence.store.loadOrDefaults();
            independent.setVolume(42);
            QVERIFY(persistence.store.save(independent).success);
            current = independent;
            coordinator.endReceiverLifecycle();
        };
        auto draft = current;
        draft.setReceiverName("Ended target");
        const auto result = coordinator.apply(draft, [] { return ReceiverApplyTiming::Immediate; });
        QCOMPARE(result.status, SettingsSubmitStatus::Interrupted);
        QVERIFY(result.settingsSaved);
        QVERIFY(!result.backendInvoked);
        QVERIFY(result.outcome->globalResult.has_value());
        QCOMPARE(current.volume(), 42);
        QCOMPARE(current.receiverName(), QString("Ended target"));
        QCOMPARE(persistence.store.loadOrDefaults().volume(), beforeWrite ? 100 : 42);
        QCOMPARE(persistence.calls, 1);
        receiver.forceState(ReceiverState::Discoverable);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(receiver.configurationBatchCount, 0);
        QCOMPARE(persistence.calls, 1);
        retainDurableObservation(persistence.store, directory.filePath("settings.json"), "ended");
    }

    void reconciliationFailureKeepsDurableFacts_data() {
        QTest::addColumn<int>("scenario");
        QTest::addColumn<bool>("beforeWrite");
        QTest::addColumn<bool>("repeatedCommit");
        for (int scenario = 0; scenario < 4; ++scenario)
            for (bool before : {false, true})
                for (bool repeated : {false, true})
                    QTest::newRow(qPrintable(QString("scenario-%1-before-%2-repeat-%3").arg(scenario).arg(before).arg(repeated)))
                        << scenario << before << repeated;
    }
    void reconciliationFailureKeepsDurableFacts() {
        QFETCH(int, scenario);
        QFETCH(bool, beforeWrite);
        QFETCH(bool, repeatedCommit);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto current = AppSettings::defaults();
        DurableCallbackPersistence persistence(directory.filePath("settings.json"));
        QVERIFY(persistence.store.save(current).success);
        persistence.callbackAt = scenario == 0 ? 1 : 2;
        persistence.beforeWrite = beforeWrite;
        persistence.repeatCallback = repeatedCommit;
        persistence.failurePath = directory.path(); // Real QSaveFile open failure: a directory.
        persistence.failAt = repeatedCommit ? 0 : persistence.callbackAt + 1;
        int independentCommits = 0;
        persistence.callback = [&] {
            auto independent = persistence.store.loadOrDefaults();
            independent.setVolume(42 + independentCommits);
            independent.setAspectRatioLock(false);
            independent.setVideoFitMode(false);
            if (persistence.store.save(independent).success) { ++independentCommits; current = independent; }
        };
        CallbackReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        if (scenario == 1 || scenario == 2) receiver.requestedConfigurationRestartError = "controlled restart failure";
        SettingsApplyCoordinator coordinator(current, nullptr, &persistence, scenario == 3 ? nullptr : &receiver);
        QSignalSpy finished(&coordinator, &SettingsApplyCoordinator::deferredApplyFinished);
        auto draft = current;
        draft.setReceiverName("Unconfirmed target");
        draft.setLanguage("zh-CN");
        const auto submit = coordinator.apply(draft, [scenario] {
            return scenario == 2 ? ReceiverApplyTiming::AfterDisconnect : ReceiverApplyTiming::Immediate;
        });
        QVERIFY(submit.settingsSaved);
        QVERIFY(submit.outcome.has_value());
        auto outcome = *submit.outcome;
        if (scenario == 2) {
            receiver.forceState(ReceiverState::Discoverable);
            QCOMPARE(finished.count(), 1);
            outcome = std::get<SettingsApplyOutcome>(qvariant_cast<SettingsDeferredResult>(finished.at(0).at(0)));
        }
        retainDurableObservation(persistence.store, directory.filePath("settings.json"), "failure");
        QVERIFY(outcome.globalResult.has_value());
        QVERIFY(!outcome.mayClose);
        QCOMPARE(independentCommits, repeatedCommit ? 2 : 1);
        QCOMPARE(persistence.calls, persistence.callbackAt + 1);
        QCOMPARE(current.volume(), repeatedCommit ? 43 : 42);
        QCOMPARE(outcome.committedSettings.volume(), current.volume());
        QVERIFY(!current.aspectRatioLock());
        QVERIFY(!current.videoFitMode());
        QCOMPARE(current.language(), QString("zh-CN"));
        QCOMPARE(current.receiverName(), scenario == 0 ? QString("Unconfirmed target") : QString("AirPlay Receiver"));
        const auto disk = persistence.store.loadOrDefaults();
        QCOMPARE(disk.volume(), beforeWrite ? (repeatedCommit ? 42 : 100) : (repeatedCommit ? 43 : 42));
        QCOMPARE(disk.receiverName(), current.receiverName());
        const auto &failure = outcome.globalResult->persistence;
        QVERIFY(!failure.success);
        if (repeatedCommit) {
            QVERIFY(!failure.failureStage.has_value());
            QCOMPARE(failure.fileError, QFileDevice::NoError);
            QVERIFY(failure.errorString.isEmpty()); // No invented native I/O error.
            QVERIFY(!resultForField(outcome.fieldResults, SettingsFieldId::receiverName())->userReason.isEmpty());
        } else {
            QCOMPARE(failure.failureStage, persistence.lastFailure.failureStage);
            QCOMPARE(failure.fileError, persistence.lastFailure.fileError);
            QCOMPARE(failure.targetPath, persistence.lastFailure.targetPath);
            QCOMPARE(failure.errorString, persistence.lastFailure.errorString);
        }
        QCOMPARE(resultForField(outcome.fieldResults, SettingsFieldId::receiverName())->status, SettingsFieldStatus::RecoveryFailed);
        if (scenario == 1 || scenario == 2) {
            const auto *field = resultForField(outcome.fieldResults, SettingsFieldId::receiverName());
            QVERIFY(field->recoveryError.contains("controlled restart failure"));
            QVERIFY(field->recoveryError.contains("AirPlay Receiver"));
        }
        LanguageManager language(QCoreApplication::instance());
        QVERIFY(language.apply("en"));
        SettingsDialog dialog(AppSettings::defaults());
        dialog.presentApplyOutcome(outcome);
        const auto visibleLabels = [&] {
            QString text;
            for (auto *label : dialog.findChildren<QLabel *>()) if (!label->isHidden()) text += label->text() + '\n';
            return text;
        };
        QVERIFY(visibleLabels().contains("Settings were saved, but their final saved state could not be confirmed. Apply again."));
        QVERIFY(!visibleLabels().contains("No changes from this Apply were committed."));
        QVERIFY(language.apply("zh-CN"));
        QEvent languageChange(QEvent::LanguageChange);
        QCoreApplication::sendEvent(&dialog, &languageChange);
        QVERIFY(visibleLabels().contains(QStringLiteral("设置已保存，但无法确认最终保存状态。请重新应用。")));
        if (repeatedCommit) QVERIFY(visibleLabels().contains(QStringLiteral("无法确认最终保存的设置。请重新应用。")));
        else QVERIFY(visibleLabels().contains(QStringLiteral("无法确认")));
        QVERIFY(!visibleLabels().contains("Could not confirm saved settings"));
        QVERIFY(language.apply("en"));
        QCOMPARE(receiver.configurationBatchCount, (scenario == 1 || scenario == 2) ? 1 : 0);
        receiver.forceState(ReceiverState::Connected);
        receiver.forceState(ReceiverState::Discoverable);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(persistence.calls, persistence.callbackAt + 1);
        QCOMPARE(receiver.configurationBatchCount, (scenario == 1 || scenario == 2) ? 1 : 0);
    }

    void independentDurableCommitSurvivesOuterSave_data() {
        QTest::addColumn<int>("scenario");
        QTest::addColumn<bool>("beforeWrite");
        for (int scenario = 0; scenario < 4; ++scenario) {
            QTest::newRow(qPrintable(QString("initial-or-compensation-%1-before").arg(scenario))) << scenario << true;
            QTest::newRow(qPrintable(QString("initial-or-compensation-%1-after").arg(scenario))) << scenario << false;
        }
    }
    void independentDurableCommitSurvivesOuterSave() {
        QFETCH(int, scenario);
        QFETCH(bool, beforeWrite);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto current = AppSettings::defaults();
        DurableCallbackPersistence persistence(directory.filePath("settings.json"));
        QVERIFY(persistence.store.save(current).success);
        persistence.callbackAt = scenario == 0 ? 1 : 2;
        persistence.beforeWrite = beforeWrite;
        bool independentlySaved = false;
        persistence.callback = [&] {
            auto independent = persistence.store.loadOrDefaults();
            independent.setVolume(42);
            independent.setAspectRatioLock(false);
            independent.setVideoFitMode(false);
            independent.setShowRecordingCompletionMessage(false);
            independentlySaved = persistence.store.save(independent).success;
            if (independentlySaved) current = independent;
        };
        CallbackReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        if (scenario == 1 || scenario == 2) receiver.requestedConfigurationRestartError = "controlled restart failure";
        SettingsApplyCoordinator coordinator(current, nullptr, &persistence, scenario == 3 ? nullptr : &receiver);
        QSignalSpy finished(&coordinator, &SettingsApplyCoordinator::deferredApplyFinished);
        auto draft = current;
        draft.setReceiverName("Durable target");
        draft.setLanguage("zh-CN");
        const auto submit = coordinator.apply(draft, [scenario] {
            return scenario == 2 ? ReceiverApplyTiming::AfterDisconnect : ReceiverApplyTiming::Immediate;
        });
        QVERIFY(submit.settingsSaved);
        QVERIFY(submit.outcome.has_value());
        auto outcome = *submit.outcome;
        if (scenario == 2) {
            receiver.forceState(ReceiverState::Discoverable);
            QCOMPARE(finished.count(), 1);
            outcome = std::get<SettingsApplyOutcome>(qvariant_cast<SettingsDeferredResult>(finished.at(0).at(0)));
        }
        const auto disk = persistence.store.loadOrDefaults();
        retainDurableObservation(persistence.store, directory.filePath("settings.json"), "success");
        QVERIFY(independentlySaved);
        for (const auto &observed : {current, disk, outcome.committedSettings}) {
            QCOMPARE(observed.volume(), 42);
            QVERIFY(!observed.aspectRatioLock());
            QVERIFY(!observed.videoFitMode());
            QVERIFY(!observed.showRecordingCompletionMessage());
            QCOMPARE(observed.language(), QString("zh-CN"));
            QCOMPARE(observed.receiverName(), scenario == 0 ? QString("Durable target") : QString("AirPlay Receiver"));
        }
        QVERIFY(!outcome.globalResult.has_value());
        QCOMPARE(receiver.configurationBatchCount, scenario == 3 ? 0 : 1);
        if (scenario != 3) {
            QCOMPARE(receiver.configurationBatchRequests.last().requestedReceiverName, QString("Durable target"));
            QCOMPARE(receiver.configurationBatchRequests.last().rollbackReceiverName, QString("AirPlay Receiver"));
        }
    }

    void modalReadyDeliveryQuiescesUntilSubmitExits_data() {
        QTest::addColumn<int>("exitKind");
        QTest::newRow("success-replaces-old-target") << 0;
        QTest::newRow("cancel-preserves-old-target") << 1;
        QTest::newRow("end-invalidates-old-target") << 2;
    }
    void modalReadyDeliveryQuiescesUntilSubmitExits() {
        QFETCH(int, exitKind);
        LifecycleFixture h;
        h.submit("Old saved target");
        MetaCallCounter counter;
        h.coordinator.installEventFilter(&counter);
        auto draft=h.current; draft.setReceiverName("New draft target");
        int firstDelivery=-1, secondDelivery=-1, thirdDelivery=-1, backendWhileBusy=-1;
        const auto result=h.coordinator.apply(draft, [&]() -> std::optional<ReceiverApplyTiming> {
            h.receiver.forceState(ReceiverState::Discoverable);
            QCoreApplication::sendPostedEvents(&h.coordinator, QEvent::MetaCall);
            firstDelivery=counter.deliveries;
            QCoreApplication::sendPostedEvents(&h.coordinator, QEvent::MetaCall);
            secondDelivery=counter.deliveries;
            QCoreApplication::sendPostedEvents(&h.coordinator, QEvent::MetaCall);
            thirdDelivery=counter.deliveries;
            backendWhileBusy=h.receiver.configurationBatchCount;
            if (exitKind==1) return std::nullopt;
            if (exitKind==2) h.coordinator.endReceiverLifecycle();
            return ReceiverApplyTiming::AfterDisconnect;
        });
        QCOMPARE(secondDelivery, firstDelivery);
        QCOMPARE(thirdDelivery, firstDelivery);
        QCOMPARE(backendWhileBusy, 0);
        QCOMPARE(result.status, exitKind==0 ? SettingsSubmitStatus::Completed :
            exitKind==1 ? SettingsSubmitStatus::Cancelled : SettingsSubmitStatus::Interrupted);
        QCOMPARE(h.receiver.configurationBatchCount, exitKind==0 ? 1 : 0);
        QCoreApplication::sendPostedEvents(&h.coordinator, QEvent::MetaCall);
        QCOMPARE(h.receiver.configurationBatchCount, exitKind==2 ? 0 : 1);
        QCOMPARE(h.completed.count(), exitKind==1 ? 1 : 0);
        QCOMPARE(h.current.receiverName(), exitKind==0 ? QString("New draft target") : QString("Old saved target"));
        if (exitKind!=2) QCOMPARE(h.receiver.receiverName(), h.current.receiverName());
        const int afterCompletion=counter.deliveries;
        QCoreApplication::sendPostedEvents(&h.coordinator, QEvent::MetaCall);
        QCOMPARE(counter.deliveries, afterCompletion);
    }
    void deliveredReadyWaitsThroughTimingSelectionRequired() {
        LifecycleFixture h;
        h.submit("Old saved target");
        auto local=h.current; local.setLanguage("zh-CN");
        h.persistence.onSave=[&] { h.receiver.forceState(ReceiverState::Discoverable); };
        h.coordinator.apply(local, {});
        QCOMPARE(h.receiver.configurationBatchCount, 0);
        MetaCallCounter counter;
        h.coordinator.installEventFilter(&counter);
        int firstDelivery=-1, secondDelivery=-1, backendWhileBusy=-1;
        h.persistence.onSave=[&] {
            QCoreApplication::sendPostedEvents(&h.coordinator, QEvent::MetaCall);
            firstDelivery=counter.deliveries;
            QCoreApplication::sendPostedEvents(&h.coordinator, QEvent::MetaCall);
            secondDelivery=counter.deliveries;
            backendWhileBusy=h.receiver.configurationBatchCount;
            h.receiver.forceState(ReceiverState::Connected);
        };
        auto draft=h.current; draft.setReceiverName("New saved target");
        const auto result=h.coordinator.apply(draft, {});
        QCOMPARE(firstDelivery, 1);
        QCOMPARE(secondDelivery, firstDelivery);
        QCOMPARE(backendWhileBusy, 0);
        QCOMPARE(result.status, SettingsSubmitStatus::TimingSelectionRequired);
        QVERIFY(result.settingsSaved);
        QVERIFY(!result.backendInvoked);
        QCoreApplication::sendPostedEvents(&h.coordinator, QEvent::MetaCall);
        QCOMPARE(h.receiver.configurationBatchCount, 0);
        QCOMPARE(h.completed.count(), 0);
        h.coordinator.apply(draft, [] { return ReceiverApplyTiming::AfterDisconnect; });
        h.receiver.forceState(ReceiverState::Discoverable);
        QCOMPARE(h.receiver.configurationBatchCount, 1);
        QCOMPARE(h.receiver.receiverName(), QString("New saved target"));
    }
    void timingSelectionRequiredPreservesPreviouslyPostedOldWork() {
        LifecycleFixture h;
        h.submit("Old saved target");
        auto local=h.current; local.setLanguage("zh-CN");
        h.persistence.onSave=[&] { h.receiver.forceState(ReceiverState::Discoverable); };
        h.coordinator.apply(local, {});
        h.receiver.forceState(ReceiverState::Connected);
        auto draft=h.current; draft.setReceiverName("Unaccepted draft");
        const auto result=h.coordinator.apply(draft, {});
        QCOMPARE(result.status, SettingsSubmitStatus::TimingSelectionRequired);
        QVERIFY(!result.settingsSaved);
        QCoreApplication::sendPostedEvents(&h.coordinator, QEvent::MetaCall);
        QCOMPARE(h.receiver.configurationBatchCount, 0);
        QCOMPARE(h.current.receiverName(), QString("Old saved target"));
        h.receiver.forceState(ReceiverState::Discoverable);
        QCOMPARE(h.receiver.configurationBatchCount, 1);
        QCOMPARE(h.receiver.receiverName(), QString("Old saved target"));
        QCOMPARE(h.completed.count(), 1);
    }
    void presentationCompletionCannotReleaseNewRecordingOrNewEpoch() {
        auto current=AppSettings::defaults();
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        SettingsApplyCoordinator::RecordingPresentationCompletion presentation;
        SettingsApplyCoordinator coordinator(current, nullptr, nullptr, &receiver, nullptr, nullptr,
            [&](auto completion) { presentation = std::move(completion); });
        coordinator.prepareReceiverStart(); receiver.start(); receiver.forceState(ReceiverState::Connected);
        receiver.startRecording({});
        auto draft=current; draft.setReceiverName("First target");
        coordinator.apply(draft, [] { return ReceiverApplyTiming::Immediate; });
        receiver.completeRecordingForTest({});
        QVERIFY(presentation);
        QCOMPARE(receiver.configurationBatchCount, 0);
        const auto oldPresentation=presentation;
        receiver.startRecording({});
        oldPresentation();
        QCOMPARE(receiver.configurationBatchCount, 0);
        receiver.stopRecording(); receiver.completeRecordingForTest({});
        QVERIFY(presentation);
        coordinator.endReceiverLifecycle(); receiver.stop(); coordinator.prepareReceiverStart(); receiver.start();
        receiver.configurationBatchCount=0;
        receiver.forceState(ReceiverState::Connected);
        draft=current; draft.setReceiverName("Next epoch target");
        coordinator.apply(draft, [] { return ReceiverApplyTiming::AfterDisconnect; });
        presentation(); oldPresentation();
        QCOMPARE(receiver.configurationBatchCount, 0);
        receiver.forceState(ReceiverState::Discoverable);
        QCOMPARE(receiver.configurationBatchCount, 1);
        QCOMPARE(receiver.receiverName(), QString("Next epoch target"));
    }
    void destroyingOwnerInvalidatesRetainedPresentationCompletion() {
        auto current=AppSettings::defaults();
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        SettingsApplyCoordinator::RecordingPresentationCompletion presentation;
        auto coordinator=std::make_unique<SettingsApplyCoordinator>(current, nullptr, nullptr, &receiver, nullptr, nullptr,
            [&](auto completion) { presentation=std::move(completion); });
        receiver.startRecording({});
        auto draft=current; draft.setReceiverName("Saved target");
        coordinator->apply(draft, {});
        receiver.completeRecordingForTest({});
        QVERIFY(presentation);
        QCOMPARE(receiver.configurationBatchCount, 0);
        coordinator.reset();
        QMetaObject::invokeMethod(&receiver, presentation, Qt::QueuedConnection);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(receiver.configurationBatchCount, 0);
        QCOMPARE(current.receiverName(), QString("Saved target"));
    }
    void fullWithdrawalDoesNotAskToRestart() {
        LifecycleFixture h;
        h.submit("Pending");
        auto draft=h.current; draft.setReceiverName("AirPlay Receiver");
        int choices=0;
        const auto result=h.coordinator.apply(draft, [&] { ++choices; return ReceiverApplyTiming::Immediate; });
        QCOMPARE(choices, 0);
        QCOMPARE(result.status, SettingsSubmitStatus::Completed);
        QCOMPARE(h.current.receiverName(), QString("AirPlay Receiver"));
        h.receiver.forceState(ReceiverState::Discoverable);
        QCOMPARE(h.receiver.configurationBatchCount, 0);
    }
    void newRecordingBeforeDisconnectStillBlocks() {
        LifecycleFixture h;
        h.submit("Saved target");
        h.receiver.setRecordingAvailableForTest(true);
        h.receiver.startRecording({});
        h.receiver.forceState(ReceiverState::Discoverable);
        QCOMPARE(h.receiver.configurationBatchCount, 0);
        h.receiver.stopRecording();
        h.receiver.completeRecordingForTest({});
        QCOMPARE(h.receiver.configurationBatchCount, 1);
    }
    void unpreparedStartingEndsOldIntent() {
        LifecycleFixture h;
        h.submit("Saved target");
        h.receiver.forceState(ReceiverState::Starting);
        h.receiver.forceState(ReceiverState::Discoverable);
        QCOMPARE(h.receiver.configurationBatchCount, 0);
        QCOMPARE(h.completed.count(), 0);
        QCOMPARE(h.current.receiverName(), QString("Saved target"));
    }
    void interruptedBackendDoesNotClaimUnappliedFieldWasApplied() {
        LifecycleFixture h;
        h.receiver.requestedConfigurationRestartError = "Failure";
        h.receiver.onApply=[&] { h.coordinator.endReceiverLifecycle(); };
        const auto result=h.submit("Saved target", ReceiverApplyTiming::Immediate);
        QCOMPARE(result.status, SettingsSubmitStatus::Interrupted);
        const auto *field=resultForField(result.outcome->fieldResults, SettingsFieldId::receiverName());
        QVERIFY(!field || field->status != SettingsFieldStatus::Applied);
        QCOMPARE(h.current.receiverName(), QString("Saved target"));
    }
    void prepareReportsUnavailableNotIdleAndBackendFailure() {
        auto current=AppSettings::defaults();
        SettingsApplyCoordinator missing(current, nullptr, nullptr, nullptr);
        const auto unavailable=missing.prepareReceiverStart();
        QCOMPARE(unavailable.status, ReceiverStartPreparationStatus::Unavailable);
        QVERIFY(!unavailable.backendResult);
        QVERIFY(!unavailable.userReason.isEmpty());
        FakeAirPlayReceiver receiver;
        SettingsApplyCoordinator coordinator(current, nullptr, nullptr, &receiver);
        receiver.start();
        const auto notIdle=coordinator.prepareReceiverStart();
        QCOMPARE(notIdle.status, ReceiverStartPreparationStatus::NotIdle);
        QVERIFY(!notIdle.backendResult);
        QVERIFY(!notIdle.userReason.isEmpty());
        receiver.stop();
        current.setReceiverName("Rejected");
        receiver.rejectedReceiverNames.append("Rejected");
        const auto failed=coordinator.prepareReceiverStart();
        QCOMPARE(failed.status, ReceiverStartPreparationStatus::BackendFailure);
        QVERIFY(failed.backendResult);
        QVERIFY(!failed.userReason.isEmpty());
        QCOMPARE(receiver.startCount, 1);
    }
    void waitingErrorReportsNotAppliedOnceWithoutCompensation() {
        LifecycleFixture h;
        h.submit("Saved target");
        h.receiver.forceState(ReceiverState::Error);
        h.receiver.forceState(ReceiverState::Error);
        h.receiver.forceState(ReceiverState::Idle);
        QCOMPARE(h.receiver.configurationBatchCount, 0);
        QCOMPARE(h.persistence.count, 1);
        QCOMPARE(h.completed.count(), 1);
        QVERIFY(std::holds_alternative<SettingsDeferredNotApplied>(qvariant_cast<SettingsDeferredResult>(h.completed.at(0).at(0))));
        QCOMPARE(h.current.receiverName(), QString("Saved target"));
        QCOMPARE(h.coordinator.prepareReceiverStart().status, ReceiverStartPreparationStatus::Prepared);
        QCOMPARE(h.receiver.receiverName(), QString("Saved target"));
    }
    void applyingErrorUsesActualBackendResult() {
        for (int failure=0; failure<3; ++failure) {
            LifecycleFixture h;
            if (failure) h.receiver.requestedConfigurationRestartError = "Apply error";
            if (failure==2) h.receiver.rollbackConfigurationRestartError = "Recovery error";
            h.receiver.onApply = [&] {
                h.receiver.forceState(ReceiverState::Error);
                h.receiver.forceState(ReceiverState::Starting);
                h.receiver.forceState(ReceiverState::Idle);
            };
            const auto result = h.submit("New target", ReceiverApplyTiming::Immediate);
            QCOMPARE(result.status, SettingsSubmitStatus::Completed);
            QVERIFY(result.backendInvoked);
            QVERIFY(result.backendResult.has_value());
            QCOMPARE(result.backendResult->status, failure==0 ? ReceiverConfigurationBatchStatus::Applied :
                failure==1 ? ReceiverConfigurationBatchStatus::ApplyFailedRolledBack : ReceiverConfigurationBatchStatus::RecoveryFailed);
            QCOMPARE(h.receiver.configurationBatchCount, 1);
            QCOMPARE(h.completed.count(), 0);
        }
    }
    void reentrantEndSuppressesOldCompensationAndCompletion() {
        LifecycleFixture h;
        h.receiver.requestedConfigurationRestartError = "Apply error";
        h.receiver.onApply = [&] { h.coordinator.endReceiverLifecycle(); };
        const auto result = h.submit("Saved target", ReceiverApplyTiming::Immediate);
        QCOMPARE(result.status, SettingsSubmitStatus::Interrupted);
        QVERIFY(result.settingsSaved);
        QVERIFY(result.backendInvoked);
        QVERIFY(result.backendResult.has_value());
        QCOMPARE(result.backendResult->status, ReceiverConfigurationBatchStatus::ApplyFailedRolledBack);
        QCOMPARE(h.receiver.configurationBatchCount, 1);
        QCOMPARE(h.persistence.count, 1);
        QCOMPARE(h.current.receiverName(), QString("Saved target"));
        QCOMPARE(h.completed.count(), 0);
    }
    void reentrantApplyIsBusyAndCurrentAdoptedBeforeBackend() {
        LifecycleFixture h;
        std::optional<SettingsSubmitResult> nested;
        QString observed;
        h.receiver.onApply = [&] {
            observed = h.current.receiverName();
            auto draft = h.current;
            draft.setLanguage("zh-CN");
            nested = h.coordinator.apply(draft, {});
        };
        h.submit("Saved target", ReceiverApplyTiming::Immediate);
        QCOMPARE(observed, QString("Saved target"));
        QVERIFY(nested.has_value());
        QCOMPARE(nested->status, SettingsSubmitStatus::Busy);
        QVERIFY(!nested->settingsSaved);
        QVERIFY(!nested->backendInvoked);
        QCOMPARE(h.persistence.count, 1);
    }
    void postedContinuationCannotAffectReplacementOrNewEpoch() {
        for (bool newEpoch : {false,true}) {
            LifecycleFixture h;
            h.submit("Old target");
            auto draft = h.current;
            draft.setLanguage("zh-CN");
            h.persistence.onSave = [&] { h.receiver.forceState(ReceiverState::Discoverable); };
            h.coordinator.apply(draft, {});
            QCOMPARE(h.receiver.configurationBatchCount, 0);
            if (newEpoch) {
                h.coordinator.endReceiverLifecycle();
                h.receiver.stop();
                h.coordinator.prepareReceiverStart();
                h.receiver.start();
                h.receiver.configurationBatchCount = 0;
            }
            h.receiver.forceState(ReceiverState::Connected);
            h.submit("Replacement");
            QCoreApplication::sendPostedEvents();
            QCOMPARE(h.receiver.configurationBatchCount, 0);
            QCOMPARE(h.completed.count(), 0);
            h.receiver.forceState(ReceiverState::Discoverable);
            QCOMPARE(h.receiver.configurationBatchCount, 1);
            QCOMPARE(h.receiver.receiverName(), QString("Replacement"));
        }
    }
    void destroyingOwnerDropsPostedContinuation() {
        auto current = AppSettings::defaults();
        ScriptedPersistence persistence;
        FakeAirPlayReceiver receiver;
        auto coordinator = std::make_unique<SettingsApplyCoordinator>(current, nullptr, &persistence, &receiver);
        coordinator->prepareReceiverStart(); receiver.start(); receiver.forceState(ReceiverState::Connected);
        auto draft=current; draft.setReceiverName("Saved target");
        coordinator->apply(draft, [] { return ReceiverApplyTiming::AfterDisconnect; });
        draft=current; draft.setLanguage("zh-CN");
        persistence.onSave = [&] { receiver.forceState(ReceiverState::Discoverable); };
        coordinator->apply(draft, {});
        QCOMPARE(receiver.configurationBatchCount, 0);
        coordinator.reset();
        QCoreApplication::sendPostedEvents();
        QCOMPARE(receiver.configurationBatchCount, 0);
        QCOMPARE(current.receiverName(), QString("Saved target"));
    }
    void timingPromptRevalidatesStateAndPreservesDraft() {
        LifecycleFixture h;
        auto draft=h.current; draft.setReceiverName("Saved target");
        auto result=h.coordinator.apply(draft, [&] {
            h.current.setVolume(42);
            h.receiver.forceState(ReceiverState::Discoverable);
            h.receiver.forceState(ReceiverState::Connected);
            return ReceiverApplyTiming::AfterDisconnect;
        });
        QCOMPARE(result.status, SettingsSubmitStatus::Completed);
        QCOMPARE(h.current.volume(), 42);
        QCOMPARE(h.receiver.configurationBatchCount, 0);
        draft=h.current; draft.setReceiverName("Cancelled");
        result=h.coordinator.apply(draft, [] { return std::optional<ReceiverApplyTiming>{}; });
        QCOMPARE(result.status, SettingsSubmitStatus::Cancelled);
        QCOMPARE(h.current.receiverName(), QString("Saved target"));
        result=h.coordinator.apply(draft, {});
        QCOMPARE(result.status, SettingsSubmitStatus::TimingSelectionRequired);
        result=h.coordinator.apply(draft, [&] { h.coordinator.endReceiverLifecycle(); return ReceiverApplyTiming::Immediate; });
        QCOMPARE(result.status, SettingsSubmitStatus::Interrupted);
        QVERIFY(!result.settingsSaved);
        QCOMPARE(h.persistence.count, 1);
    }
    void connectionDuringSaveRequiresTimingSelection() {
        LifecycleFixture h;
        h.receiver.forceState(ReceiverState::Discoverable);
        auto draft=h.current; draft.setReceiverName("Saved target");
        h.persistence.onSave=[&] { h.receiver.forceState(ReceiverState::Connected); };
        const auto result=h.coordinator.apply(draft, {});
        QCOMPARE(result.status, SettingsSubmitStatus::TimingSelectionRequired);
        QVERIFY(result.settingsSaved);
        QVERIFY(!result.backendInvoked);
        QCOMPARE(h.current.receiverName(), QString("Saved target"));
        h.receiver.forceState(ReceiverState::Discoverable);
        QCOMPARE(h.receiver.configurationBatchCount, 0);
    }
    void explicitApplyCanChooseTimingForAlreadySavedTarget() {
        LifecycleFixture h;
        h.receiver.forceState(ReceiverState::Discoverable);
        auto draft=h.current; draft.setReceiverName("Saved target");
        h.persistence.onSave=[&] { h.receiver.forceState(ReceiverState::Connected); };
        QCOMPARE(h.coordinator.apply(draft, {}).status, SettingsSubmitStatus::TimingSelectionRequired);
        int choices=0;
        const auto result=h.coordinator.apply(draft, [&] { ++choices; return ReceiverApplyTiming::AfterDisconnect; });
        QCOMPARE(choices, 1);
        QCOMPARE(result.status, SettingsSubmitStatus::Completed);
        QCOMPARE(h.receiver.configurationBatchCount, 0);
        h.receiver.forceState(ReceiverState::Discoverable);
        QCOMPARE(h.receiver.configurationBatchCount, 1);
        QCOMPARE(h.receiver.receiverName(), QString("Saved target"));
    }
    void recordingIdleWaitsForTerminalResult() {
        LifecycleFixture h;
        h.receiver.setRecordingAvailableForTest(true);
        h.receiver.startRecording({});
        h.submit("Saved target", ReceiverApplyTiming::Immediate);
        int countAtIdle = -1;
        connect(&h.receiver, &AirPlayReceiver::recordingStateChanged, &h.coordinator, [&](RecordingState state) {
            if (state == RecordingState::Idle) countAtIdle = h.receiver.configurationBatchCount;
        });
        h.receiver.completeRecordingForTest({});
        QCOMPARE(countAtIdle, 0);
        QCOMPARE(h.receiver.configurationBatchCount, 1);
        emit h.receiver.recordingFinished({});
        QCOMPARE(h.receiver.configurationBatchCount, 1);
        QCOMPARE(h.completed.count(), 1);
    }
    void explicitEndPreservesSavedTargetAndRequiresFreshPrepare() {
        auto current = AppSettings::defaults();
        FakeAirPlayReceiver receiver;
        SettingsApplyCoordinator coordinator(current, nullptr, nullptr, &receiver);
        QCOMPARE(coordinator.prepareReceiverStart().status, ReceiverStartPreparationStatus::Prepared);
        receiver.start();
        receiver.startCount = 0;
        receiver.forceState(ReceiverState::Connected);
        QSignalSpy completed(&coordinator, &SettingsApplyCoordinator::deferredApplyFinished);
        auto draft = current;
        draft.setReceiverName("Saved target");
        QCOMPARE(coordinator.apply(draft, [] { return ReceiverApplyTiming::AfterDisconnect; }).status,
                 SettingsSubmitStatus::Completed);
        coordinator.endReceiverLifecycle();
        receiver.stop();
        QCOMPARE(current.receiverName(), QString("Saved target"));
        QCOMPARE(receiver.configurationBatchCount, 0);
        QCOMPARE(completed.count(), 0);
        draft.setReceiverName("Next target");
        coordinator.apply(draft, {});
        QCOMPARE(receiver.configurationBatchCount, 0);
        QCOMPARE(coordinator.prepareReceiverStart().status, ReceiverStartPreparationStatus::Prepared);
        QCOMPARE(receiver.receiverName(), QString("Next target"));
        QCOMPARE(receiver.startCount, 0);
        receiver.start();
        QCOMPARE(receiver.startCount, 1);
    }
};
QTEST_MAIN(DeferredSettingsLifecycleTest)
#include "DeferredSettingsLifecycleTest.moc"
