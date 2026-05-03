#include "app/AppSettingsStore.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

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
    for (const ShortcutBinding &binding : settings.shortcuts()) {
        const QString value = shortcuts.value(keyFor(binding.action)).toString();
        if (!value.isEmpty()) {
            settings.setShortcut(binding.action, QKeySequence(value));
        }
    }
    return settings;
}

bool AppSettingsStore::save(const AppSettings &settings) const {
    QJsonObject shortcuts;
    for (const ShortcutBinding &binding : settings.shortcuts()) {
        shortcuts.insert(keyFor(binding.action), binding.sequence.toString(QKeySequence::PortableText));
    }

    QJsonObject root;
    root.insert("shortcuts", shortcuts);

    QFile file(path_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}
