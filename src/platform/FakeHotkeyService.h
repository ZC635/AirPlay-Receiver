#pragma once

#include "platform/HotkeyService.h"

class FakeHotkeyService final : public HotkeyService {
public:
    using HotkeyService::HotkeyService;

    bool registerShortcut(ShortcutAction, const QKeySequence &) override {
        return true;
    }

    void unregisterAll() override {}
};
