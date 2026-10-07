#include <QtTest/QtTest>

#include "app/AppSettingsStore.h"
#include "app/SettingsApplyCoordinator.h"
#include "app/SettingsChangeDeferrer.h"
#include "app/UiMessage.h"
#include "backend/FakeAirPlayReceiver.h"
#include "platform/FakeHotkeyService.h"

#include <QHash>
#include <QCoreApplication>
#include <QTranslator>

namespace {

class Task8Translator final : public QTranslator {
public:
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override {
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        if (QString::fromLatin1(context) == QStringLiteral("SettingsApplyCoordinator")
            && QString::fromLatin1(sourceText) == QStringLiteral("Receiver name cannot be empty.")) {
            return QStringLiteral("接收器名称不能为空。");
        }
        if (QString::fromLatin1(context) == QStringLiteral("SettingsApplyCoordinator")
            && QString::fromLatin1(sourceText) == QStringLiteral("Shortcut registration failed: %1")) {
            return QStringLiteral("快捷键注册失败：%1");
        }
        if (QString::fromLatin1(context) == QStringLiteral("SettingsApplyCoordinator")
            && QString::fromLatin1(sourceText) == QStringLiteral("Shortcut registration failed: %1. Previous shortcut was restored.")) {
            return QStringLiteral("快捷键注册失败：%1。已恢复上一个快捷键。");
        }
        if (QString::fromLatin1(context) == QStringLiteral("SettingsApplyCoordinator")
            && QString::fromLatin1(sourceText) == QStringLiteral("Receiver configuration could not be applied: %1")) {
            return QStringLiteral("接收器配置无法应用：%1");
        }
        if (QString::fromLatin1(context) == QStringLiteral("SettingsApplyCoordinator")
            && QString::fromLatin1(sourceText) == QStringLiteral("Receiver restoration failed: %1")) {
            return QStringLiteral("接收器恢复失败：%1");
        }
        if (QString::fromLatin1(context) == QStringLiteral("SettingsApplyCoordinator")
            && QString::fromLatin1(sourceText) == QStringLiteral("Shortcut restoration failed: %1 (native error %2)")) {
            return QStringLiteral("快捷键恢复失败：%1（原生错误 %2）");
        }
        if (QString::fromLatin1(context) == QStringLiteral("SettingsApplyCoordinator")
            && QString::fromLatin1(sourceText) == QStringLiteral("Receiver configuration did not start. The saved state was restored.")) {
            return QStringLiteral("接收器配置未启动。已恢复保存的状态。");
        }
        return {};
    }

};

class CountingPersistence final : public SettingsPersistence {
public:
    AppSettingsSaveResult save(const AppSettings &) const override {
        ++saveCount;
        return {};
    }

    mutable int saveCount = 0;
};

class RecordingSettingsPersistence final : public SettingsPersistence {
public:
    AppSettingsSaveResult save(const AppSettings &settings) const override {
        saved.append(settings);
        const AppSettingsSaveResult result = responses.isEmpty()
            ? AppSettingsSaveResult{true} : responses.takeFirst();
        if (result.success) {
            confirmed = settings;
        }
        return result;
    }

    mutable QVector<AppSettings> saved;
    mutable QVector<AppSettingsSaveResult> responses;
    mutable AppSettings confirmed = AppSettings::defaults();
};

class RecordingHotkeyService final : public HotkeyService {
public:
    struct Call {
        ShortcutAction action;
        QKeySequence sequence;
    };

    using HotkeyService::HotkeyService;

    HotkeyRegistrationResult registerShortcut(ShortcutAction action,
                                              const QKeySequence &sequence) override {
        calls.append({action, sequence});
        const HotkeyRegistrationResult result = responses.isEmpty()
            ? defaultResult(action, sequence)
            : responses.takeFirst();
        const int key = static_cast<int>(action);
        if (result.registered) {
            activeBindings.insert(key, sequence);
        } else if (!result.previousRestored) {
            activeBindings.remove(key);
        }
        return result;
    }

    void unregisterAll() override { ++unregisterAllCount; }

    void seed(const AppSettings &settings) {
        for (const SettingsFieldId &field : allSettingsFields()) {
            if (field.kind == SettingsFieldKind::Shortcut && field.shortcutAction.has_value()) {
                activeBindings.insert(static_cast<int>(*field.shortcutAction),
                                      settings.shortcutFor(*field.shortcutAction));
            }
        }
    }

    QKeySequence active(ShortcutAction action) const {
        return activeBindings.value(static_cast<int>(action));
    }

    QVector<Call> calls;
    QVector<HotkeyRegistrationResult> responses;
    QHash<int, QKeySequence> activeBindings;
    int unregisterAllCount = 0;

private:
    HotkeyRegistrationResult defaultResult(ShortcutAction action,
                                           const QKeySequence &sequence) const {
        return {true, active(action) == sequence};
    }
};

struct ObservedApply : SettingsApplyOutcome { bool timingChosen = false; };
ObservedApply observeApply(const AppSettings &baseline, const AppSettings &candidate, bool active) {
    auto current = baseline;
    FakeAirPlayReceiver receiver;
    receiver.applyReceiverName(baseline.receiverName());
    receiver.applyVideoQuality(baseline.videoQuality());
    SettingsApplyCoordinator coordinator(current, nullptr, nullptr, &receiver);
    if (active) receiver.forceState(ReceiverState::Connected);
    bool chosen = false;
    const auto submitted = coordinator.apply(candidate, [&] { chosen = true; return ReceiverApplyTiming::Immediate; });
    Q_ASSERT(submitted.outcome.has_value());
    ObservedApply result;
    static_cast<SettingsApplyOutcome &>(result) = *submitted.outcome;
    result.timingChosen = chosen;
    return result;
}
void verifyValid(const SettingsApplyOutcome &outcome, const SettingsFieldId &field) {
    const auto *result = resultForField(outcome.fieldResults, field);
    QVERIFY(result);
    QVERIFY(result->status == SettingsFieldStatus::Unchanged || result->status == SettingsFieldStatus::Applied);
    QVERIFY(result->reason.isEmpty());
    QVERIFY(!result->nativeErrorCode.has_value());
    QVERIFY(result->recoveryError.isEmpty());
}
void verifyValidationFailure(const SettingsApplyOutcome &outcome, const SettingsFieldId &field, const QString &reason) {
    const auto *result = resultForField(outcome.fieldResults, field);
    QVERIFY(result);
    QCOMPARE(result->status, SettingsFieldStatus::ValidationFailed);
    QCOMPARE(result->reason, reason);
    QVERIFY(!result->nativeErrorCode.has_value());
    QVERIFY(result->recoveryError.isEmpty());
}

const SettingsFieldResult &requireResult(const SettingsApplyOutcome &outcome,
                                         const SettingsFieldId &field) {
    const SettingsFieldResult *result = resultForField(outcome.fieldResults, field);
    Q_ASSERT(result != nullptr);
    return *result;
}

void verifyStatus(const SettingsApplyOutcome &outcome, const SettingsFieldId &field,
                  SettingsFieldStatus status) {
    QCOMPARE(requireResult(outcome, field).status, status);
}

QVector<ShortcutAction> shortcutActionsInSettingsOrder() {
    QVector<ShortcutAction> actions;
    for (const SettingsFieldId &field : allSettingsFields()) {
        if (field.kind == SettingsFieldKind::Shortcut && field.shortcutAction.has_value()) {
            actions.append(*field.shortcutAction);
        }
    }
    return actions;
}

void seed(FakeHotkeyService *hotkeys, const AppSettings &settings) {
    Q_ASSERT(hotkeys != nullptr);
    for (ShortcutAction action : shortcutActionsInSettingsOrder()) {
        const HotkeyRegistrationResult result = hotkeys->registerShortcut(action,
                                                                           settings.shortcutFor(action));
        Q_ASSERT(result.registered);
    }
    hotkeys->attempts.clear();
    hotkeys->batchRequests.clear();
    hotkeys->unregisteredActions.clear();
}

} // namespace

class CoordinatorHarness {
public:
    CoordinatorHarness() : coordinator(current, nullptr, &persistence, &receiver) {
        Q_ASSERT(current.receiverName() == receiver.receiverName());
        Q_ASSERT(current.videoQuality() == receiver.videoQuality());
        QObject::connect(&coordinator, &SettingsApplyCoordinator::deferredApplyFinished, &receiver,
            [this](const SettingsDeferredResult &result) { completion = std::get<SettingsApplyOutcome>(result); });
    }
    SettingsApplyOutcome submit(const AppSettings &candidate, ReceiverApplyTiming timing) {
        return *coordinator.apply(candidate, [timing] { return timing; }).outcome;
    }
    void disconnectSession() { receiver.forceState(ReceiverState::Discoverable); }
    AppSettings current = AppSettings::defaults();
    RecordingSettingsPersistence persistence;
    FakeAirPlayReceiver receiver;
    SettingsApplyCoordinator coordinator;
    std::optional<SettingsApplyOutcome> completion;
};
class SettingsApplyCoordinatorTest : public QObject {
    Q_OBJECT

private slots:
    void invalidNameWithValidQualityFailurePreservesValidation() {
        for (int failure = 0; failure < 3; ++failure) {
            CoordinatorHarness h;
            h.receiver.forceState(ReceiverState::Connected);
            auto name = h.current;
            name.setReceiverName("Desk Receiver");
            h.submit(name, ReceiverApplyTiming::AfterDisconnect);
            auto candidate = h.current;
            candidate.setReceiverName("");
            candidate.setVideoQuality({VideoResolution::P720, VideoFrameRate::Fps60});
            h.receiver.requestedConfigurationRestartError = "Requested restart failed";
            if (failure == 1) {
                h.receiver.rollbackConfigurationRestartError = "Recovery restart failed";
            } else if (failure == 2) {
                h.persistence.responses.append(AppSettingsSaveResult{true});
                h.persistence.responses.append(
                    {false, "C:/settings.json", AppSettingsSaveStage::Commit,
                     QFileDevice::WriteError, "Compensation commit failed"});
            }
            const auto outcome = h.submit(candidate, ReceiverApplyTiming::Immediate);
            const auto &invalid = requireResult(outcome, SettingsFieldId::receiverName());
            QCOMPARE(invalid.status, SettingsFieldStatus::ValidationFailed);
            QCOMPARE(std::get<QString>(invalid.attemptedValue), QString(""));
            QCOMPARE(invalid.reason, QString("Receiver name cannot be empty."));
            QCOMPARE(h.receiver.configurationBatchCount, 1);
            QCOMPARE(h.receiver.configurationBatchRequests.constLast().rollbackReceiverName,
                     QString("AirPlay Receiver"));
            QCOMPARE(h.current.receiverName(),
                     failure == 0 ? QString("AirPlay Receiver") : QString("Desk Receiver"));
            QCOMPARE(h.current.videoQuality(),
                     failure == 0 ? VideoQualitySettings() : candidate.videoQuality());
            QCOMPARE(h.persistence.confirmed.receiverName(), h.current.receiverName());
            QCOMPARE(h.persistence.confirmed.videoQuality(), h.current.videoQuality());
        }
    }
    void mergedFailureUsesActualBaselineAndLatestOtherSettings_data() {
        QTest::addColumn<bool>("recoveryFails");
        QTest::addColumn<bool>("compensationFails");
        QTest::newRow("restored") << false << false;
        QTest::newRow("recovery-fails") << true << false;
        QTest::newRow("compensation-commit-fails") << false << true;
    }

    void mergedFailureUsesActualBaselineAndLatestOtherSettings() {
        QFETCH(bool, recoveryFails);
        QFETCH(bool, compensationFails);
        CoordinatorHarness h;
        h.receiver.forceState(ReceiverState::Connected);
        auto name = h.current;
        name.setReceiverName("Desk Receiver");
        h.submit(name, ReceiverApplyTiming::AfterDisconnect);
        auto quality = h.current;
        quality.setVideoQuality({VideoResolution::P720, VideoFrameRate::Fps60});
        h.submit(quality, ReceiverApplyTiming::AfterDisconnect);
        auto other = h.current;
        other.setLanguage("zh-CN");
        h.submit(other, ReceiverApplyTiming::Immediate);
        h.current.setVolume(42);
        QVERIFY(h.persistence.save(h.current).success);
        h.receiver.requestedConfigurationRestartError = "Requested restart failed";
        if (recoveryFails) {
            h.receiver.rollbackConfigurationRestartError = "Recovery restart failed";
        }
        if (compensationFails) {
            h.persistence.responses.append({false, "C:/settings.json", AppSettingsSaveStage::Commit,
                                            QFileDevice::WriteError, "Compensation commit failed"});
        }
        h.disconnectSession();
        QVERIFY(h.completion.has_value());
        QCOMPARE(h.receiver.configurationBatchCount, 1);
        QCOMPARE(h.receiver.configurationBatchRequests.constLast().rollbackReceiverName,
                 QString("AirPlay Receiver"));
        QCOMPARE(h.receiver.configurationBatchRequests.constLast().rollbackVideoQuality,
                 VideoQualitySettings());
        QCOMPARE(h.current.volume(), 42);
        QCOMPARE(h.current.language(), QString("zh-CN"));
        if (recoveryFails || compensationFails) {
            QCOMPARE(h.current.receiverName(), QString("Desk Receiver"));
            QCOMPARE(h.current.videoQuality(), quality.videoQuality());
            verifyStatus(*h.completion, SettingsFieldId::receiverName(),
                         SettingsFieldStatus::RecoveryFailed);
        } else {
            QCOMPARE(h.current.receiverName(), QString("AirPlay Receiver"));
            QCOMPARE(h.current.videoQuality(), VideoQualitySettings());
            verifyStatus(*h.completion, SettingsFieldId::receiverName(),
                         SettingsFieldStatus::ApplyFailedRolledBack);
        }
        if (compensationFails) {
            QVERIFY(h.completion->globalResult.has_value());
            QCOMPARE(h.completion->globalResult->status, SettingsApplyGlobalStatus::PersistenceFailed);
        }
        QCOMPARE(h.persistence.confirmed.receiverName(), h.current.receiverName());
        QCOMPARE(h.persistence.confirmed.videoQuality(), h.current.videoQuality());
        QCOMPARE(h.persistence.confirmed.volume(), h.current.volume());
        QCOMPARE(h.persistence.confirmed.language(), h.current.language());
        QCOMPARE(h.receiver.receiverName(), QString("AirPlay Receiver"));
        QCOMPARE(h.receiver.videoQuality(), VideoQualitySettings());
    }

    void mergesSuccessiveNameAndQualityAfterDisconnect() {
        CoordinatorHarness h;
        h.receiver.forceState(ReceiverState::Connected);
        auto name = h.current;
        name.setReceiverName("Desk Receiver");
        h.submit(name, ReceiverApplyTiming::AfterDisconnect);
        auto quality = h.current;
        quality.setVideoQuality({VideoResolution::P720, VideoFrameRate::Fps60});
        h.submit(quality, ReceiverApplyTiming::AfterDisconnect);
        QCOMPARE(h.receiver.configurationBatchCount, 0);
        QCOMPARE(h.persistence.confirmed.receiverName(), QString("Desk Receiver"));
        QCOMPARE(h.persistence.confirmed.videoQuality(), quality.videoQuality());
        h.disconnectSession();
        QCOMPARE(h.receiver.configurationBatchCount, 1);
        QCOMPARE(h.receiver.configurationRestartCount, 1);
        QCOMPARE(h.receiver.receiverName(), QString("Desk Receiver"));
        QCOMPARE(h.receiver.videoQuality(), quality.videoQuality());
        QCOMPARE(h.current.receiverName(), h.receiver.receiverName());
    }

    void latestReceiverTimingControlsMergedBatch() {
        for (bool recording : {false, true}) {
            CoordinatorHarness h;
            h.receiver.forceState(ReceiverState::Connected);
            auto name = h.current;
            name.setReceiverName("Desk Receiver");
            h.submit(name, ReceiverApplyTiming::AfterDisconnect);
            if (recording) {
                h.receiver.setRecordingAvailableForTest(true);
                QVERIFY(h.receiver.startRecording({}).accepted);
            }
            auto quality = h.current;
            quality.setVideoQuality({VideoResolution::P720, VideoFrameRate::Fps60});
            h.submit(quality, ReceiverApplyTiming::Immediate);
            QCOMPARE(h.receiver.configurationBatchCount, recording ? 0 : 1);
            if (recording) {
                QCOMPARE(h.receiver.recordingState(), RecordingState::Finalizing);
                h.receiver.completeRecordingForTest({});
            }
            QCOMPARE(h.receiver.configurationBatchCount, 1);
            QCOMPARE(h.receiver.receiverName(), QString("Desk Receiver"));
            QCOMPARE(h.receiver.videoQuality(), quality.videoQuality());
            QCOMPARE(h.receiver.configurationBatchRequests.constLast().rollbackReceiverName,
                     QString("AirPlay Receiver"));
        }
    }

    void withdrawnFieldsDoNotRestartButOtherFieldsRemainPending() {
        for (int withdrawnField = 0; withdrawnField < 3; ++withdrawnField) {
            CoordinatorHarness h;
            h.receiver.forceState(ReceiverState::Connected);
            auto target = h.current;
            target.setReceiverName("Desk Receiver");
            target.setVideoQuality({VideoResolution::P720, VideoFrameRate::Fps60});
            h.submit(target, ReceiverApplyTiming::AfterDisconnect);
            auto withdrawn = h.current;
            if (withdrawnField == 0) {
                withdrawn.setReceiverName("AirPlay Receiver");
            } else {
                auto quality = withdrawn.videoQuality();
                if (withdrawnField == 1) {
                    quality.resolution = VideoResolution::P1080;
                } else {
                    quality.frameRate = VideoFrameRate::Fps30;
                }
                withdrawn.setVideoQuality(quality);
            }
            h.submit(withdrawn, ReceiverApplyTiming::AfterDisconnect);
            h.disconnectSession();
            QCOMPARE(h.receiver.configurationBatchCount, 1);
            QCOMPARE(h.receiver.configurationRestartCount, 1);
            const auto &batch = h.receiver.configurationBatchRequests.constLast();
            QCOMPARE(batch.receiverNameChanged, withdrawnField != 0);
            QCOMPARE(batch.resolutionChanged, withdrawnField != 1);
            QCOMPARE(batch.frameRateChanged, withdrawnField != 2);
            QCOMPARE(h.receiver.receiverName(), withdrawn.receiverName());
            QCOMPARE(h.receiver.videoQuality(), withdrawn.videoQuality());
        }

        CoordinatorHarness all;
        all.receiver.forceState(ReceiverState::Connected);
        auto changed = all.current;
        changed.setReceiverName("Desk Receiver");
        changed.setVideoQuality({VideoResolution::P720, VideoFrameRate::Fps60});
        all.submit(changed, ReceiverApplyTiming::AfterDisconnect);
        auto original = all.current;
        original.setReceiverName("AirPlay Receiver");
        original.setVideoQuality(VideoQualitySettings());
        all.submit(original, ReceiverApplyTiming::AfterDisconnect);
        QCOMPARE(all.receiver.configurationBatchCount, 0);
        all.disconnectSession();
        QCOMPARE(all.receiver.configurationBatchCount, 0);
    }
    void nonReceiverInvalidAndSaveFailurePreservePendingGates() {
        for (AppSettingsSaveStage stage : {AppSettingsSaveStage::Open,
                                           AppSettingsSaveStage::Write,
                                           AppSettingsSaveStage::Commit}) {
            CoordinatorHarness h;
            h.receiver.forceState(ReceiverState::Connected);
            auto name = h.current;
            name.setReceiverName("Desk Receiver");
            h.submit(name, ReceiverApplyTiming::AfterDisconnect);
            auto language = h.current;
            language.setLanguage("zh-CN");
            h.submit(language, ReceiverApplyTiming::Immediate);
            auto invalid = h.current;
            invalid.setReceiverName("");
            const auto invalidOutcome = h.submit(invalid, ReceiverApplyTiming::Immediate);
            verifyStatus(invalidOutcome, SettingsFieldId::receiverName(),
                         SettingsFieldStatus::ValidationFailed);
            auto failed = h.current;
            failed.setReceiverName("Failed Receiver");
            h.persistence.responses.append({false, "C:/settings.json", stage,
                                            QFileDevice::WriteError, "Exact save failure"});
            QVERIFY(h.submit(failed, ReceiverApplyTiming::Immediate).globalResult.has_value());
            QCOMPARE(h.receiver.configurationBatchCount, 0);
            h.disconnectSession();
            QCOMPARE(h.receiver.configurationBatchCount, 1);
            QCOMPARE(h.receiver.receiverName(), QString("Desk Receiver"));
            QCOMPARE(h.current.language(), QString("zh-CN"));
            QCOMPARE(h.persistence.confirmed.receiverName(), QString("Desk Receiver"));
        }
    }

    void invalidNameWithValidQualityPreservesValidationAndPriorTarget() {
        CoordinatorHarness h;
        h.receiver.forceState(ReceiverState::Connected);
        auto name = h.current;
        name.setReceiverName("Desk Receiver");
        h.submit(name, ReceiverApplyTiming::AfterDisconnect);
        auto candidate = h.current;
        candidate.setReceiverName("");
        candidate.setVideoQuality({VideoResolution::P720, VideoFrameRate::Fps60});
        const auto outcome = h.submit(candidate, ReceiverApplyTiming::AfterDisconnect);
        const auto &invalid = requireResult(outcome, SettingsFieldId::receiverName());
        QCOMPARE(invalid.status, SettingsFieldStatus::ValidationFailed);
        QCOMPARE(std::get<QString>(invalid.attemptedValue), QString(""));
        QCOMPARE(invalid.reason, QString("Receiver name cannot be empty."));
        QCOMPARE(h.current.receiverName(), QString("Desk Receiver"));
        h.disconnectSession();
        QCOMPARE(h.receiver.configurationBatchCount, 1);
        QCOMPARE(h.receiver.receiverName(), QString("Desk Receiver"));
        QCOMPARE(h.receiver.videoQuality(), candidate.videoQuality());
    }

    void emptyReceiverNameDoesNotInvalidateFrameRate() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("   ");
        VideoQualitySettings quality = candidate.videoQuality();
        quality.frameRate = VideoFrameRate::Fps60;
        candidate.setVideoQuality(quality);

        const ObservedApply plan = observeApply(baseline, candidate, true);

        QCOMPARE(plan.fieldResults.size(), 15);
        verifyValidationFailure(plan, SettingsFieldId::receiverName(),
                                QStringLiteral("Receiver name cannot be empty."));
        verifyValid(plan, SettingsFieldId::language());
        verifyValid(plan, SettingsFieldId::videoFrameRate());
        QVERIFY(plan.timingChosen);
    }

    void duplicateShortcutMarksBothRowsInvalid() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        const QKeySequence duplicate("Ctrl+Alt+B");
        candidate.setShortcut(ShortcutAction::ToggleToolbar, duplicate);
        candidate.setShortcut(ShortcutAction::ToggleRecording, duplicate);

        const ObservedApply plan = observeApply(baseline, candidate, false);

        const QString reason = QStringLiteral("Shortcut Ctrl+Alt+B is assigned to more than one action.");
        verifyValidationFailure(plan, SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar), reason);
        verifyValidationFailure(plan, SettingsFieldId::shortcut(ShortcutAction::ToggleRecording), reason);
        QCOMPARE(std::get<QKeySequence>(requireResult(plan, SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar)).attemptedValue), duplicate);
        QCOMPARE(std::get<QKeySequence>(requireResult(plan, SettingsFieldId::shortcut(ShortcutAction::ToggleRecording)).attemptedValue), duplicate);
        for (ShortcutAction action : {ShortcutAction::ToggleAlwaysOnTop, ShortcutAction::VolumeUp,
                                      ShortcutAction::VolumeDown, ShortcutAction::ToggleAspectRatio,
                                      ShortcutAction::ToggleVideoFit}) {
            verifyValid(plan, SettingsFieldId::shortcut(action));
        }
    }

    void duplicateMultiCombinationKeepsIntrinsicReason() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        const QKeySequence multipleCombinations("Ctrl+A, Ctrl+B");
        candidate.setShortcut(ShortcutAction::ToggleToolbar, multipleCombinations);
        candidate.setShortcut(ShortcutAction::ToggleRecording, multipleCombinations);

        const ObservedApply plan = observeApply(baseline, candidate, false);

        const QString reason = QStringLiteral("Shortcut must use a single key combination.");
        verifyValidationFailure(plan, SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar), reason);
        verifyValidationFailure(plan, SettingsFieldId::shortcut(ShortcutAction::ToggleRecording), reason);
    }

    void duplicateUnsupportedKeepsIntrinsicReason() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        const QKeySequence unsupported(Qt::CTRL | Qt::Key_Pause);
        candidate.setShortcut(ShortcutAction::ToggleToolbar, unsupported);
        candidate.setShortcut(ShortcutAction::ToggleRecording, unsupported);

        const ObservedApply plan = observeApply(baseline, candidate, false);

        const QString reason = QStringLiteral("Shortcut is not supported as a Windows global hotkey.");
        verifyValidationFailure(plan, SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar), reason);
        verifyValidationFailure(plan, SettingsFieldId::shortcut(ShortcutAction::ToggleRecording), reason);
    }

    void multipleValidationFailuresAreAllReported() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName({});
        candidate.setShortcut(ShortcutAction::ToggleAlwaysOnTop, {});
        candidate.setShortcut(ShortcutAction::VolumeUp, QKeySequence("Ctrl+A, Ctrl+B"));
        candidate.setShortcut(ShortcutAction::VolumeDown, QKeySequence(Qt::CTRL | Qt::Key_Pause));
        const QKeySequence duplicate("Ctrl+Alt+B");
        candidate.setShortcut(ShortcutAction::ToggleToolbar, duplicate);
        candidate.setShortcut(ShortcutAction::ToggleRecording, duplicate);

        const ObservedApply plan = observeApply(baseline, candidate, false);

        verifyValidationFailure(plan, SettingsFieldId::receiverName(),
                                QStringLiteral("Receiver name cannot be empty."));
        verifyValidationFailure(plan, SettingsFieldId::shortcut(ShortcutAction::ToggleAlwaysOnTop),
                                QStringLiteral("Shortcut cannot be empty."));
        verifyValidationFailure(plan, SettingsFieldId::shortcut(ShortcutAction::VolumeUp),
                                QStringLiteral("Shortcut must use a single key combination."));
        verifyValidationFailure(plan, SettingsFieldId::shortcut(ShortcutAction::VolumeDown),
                                QStringLiteral("Shortcut is not supported as a Windows global hotkey."));
        const QString duplicateReason = QStringLiteral("Shortcut Ctrl+Alt+B is assigned to more than one action.");
        verifyValidationFailure(plan, SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar), duplicateReason);
        verifyValidationFailure(plan, SettingsFieldId::shortcut(ShortcutAction::ToggleRecording), duplicateReason);
        verifyValid(plan, SettingsFieldId::language());
        verifyValid(plan, SettingsFieldId::videoResolution());
        verifyValid(plan, SettingsFieldId::videoFrameRate());
        verifyValid(plan, SettingsFieldId::shortcut(ShortcutAction::ToggleAspectRatio));
        verifyValid(plan, SettingsFieldId::shortcut(ShortcutAction::ToggleVideoFit));
        verifyValid(plan, SettingsFieldId::recordingFormat());
        verifyValid(plan, SettingsFieldId::recordingOutputDirectory());
        verifyValid(plan, SettingsFieldId::recordingCompletionNotification());
    }

    void unchangedOrInvalidReceiverFieldsDoNotRequestTimingDecision() {
        const auto baseline=AppSettings::defaults();
        QVERIFY(!observeApply(baseline, baseline, true).timingChosen);
        auto invalid=baseline; invalid.setReceiverName({});
        const auto result=observeApply(baseline, invalid, true);
        QVERIFY(!result.timingChosen);
        verifyStatus(result, SettingsFieldId::receiverName(), SettingsFieldStatus::ValidationFailed);
    }

    void oneValidChangedReceiverFieldRequestsTimingDecisionDuringSession() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        VideoQualitySettings quality = candidate.videoQuality();
        quality.resolution = VideoResolution::P720;
        candidate.setVideoQuality(quality);

        const ObservedApply plan = observeApply(baseline, candidate, true);

        QVERIFY(plan.timingChosen);
    }

    void validReceiverChangeDoesNotRequestDecisionWhenSessionInactive() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("Desk Receiver");

        const ObservedApply plan = observeApply(baseline, candidate, false);

        QVERIFY(!plan.timingChosen);
    }

    void invalidReceiverFieldAlongsideValidChangeStillRequestsDecision() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName({});
        VideoQualitySettings quality = candidate.videoQuality();
        quality.frameRate = VideoFrameRate::Fps60;
        candidate.setVideoQuality(quality);

        const ObservedApply plan = observeApply(baseline, candidate, true);

        QVERIFY(plan.timingChosen);
    }

    void languageOnlyChangeDoesNotRequestReceiverTimingDecision() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setLanguage("zh-CN");

        const ObservedApply plan = observeApply(baseline, candidate, true);

        QVERIFY(!plan.timingChosen);
        verifyValid(plan, SettingsFieldId::language());
    }

    void languagePersistsAsIndependentField() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setLanguage("zh-CN");
        RecordingSettingsPersistence persistence;
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, &persistence, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        QCOMPARE(persistence.saved.size(), 1);
        QCOMPARE(persistence.saved.constFirst().language(), QString("zh-CN"));
        QCOMPARE(outcome.committedSettings.language(), QString("zh-CN"));
        verifyStatus(outcome, SettingsFieldId::language(), SettingsFieldStatus::Applied);
        QVERIFY(outcome.mayClose);
    }

    void languagePersistenceFailureRollsBackCommittedLanguage() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setLanguage("zh-CN");
        RecordingSettingsPersistence persistence;
        persistence.responses.append({false, "C:/settings.json", AppSettingsSaveStage::Commit,
                                      QFileDevice::WriteError, "Disk full"});
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, &persistence, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        QCOMPARE(persistence.saved.size(), 1);
        QCOMPARE(outcome.committedSettings.language(), QString("system"));
        verifyStatus(outcome, SettingsFieldId::language(),
                     SettingsFieldStatus::ApplyFailedRolledBack);
        QVERIFY(outcome.globalResult.has_value());
        QVERIFY(!outcome.mayClose);
    }

    void toolbarHoverRevealAppliesLocallyDuringReceiverSession() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setToolbarHoverReveal(false);
        RecordingSettingsPersistence persistence;
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, &persistence, nullptr);

        const ObservedApply plan = observeApply(baseline, candidate, true);
        QVERIFY(!plan.timingChosen);
        verifyValid(plan, SettingsFieldId::toolbarHoverReveal());
        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        QCOMPARE(persistence.saved.size(), 1);
        QVERIFY(!persistence.saved.constFirst().toolbarHoverReveal());
        QVERIFY(!outcome.committedSettings.toolbarHoverReveal());
        verifyStatus(outcome, SettingsFieldId::toolbarHoverReveal(),
                     SettingsFieldStatus::Applied);
        QVERIFY(outcome.mayClose);
    }

    void toolbarHoverRevealSaveFailureKeepsCommittedValue() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setToolbarHoverReveal(false);
        RecordingSettingsPersistence persistence;
        persistence.responses.append({false, "C:/settings.json", AppSettingsSaveStage::Commit,
                                      QFileDevice::WriteError, "Disk full"});
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, &persistence, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        QCOMPARE(persistence.saved.size(), 1);
        QVERIFY(outcome.committedSettings.toolbarHoverReveal());
        verifyStatus(outcome, SettingsFieldId::toolbarHoverReveal(),
                     SettingsFieldStatus::ApplyFailedRolledBack);
        QVERIFY(outcome.globalResult.has_value());
        QVERIFY(!outcome.mayClose);
    }

    void recordingIdleControlsApplyGate() {
        for (auto state : {RecordingState::Idle, RecordingState::Recording, RecordingState::Finalizing}) {
            CoordinatorHarness h;
            if (state != RecordingState::Idle) {
                h.receiver.setRecordingAvailableForTest(true);
                h.receiver.startRecording({});
                if (state == RecordingState::Finalizing) h.receiver.stopRecording();
            }
            auto draft=h.current; draft.setReceiverName("New name");
            h.submit(draft, ReceiverApplyTiming::Immediate);
            QCOMPARE(h.receiver.configurationBatchCount, state == RecordingState::Idle ? 1 : 0);
            QCOMPARE(h.receiver.stopRecordingCount, state == RecordingState::Idle ? 0 : 1);
        }
    }

    void validationOrderAndAttemptedValuesCoverEveryFieldType() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("Desk Receiver");
        candidate.setLanguage("zh-CN");
        candidate.setVideoQuality({VideoResolution::P720, VideoFrameRate::Fps60});
        candidate.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+Y"));
        candidate.setRecordingOutputDirectory("C:/Temp/AirPlay");
        candidate.setShowRecordingCompletionMessage(false);

        const ObservedApply plan = observeApply(baseline, candidate, false);

        QCOMPARE(baseline.receiverName(), QString("AirPlay Receiver"));
        QCOMPARE(plan.committedSettings.receiverName(), candidate.receiverName());
        QCOMPARE(plan.timingChosen, false);
        QCOMPARE(plan.fieldResults.size(), allSettingsFields().size());
        const QVector<SettingsFieldId> fields = allSettingsFields();
        for (qsizetype index = 0; index < plan.fieldResults.size(); ++index) {
            const SettingsFieldId &field = fields.at(index);
            QCOMPARE(plan.fieldResults.at(index).field, field);
            QVERIFY(plan.fieldResults.at(index).attemptedValue
                    == settingsFieldValue(candidate, field));
        }
        QVERIFY(std::holds_alternative<QString>(plan.fieldResults.at(0).attemptedValue));
        QCOMPARE(std::get<QString>(plan.fieldResults.at(1).attemptedValue), QString("zh-CN"));
        QVERIFY(std::holds_alternative<VideoResolution>(plan.fieldResults.at(2).attemptedValue));
        QVERIFY(std::holds_alternative<VideoFrameRate>(plan.fieldResults.at(3).attemptedValue));
        QVERIFY(std::holds_alternative<QKeySequence>(plan.fieldResults.at(4).attemptedValue));
        QVERIFY(std::holds_alternative<RecordingFormat>(plan.fieldResults.at(11).attemptedValue));
        QVERIFY(std::holds_alternative<QString>(plan.fieldResults.at(12).attemptedValue));
        QVERIFY(std::holds_alternative<bool>(plan.fieldResults.at(13).attemptedValue));
    }

    void cancelledTimingChoiceInvokesNoApplyDependencies() {
        auto current = AppSettings::defaults();
        FakeHotkeyService hotkeys;
        CountingPersistence persistence;
        FakeAirPlayReceiver receiver;
        SettingsApplyCoordinator coordinator(current, &hotkeys, &persistence, &receiver);
        receiver.forceState(ReceiverState::Connected);
        auto draft=current; draft.setReceiverName("Cancelled");
        const auto result=coordinator.apply(draft, [&] {
            Q_ASSERT(hotkeys.attempts.isEmpty() && persistence.saveCount == 0 && receiver.configurationBatchCount == 0);
            return std::optional<ReceiverApplyTiming>{};
        });
        QCOMPARE(result.status, SettingsSubmitStatus::Cancelled);
        QVERIFY(hotkeys.attempts.isEmpty());
        QCOMPARE(persistence.saveCount, 0);
        QCOMPARE(receiver.configurationBatchCount, 0);
        SettingsApplyCoordinator nullCoordinator(current, nullptr, nullptr, nullptr);
        QCOMPARE(nullCoordinator.apply(current, {}).outcome->fieldResults.size(), 15);
    }

    void oneShortcutFailureDoesNotRollbackAnotherShortcut() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        const QKeySequence alwaysOnTop("Ctrl+Alt+Y");
        const QKeySequence volumeUp("Ctrl+Alt+U");
        candidate.setShortcut(ShortcutAction::ToggleAlwaysOnTop, alwaysOnTop);
        candidate.setShortcut(ShortcutAction::VolumeUp, volumeUp);
        RecordingHotkeyService hotkeys;
        hotkeys.seed(baseline);
        hotkeys.responses = {{true, false},
                              {false, false, HotkeyError{123, "Candidate rejected"}, true}};
        RecordingSettingsPersistence persistence;
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, &hotkeys, &persistence, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        QCOMPARE(persistence.saved.size(), 1);
        QCOMPARE(persistence.saved.first().shortcutFor(ShortcutAction::ToggleAlwaysOnTop), alwaysOnTop);
        QCOMPARE(persistence.saved.first().shortcutFor(ShortcutAction::VolumeUp),
                 baseline.shortcutFor(ShortcutAction::VolumeUp));
        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::ToggleAlwaysOnTop), alwaysOnTop);
        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::VolumeUp),
                 baseline.shortcutFor(ShortcutAction::VolumeUp));
        verifyStatus(outcome, SettingsFieldId::shortcut(ShortcutAction::ToggleAlwaysOnTop),
                     SettingsFieldStatus::Applied);
        verifyStatus(outcome, SettingsFieldId::shortcut(ShortcutAction::VolumeUp),
                     SettingsFieldStatus::ApplyFailedRolledBack);
        QVERIFY(requireResult(outcome, SettingsFieldId::shortcut(ShortcutAction::VolumeUp))
                    .reason.contains("previous shortcut was restored"));
        QCOMPARE(hotkeys.active(ShortcutAction::ToggleAlwaysOnTop), alwaysOnTop);
        QVERIFY(!outcome.mayClose);
    }

    void successfulShortcutSwapCommitsBothFields() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setShortcut(ShortcutAction::ToggleToolbar,
                              baseline.shortcutFor(ShortcutAction::VolumeUp));
        candidate.setShortcut(ShortcutAction::VolumeUp,
                              baseline.shortcutFor(ShortcutAction::ToggleToolbar));
        FakeHotkeyService hotkeys;
        seed(&hotkeys, baseline);
        RecordingSettingsPersistence persistence;
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, &hotkeys, &persistence, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        QCOMPARE(outcome.fieldResults.size(), allSettingsFields().size());
        for (qsizetype index = 0; index < outcome.fieldResults.size(); ++index) {
            QCOMPARE(outcome.fieldResults.at(index).field, allSettingsFields().at(index));
        }
        verifyStatus(outcome, SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar),
                     SettingsFieldStatus::Applied);
        verifyStatus(outcome, SettingsFieldId::shortcut(ShortcutAction::VolumeUp),
                     SettingsFieldStatus::Applied);
        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::ToggleToolbar),
                 candidate.shortcutFor(ShortcutAction::ToggleToolbar));
        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::VolumeUp),
                 candidate.shortcutFor(ShortcutAction::VolumeUp));
        QCOMPARE(hotkeys.activeBindings.value(static_cast<int>(ShortcutAction::ToggleToolbar)),
                 candidate.shortcutFor(ShortcutAction::ToggleToolbar));
        QCOMPARE(hotkeys.activeBindings.value(static_cast<int>(ShortcutAction::VolumeUp)),
                 candidate.shortcutFor(ShortcutAction::VolumeUp));
        QCOMPARE(hotkeys.unregisterAllCount, 0);
        QCOMPARE(persistence.saved.size(), 1);
    }

    void failedAcyclicShortcutOwnerPropagatesFailureWithoutPersistingDependentCandidate() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setShortcut(ShortcutAction::ToggleToolbar,
                              baseline.shortcutFor(ShortcutAction::VolumeUp));
        candidate.setShortcut(ShortcutAction::VolumeUp, QKeySequence("Ctrl+Alt+C"));
        candidate.setShortcut(ShortcutAction::VolumeDown, QKeySequence("Ctrl+Alt+D"));
        FakeHotkeyService hotkeys;
        seed(&hotkeys, baseline);
        hotkeys.reject(ShortcutAction::VolumeUp,
                       candidate.shortcutFor(ShortcutAction::VolumeUp), 1409,
                       "Volume Up candidate rejected");
        hotkeys.reject(ShortcutAction::VolumeUp,
                       baseline.shortcutFor(ShortcutAction::VolumeUp), 1409,
                       "Volume Up restoration rejected");
        RecordingSettingsPersistence persistence;
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, &hotkeys, &persistence, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        QCOMPARE(persistence.saved.size(), 1);
        const AppSettings &saved = persistence.saved.constFirst();
        QCOMPARE(saved.shortcutFor(ShortcutAction::ToggleToolbar),
                 baseline.shortcutFor(ShortcutAction::ToggleToolbar));
        QCOMPARE(saved.shortcutFor(ShortcutAction::VolumeUp),
                 baseline.shortcutFor(ShortcutAction::VolumeUp));
        QCOMPARE(saved.shortcutFor(ShortcutAction::VolumeDown),
                 candidate.shortcutFor(ShortcutAction::VolumeDown));
        QVERIFY(saved.shortcutFor(ShortcutAction::ToggleToolbar)
                != saved.shortcutFor(ShortcutAction::VolumeUp));
        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::ToggleToolbar),
                 saved.shortcutFor(ShortcutAction::ToggleToolbar));
        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::VolumeUp),
                 saved.shortcutFor(ShortcutAction::VolumeUp));
        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::VolumeDown),
                 saved.shortcutFor(ShortcutAction::VolumeDown));

        const SettingsFieldResult &dependent = requireResult(
            outcome, SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar));
        QCOMPARE(dependent.status, SettingsFieldStatus::ApplyFailedRolledBack);
        QVERIFY(dependent.reason.contains("could not move"));
        QVERIFY(dependent.reason.contains("previous shortcut was restored"));
        const SettingsFieldResult &owner = requireResult(
            outcome, SettingsFieldId::shortcut(ShortcutAction::VolumeUp));
        QCOMPARE(owner.status, SettingsFieldStatus::RecoveryFailed);
        QVERIFY(owner.reason.contains("Volume Up candidate rejected"));
        QVERIFY(owner.recoveryError.contains("Volume Up restoration rejected"));
        QCOMPARE(owner.nativeErrorCode, std::optional<quint32>{1409});
        verifyStatus(outcome, SettingsFieldId::shortcut(ShortcutAction::VolumeDown),
                     SettingsFieldStatus::Applied);

        QCOMPARE(hotkeys.activeBindings.value(static_cast<int>(ShortcutAction::ToggleToolbar)),
                 baseline.shortcutFor(ShortcutAction::ToggleToolbar));
        QVERIFY(!hotkeys.activeBindings.contains(static_cast<int>(ShortcutAction::VolumeUp)));
        QCOMPARE(hotkeys.activeBindings.value(static_cast<int>(ShortcutAction::VolumeDown)),
                 candidate.shortcutFor(ShortcutAction::VolumeDown));
        QVERIFY(!outcome.mayClose);
    }

    void failedShortcutCycleRollsBackOnlyCycleFields() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setShortcut(ShortcutAction::ToggleToolbar,
                              baseline.shortcutFor(ShortcutAction::VolumeUp));
        candidate.setShortcut(ShortcutAction::VolumeUp,
                              baseline.shortcutFor(ShortcutAction::ToggleToolbar));
        candidate.setShortcut(ShortcutAction::VolumeDown, QKeySequence("Ctrl+Alt+D"));
        FakeHotkeyService hotkeys;
        seed(&hotkeys, baseline);
        hotkeys.reject(ShortcutAction::VolumeUp,
                       candidate.shortcutFor(ShortcutAction::VolumeUp));
        RecordingSettingsPersistence persistence;
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, &hotkeys, &persistence, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        verifyStatus(outcome, SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar),
                     SettingsFieldStatus::ApplyFailedRolledBack);
        verifyStatus(outcome, SettingsFieldId::shortcut(ShortcutAction::VolumeUp),
                     SettingsFieldStatus::ApplyFailedRolledBack);
        verifyStatus(outcome, SettingsFieldId::shortcut(ShortcutAction::VolumeDown),
                     SettingsFieldStatus::Applied);
        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::ToggleToolbar),
                 baseline.shortcutFor(ShortcutAction::ToggleToolbar));
        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::VolumeUp),
                 baseline.shortcutFor(ShortcutAction::VolumeUp));
        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::VolumeDown),
                 candidate.shortcutFor(ShortcutAction::VolumeDown));
        QCOMPARE(hotkeys.activeBindings.value(static_cast<int>(ShortcutAction::VolumeDown)),
                 candidate.shortcutFor(ShortcutAction::VolumeDown));
    }

    void persistenceFailureRestoresSuccessfulShortcutSwap() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setShortcut(ShortcutAction::ToggleToolbar,
                              baseline.shortcutFor(ShortcutAction::VolumeUp));
        candidate.setShortcut(ShortcutAction::VolumeUp,
                              baseline.shortcutFor(ShortcutAction::ToggleToolbar));
        FakeHotkeyService hotkeys;
        seed(&hotkeys, baseline);
        RecordingSettingsPersistence persistence;
        persistence.responses.append({false, "C:/settings.json", AppSettingsSaveStage::Commit,
                                      QFileDevice::WriteError, "Disk full"});
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, &hotkeys, &persistence, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        QVERIFY(outcome.globalResult.has_value());
        verifyStatus(outcome, SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar),
                     SettingsFieldStatus::ApplyFailedRolledBack);
        verifyStatus(outcome, SettingsFieldId::shortcut(ShortcutAction::VolumeUp),
                     SettingsFieldStatus::ApplyFailedRolledBack);
        QCOMPARE(hotkeys.batchRequests.size(), 2);
        QCOMPARE(hotkeys.batchRequests.at(0).size(), shortcutActionsInSettingsOrder().size());
        QCOMPARE(hotkeys.batchRequests.at(1).size(), 2);
        QCOMPARE(hotkeys.batchRequests.at(1).at(0).action, ShortcutAction::ToggleToolbar);
        QCOMPARE(hotkeys.batchRequests.at(1).at(0).sequence,
                 baseline.shortcutFor(ShortcutAction::ToggleToolbar));
        QCOMPARE(hotkeys.batchRequests.at(1).at(1).action, ShortcutAction::VolumeUp);
        QCOMPARE(hotkeys.batchRequests.at(1).at(1).sequence,
                 baseline.shortcutFor(ShortcutAction::VolumeUp));
        QCOMPARE(hotkeys.activeBindings.value(static_cast<int>(ShortcutAction::ToggleToolbar)),
                 baseline.shortcutFor(ShortcutAction::ToggleToolbar));
        QCOMPARE(hotkeys.activeBindings.value(static_cast<int>(ShortcutAction::VolumeUp)),
                 baseline.shortcutFor(ShortcutAction::VolumeUp));
    }

    void failedShortcutKeepsBaselineWhileRecordingFieldsCommit() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+Y"));
        candidate.setRecordingOutputDirectory("C:/AirPlay-recordings");
        candidate.setShowRecordingCompletionMessage(false);
        VideoQualitySettings quality = candidate.videoQuality();
        quality.frameRate = VideoFrameRate::Fps60;
        candidate.setVideoQuality(quality);
        RecordingHotkeyService hotkeys;
        hotkeys.seed(baseline);
        hotkeys.responses = {{false, false, HotkeyError{321, "Denied"}, true}};
        RecordingSettingsPersistence persistence;
        FakeAirPlayReceiver receiver;
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, &hotkeys, &persistence, &receiver);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::ToggleAlwaysOnTop),
                 baseline.shortcutFor(ShortcutAction::ToggleAlwaysOnTop));
        QCOMPARE(outcome.committedSettings.videoQuality().frameRate, VideoFrameRate::Fps60);
        QCOMPARE(outcome.committedSettings.recordingOutputDirectory(),
                 candidate.recordingOutputDirectory());
        QCOMPARE(outcome.committedSettings.showRecordingCompletionMessage(), false);
        QCOMPARE(receiver.configurationBatchCount, 1);
        QCOMPARE(receiver.videoQuality(), candidate.videoQuality());
        verifyStatus(outcome, SettingsFieldId::videoFrameRate(), SettingsFieldStatus::Applied);
        verifyStatus(outcome, SettingsFieldId::recordingOutputDirectory(),
                     SettingsFieldStatus::Applied);
        QVERIFY(!outcome.mayClose);
    }

    void candidateAndRestorationFailureReportsRecoveryFailed() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+Y"));
        RecordingHotkeyService hotkeys;
        hotkeys.seed(baseline);
        hotkeys.responses = {{false, false, HotkeyError{87, "Candidate rejected"}, false,
                              HotkeyError{88, "Restore rejected"}}};
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, &hotkeys, nullptr, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;
        const SettingsFieldResult &result = requireResult(
            outcome, SettingsFieldId::shortcut(ShortcutAction::ToggleAlwaysOnTop));

        QCOMPARE(result.status, SettingsFieldStatus::RecoveryFailed);
        QCOMPARE(result.nativeErrorCode, std::optional<quint32>(87));
        QVERIFY(result.reason.contains("Candidate rejected"));
        QVERIFY(result.recoveryError.contains("previous shortcut could not be restored"));
        QVERIFY(result.recoveryError.contains("no confirmed global shortcut"));
        QVERIFY(result.recoveryError.contains("Restore rejected"));
        QVERIFY(result.recoveryError.contains("88"));
        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::ToggleAlwaysOnTop),
                 baseline.shortcutFor(ShortcutAction::ToggleAlwaysOnTop));
        QVERIFY(!outcome.mayClose);
    }

    void executeProcessesAllSevenValidShortcutRowsIncludingUnchanged() {
        const AppSettings baseline = AppSettings::defaults();
        RecordingHotkeyService hotkeys;
        hotkeys.seed(baseline);
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, &hotkeys, nullptr, nullptr);

        *coordinator.apply(baseline, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        const QVector<ShortcutAction> expected = shortcutActionsInSettingsOrder();
        QCOMPARE(hotkeys.calls.size(), expected.size());
        for (qsizetype index = 0; index < expected.size(); ++index) {
            QCOMPARE(hotkeys.calls.at(index).action, expected.at(index));
            QCOMPARE(hotkeys.calls.at(index).sequence, baseline.shortcutFor(expected.at(index)));
        }
        QCOMPARE(hotkeys.unregisterAllCount, 0);
    }

    void secondApplyProcessesEveryShortcutAgain() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+Y"));
        RecordingHotkeyService hotkeys;
        hotkeys.seed(baseline);
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, &hotkeys, nullptr, nullptr);

        const SettingsApplyOutcome first = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;
        *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        QCOMPARE(hotkeys.calls.size(), 14);
        QCOMPARE(hotkeys.calls.at(7).action, ShortcutAction::ToggleAlwaysOnTop);
    }

    void multipleValidationFailuresDoNotPreventUnrelatedPersistence() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName({});
        candidate.setShortcut(ShortcutAction::VolumeUp, {});
        candidate.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+Y"));
        candidate.setRecordingOutputDirectory("C:/AirPlay-recordings");
        RecordingHotkeyService hotkeys;
        hotkeys.seed(baseline);
        RecordingSettingsPersistence persistence;
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, &hotkeys, &persistence, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        QCOMPARE(persistence.saved.size(), 1);
        QCOMPARE(outcome.committedSettings.receiverName(), baseline.receiverName());
        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::VolumeUp),
                 baseline.shortcutFor(ShortcutAction::VolumeUp));
        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::ToggleAlwaysOnTop),
                 candidate.shortcutFor(ShortcutAction::ToggleAlwaysOnTop));
        QCOMPARE(outcome.committedSettings.recordingOutputDirectory(), candidate.recordingOutputDirectory());
        verifyStatus(outcome, SettingsFieldId::receiverName(), SettingsFieldStatus::ValidationFailed);
        verifyStatus(outcome, SettingsFieldId::shortcut(ShortcutAction::VolumeUp),
                     SettingsFieldStatus::ValidationFailed);
    }

    void persistenceOpenWriteAndCommitFailuresCommitNothing() {
        for (AppSettingsSaveStage stage : {AppSettingsSaveStage::Open, AppSettingsSaveStage::Write,
                                           AppSettingsSaveStage::Commit}) {
            const AppSettings baseline = AppSettings::defaults();
            AppSettings candidate = baseline;
            candidate.setReceiverName("Desk Receiver");
            RecordingHotkeyService hotkeys;
            hotkeys.seed(baseline);
            RecordingSettingsPersistence persistence;
            const AppSettingsSaveResult failure{false, "C:/settings.json", stage,
                                                QFileDevice::WriteError, "Exact save failure"};
            persistence.responses.append(failure);
            FakeAirPlayReceiver receiver;
                AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, &hotkeys, &persistence, &receiver);

            const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

            QCOMPARE(persistence.saved.size(), 1);
            QVERIFY(outcome.globalResult.has_value());
            QCOMPARE(outcome.globalResult->persistence.success, failure.success);
            QCOMPARE(outcome.globalResult->persistence.targetPath, failure.targetPath);
            QCOMPARE(outcome.globalResult->persistence.failureStage, failure.failureStage);
            QCOMPARE(outcome.globalResult->persistence.fileError, failure.fileError);
            QCOMPARE(outcome.globalResult->persistence.errorString, failure.errorString);
            QCOMPARE(outcome.committedSettings.receiverName(), baseline.receiverName());
            QCOMPARE(receiver.configurationBatchCount, 0);
            QCOMPARE(receiver.configurationBatchCount, 0);
            QVERIFY(!outcome.mayClose);
        }
    }

    void persistenceFailureCompensatesEveryChangedHotkeyInReverseOrder() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+Y"));
        candidate.setShortcut(ShortcutAction::VolumeUp, QKeySequence("Ctrl+Alt+U"));
        RecordingHotkeyService hotkeys;
        hotkeys.seed(baseline);
        RecordingSettingsPersistence persistence;
        persistence.responses.append({false, "C:/settings.json", AppSettingsSaveStage::Commit,
                                      QFileDevice::WriteError, "Commit failed"});
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, &hotkeys, &persistence, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        QCOMPARE(hotkeys.calls.size(), 9);
        QCOMPARE(hotkeys.calls.at(7).action, ShortcutAction::VolumeUp);
        QCOMPARE(hotkeys.calls.at(7).sequence, baseline.shortcutFor(ShortcutAction::VolumeUp));
        QCOMPARE(hotkeys.calls.at(8).action, ShortcutAction::ToggleAlwaysOnTop);
        QCOMPARE(hotkeys.calls.at(8).sequence, baseline.shortcutFor(ShortcutAction::ToggleAlwaysOnTop));
        QCOMPARE(hotkeys.active(ShortcutAction::ToggleAlwaysOnTop),
                 baseline.shortcutFor(ShortcutAction::ToggleAlwaysOnTop));
        QCOMPARE(hotkeys.active(ShortcutAction::VolumeUp), baseline.shortcutFor(ShortcutAction::VolumeUp));
        verifyStatus(outcome, SettingsFieldId::shortcut(ShortcutAction::ToggleAlwaysOnTop),
                     SettingsFieldStatus::ApplyFailedRolledBack);
        verifyStatus(outcome, SettingsFieldId::shortcut(ShortcutAction::VolumeUp),
                     SettingsFieldStatus::ApplyFailedRolledBack);
        QCOMPARE(requireResult(outcome, SettingsFieldId::shortcut(ShortcutAction::ToggleAlwaysOnTop)).reason,
                 QString("Not committed because the settings file could not be saved."));
        QCOMPARE(requireResult(outcome, SettingsFieldId::shortcut(ShortcutAction::VolumeUp)).reason,
                 QString("Not committed because the settings file could not be saved."));
        QVERIFY(!outcome.mayClose);
    }

    void persistenceFailureWithHotkeyCompensationFailureReportsRecoveryFailed() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+Y"));
        candidate.setShortcut(ShortcutAction::VolumeUp, QKeySequence("Ctrl+Alt+U"));
        RecordingHotkeyService hotkeys;
        hotkeys.seed(baseline);
        hotkeys.responses = {{true, false}, {true, false}, {true, true}, {true, true}, {true, true},
                              {true, true}, {true, true},
                              {false, false, HotkeyError{91, "Baseline rejected"}, false,
                               HotkeyError{92, "Candidate also unavailable"}},
                              {true, false}};
        RecordingSettingsPersistence persistence;
        persistence.responses.append({false, "C:/settings.json", AppSettingsSaveStage::Commit,
                                      QFileDevice::WriteError, "Commit failed"});
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, &hotkeys, &persistence, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;
        const SettingsFieldResult &volumeUp = requireResult(
            outcome, SettingsFieldId::shortcut(ShortcutAction::VolumeUp));

        QVERIFY(outcome.globalResult.has_value());
        QCOMPARE(volumeUp.status, SettingsFieldStatus::RecoveryFailed);
        QCOMPARE(volumeUp.reason, QString("Not committed because the settings file could not be saved."));
        QVERIFY(volumeUp.recoveryError.contains("baseline shortcut restoration failed",
                                                Qt::CaseInsensitive));
        QVERIFY(volumeUp.recoveryError.contains("Baseline rejected"));
        QVERIFY(volumeUp.recoveryError.contains("Candidate also unavailable"));
        QCOMPARE(hotkeys.active(ShortcutAction::VolumeUp), QKeySequence());
        QCOMPARE(hotkeys.active(ShortcutAction::ToggleAlwaysOnTop),
                 baseline.shortcutFor(ShortcutAction::ToggleAlwaysOnTop));
    }

    void nullPersistenceSucceedsAndNullHotkeysAdoptChangedShortcuts() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+Y"));
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, nullptr, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::AfterDisconnect; }).outcome;

        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::ToggleAlwaysOnTop),
                 candidate.shortcutFor(ShortcutAction::ToggleAlwaysOnTop));
        verifyStatus(outcome, SettingsFieldId::shortcut(ShortcutAction::ToggleAlwaysOnTop),
                     SettingsFieldStatus::Applied);
        QVERIFY(!outcome.globalResult.has_value());
        QVERIFY(outcome.mayClose);
    }

    void combinedNameAndQualityChangeUsesOneBatch() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("Desk Receiver");
        candidate.setVideoQuality({VideoResolution::P720, VideoFrameRate::Fps60});
        RecordingSettingsPersistence persistence;
        FakeAirPlayReceiver receiver;
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, &persistence, &receiver);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        QCOMPARE(persistence.saved.size(), 1);
        QCOMPARE(receiver.configurationBatchCount, 1);
        QCOMPARE(receiver.configurationBatchRequests.size(), 1);
        const ReceiverConfigurationBatchRequest &batch = receiver.configurationBatchRequests.constFirst();
        QVERIFY(batch.receiverNameChanged);
        QVERIFY(batch.resolutionChanged);
        QVERIFY(batch.frameRateChanged);
        QCOMPARE(batch.requestedReceiverName, candidate.receiverName());
        QCOMPARE(batch.requestedVideoQuality, candidate.videoQuality());
        verifyStatus(outcome, SettingsFieldId::receiverName(), SettingsFieldStatus::Applied);
        verifyStatus(outcome, SettingsFieldId::videoResolution(), SettingsFieldStatus::Applied);
        verifyStatus(outcome, SettingsFieldId::videoFrameRate(), SettingsFieldStatus::Applied);
    }

    void sharedRestartFailureRollsBackOnlyChangedReceiverFields() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("Desk Receiver");
        VideoQualitySettings quality = candidate.videoQuality();
        quality.frameRate = VideoFrameRate::Fps60;
        candidate.setVideoQuality(quality);
        candidate.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+Y"));
        candidate.setShowRecordingCompletionMessage(false);
        RecordingSettingsPersistence persistence;
        FakeAirPlayReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        receiver.requestedConfigurationRestartError = "Requested restart failed";
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, &persistence, &receiver);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        QCOMPARE(receiver.configurationBatchCount, 1);
        QCOMPARE(persistence.saved.size(), 2);
        QCOMPARE(outcome.committedSettings.receiverName(), baseline.receiverName());
        QCOMPARE(outcome.committedSettings.videoQuality().frameRate,
                 baseline.videoQuality().frameRate);
        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::ToggleAlwaysOnTop),
                 candidate.shortcutFor(ShortcutAction::ToggleAlwaysOnTop));
        QCOMPARE(outcome.committedSettings.showRecordingCompletionMessage(), false);
        verifyStatus(outcome, SettingsFieldId::receiverName(),
                     SettingsFieldStatus::ApplyFailedRolledBack);
        verifyStatus(outcome, SettingsFieldId::videoFrameRate(),
                     SettingsFieldStatus::ApplyFailedRolledBack);
        verifyStatus(outcome, SettingsFieldId::shortcut(ShortcutAction::ToggleAlwaysOnTop),
                     SettingsFieldStatus::Applied);
        verifyStatus(outcome, SettingsFieldId::recordingCompletionNotification(),
                     SettingsFieldStatus::Applied);
    }

    void unchangedReceiverFieldIsNotMarkedFailedBySharedRestart() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("Desk Receiver");
        VideoQualitySettings quality = candidate.videoQuality();
        quality.frameRate = VideoFrameRate::Fps60;
        candidate.setVideoQuality(quality);
        RecordingSettingsPersistence persistence;
        FakeAirPlayReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        receiver.requestedConfigurationRestartError = "Requested restart failed";
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, &persistence, &receiver);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        verifyStatus(outcome, SettingsFieldId::videoResolution(), SettingsFieldStatus::Unchanged);
        QCOMPARE(outcome.committedSettings.videoQuality().resolution,
                 baseline.videoQuality().resolution);
    }

    void receiverRestorationFailureIsRecoveryFailed() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("Desk Receiver");
        RecordingSettingsPersistence persistence;
        FakeAirPlayReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        receiver.requestedConfigurationRestartError = "Requested restart failed";
        receiver.rollbackConfigurationRestartError = "Rollback restart failed";
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, &persistence, &receiver);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;
        const SettingsFieldResult &result = requireResult(outcome, SettingsFieldId::receiverName());

        QCOMPARE(result.status, SettingsFieldStatus::RecoveryFailed);
        QVERIFY(result.reason.contains("Requested restart failed"));
        QVERIFY(result.reason.contains("Known saved state"));
        QVERIFY(result.recoveryError.contains("Rollback restart failed"));
        QVERIFY(result.recoveryError.contains("Known runtime state"));
        QVERIFY(result.recoveryError.contains("cannot be confirmed"));
        QCOMPARE(persistence.saved.size(), 1);
        QCOMPARE(outcome.committedSettings.receiverName(), candidate.receiverName());
    }

    void compensatingJsonFailureIsRecoveryFailedWithKnownStates() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("Desk Receiver");
        RecordingSettingsPersistence persistence;
        persistence.responses = {{true}, {false, "C:/settings.json", AppSettingsSaveStage::Commit,
                                  QFileDevice::WriteError, "Compensation commit failed"}};
        FakeAirPlayReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        receiver.requestedConfigurationRestartError = "Requested restart failed";
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, &persistence, &receiver);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;
        const SettingsFieldResult &result = requireResult(outcome, SettingsFieldId::receiverName());

        QCOMPARE(result.status, SettingsFieldStatus::RecoveryFailed);
        QVERIFY(result.reason.contains("Known saved state"));
        QVERIFY(result.recoveryError.contains("Known runtime state"));
        QVERIFY(result.recoveryError.contains("Compensation commit failed"));
        QVERIFY(result.recoveryError.contains("cannot be confirmed"));
        QCOMPARE(persistence.saved.size(), 2);
        QCOMPARE(outcome.committedSettings.receiverName(), candidate.receiverName());
    }

    void afterDisconnectTimingPersistsAndDefersOneBatch() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("Desk Receiver");
        VideoQualitySettings quality = candidate.videoQuality();
        quality.resolution = VideoResolution::P720;
        candidate.setVideoQuality(quality);
        RecordingSettingsPersistence persistence;
        FakeAirPlayReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, &persistence, &receiver);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::AfterDisconnect; }).outcome;

        QCOMPARE(persistence.saved.size(), 1);
        QCOMPARE(receiver.configurationBatchCount, 0);
        QCOMPARE(receiver.configurationBatchCount, 0);
        QVERIFY(outcome.airPlayDeferred);
        verifyStatus(outcome, SettingsFieldId::receiverName(), SettingsFieldStatus::Deferred);
        verifyStatus(outcome, SettingsFieldId::videoResolution(), SettingsFieldStatus::Deferred);
        QVERIFY(outcome.mayClose);
    }

    void recordingInProgressDefersImmediateTimingUntilIdle() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("Desk Receiver");
        RecordingSettingsPersistence persistence;
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        QVERIFY(receiver.startRecording({}).accepted);
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, &persistence, &receiver);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        QCOMPARE(receiver.stopRecordingCount, 1);
        QCOMPARE(receiver.configurationBatchCount, 0);
        QCOMPARE(receiver.configurationBatchCount, 0);
        QVERIFY(outcome.airPlayDeferred);
        verifyStatus(outcome, SettingsFieldId::receiverName(), SettingsFieldStatus::Deferred);
        QVERIFY(outcome.mayClose);
    }

    void deferredFailureRestoresAffectedJsonAndLeavesNoDraft() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("Desk Receiver");
        candidate.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+Y"));
        RecordingSettingsPersistence persistence;
        FakeAirPlayReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        receiver.requestedConfigurationRestartError = "Requested restart failed";
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, &persistence, &receiver);
        const SettingsApplyOutcome initial = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::AfterDisconnect; }).outcome;
        verifyStatus(initial, SettingsFieldId::receiverName(), SettingsFieldStatus::Deferred);
        QVERIFY(initial.mayClose);
        QSignalSpy completed(&coordinator, &SettingsApplyCoordinator::deferredApplyFinished);
        receiver.forceState(ReceiverState::Discoverable);
        QCOMPARE(completed.count(), 1);
        const SettingsApplyOutcome completion = std::get<SettingsApplyOutcome>(
            qvariant_cast<SettingsDeferredResult>(completed.at(0).at(0)));

        QCOMPARE(persistence.saved.size(), 2);
        QCOMPARE(completion.committedSettings.receiverName(), baseline.receiverName());
        QCOMPARE(completion.committedSettings.shortcutFor(ShortcutAction::ToggleAlwaysOnTop),
                 candidate.shortcutFor(ShortcutAction::ToggleAlwaysOnTop));
        verifyStatus(completion, SettingsFieldId::receiverName(),
                     SettingsFieldStatus::ApplyFailedRolledBack);
        verifyStatus(completion, SettingsFieldId::shortcut(ShortcutAction::ToggleAlwaysOnTop),
                     SettingsFieldStatus::Unchanged);
    }

    void missingImmediateReceiverCompensatesOnlyReceiverSettingsForRetry() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("Desk Receiver");
        candidate.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+Y"));
        candidate.setShowRecordingCompletionMessage(false);
        RecordingSettingsPersistence persistence;
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, &persistence, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;

        QCOMPARE(persistence.saved.size(), 2);
        QCOMPARE(outcome.committedSettings.receiverName(), baseline.receiverName());
        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::ToggleAlwaysOnTop),
                 candidate.shortcutFor(ShortcutAction::ToggleAlwaysOnTop));
        QCOMPARE(outcome.committedSettings.showRecordingCompletionMessage(), false);
        QCOMPARE(persistence.saved.last().receiverName(), baseline.receiverName());
        QCOMPARE(persistence.saved.last().shortcutFor(ShortcutAction::ToggleAlwaysOnTop),
                 candidate.shortcutFor(ShortcutAction::ToggleAlwaysOnTop));
        verifyStatus(outcome, SettingsFieldId::receiverName(), SettingsFieldStatus::RecoveryFailed);
        QVERIFY(requireResult(outcome, SettingsFieldId::receiverName()).reason.contains("saved state was restored"));
        QVERIFY(requireResult(outcome, SettingsFieldId::receiverName()).recoveryError.contains("unavailable"));
        verifyStatus(outcome, SettingsFieldId::shortcut(ShortcutAction::ToggleAlwaysOnTop),
                     SettingsFieldStatus::Applied);
        QVERIFY(!outcome.mayClose);

        const auto retry = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::AfterDisconnect; }).outcome;
        verifyStatus(retry, SettingsFieldId::receiverName(), SettingsFieldStatus::RecoveryFailed);
        QCOMPARE(persistence.saved.size(), 4);
    }

    void internalDeferrerAlwaysInstallsAndConsumesPendingTarget() {
        CoordinatorHarness h;
        h.receiver.forceState(ReceiverState::Connected);
        auto draft=h.current; draft.setReceiverName("Owned pending target");
        const auto outcome=h.submit(draft, ReceiverApplyTiming::AfterDisconnect);
        verifyStatus(outcome, SettingsFieldId::receiverName(), SettingsFieldStatus::Deferred);
        QCOMPARE(h.receiver.configurationBatchCount, 0);
        h.disconnectSession();
        QCOMPARE(h.receiver.configurationBatchCount, 1);
        QCOMPARE(h.receiver.receiverName(), QString("Owned pending target"));
        QVERIFY(h.completion.has_value());
    }

    void missingReceiverCompensationSaveFailureKeepsKnownRequestedSnapshot() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("Desk Receiver");
        RecordingSettingsPersistence persistence;
        const AppSettingsSaveResult compensationFailure{
            false, "C:/settings.json", AppSettingsSaveStage::Commit, QFileDevice::WriteError,
            "Compensation commit failed"};
        persistence.responses = {{true}, compensationFailure};
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, &persistence, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;
        const SettingsFieldResult &result = requireResult(outcome, SettingsFieldId::receiverName());

        QCOMPARE(persistence.saved.size(), 2);
        QVERIFY(outcome.globalResult.has_value());
        QCOMPARE(outcome.globalResult->persistence.success, compensationFailure.success);
        QCOMPARE(outcome.globalResult->persistence.targetPath, compensationFailure.targetPath);
        QCOMPARE(outcome.globalResult->persistence.failureStage, compensationFailure.failureStage);
        QCOMPARE(outcome.globalResult->persistence.errorString, compensationFailure.errorString);
        QCOMPARE(outcome.committedSettings.receiverName(), candidate.receiverName());
        QCOMPARE(result.status, SettingsFieldStatus::RecoveryFailed);
        QVERIFY(result.reason.contains("requested JSON was saved"));
        QVERIFY(result.recoveryError.contains("saved state cannot be confirmed"));
    }

    void destroyedReceiverRetainsOldTargetAndNewApplyReportsUnavailable() {
        auto current=AppSettings::defaults();
        RecordingSettingsPersistence persistence;
        auto receiver=std::make_unique<FakeAirPlayReceiver>();
        SettingsApplyCoordinator coordinator(current, nullptr, &persistence, receiver.get());
        QSignalSpy completed(&coordinator, &SettingsApplyCoordinator::deferredApplyFinished);
        receiver->forceState(ReceiverState::Connected);
        auto draft=current; draft.setReceiverName("Saved target");
        coordinator.apply(draft, [] { return ReceiverApplyTiming::AfterDisconnect; });
        receiver.reset();
        QCoreApplication::sendPostedEvents();
        QCOMPARE(current.receiverName(), QString("Saved target"));
        QCOMPARE(persistence.saved.size(), 1);
        QCOMPARE(completed.count(), 0);
        draft.setReceiverName("New unavailable target");
        draft.setLanguage("zh-CN");
        const auto outcome=*coordinator.apply(draft, {}).outcome;
        QCOMPARE(current.receiverName(), QString("Saved target"));
        QCOMPARE(current.language(), QString("zh-CN"));
        verifyStatus(outcome, SettingsFieldId::receiverName(), SettingsFieldStatus::RecoveryFailed);
        QCOMPARE(persistence.saved.size(), 3);
    }

    void mayCloseAndSnapshotMergeFollowExecutionResults() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName({});
        candidate.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+Y"));
        candidate.setRecordingOutputDirectory("C:/AirPlay-recordings");
        candidate.setVolume(5);
        candidate.setAspectRatioLock(true);
        candidate.setVideoFitMode(true);
        RecordingSettingsPersistence persistence;
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, &persistence, nullptr);

        const SettingsApplyOutcome failedValidation = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;
        QVERIFY(!failedValidation.mayClose);
        QCOMPARE(failedValidation.committedSettings.receiverName(), baseline.receiverName());
        QCOMPARE(failedValidation.committedSettings.shortcutFor(ShortcutAction::ToggleAlwaysOnTop),
                 candidate.shortcutFor(ShortcutAction::ToggleAlwaysOnTop));
        QCOMPARE(failedValidation.committedSettings.recordingOutputDirectory(),
                 candidate.recordingOutputDirectory());
        QCOMPARE(failedValidation.committedSettings.volume(), baseline.volume());
        QCOMPARE(failedValidation.committedSettings.aspectRatioLock(), baseline.aspectRatioLock());
        QCOMPARE(failedValidation.committedSettings.videoFitMode(), baseline.videoFitMode());

        const SettingsApplyOutcome success = *coordinator.apply(baseline, [] { return ReceiverApplyTiming::Immediate; }).outcome;
        QVERIFY(success.mayClose);
    }
    void validationReasonRendersUsingCurrentTranslator() {
        AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName(QString());

        const ObservedApply plan = observeApply(baseline, candidate, false);
        const SettingsFieldResult &result = requireResult(plan, SettingsFieldId::receiverName());
        QCOMPARE(result.reason, QString("Receiver name cannot be empty."));
        QCOMPARE(result.userReason.render(), QString("Receiver name cannot be empty."));

        Task8Translator translator;
        QCoreApplication::installTranslator(&translator);
        QCOMPARE(result.userReason.render(), QString("接收器名称不能为空。"));
        QCoreApplication::removeTranslator(&translator);
    }

    void delayedShortcutAndReceiverReasonsTranslateWithoutEnglishFragments() {
        AppSettings baseline = AppSettings::defaults();
        AppSettings shortcutCandidate = baseline;
        shortcutCandidate.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+Y"));
        RecordingHotkeyService hotkeys;
        hotkeys.seed(baseline);
        hotkeys.responses = {{false, false, HotkeyError{87, "Raw registration detail"}, true}};
        AppSettings shortcutCoordinatorCurrent = baseline;
        SettingsApplyCoordinator shortcutCoordinator(shortcutCoordinatorCurrent, &hotkeys, nullptr, nullptr);
        const SettingsApplyOutcome shortcutOutcome = *shortcutCoordinator.apply(shortcutCandidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;
        const SettingsFieldResult &shortcutResult = requireResult(
            shortcutOutcome, SettingsFieldId::shortcut(ShortcutAction::ToggleAlwaysOnTop));

        AppSettings receiverCandidate = baseline;
        receiverCandidate.setReceiverName("New Receiver");
        FakeAirPlayReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        receiver.requestedConfigurationRestartError = "Raw apply detail";
        receiver.rollbackConfigurationRestartError = "Raw recovery detail";
        AppSettings receiverCoordinatorCurrent = baseline;
        SettingsApplyCoordinator receiverCoordinator(receiverCoordinatorCurrent, nullptr, nullptr, &receiver);
        const SettingsApplyOutcome receiverOutcome = *receiverCoordinator.apply(receiverCandidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;
        const SettingsFieldResult &receiverResult = requireResult(receiverOutcome, SettingsFieldId::receiverName());

        QCOMPARE(shortcutResult.userReason.render(),
                 QString("Shortcut registration failed: Raw registration detail. Previous shortcut was restored."));
        QCOMPARE(receiverResult.userReason.render(),
                 QString("Receiver configuration could not be applied: Raw apply detail"));
        QCOMPARE(receiverResult.userRecoveryError.render(),
                 QString("Receiver restoration failed: Raw recovery detail"));

        Task8Translator translator;
        QCoreApplication::installTranslator(&translator);
        QCOMPARE(shortcutResult.userReason.render(), QString("快捷键注册失败：Raw registration detail。已恢复上一个快捷键。"));
        QCOMPARE(receiverResult.userReason.render(), QString("接收器配置无法应用：Raw apply detail"));
        QCOMPARE(receiverResult.userRecoveryError.render(), QString("接收器恢复失败：Raw recovery detail"));
        QCoreApplication::removeTranslator(&translator);
    }

    void unavailableReceiverReasonRetranslatesWithoutUnavailableDetail() {
        AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("New Receiver");
        FakeAirPlayReceiver receiver;
        RecordingSettingsPersistence persistence;
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, nullptr, &persistence, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::AfterDisconnect; }).outcome;
        const SettingsFieldResult &result = requireResult(outcome, SettingsFieldId::receiverName());
        QCOMPARE(result.reason,
                 QString("Receiver configuration did not start: AirPlay receiver is unavailable. The saved state was restored to receiver name 'AirPlay Receiver', 1080p, 30 fps."));
        QCOMPARE(result.userReason.render(),
                 QString("Receiver configuration did not start. The saved state was restored."));

        Task8Translator translator;
        QCoreApplication::installTranslator(&translator);
        QCOMPARE(result.userReason.render(), QString("接收器配置未启动。已恢复保存的状态。"));
        QCoreApplication::removeTranslator(&translator);
    }

    void delayedShortcutRecoveryIncludesNativeCodeAfterTranslation() {
        AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+Y"));
        RecordingHotkeyService hotkeys;
        hotkeys.seed(baseline);
        hotkeys.responses = {{false, false, HotkeyError{87, "Raw candidate detail"}, false,
                              HotkeyError{88, "Raw recovery detail"}}};
        AppSettings coordinatorCurrent = baseline;
        SettingsApplyCoordinator coordinator(coordinatorCurrent, &hotkeys, nullptr, nullptr);

        const SettingsApplyOutcome outcome = *coordinator.apply(candidate, [] { return ReceiverApplyTiming::Immediate; }).outcome;
        const SettingsFieldResult &result = requireResult(
            outcome, SettingsFieldId::shortcut(ShortcutAction::ToggleAlwaysOnTop));
        QCOMPARE(result.userRecoveryError.render(),
                 QString("Shortcut restoration failed: Raw recovery detail (native error 88)"));

        Task8Translator translator;
        QCoreApplication::installTranslator(&translator);
        QCOMPARE(result.userRecoveryError.render(), QString("快捷键恢复失败：Raw recovery detail（原生错误 88）"));
        QCoreApplication::removeTranslator(&translator);
    }
};

QTEST_MAIN(SettingsApplyCoordinatorTest)
#include "SettingsApplyCoordinatorTest.moc"
