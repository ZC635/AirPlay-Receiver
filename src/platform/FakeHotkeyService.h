#pragma once

#include "platform/HotkeyService.h"

#include <QHash>
#include <QVector>

#include <optional>

class FakeHotkeyService final : public HotkeyService {
public:
    struct Registration {
        ShortcutAction action;
        QKeySequence sequence;
    };

    struct Rejection {
        ShortcutAction action;
        QKeySequence sequence;
        std::optional<quint32> nativeCode;
        QString message;
    };

    using HotkeyService::HotkeyService;

    HotkeyRegistrationResult registerShortcut(ShortcutAction action,
                                              const QKeySequence &sequence) override {
        if (sequence.isEmpty() || sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown) {
            return {false, false, HotkeyError{std::nullopt,
                                              QStringLiteral("Shortcut is empty or unsupported.")}};
        }

        const int id = static_cast<int>(action);
        const auto active = activeBindings.constFind(id);
        if (active != activeBindings.constEnd() && active.value() == sequence) {
            return {true, true};
        }

        const std::optional<QKeySequence> previous = active == activeBindings.constEnd()
            ? std::nullopt
            : std::optional<QKeySequence>(active.value());
        if (previous.has_value()) {
            activeBindings.remove(id);
            removeRegistration(action);
        }

        attempts.append({action, sequence});
        if (const auto rejection = rejectionFor(action, sequence); rejection.has_value()) {
            HotkeyRegistrationResult result;
            result.error = HotkeyError{rejection->nativeCode, rejection->message};
            if (previous.has_value()) {
                attempts.append({action, *previous});
                if (const auto recovery = rejectionFor(action, *previous); recovery.has_value()) {
                    result.recoveryError = HotkeyError{recovery->nativeCode, recovery->message};
                } else {
                    activeBindings.insert(id, *previous);
                    registrations.append({action, *previous});
                    result.previousRestored = true;
                }
            }
            return result;
        }

        activeBindings.insert(id, sequence);
        registrations.append({action, sequence});
        return {true};
    }

    void unregisterAll() override {
        registrations.clear();
        activeBindings.clear();
    }

    void reject(ShortcutAction action, const QKeySequence &sequence,
                std::optional<quint32> nativeCode = std::nullopt,
                const QString &message = QStringLiteral("Hotkey registration was rejected.")) {
        rejections.append({action, sequence, nativeCode, message});
    }

    QVector<Registration> registrations;
    QVector<Registration> attempts;
    QVector<Registration> rejectedRegistrations;
    QVector<Rejection> rejections;
    QHash<int, QKeySequence> activeBindings;

private:
    std::optional<Rejection> rejectionFor(ShortcutAction action, const QKeySequence &sequence) const {
        for (const Rejection &rejection : rejections) {
            if (rejection.action == action && rejection.sequence == sequence) {
                return rejection;
            }
        }
        for (const Registration &rejection : rejectedRegistrations) {
            if (rejection.action == action && rejection.sequence == sequence) {
                return Rejection{action, sequence, std::nullopt,
                                 QStringLiteral("Hotkey registration was rejected.")};
            }
        }
        return std::nullopt;
    }

    void removeRegistration(ShortcutAction action) {
        for (qsizetype index = 0; index < registrations.size(); ++index) {
            if (registrations[index].action == action) {
                registrations.removeAt(index);
                return;
            }
        }
    }
};
