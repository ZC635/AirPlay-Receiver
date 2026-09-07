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
        if (responses.isEmpty()) {
            return {true};
        }
        return responses.takeFirst();
    }

    mutable QVector<AppSettings> saved;
    mutable QVector<AppSettingsSaveResult> responses;
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

const SettingsFieldResult &requireResult(const SettingsApplyPlan &plan, const SettingsFieldId &field) {
    const SettingsFieldResult *result = resultForField(plan.validationResults, field);
    Q_ASSERT(result != nullptr);
    return *result;
}

void verifyValid(const SettingsApplyPlan &plan, const SettingsFieldId &field) {
    const SettingsFieldResult &result = requireResult(plan, field);
    QCOMPARE(result.status, SettingsFieldStatus::Unchanged);
    QVERIFY(result.reason.isEmpty());
    QVERIFY(!result.nativeErrorCode.has_value());
    QVERIFY(result.recoveryError.isEmpty());
}

void verifyValidationFailure(const SettingsApplyPlan &plan, const SettingsFieldId &field,
                             const QString &reason) {
    const SettingsFieldResult &result = requireResult(plan, field);
    QCOMPARE(result.status, SettingsFieldStatus::ValidationFailed);
    QCOMPARE(result.reason, reason);
    QVERIFY(!result.nativeErrorCode.has_value());
    QVERIFY(result.recoveryError.isEmpty());
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

class SettingsApplyCoordinatorTest : public QObject {
    Q_OBJECT

private slots:
    void emptyReceiverNameDoesNotInvalidateFrameRate() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("   ");
        VideoQualitySettings quality = candidate.videoQuality();
        quality.frameRate = VideoFrameRate::Fps60;
        candidate.setVideoQuality(quality);

        const SettingsApplyPlan plan = SettingsApplyCoordinator(nullptr, nullptr, nullptr, nullptr)
                                           .plan(baseline, candidate, true, RecordingState::Idle);

        QCOMPARE(plan.validationResults.size(), 14);
        verifyValidationFailure(plan, SettingsFieldId::receiverName(),
                                QStringLiteral("Receiver name cannot be empty."));
        verifyValid(plan, SettingsFieldId::language());
        verifyValid(plan, SettingsFieldId::videoFrameRate());
        QVERIFY(plan.validChangedReceiverFields
                == QVector<SettingsFieldId>{SettingsFieldId::videoFrameRate()});
        QVERIFY(plan.requiresReceiverTimingDecision);
    }

    void duplicateShortcutMarksBothRowsInvalid() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        const QKeySequence duplicate("Ctrl+Alt+B");
        candidate.setShortcut(ShortcutAction::ToggleToolbar, duplicate);
        candidate.setShortcut(ShortcutAction::ToggleRecording, duplicate);

        const SettingsApplyPlan plan = SettingsApplyCoordinator(nullptr, nullptr, nullptr, nullptr)
                                           .plan(baseline, candidate, false, RecordingState::Idle);

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

        const SettingsApplyPlan plan = SettingsApplyCoordinator(nullptr, nullptr, nullptr, nullptr)
                                           .plan(baseline, candidate, false, RecordingState::Idle);

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

        const SettingsApplyPlan plan = SettingsApplyCoordinator(nullptr, nullptr, nullptr, nullptr)
                                           .plan(baseline, candidate, false, RecordingState::Idle);

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

        const SettingsApplyPlan plan = SettingsApplyCoordinator(nullptr, nullptr, nullptr, nullptr)
                                           .plan(baseline, candidate, false, RecordingState::Idle);

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
        const AppSettings baseline = AppSettings::defaults();
        const SettingsApplyCoordinator coordinator(nullptr, nullptr, nullptr, nullptr);
        QVERIFY(!coordinator.plan(baseline, baseline, true, RecordingState::Idle)
                     .requiresReceiverTimingDecision);

        AppSettings invalidName = baseline;
        invalidName.setReceiverName({});
        const SettingsApplyPlan invalidPlan = coordinator.plan(baseline, invalidName, true,
                                                               RecordingState::Idle);
        QVERIFY(!invalidPlan.requiresReceiverTimingDecision);
        QVERIFY(invalidPlan.validChangedReceiverFields.isEmpty());
    }

    void oneValidChangedReceiverFieldRequestsTimingDecisionDuringSession() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        VideoQualitySettings quality = candidate.videoQuality();
        quality.resolution = VideoResolution::P720;
        candidate.setVideoQuality(quality);

        const SettingsApplyPlan plan = SettingsApplyCoordinator(nullptr, nullptr, nullptr, nullptr)
                                           .plan(baseline, candidate, true, RecordingState::Idle);

        QVERIFY(plan.requiresReceiverTimingDecision);
        QVERIFY(plan.validChangedReceiverFields
                == QVector<SettingsFieldId>{SettingsFieldId::videoResolution()});
    }

    void validReceiverChangeDoesNotRequestDecisionWhenSessionInactive() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("Desk Receiver");

        const SettingsApplyPlan plan = SettingsApplyCoordinator(nullptr, nullptr, nullptr, nullptr)
                                           .plan(baseline, candidate, false, RecordingState::Idle);

        QVERIFY(!plan.requiresReceiverTimingDecision);
        QVERIFY(plan.validChangedReceiverFields
                == QVector<SettingsFieldId>{SettingsFieldId::receiverName()});
    }

    void invalidReceiverFieldAlongsideValidChangeStillRequestsDecision() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName({});
        VideoQualitySettings quality = candidate.videoQuality();
        quality.frameRate = VideoFrameRate::Fps60;
        candidate.setVideoQuality(quality);

        const SettingsApplyPlan plan = SettingsApplyCoordinator(nullptr, nullptr, nullptr, nullptr)
                                           .plan(baseline, candidate, true, RecordingState::Idle);

        QVERIFY(plan.requiresReceiverTimingDecision);
        QVERIFY(plan.validChangedReceiverFields
                == QVector<SettingsFieldId>{SettingsFieldId::videoFrameRate()});
    }

    void languageOnlyChangeDoesNotRequestReceiverTimingDecision() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setLanguage("zh-CN");

        const SettingsApplyPlan plan = SettingsApplyCoordinator(nullptr, nullptr, nullptr, nullptr)
                                           .plan(baseline, candidate, true, RecordingState::Idle);

        QVERIFY(!plan.requiresReceiverTimingDecision);
        QVERIFY(plan.validChangedReceiverFields.isEmpty());
        verifyValid(plan, SettingsFieldId::language());
    }

    void languagePersistsAsIndependentField() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setLanguage("zh-CN");
        RecordingSettingsPersistence persistence;
        SettingsApplyCoordinator coordinator(nullptr, &persistence, nullptr, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, true, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);

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
        SettingsApplyCoordinator coordinator(nullptr, &persistence, nullptr, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);

        QCOMPARE(persistence.saved.size(), 1);
        QCOMPARE(outcome.committedSettings.language(), QString("system"));
        verifyStatus(outcome, SettingsFieldId::language(),
                     SettingsFieldStatus::ApplyFailedRolledBack);
        QVERIFY(outcome.globalResult.has_value());
        QVERIFY(!outcome.mayClose);
    }

    void planCopiesRecordingIdleState() {
        const AppSettings settings = AppSettings::defaults();
        const SettingsApplyCoordinator coordinator(nullptr, nullptr, nullptr, nullptr);
        QVERIFY(coordinator.plan(settings, settings, false, RecordingState::Idle).recordingIdle);
        QVERIFY(!coordinator.plan(settings, settings, false, RecordingState::Recording).recordingIdle);
        QVERIFY(!coordinator.plan(settings, settings, false, RecordingState::Finalizing).recordingIdle);
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

        const SettingsApplyPlan plan = SettingsApplyCoordinator(nullptr, nullptr, nullptr, nullptr)
                                           .plan(baseline, candidate, false, RecordingState::Idle);

        QCOMPARE(plan.baseline.receiverName(), baseline.receiverName());
        QCOMPARE(plan.candidate.receiverName(), candidate.receiverName());
        QCOMPARE(plan.receiverSessionActive, false);
        QCOMPARE(plan.validationResults.size(), allSettingsFields().size());
        const QVector<SettingsFieldId> fields = allSettingsFields();
        for (qsizetype index = 0; index < plan.validationResults.size(); ++index) {
            const SettingsFieldId &field = fields.at(index);
            QCOMPARE(plan.validationResults.at(index).field, field);
            QVERIFY(plan.validationResults.at(index).attemptedValue
                    == settingsFieldValue(candidate, field));
        }
        QVERIFY(std::holds_alternative<QString>(plan.validationResults.at(0).attemptedValue));
        QCOMPARE(std::get<QString>(plan.validationResults.at(1).attemptedValue), QString("zh-CN"));
        QVERIFY(std::holds_alternative<VideoResolution>(plan.validationResults.at(2).attemptedValue));
        QVERIFY(std::holds_alternative<VideoFrameRate>(plan.validationResults.at(3).attemptedValue));
        QVERIFY(std::holds_alternative<QKeySequence>(plan.validationResults.at(4).attemptedValue));
        QVERIFY(std::holds_alternative<RecordingFormat>(plan.validationResults.at(11).attemptedValue));
        QVERIFY(std::holds_alternative<QString>(plan.validationResults.at(12).attemptedValue));
        QVERIFY(std::holds_alternative<bool>(plan.validationResults.at(13).attemptedValue));
    }

    void planDoesNotInvokeDependenciesAndAllowsNullDependencies() {
        const AppSettings settings = AppSettings::defaults();
        FakeHotkeyService hotkeys;
        CountingPersistence persistence;
        FakeAirPlayReceiver receiver;
        SettingsChangeDeferrer deferrer;
        const SettingsApplyCoordinator coordinator(&hotkeys, &persistence, &receiver, &deferrer);

        coordinator.plan(settings, settings, false, RecordingState::Idle);

        QVERIFY(hotkeys.attempts.isEmpty());
        QCOMPARE(persistence.saveCount, 0);
        QCOMPARE(receiver.configurationBatchCount, 0);
        QVERIFY(!deferrer.hasPendingReceiverConfiguration());
        const SettingsApplyPlan nullPlan = SettingsApplyCoordinator(nullptr, nullptr, nullptr, nullptr)
                                                .plan(settings, settings, false, RecordingState::Idle);
        QCOMPARE(nullPlan.validationResults.size(), 14);
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
        SettingsApplyCoordinator coordinator(&hotkeys, &persistence, nullptr, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);

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
        SettingsApplyCoordinator coordinator(&hotkeys, &persistence, nullptr, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);

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
        SettingsApplyCoordinator coordinator(&hotkeys, &persistence, nullptr, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);

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
        SettingsApplyCoordinator coordinator(&hotkeys, &persistence, nullptr, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);

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
        SettingsApplyCoordinator coordinator(&hotkeys, &persistence, nullptr, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);

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
        SettingsChangeDeferrer deferrer;
        SettingsApplyCoordinator coordinator(&hotkeys, &persistence, &receiver, &deferrer);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);

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
        SettingsApplyCoordinator coordinator(&hotkeys, nullptr, nullptr, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);
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
        SettingsApplyCoordinator coordinator(&hotkeys, nullptr, nullptr, nullptr);

        coordinator.execute(coordinator.plan(baseline, baseline, false, RecordingState::Idle),
                            ReceiverApplyTiming::Immediate);

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
        SettingsApplyCoordinator coordinator(&hotkeys, nullptr, nullptr, nullptr);

        const SettingsApplyOutcome first = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);
        coordinator.execute(coordinator.plan(first.committedSettings, candidate, false, RecordingState::Idle),
                            ReceiverApplyTiming::Immediate);

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
        SettingsApplyCoordinator coordinator(&hotkeys, &persistence, nullptr, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);

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
            SettingsChangeDeferrer deferrer;
            SettingsApplyCoordinator coordinator(&hotkeys, &persistence, &receiver, &deferrer);

            const SettingsApplyOutcome outcome = coordinator.execute(
                coordinator.plan(baseline, candidate, false, RecordingState::Idle),
                ReceiverApplyTiming::Immediate);

            QCOMPARE(persistence.saved.size(), 1);
            QVERIFY(outcome.globalResult.has_value());
            QCOMPARE(outcome.globalResult->persistence.success, failure.success);
            QCOMPARE(outcome.globalResult->persistence.targetPath, failure.targetPath);
            QCOMPARE(outcome.globalResult->persistence.failureStage, failure.failureStage);
            QCOMPARE(outcome.globalResult->persistence.fileError, failure.fileError);
            QCOMPARE(outcome.globalResult->persistence.errorString, failure.errorString);
            QCOMPARE(outcome.committedSettings.receiverName(), baseline.receiverName());
            QCOMPARE(receiver.configurationBatchCount, 0);
            QVERIFY(!deferrer.hasPendingReceiverConfiguration());
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
        SettingsApplyCoordinator coordinator(&hotkeys, &persistence, nullptr, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);

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
        SettingsApplyCoordinator coordinator(&hotkeys, &persistence, nullptr, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);
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
        SettingsApplyCoordinator coordinator(nullptr, nullptr, nullptr, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::AfterDisconnect);

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
        SettingsChangeDeferrer deferrer;
        SettingsApplyCoordinator coordinator(nullptr, &persistence, &receiver, &deferrer);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);

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
        SettingsChangeDeferrer deferrer;
        SettingsApplyCoordinator coordinator(nullptr, &persistence, &receiver, &deferrer);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);

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
        SettingsChangeDeferrer deferrer;
        SettingsApplyCoordinator coordinator(nullptr, &persistence, &receiver, &deferrer);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);

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
        SettingsApplyCoordinator coordinator(nullptr, &persistence, &receiver, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);
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
        SettingsApplyCoordinator coordinator(nullptr, &persistence, &receiver, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);
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
        SettingsChangeDeferrer deferrer;
        SettingsApplyCoordinator coordinator(nullptr, &persistence, &receiver, &deferrer);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, true, RecordingState::Idle),
            ReceiverApplyTiming::AfterDisconnect);

        QCOMPARE(persistence.saved.size(), 1);
        QCOMPARE(receiver.configurationBatchCount, 0);
        QVERIFY(deferrer.hasPendingReceiverConfiguration());
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
        SettingsChangeDeferrer deferrer;
        SettingsApplyCoordinator coordinator(nullptr, &persistence, &receiver, &deferrer);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Recording),
            ReceiverApplyTiming::Immediate);

        QCOMPARE(receiver.stopRecordingCount, 1);
        QCOMPARE(receiver.configurationBatchCount, 0);
        QVERIFY(deferrer.hasPendingReceiverConfiguration());
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
        SettingsChangeDeferrer deferrer;
        SettingsApplyCoordinator coordinator(nullptr, &persistence, &receiver, &deferrer);
        const SettingsApplyPlan plan = coordinator.plan(baseline, candidate, true, RecordingState::Idle);
        const SettingsApplyOutcome initial = coordinator.execute(plan, ReceiverApplyTiming::AfterDisconnect);
        verifyStatus(initial, SettingsFieldId::receiverName(), SettingsFieldStatus::Deferred);
        QVERIFY(initial.mayClose);
        ReceiverConfigurationBatchRequest batch;
        batch.receiverNameChanged = true;
        batch.requestedReceiverName = candidate.receiverName();
        batch.rollbackReceiverName = baseline.receiverName();
        batch.requestedVideoQuality = candidate.videoQuality();
        batch.rollbackVideoQuality = baseline.videoQuality();

        const SettingsApplyOutcome completion =
            coordinator.completeDeferredReceiverApply(batch, initial.committedSettings);

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
        SettingsApplyCoordinator coordinator(nullptr, &persistence, nullptr, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);

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

        const SettingsApplyPlan retry = coordinator.plan(outcome.committedSettings, candidate, false,
                                                          RecordingState::Idle);
        QVERIFY(retry.validChangedReceiverFields.contains(SettingsFieldId::receiverName()));
    }

    void missingDeferrerCompensatesOnlyReceiverSettingsForRetry() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        VideoQualitySettings quality = candidate.videoQuality();
        quality.frameRate = VideoFrameRate::Fps60;
        candidate.setVideoQuality(quality);
        candidate.setRecordingOutputDirectory("C:/AirPlay-recordings");
        RecordingSettingsPersistence persistence;
        FakeAirPlayReceiver receiver;
        SettingsApplyCoordinator coordinator(nullptr, &persistence, &receiver, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, true, RecordingState::Idle),
            ReceiverApplyTiming::AfterDisconnect);

        QCOMPARE(receiver.configurationBatchCount, 0);
        QCOMPARE(persistence.saved.size(), 2);
        QCOMPARE(outcome.committedSettings.videoQuality().frameRate,
                 baseline.videoQuality().frameRate);
        QCOMPARE(outcome.committedSettings.recordingOutputDirectory(),
                 candidate.recordingOutputDirectory());
        QCOMPARE(persistence.saved.last().videoQuality().frameRate,
                 baseline.videoQuality().frameRate);
        verifyStatus(outcome, SettingsFieldId::videoFrameRate(), SettingsFieldStatus::RecoveryFailed);
        QVERIFY(requireResult(outcome, SettingsFieldId::videoFrameRate())
                     .reason.contains("saved state was restored"));
        QVERIFY(requireResult(outcome, SettingsFieldId::videoFrameRate())
                     .recoveryError.contains("unchanged"));

        const SettingsApplyPlan retry = coordinator.plan(outcome.committedSettings, candidate, true,
                                                          RecordingState::Idle);
        QVERIFY(retry.validChangedReceiverFields.contains(SettingsFieldId::videoFrameRate()));
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
        SettingsApplyCoordinator coordinator(nullptr, &persistence, nullptr, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);
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

    void deferredCompletionWithoutReceiverCompensatesReceiverSettingsForRetry() {
        const AppSettings baseline = AppSettings::defaults();
        AppSettings currentlyCommitted = baseline;
        currentlyCommitted.setReceiverName("Desk Receiver");
        currentlyCommitted.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+Y"));
        RecordingSettingsPersistence persistence;
        SettingsApplyCoordinator coordinator(nullptr, &persistence, nullptr, nullptr);
        ReceiverConfigurationBatchRequest batch;
        batch.receiverNameChanged = true;
        batch.requestedReceiverName = currentlyCommitted.receiverName();
        batch.rollbackReceiverName = baseline.receiverName();
        batch.requestedVideoQuality = currentlyCommitted.videoQuality();
        batch.rollbackVideoQuality = baseline.videoQuality();

        const SettingsApplyOutcome outcome =
            coordinator.completeDeferredReceiverApply(batch, currentlyCommitted);

        QCOMPARE(persistence.saved.size(), 1);
        QCOMPARE(outcome.committedSettings.receiverName(), baseline.receiverName());
        QCOMPARE(outcome.committedSettings.shortcutFor(ShortcutAction::ToggleAlwaysOnTop),
                 currentlyCommitted.shortcutFor(ShortcutAction::ToggleAlwaysOnTop));
        QCOMPARE(persistence.saved.constFirst().receiverName(), baseline.receiverName());
        verifyStatus(outcome, SettingsFieldId::receiverName(), SettingsFieldStatus::RecoveryFailed);
        QVERIFY(requireResult(outcome, SettingsFieldId::receiverName()).reason.contains("saved state was restored"));
        QVERIFY(requireResult(outcome, SettingsFieldId::receiverName()).recoveryError.contains("unavailable"));

        const SettingsApplyPlan retry = coordinator.plan(outcome.committedSettings, currentlyCommitted,
                                                          false, RecordingState::Idle);
        QVERIFY(retry.validChangedReceiverFields.contains(SettingsFieldId::receiverName()));
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
        SettingsApplyCoordinator coordinator(nullptr, &persistence, nullptr, nullptr);

        const SettingsApplyOutcome failedValidation = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);
        QVERIFY(!failedValidation.mayClose);
        QCOMPARE(failedValidation.committedSettings.receiverName(), baseline.receiverName());
        QCOMPARE(failedValidation.committedSettings.shortcutFor(ShortcutAction::ToggleAlwaysOnTop),
                 candidate.shortcutFor(ShortcutAction::ToggleAlwaysOnTop));
        QCOMPARE(failedValidation.committedSettings.recordingOutputDirectory(),
                 candidate.recordingOutputDirectory());
        QCOMPARE(failedValidation.committedSettings.volume(), baseline.volume());
        QCOMPARE(failedValidation.committedSettings.aspectRatioLock(), baseline.aspectRatioLock());
        QCOMPARE(failedValidation.committedSettings.videoFitMode(), baseline.videoFitMode());

        const SettingsApplyOutcome success = coordinator.execute(
            coordinator.plan(baseline, baseline, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);
        QVERIFY(success.mayClose);
    }
    void validationReasonRendersUsingCurrentTranslator() {
        AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName(QString());

        const SettingsApplyPlan plan = SettingsApplyCoordinator(nullptr, nullptr, nullptr, nullptr)
                                           .plan(baseline, candidate, false, RecordingState::Idle);
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
        SettingsApplyCoordinator shortcutCoordinator(&hotkeys, nullptr, nullptr, nullptr);
        const SettingsApplyOutcome shortcutOutcome = shortcutCoordinator.execute(
            shortcutCoordinator.plan(baseline, shortcutCandidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);
        const SettingsFieldResult &shortcutResult = requireResult(
            shortcutOutcome, SettingsFieldId::shortcut(ShortcutAction::ToggleAlwaysOnTop));

        AppSettings receiverCandidate = baseline;
        receiverCandidate.setReceiverName("New Receiver");
        FakeAirPlayReceiver receiver;
        receiver.forceState(ReceiverState::Connected);
        receiver.requestedConfigurationRestartError = "Raw apply detail";
        receiver.rollbackConfigurationRestartError = "Raw recovery detail";
        SettingsApplyCoordinator receiverCoordinator(nullptr, nullptr, &receiver, nullptr);
        const SettingsApplyOutcome receiverOutcome = receiverCoordinator.execute(
            receiverCoordinator.plan(baseline, receiverCandidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);
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

    void unavailableDeferrerReasonRetranslatesWithoutUnavailableDetail() {
        AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("New Receiver");
        FakeAirPlayReceiver receiver;
        RecordingSettingsPersistence persistence;
        SettingsApplyCoordinator coordinator(nullptr, &persistence, &receiver, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, true, RecordingState::Idle),
            ReceiverApplyTiming::AfterDisconnect);
        const SettingsFieldResult &result = requireResult(outcome, SettingsFieldId::receiverName());
        QCOMPARE(result.reason,
                 QString("Receiver configuration did not start: Receiver configuration deferrer is unavailable. The saved state was restored to receiver name 'AirPlay Receiver', 1080p, 30 fps."));
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
        SettingsApplyCoordinator coordinator(&hotkeys, nullptr, nullptr, nullptr);

        const SettingsApplyOutcome outcome = coordinator.execute(
            coordinator.plan(baseline, candidate, false, RecordingState::Idle),
            ReceiverApplyTiming::Immediate);
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
