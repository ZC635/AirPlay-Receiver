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

    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject shortcuts = root.value("shortcuts").toObject();
    AppSettings settings = AppSettings::defaults();
    const QString receiverName = root.value("receiverName").toString().trimmed();
    if (!receiverName.isEmpty()) {
        settings.setReceiverName(receiverName);
    }
    for (const ShortcutBinding &binding : settings.shortcuts()) {
        const QString value = shortcuts.value(keyFor(binding.action)).toString();
        if (!value.isEmpty()) {
            settings.setShortcut(binding.action, QKeySequence::fromString(value, QKeySequence::PortableText));
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
