#include "app/AppSettings.h"

#include <algorithm>
#include <utility>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>

bool AppKeySequence::isValid() const {
    return !isEmpty();
}

AppSettings AppSettings::defaults() {
    AppSettings settings;
    settings.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+T"));
    settings.setShortcut(ShortcutAction::VolumeUp, QKeySequence("Ctrl+Alt+Up"));
    settings.setShortcut(ShortcutAction::VolumeDown, QKeySequence("Ctrl+Alt+Down"));
    settings.setShortcut(ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+B"));
    settings.setShortcut(ShortcutAction::ToggleAspectRatio, QKeySequence("Ctrl+Alt+A"));
    settings.setShortcut(ShortcutAction::ToggleVideoFit, QKeySequence("Ctrl+Alt+F"));
    settings.setShortcut(ShortcutAction::ToggleRecording, QKeySequence("Ctrl+Alt+R"));
    const QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    const QString outputBase = movies.isEmpty()
        ? QDir(QDir::homePath()).filePath("Videos")
        : movies;
    settings.setRecordingOutputDirectory(QDir(outputBase).filePath("AirPlay Receiver Recording"));
    return settings;
}

QVector<ShortcutBinding> AppSettings::shortcuts() const {
    QVector<ShortcutBinding> result;
    for (auto it = shortcuts_.cbegin(); it != shortcuts_.cend(); ++it) {
        result.push_back({static_cast<ShortcutAction>(it.key()), it.value()});
    }
    return result;
}

AppKeySequence AppSettings::shortcutFor(ShortcutAction action) const {
    return shortcuts_.value(static_cast<int>(action));
}

void AppSettings::setShortcut(ShortcutAction action, const QKeySequence &sequence) {
    shortcuts_.insert(static_cast<int>(action), sequence);
}

int AppSettings::volume() const {
    return volume_;
}

void AppSettings::setVolume(int value) {
    volume_ = std::clamp(value, 0, 100);
}

QString AppSettings::receiverName() const {
    return receiverName_;
}

void AppSettings::setReceiverName(QString name) {
    receiverName_ = std::move(name).trimmed();
}

QString AppSettings::language() const {
    return language_;
}

void AppSettings::setLanguage(QString language) {
    language_ = std::move(language).trimmed();
    if (language_.isEmpty()) {
        language_ = QStringLiteral("system");
    }
}

bool AppSettings::aspectRatioLock() const {
    return aspectRatioLock_;
}

void AppSettings::setAspectRatioLock(bool enabled) {
    aspectRatioLock_ = enabled;
}

bool AppSettings::videoFitMode() const {
    return videoFitMode_;
}

void AppSettings::setVideoFitMode(bool enabled) {
    videoFitMode_ = enabled;
}

bool AppSettings::toolbarHoverReveal() const {
    return toolbarHoverReveal_;
}

void AppSettings::setToolbarHoverReveal(bool enabled) {
    toolbarHoverReveal_ = enabled;
}

VideoQualitySettings AppSettings::videoQuality() const {
    return videoQuality_;
}

void AppSettings::setVideoQuality(VideoQualitySettings quality) {
    videoQuality_ = quality;
}

RecordingFormat AppSettings::recordingFormat() const {
    return recordingFormat_;
}

void AppSettings::setRecordingFormat(RecordingFormat format) {
    recordingFormat_ = format;
}

QString AppSettings::recordingOutputDirectory() const {
    return recordingOutputDirectory_;
}

void AppSettings::setRecordingOutputDirectory(QString path) {
    recordingOutputDirectory_ = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool AppSettings::showRecordingCompletionMessage() const {
    return showRecordingCompletionMessage_;
}

void AppSettings::setShowRecordingCompletionMessage(bool enabled) {
    showRecordingCompletionMessage_ = enabled;
}

QStringList AppSettings::validateGeneral() const {
    QStringList errors;
    if (receiverName_.trimmed().isEmpty()) {
        errors.push_back("Receiver name cannot be empty");
    }
    return errors;
}

QStringList AppSettings::validateShortcuts() const {
    QStringList errors;
    QSet<QString> seen;
    for (const ShortcutBinding &binding : shortcuts()) {
        const QString normalized = binding.sequence.toString(QKeySequence::PortableText);
        if (normalized.isEmpty()) {
            errors.push_back("Shortcut cannot be empty");
            continue;
        }
        if (seen.contains(normalized)) {
            errors.push_back(QString("Duplicate shortcut: %1").arg(normalized));
        }
        seen.insert(normalized);
    }
    return errors;
}
