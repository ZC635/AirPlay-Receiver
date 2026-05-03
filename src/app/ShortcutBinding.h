#pragma once

#include <QKeySequence>
#include "app/ShortcutAction.h"

struct ShortcutBinding {
    ShortcutAction action;
    QKeySequence sequence;
};
