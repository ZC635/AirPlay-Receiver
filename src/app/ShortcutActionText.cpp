#include "app/ShortcutActionText.h"

#include <QCoreApplication>

QString shortcutActionDisplayName(ShortcutAction action) {
    switch (action) {
    case ShortcutAction::ToggleAlwaysOnTop:
        return QCoreApplication::translate("ShortcutActions", "Toggle always on top");
    case ShortcutAction::VolumeUp:
        return QCoreApplication::translate("ShortcutActions", "Volume up");
    case ShortcutAction::VolumeDown:
        return QCoreApplication::translate("ShortcutActions", "Volume down");
    case ShortcutAction::ToggleToolbar:
        return QCoreApplication::translate("ShortcutActions", "Toggle toolbar");
    case ShortcutAction::ToggleAspectRatio:
        return QCoreApplication::translate("ShortcutActions", "Toggle aspect ratio");
    case ShortcutAction::ToggleVideoFit:
        return QCoreApplication::translate("ShortcutActions", "Toggle video fit");
    case ShortcutAction::ToggleRecording:
        return QCoreApplication::translate("ShortcutActions", "Toggle recording");
    }
    return QCoreApplication::translate("ShortcutActions", "Shortcut");
}
