#pragma once

#include <QKeySequence>
#include <QObject>

#include "app/ShortcutAction.h"

class HotkeyService : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    virtual bool registerShortcut(ShortcutAction action, const QKeySequence &sequence) = 0;
    virtual void unregisterAll() = 0;

signals:
    void activated(ShortcutAction action);
};
