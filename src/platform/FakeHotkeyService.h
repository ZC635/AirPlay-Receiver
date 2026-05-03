#pragma once

#include "platform/HotkeyService.h"

#include <QVector>

class FakeHotkeyService final : public HotkeyService {
public:
    struct Registration {
        ShortcutAction action;
        QKeySequence sequence;
    };

    using HotkeyService::HotkeyService;

    bool registerShortcut(ShortcutAction action, const QKeySequence &sequence) override {
        registrations.append({action, sequence});
        return true;
    }

    void unregisterAll() override {
        registrations.clear();
    }

    QVector<Registration> registrations;
};
