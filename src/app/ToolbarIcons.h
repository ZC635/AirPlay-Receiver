#pragma once

#include <QIcon>
#include <QPalette>

namespace ToolbarIcons {

enum class Glyph {
    Volume,
    Pin,
    AspectRatio,
    VideoFit,
    Record,
    Stop,
    Saving,
    Fullscreen,
    ExitFullscreen,
    Settings
};

QIcon create(Glyph glyph, const QPalette &palette);

} // namespace ToolbarIcons
