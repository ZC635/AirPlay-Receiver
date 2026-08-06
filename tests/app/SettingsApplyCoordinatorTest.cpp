#include <QtTest/QtTest>

#include "app/AppSettingsStore.h"
#include "app/SettingsApplyCoordinator.h"
#include "app/SettingsChangeDeferrer.h"
#include "backend/FakeAirPlayReceiver.h"
#include "platform/FakeHotkeyService.h"

namespace {

class CountingPersistence final : public SettingsPersistence {
public:
    AppSettingsSaveResult save(const AppSettings &) const override {
        ++saveCount;
        return {};
    }

    mutable int saveCount = 0;
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

        QCOMPARE(plan.validationResults.size(), 13);
        verifyValidationFailure(plan, SettingsFieldId::receiverName(),
                                QStringLiteral("Receiver name cannot be empty."));
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
        for (qsizetype index = 0; index < plan.validationResults.size(); ++index) {
            const SettingsFieldId &field = allSettingsFields().at(index);
            QCOMPARE(plan.validationResults.at(index).field, field);
            QVERIFY(plan.validationResults.at(index).attemptedValue
                    == settingsFieldValue(candidate, field));
        }
        QVERIFY(std::holds_alternative<QString>(plan.validationResults.at(0).attemptedValue));
        QVERIFY(std::holds_alternative<VideoResolution>(plan.validationResults.at(1).attemptedValue));
        QVERIFY(std::holds_alternative<VideoFrameRate>(plan.validationResults.at(2).attemptedValue));
        QVERIFY(std::holds_alternative<QKeySequence>(plan.validationResults.at(3).attemptedValue));
        QVERIFY(std::holds_alternative<RecordingFormat>(plan.validationResults.at(10).attemptedValue));
        QVERIFY(std::holds_alternative<QString>(plan.validationResults.at(11).attemptedValue));
        QVERIFY(std::holds_alternative<bool>(plan.validationResults.at(12).attemptedValue));
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
        QCOMPARE(nullPlan.validationResults.size(), 13);
    }
};

QTEST_MAIN(SettingsApplyCoordinatorTest)
#include "SettingsApplyCoordinatorTest.moc"
