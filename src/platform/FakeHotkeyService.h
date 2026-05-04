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
        for (const Registration &registration : rejectedRegistrations) {
            if (registration.action == action && registration.sequence == sequence) {
                return false;
            }
        }
        registrations.append({action, sequence});
        return true;
    }

    void unregisterAll() override {
        registrations.clear();
    }

    QVector<Registration> registrations;
    QVector<Registration> rejectedRegistrations;
};
