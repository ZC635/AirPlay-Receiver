#include "app/AppSettingsStore.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <utility>

namespace {
QString keyFor(ShortcutAction action) {
    switch (action) {
    case ShortcutAction::ToggleAlwaysOnTop: return "toggleAlwaysOnTop";
    case ShortcutAction::VolumeUp: return "volumeUp";
    case ShortcutAction::VolumeDown: return "volumeDown";
    case ShortcutAction::ToggleToolbar: return "toggleToolbar";
    case ShortcutAction::ToggleAspectRatio: return "toggleAspectRatio";
    case ShortcutAction::ToggleVideoFit: return "toggleVideoFit";
    }
    return {};
}
}

AppSettingsStore::AppSettingsStore(QString path)
    : path_(std::move(path)) {}

AppSettings AppSettingsStore::loadOrDefaults() const {
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        return AppSettings::defaults();
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isNull() || !doc.isObject()) {
        return AppSettings::defaults();
    }

    const QJsonObject root = doc.object();
    const QJsonObject shortcuts = root.value("shortcuts").toObject();
    AppSettings settings = AppSettings::defaults();
    const QString receiverName = root.value("receiverName").toString().trimmed();
    if (!receiverName.isEmpty()) {
        settings.setReceiverName(receiverName);
    }
    for (const ShortcutBinding &binding : settings.shortcuts()) {
        const QString value = shortcuts.value(keyFor(binding.action)).toString();
        if (!value.isEmpty()) {
            const QKeySequence parsed = QKeySequence::fromString(value, QKeySequence::PortableText);
            if (!parsed.toString(QKeySequence::PortableText).isEmpty() && parsed.count() == 1) {
                settings.setShortcut(binding.action, parsed);
            }
        }
    }
    const QJsonValue volume = root.value("volume");
    if (volume.isDouble()) {
        settings.setVolume(volume.toInt());
    }
    const QJsonValue aspectLock = root.value("aspectRatioLock");
    if (aspectLock.isBool()) {
        settings.setAspectRatioLock(aspectLock.toBool());
    }
    const QJsonValue videoFit = root.value("videoFitMode");
    if (videoFit.isBool()) {
        settings.setVideoFitMode(videoFit.toBool());
    }
    return settings;
}

bool AppSettingsStore::save(const AppSettings &settings) const {
    QJsonObject shortcuts;
    for (const ShortcutBinding &binding : settings.shortcuts()) {
        shortcuts.insert(keyFor(binding.action), binding.sequence.toString(QKeySequence::PortableText));
    }

    QJsonObject root;
    root.insert("receiverName", settings.receiverName());
    root.insert("shortcuts", shortcuts);
    root.insert("volume", settings.volume());
    root.insert("aspectRatioLock", settings.aspectRatioLock());
    root.insert("videoFitMode", settings.videoFitMode());

    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        return false;
    }
    return file.commit();
}
