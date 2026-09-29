#include "app/SettingsApplyTypes.h"

#include "app/ShortcutActionText.h"

#include <QCoreApplication>

#include <algorithm>
#include <type_traits>

namespace {

QString formatVideoResolution(VideoResolution resolution) {
    switch (resolution) {
    case VideoResolution::P540:
        return "540p";
    case VideoResolution::P720:
        return "720p";
    case VideoResolution::P1080:
        return "1080p";
    }
    return {};
}

QString formatVideoFrameRate(VideoFrameRate frameRate) {
    switch (frameRate) {
    case VideoFrameRate::Fps15:
        return "15 fps";
    case VideoFrameRate::Fps30:
        return "30 fps";
    case VideoFrameRate::Fps60:
        return "60 fps";
    }
    return {};
}

QString formatRecordingFormat(RecordingFormat format) {
    switch (format) {
    case RecordingFormat::Mp4:
        return "MP4";
    }
    return {};
}

} // namespace

SettingsFieldId SettingsFieldId::receiverName() {
    return {SettingsFieldKind::ReceiverName, std::nullopt};
}

SettingsFieldId SettingsFieldId::language() {
    return {SettingsFieldKind::Language, std::nullopt};
}

SettingsFieldId SettingsFieldId::videoResolution() {
    return {SettingsFieldKind::VideoResolution, std::nullopt};
}

SettingsFieldId SettingsFieldId::videoFrameRate() {
    return {SettingsFieldKind::VideoFrameRate, std::nullopt};
}

SettingsFieldId SettingsFieldId::shortcut(ShortcutAction action) {
    return {SettingsFieldKind::Shortcut, action};
}

SettingsFieldId SettingsFieldId::recordingFormat() {
    return {SettingsFieldKind::RecordingFormat, std::nullopt};
}

SettingsFieldId SettingsFieldId::recordingOutputDirectory() {
    return {SettingsFieldKind::RecordingOutputDirectory, std::nullopt};
}

SettingsFieldId SettingsFieldId::recordingCompletionNotification() {
    return {SettingsFieldKind::RecordingCompletionNotification, std::nullopt};
}

SettingsFieldId SettingsFieldId::toolbarHoverReveal() {
    return {SettingsFieldKind::ToolbarHoverReveal, std::nullopt};
}

bool operator==(const SettingsFieldId &left, const SettingsFieldId &right) {
    return left.kind == right.kind && left.shortcutAction == right.shortcutAction;
}

bool operator!=(const SettingsFieldId &left, const SettingsFieldId &right) {
    return !(left == right);
}

QVector<SettingsFieldId> allSettingsFields() {
    return {
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
}

SettingsFieldValue settingsFieldValue(const AppSettings &settings, const SettingsFieldId &field) {
    switch (field.kind) {
    case SettingsFieldKind::ReceiverName:
        return settings.receiverName();
    case SettingsFieldKind::Language:
        return settings.language();
    case SettingsFieldKind::VideoResolution:
        return settings.videoQuality().resolution;
    case SettingsFieldKind::VideoFrameRate:
        return settings.videoQuality().frameRate;
    case SettingsFieldKind::Shortcut:
        return field.shortcutAction.has_value()
            ? QKeySequence(settings.shortcutFor(*field.shortcutAction))
            : QKeySequence();
    case SettingsFieldKind::RecordingFormat:
        return settings.recordingFormat();
    case SettingsFieldKind::RecordingOutputDirectory:
        return settings.recordingOutputDirectory();
    case SettingsFieldKind::RecordingCompletionNotification:
        return settings.showRecordingCompletionMessage();
    case SettingsFieldKind::ToolbarHoverReveal:
        return settings.toolbarHoverReveal();
    }
    return QString();
}

void copySettingsField(const AppSettings &source,
                       const SettingsFieldId &field,
                       AppSettings *destination) {
    if (destination == nullptr) {
        return;
    }

    switch (field.kind) {
    case SettingsFieldKind::ReceiverName:
        destination->setReceiverName(source.receiverName());
        return;
    case SettingsFieldKind::Language:
        destination->setLanguage(source.language());
        return;
    case SettingsFieldKind::VideoResolution: {
        VideoQualitySettings quality = destination->videoQuality();
        quality.resolution = source.videoQuality().resolution;
        destination->setVideoQuality(quality);
        return;
    }
    case SettingsFieldKind::VideoFrameRate: {
        VideoQualitySettings quality = destination->videoQuality();
        quality.frameRate = source.videoQuality().frameRate;
        destination->setVideoQuality(quality);
        return;
    }
    case SettingsFieldKind::Shortcut:
        if (field.shortcutAction.has_value()) {
            destination->setShortcut(*field.shortcutAction,
                                     source.shortcutFor(*field.shortcutAction));
        }
        return;
    case SettingsFieldKind::RecordingFormat:
        destination->setRecordingFormat(source.recordingFormat());
        return;
    case SettingsFieldKind::RecordingOutputDirectory:
        destination->setRecordingOutputDirectory(source.recordingOutputDirectory());
        return;
    case SettingsFieldKind::RecordingCompletionNotification:
        destination->setShowRecordingCompletionMessage(source.showRecordingCompletionMessage());
        return;
    case SettingsFieldKind::ToolbarHoverReveal:
        destination->setToolbarHoverReveal(source.toolbarHoverReveal());
        return;
    }
}

QString settingsFieldDisplayName(const SettingsFieldId &field) {
    switch (field.kind) {
    case SettingsFieldKind::ReceiverName:
        return QCoreApplication::translate("SettingsFields", "Receiver name");
    case SettingsFieldKind::Language:
        return QCoreApplication::translate("SettingsFields", "Language");
    case SettingsFieldKind::VideoResolution:
        return QCoreApplication::translate("SettingsFields", "Resolution");
    case SettingsFieldKind::VideoFrameRate:
        return QCoreApplication::translate("SettingsFields", "Frame rate");
    case SettingsFieldKind::Shortcut:
        return field.shortcutAction.has_value()
            ? shortcutActionDisplayName(*field.shortcutAction)
            : QCoreApplication::translate("SettingsFields", "Shortcut");
    case SettingsFieldKind::RecordingFormat:
        return QCoreApplication::translate("SettingsFields", "Format");
    case SettingsFieldKind::RecordingOutputDirectory:
        return QCoreApplication::translate("SettingsFields", "Output folder");
    case SettingsFieldKind::RecordingCompletionNotification:
        return QCoreApplication::translate("SettingsFields",
                                           "Show a message when recording completes");
    case SettingsFieldKind::ToolbarHoverReveal:
        return QCoreApplication::translate("SettingsFields",
                                           "Show hidden toolbar when the pointer reaches the top");
    }
    return {};
}

QString formatSettingsFieldValue(const SettingsFieldValue &value) {
    return std::visit([](const auto &fieldValue) -> QString {
        using ValueType = std::decay_t<decltype(fieldValue)>;
        if constexpr (std::is_same_v<ValueType, QString>) {
            return fieldValue;
        } else if constexpr (std::is_same_v<ValueType, VideoResolution>) {
            return formatVideoResolution(fieldValue);
        } else if constexpr (std::is_same_v<ValueType, VideoFrameRate>) {
            return formatVideoFrameRate(fieldValue);
        } else if constexpr (std::is_same_v<ValueType, QKeySequence>) {
            return fieldValue.toString(QKeySequence::NativeText);
        } else if constexpr (std::is_same_v<ValueType, RecordingFormat>) {
            return formatRecordingFormat(fieldValue);
        } else {
            return fieldValue
                ? QCoreApplication::translate("SettingsFields", "Enabled")
                : QCoreApplication::translate("SettingsFields", "Disabled");
        }
    }, value);
}

const SettingsFieldResult *resultForField(const QVector<SettingsFieldResult> &results,
                                          const SettingsFieldId &field) {
    const auto result = std::find_if(results.cbegin(), results.cend(), [&field](const SettingsFieldResult &item) {
        return item.field == field;
    });
    return result == results.cend() ? nullptr : &*result;
}

bool isFailureStatus(SettingsFieldStatus status) {
    return status == SettingsFieldStatus::ValidationFailed
        || status == SettingsFieldStatus::ApplyFailedRolledBack
        || status == SettingsFieldStatus::RecoveryFailed;
}
