#include "app/ShortcutActionKey.h"

QString shortcutActionKey(ShortcutAction action) {
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
