#include "app/AppSettings.h"

#include <algorithm>
#include <QSet>

bool AppKeySequence::isValid() const {
    return !isEmpty();
}

AppSettings AppSettings::defaults() {
    AppSettings settings;
    settings.setShortcut(ShortcutAction::ToggleAlwaysOnTop, QKeySequence("Ctrl+Alt+T"));
    settings.setShortcut(ShortcutAction::VolumeUp, QKeySequence("Ctrl+Alt+Up"));
    settings.setShortcut(ShortcutAction::VolumeDown, QKeySequence("Ctrl+Alt+Down"));
    settings.setShortcut(ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Alt+B"));
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
