#include <QtTest/QtTest>

#include <algorithm>
#include <variant>

#include "app/AppSettings.h"
#include "app/SettingsApplyTypes.h"

class SettingsApplyTypesTest : public QObject {
    Q_OBJECT

private slots:
    void containsEverySettingsDialogField() {
        const QVector<SettingsFieldId> fields = allSettingsFields();
        QCOMPARE(fields.size(), 13);
        QCOMPARE(std::count_if(fields.cbegin(), fields.cend(), [](const SettingsFieldId &id) {
            return id.kind == SettingsFieldKind::Shortcut;
        }), 7);
    }

    void copiesOnlyRequestedField() {
        AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        candidate.setReceiverName("Desk Receiver");
        candidate.setShortcut(ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+H"));
        copySettingsField(candidate, SettingsFieldId::receiverName(), &baseline);
        QCOMPARE(baseline.receiverName(), QString("Desk Receiver"));
        QCOMPARE(baseline.shortcutFor(ShortcutAction::ToggleToolbar),
                 AppSettings::defaults().shortcutFor(ShortcutAction::ToggleToolbar));
    }

    void hasDeterministicFieldOrderAndDisplayNames() {
        const QVector<SettingsFieldId> fields = allSettingsFields();
        const QVector<SettingsFieldId> expected = {
            SettingsFieldId::receiverName(),
            SettingsFieldId::videoResolution(),
            SettingsFieldId::videoFrameRate(),
            SettingsFieldId::shortcut(ShortcutAction::ToggleAlwaysOnTop),
            SettingsFieldId::shortcut(ShortcutAction::VolumeUp),
            SettingsFieldId::shortcut(ShortcutAction::VolumeDown),
            SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar),
            SettingsFieldId::shortcut(ShortcutAction::ToggleAspectRatio),
            SettingsFieldId::shortcut(ShortcutAction::ToggleVideoFit),
            SettingsFieldId::shortcut(ShortcutAction::ToggleRecording),
            SettingsFieldId::recordingFormat(),
            SettingsFieldId::recordingOutputDirectory(),
            SettingsFieldId::recordingCompletionNotification(),
        };
        const QStringList expectedNames = {
            "Receiver name", "Resolution", "Frame rate", "Toggle always on top",
            "Volume up", "Volume down", "Toggle toolbar", "Toggle aspect ratio",
            "Toggle video fit", "Toggle recording", "Format", "Output folder",
            "Show a message when recording completes",
        };

        QCOMPARE(fields, expected);
        for (qsizetype index = 0; index < fields.size(); ++index) {
            QCOMPARE(settingsFieldDisplayName(fields.at(index)), expectedNames.at(index));
        }
        QVERIFY(SettingsFieldId::shortcut(ShortcutAction::VolumeUp)
                != SettingsFieldId::shortcut(ShortcutAction::VolumeDown));
        QVERIFY(!SettingsFieldId::receiverName().shortcutAction.has_value());
        QCOMPARE(SettingsFieldId::shortcut(ShortcutAction::VolumeUp).shortcutAction,
                 std::optional<ShortcutAction>(ShortcutAction::VolumeUp));
    }

    void readsEveryFieldValue() {
        AppSettings settings = AppSettings::defaults();
        VideoQualitySettings quality;
        quality.resolution = VideoResolution::P720;
        quality.frameRate = VideoFrameRate::Fps60;
        settings.setReceiverName("Desk Receiver");
        settings.setVideoQuality(quality);
        settings.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Shift+A"));
        settings.setShortcut(ShortcutAction::VolumeUp, QKeySequence("Ctrl+Shift+U"));
        settings.setShortcut(ShortcutAction::VolumeDown, QKeySequence("Ctrl+Shift+D"));
        settings.setShortcut(ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+T"));
        settings.setShortcut(ShortcutAction::ToggleAspectRatio, QKeySequence("Ctrl+Shift+R"));
        settings.setShortcut(ShortcutAction::ToggleVideoFit, QKeySequence("Ctrl+Shift+F"));
        settings.setShortcut(ShortcutAction::ToggleRecording, QKeySequence("Ctrl+Shift+G"));
        settings.setRecordingOutputDirectory("field-value-recordings");
        settings.setShowRecordingCompletionMessage(false);

        QCOMPARE(std::get<QString>(settingsFieldValue(settings, SettingsFieldId::receiverName())),
                 QString("Desk Receiver"));
        QCOMPARE(std::get<VideoResolution>(settingsFieldValue(settings, SettingsFieldId::videoResolution())),
                 VideoResolution::P720);
        QCOMPARE(std::get<VideoFrameRate>(settingsFieldValue(settings, SettingsFieldId::videoFrameRate())),
                 VideoFrameRate::Fps60);
        for (const SettingsFieldId &field : allSettingsFields()) {
            if (field.kind == SettingsFieldKind::Shortcut) {
                QCOMPARE(std::get<QKeySequence>(settingsFieldValue(settings, field)),
                         QKeySequence(settings.shortcutFor(*field.shortcutAction)));
            }
        }
        QCOMPARE(std::get<RecordingFormat>(settingsFieldValue(settings, SettingsFieldId::recordingFormat())),
                 RecordingFormat::Mp4);
        QCOMPARE(std::get<QString>(settingsFieldValue(settings, SettingsFieldId::recordingOutputDirectory())),
                 settings.recordingOutputDirectory());
        QCOMPARE(std::get<bool>(settingsFieldValue(
                     settings, SettingsFieldId::recordingCompletionNotification())), false);
    }

    void copiesEverySupportedField() {
        AppSettings baseline = AppSettings::defaults();
        AppSettings candidate = baseline;
        VideoQualitySettings quality;
        quality.resolution = VideoResolution::P540;
        quality.frameRate = VideoFrameRate::Fps15;
        candidate.setReceiverName("Copy destination");
        candidate.setVideoQuality(quality);
        for (const SettingsFieldId &field : allSettingsFields()) {
            if (field.kind == SettingsFieldKind::Shortcut) {
                candidate.setShortcut(*field.shortcutAction, QKeySequence("Ctrl+Shift+K"));
            }
        }
        candidate.setRecordingOutputDirectory("copy-field-recordings");
        candidate.setShowRecordingCompletionMessage(false);

        for (const SettingsFieldId &field : allSettingsFields()) {
            copySettingsField(candidate, field, &baseline);
            QCOMPARE(settingsFieldValue(baseline, field), settingsFieldValue(candidate, field));
        }
    }

    void formatsEverySupportedValue() {
        QCOMPARE(formatSettingsFieldValue(SettingsFieldValue(QString("Desk Receiver"))),
                 QString("Desk Receiver"));
        QCOMPARE(formatSettingsFieldValue(SettingsFieldValue(VideoResolution::P540)), QString("540p"));
        QCOMPARE(formatSettingsFieldValue(SettingsFieldValue(VideoResolution::P720)), QString("720p"));
        QCOMPARE(formatSettingsFieldValue(SettingsFieldValue(VideoResolution::P1080)), QString("1080p"));
        QCOMPARE(formatSettingsFieldValue(SettingsFieldValue(VideoFrameRate::Fps15)), QString("15 fps"));
        QCOMPARE(formatSettingsFieldValue(SettingsFieldValue(VideoFrameRate::Fps30)), QString("30 fps"));
        QCOMPARE(formatSettingsFieldValue(SettingsFieldValue(VideoFrameRate::Fps60)), QString("60 fps"));
        QCOMPARE(formatSettingsFieldValue(SettingsFieldValue(QKeySequence("Ctrl+Shift+H"))),
                 QKeySequence("Ctrl+Shift+H").toString(QKeySequence::NativeText));
        QCOMPARE(formatSettingsFieldValue(SettingsFieldValue(RecordingFormat::Mp4)), QString("MP4"));
        QCOMPARE(formatSettingsFieldValue(SettingsFieldValue(true)), QString("Enabled"));
        QCOMPARE(formatSettingsFieldValue(SettingsFieldValue(false)), QString("Disabled"));
    }

    void findsResultsAndClassifiesFailureStatuses() {
        SettingsFieldResult result;
        result.field = SettingsFieldId::videoResolution();
        result.attemptedValue = VideoResolution::P720;
        QVector<SettingsFieldResult> results = {result};

        QCOMPARE(resultForField(results, SettingsFieldId::videoResolution()), &results.front());
        QCOMPARE(resultForField(results, SettingsFieldId::videoFrameRate()), nullptr);
        QCOMPARE(result.status, SettingsFieldStatus::Unchanged);
        QVERIFY(!isFailureStatus(SettingsFieldStatus::Applied));
        QVERIFY(!isFailureStatus(SettingsFieldStatus::Unchanged));
        QVERIFY(!isFailureStatus(SettingsFieldStatus::Deferred));
        QVERIFY(isFailureStatus(SettingsFieldStatus::ValidationFailed));
        QVERIFY(isFailureStatus(SettingsFieldStatus::ApplyFailedRolledBack));
        QVERIFY(isFailureStatus(SettingsFieldStatus::RecoveryFailed));
    }

    void declaresReceiverTimingAndReservedGlobalFailureStatus() {
        const SettingsApplyPlan plan;
        QCOMPARE(plan.validationResults.size(), 0);
        QVERIFY(!plan.receiverSessionActive);
        QVERIFY(!plan.recordingIdle);
        QVERIFY(!plan.requiresReceiverTimingDecision);
        QVERIFY(ReceiverApplyTiming::Immediate != ReceiverApplyTiming::AfterDisconnect);
        QVERIFY(SettingsApplyGlobalStatus::PersistenceFailed
                == SettingsApplyGlobalStatus::PersistenceFailed);
    }
};

QTEST_MAIN(SettingsApplyTypesTest)
#include "SettingsApplyTypesTest.moc"
