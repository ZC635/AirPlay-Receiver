#include <QtTest/QtTest>

#include <QTranslator>

#include <algorithm>
#include <variant>

#include "app/AppSettings.h"
#include "app/SettingsApplyTypes.h"

namespace {

class BooleanValueTranslator final : public QTranslator {
public:
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override {
        Q_UNUSED(disambiguation);
        Q_UNUSED(n);
        if (qstrcmp(context, "SettingsFields") == 0) {
            if (qstrcmp(sourceText, "Enabled") == 0) {
                return QString::fromUtf8(u8"已启用");
            }
            if (qstrcmp(sourceText, "Disabled") == 0) {
                return QString::fromUtf8(u8"已禁用");
            }
        }
        return {};
    }
};

class InstalledTranslator final {
public:
    explicit InstalledTranslator(QTranslator *translator) : translator_(translator) {
        QCoreApplication::installTranslator(translator_);
    }
    ~InstalledTranslator() {
        QCoreApplication::removeTranslator(translator_);
    }

private:
    QTranslator *translator_;
};

} // namespace

class SettingsApplyTypesTest : public QObject {
    Q_OBJECT

private slots:
    void containsEverySettingsDialogField() {
        const QVector<SettingsFieldId> fields = allSettingsFields();
        QCOMPARE(fields.size(), 15);
        QCOMPARE(std::count_if(fields.cbegin(), fields.cend(), [](const SettingsFieldId &id) {
            return id.kind == SettingsFieldKind::Shortcut;
        }), 7);
        QVERIFY(fields.contains(SettingsFieldId::language()));
        QVERIFY(fields.contains(SettingsFieldId::toolbarHoverReveal()));
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
            SettingsFieldId::language(),
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
            SettingsFieldId::toolbarHoverReveal(),
        };
        const QStringList expectedNames = {
            "Receiver name", "Language", "Resolution", "Frame rate", "Toggle always on top",
            "Volume up", "Volume down", "Toggle toolbar", "Toggle aspect ratio",
            "Toggle video fit", "Toggle recording", "Format", "Output folder",
            "Show a message when recording completes",
            "Show hidden toolbar when the pointer reaches the top",
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
        settings.setLanguage("zh-CN");
        settings.setToolbarHoverReveal(false);
        settings.setRecordingOutputDirectory("field-value-recordings");
        settings.setShowRecordingCompletionMessage(false);

        QCOMPARE(std::get<QString>(settingsFieldValue(settings, SettingsFieldId::receiverName())),
                 QString("Desk Receiver"));
        QCOMPARE(std::get<QString>(settingsFieldValue(settings, SettingsFieldId::language())),
                 QString("zh-CN"));
        QCOMPARE(std::get<bool>(settingsFieldValue(settings, SettingsFieldId::toolbarHoverReveal())), false);
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
        candidate.setLanguage("zh-CN");
        candidate.setToolbarHoverReveal(false);
        candidate.setRecordingOutputDirectory("copy-field-recordings");
        candidate.setShowRecordingCompletionMessage(false);

        for (const SettingsFieldId &field : allSettingsFields()) {
            copySettingsField(candidate, field, &baseline);
            QCOMPARE(settingsFieldValue(baseline, field), settingsFieldValue(candidate, field));
        }
    }

    void copiesVideoResolutionWithoutChangingDestinationFrameRate() {
        AppSettings source = AppSettings::defaults();
        AppSettings destination = AppSettings::defaults();
        source.setVideoQuality({VideoResolution::P540, VideoFrameRate::Fps60});
        destination.setVideoQuality({VideoResolution::P1080, VideoFrameRate::Fps15});

        copySettingsField(source, SettingsFieldId::videoResolution(), &destination);

        QCOMPARE(destination.videoQuality().resolution, VideoResolution::P540);
        QCOMPARE(destination.videoQuality().frameRate, VideoFrameRate::Fps15);
    }

    void copiesVideoFrameRateWithoutChangingDestinationResolution() {
        AppSettings source = AppSettings::defaults();
        AppSettings destination = AppSettings::defaults();
        source.setVideoQuality({VideoResolution::P540, VideoFrameRate::Fps60});
        destination.setVideoQuality({VideoResolution::P1080, VideoFrameRate::Fps15});

        copySettingsField(source, SettingsFieldId::videoFrameRate(), &destination);

        QCOMPARE(destination.videoQuality().resolution, VideoResolution::P1080);
        QCOMPARE(destination.videoQuality().frameRate, VideoFrameRate::Fps60);
    }

    void copiesLanguageIndependently() {
        AppSettings source = AppSettings::defaults();
        AppSettings destination = AppSettings::defaults();
        source.setLanguage("zh-CN");
        destination.setReceiverName("Destination");

        copySettingsField(source, SettingsFieldId::language(), &destination);

        QCOMPARE(destination.language(), QString("zh-CN"));
        QCOMPARE(destination.receiverName(), QString("Destination"));
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

    void translatesBooleanValuesThroughStableContext() {
        BooleanValueTranslator translator;
        const InstalledTranslator installed(&translator);

        QCOMPARE(formatSettingsFieldValue(SettingsFieldValue(true)),
                 QString::fromUtf8(u8"已启用"));
        QCOMPARE(formatSettingsFieldValue(SettingsFieldValue(false)),
                 QString::fromUtf8(u8"已禁用"));
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
        SettingsSubmitResult result;
        result.status = SettingsSubmitStatus::TimingSelectionRequired;
        QVERIFY(!result.outcome);
        QVERIFY(!result.settingsSaved);
        QVERIFY(!result.backendInvoked);
        QVERIFY(!result.backendResult);
        QVERIFY(ReceiverApplyTiming::Immediate != ReceiverApplyTiming::AfterDisconnect);
        QVERIFY(SettingsApplyGlobalStatus::PersistenceFailed
                == SettingsApplyGlobalStatus::PersistenceFailed);
    }

    void storesPersistenceFailureInGlobalOutcome() {
        SettingsApplyOutcome outcome;
        outcome.committedSettings = AppSettings::defaults();
        outcome.fieldResults = {{SettingsFieldId::receiverName(),
                                 QString("Desk Receiver"),
                                 SettingsFieldStatus::Applied,
                                 {},
                                 std::nullopt,
                                 {}}};
        outcome.globalResult = SettingsApplyGlobalResult{
            SettingsApplyGlobalStatus::PersistenceFailed,
            {false,
             "settings.json",
             AppSettingsSaveStage::Commit,
             QFileDevice::RenameError,
             "cannot replace settings"},
        };
        outcome.airPlayDeferred = true;

        QCOMPARE(outcome.committedSettings.receiverName(), AppSettings::defaults().receiverName());
        QCOMPARE(outcome.fieldResults.size(), 1);
        QVERIFY(outcome.globalResult.has_value());
        QCOMPARE(outcome.globalResult->status, SettingsApplyGlobalStatus::PersistenceFailed);
        QCOMPARE(outcome.globalResult->persistence.failureStage,
                 std::optional<AppSettingsSaveStage>(AppSettingsSaveStage::Commit));
        QVERIFY(outcome.airPlayDeferred);
        QVERIFY(!outcome.mayClose);
    }
};

QTEST_MAIN(SettingsApplyTypesTest)
#include "SettingsApplyTypesTest.moc"
