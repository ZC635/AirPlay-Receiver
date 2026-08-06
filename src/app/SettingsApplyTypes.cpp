#include "app/SettingsApplyTypes.h"

#include <algorithm>
#include <type_traits>

namespace {

QString shortcutDisplayName(ShortcutAction action) {
    switch (action) {
    case ShortcutAction::ToggleAlwaysOnTop:
        return "Toggle always on top";
    case ShortcutAction::VolumeUp:
        return "Volume up";
    case ShortcutAction::VolumeDown:
        return "Volume down";
    case ShortcutAction::ToggleToolbar:
        return "Toggle toolbar";
    case ShortcutAction::ToggleAspectRatio:
        return "Toggle aspect ratio";
    case ShortcutAction::ToggleVideoFit:
        return "Toggle video fit";
    case ShortcutAction::ToggleRecording:
        return "Toggle recording";
    }
    return "Shortcut";
}

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

bool operator==(const SettingsFieldId &left, const SettingsFieldId &right) {
    return left.kind == right.kind && left.shortcutAction == right.shortcutAction;
}

bool operator!=(const SettingsFieldId &left, const SettingsFieldId &right) {
    return !(left == right);
}

QVector<SettingsFieldId> allSettingsFields() {
    return {
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
}

SettingsFieldValue settingsFieldValue(const AppSettings &settings, const SettingsFieldId &field) {
    switch (field.kind) {
    case SettingsFieldKind::ReceiverName:
        return settings.receiverName();
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
    }
}

QString settingsFieldDisplayName(const SettingsFieldId &field) {
    switch (field.kind) {
    case SettingsFieldKind::ReceiverName:
        return "Receiver name";
    case SettingsFieldKind::VideoResolution:
        return "Resolution";
    case SettingsFieldKind::VideoFrameRate:
        return "Frame rate";
    case SettingsFieldKind::Shortcut:
        return field.shortcutAction.has_value() ? shortcutDisplayName(*field.shortcutAction) : "Shortcut";
    case SettingsFieldKind::RecordingFormat:
        return "Format";
    case SettingsFieldKind::RecordingOutputDirectory:
        return "Output folder";
    case SettingsFieldKind::RecordingCompletionNotification:
        return "Show a message when recording completes";
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
            return fieldValue ? "Enabled" : "Disabled";
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
