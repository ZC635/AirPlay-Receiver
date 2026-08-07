#pragma once

#include "platform/HotkeyService.h"

#include <QHash>
#include <QVector>

#include <functional>
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
        if (!isValid(sequence)) {
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
            unregisterAction(action);
        }

        if (const auto rejection = attemptError(action, sequence); rejection.has_value()) {
            HotkeyRegistrationResult result;
            result.error = *rejection;
            if (previous.has_value()) {
                if (const auto recovery = attemptError(action, *previous); recovery.has_value()) {
                    result.recoveryError = *recovery;
                } else {
                    addActive(action, *previous);
                    result.previousRestored = true;
                }
            }
            return result;
        }

        addActive(action, sequence);
        return {true};
    }

    QVector<HotkeyActionRegistrationResult> registerShortcuts(
        const QVector<HotkeyRegistrationRequest> &requests) override {
        batchRequests.append(requests);
        QVector<HotkeyActionRegistrationResult> results;
        results.reserve(requests.size());
        for (const HotkeyRegistrationRequest &request : requests) {
            results.append({request.action, request.sequence, {}});
        }

        QVector<bool> changed(requests.size(), false);
        QHash<int, int> changedRequestForAction;
        QHash<QString, int> activeOwnerForSequence;
        for (auto active = activeBindings.cbegin(); active != activeBindings.cend(); ++active) {
            activeOwnerForSequence.insert(active.value().toString(QKeySequence::PortableText), active.key());
        }
        for (qsizetype index = 0; index < requests.size(); ++index) {
            const HotkeyRegistrationRequest &request = requests.at(index);
            const int id = static_cast<int>(request.action);
            const auto active = activeBindings.constFind(id);
            if (!isValid(request.sequence)
                || (active != activeBindings.constEnd() && active.value() == request.sequence)) {
                results[index].registration = registerShortcut(request.action, request.sequence);
                continue;
            }
            changed[index] = true;
            changedRequestForAction.insert(id, index);
        }

        QVector<int> dependency(requests.size(), -1);
        for (qsizetype index = 0; index < requests.size(); ++index) {
            if (!changed.at(index)) {
                continue;
            }
            const auto owner = activeOwnerForSequence.constFind(
                requests.at(index).sequence.toString(QKeySequence::PortableText));
            if (owner == activeOwnerForSequence.constEnd()
                || owner.value() == static_cast<int>(requests.at(index).action)) {
                continue;
            }
            const auto ownerRequest = changedRequestForAction.constFind(owner.value());
            if (ownerRequest != changedRequestForAction.constEnd()) {
                dependency[index] = ownerRequest.value();
            }
        }

        QVector<int> visitState(requests.size(), 0);
        QVector<int> visitStack;
        QVector<QVector<int>> cycles;
        std::function<void(int)> findCycles = [&](int index) {
            visitState[index] = 1;
            visitStack.append(index);
            const int next = dependency.at(index);
            if (next >= 0 && changed.at(next)) {
                if (visitState.at(next) == 0) {
                    findCycles(next);
                } else if (visitState.at(next) == 1) {
                    const qsizetype start = visitStack.indexOf(next);
                    QVector<int> cycle;
                    for (qsizetype member = start; member < visitStack.size(); ++member) {
                        cycle.append(visitStack.at(member));
                    }
                    cycles.append(cycle);
                }
            }
            visitStack.removeLast();
            visitState[index] = 2;
        };
        for (qsizetype index = 0; index < requests.size(); ++index) {
            if (changed.at(index) && visitState.at(index) == 0) {
                findCycles(index);
            }
        }

        QVector<bool> processed(requests.size(), false);
        for (qsizetype index = 0; index < requests.size(); ++index) {
            if (!changed.at(index)) {
                processed[index] = true;
            }
        }
        for (const QVector<int> &cycle : cycles) {
            QVector<Registration> previous;
            previous.reserve(cycle.size());
            for (int index : cycle) {
                const ShortcutAction action = requests.at(index).action;
                const auto active = activeBindings.constFind(static_cast<int>(action));
                if (active != activeBindings.constEnd()) {
                    previous.append({action, active.value()});
                    unregisterAction(action);
                }
            }

            QVector<int> registeredCandidates;
            int failedIndex = -1;
            for (int index : cycle) {
                const HotkeyRegistrationRequest &request = requests.at(index);
                if (const auto error = attemptError(request.action, request.sequence); error.has_value()) {
                    results[index].registration.error = *error;
                    failedIndex = index;
                    break;
                }
                addActive(request.action, request.sequence);
                registeredCandidates.append(index);
            }
            if (failedIndex < 0) {
                for (int index : cycle) {
                    results[index].registration = {true};
                    processed[index] = true;
                }
                continue;
            }

            for (int index : registeredCandidates) {
                unregisterAction(requests.at(index).action);
            }
            QHash<int, HotkeyError> recoveryErrors;
            QHash<int, bool> restored;
            for (const Registration &entry : previous) {
                if (const auto error = attemptError(entry.action, entry.sequence); error.has_value()) {
                    recoveryErrors.insert(static_cast<int>(entry.action), *error);
                } else {
                    addActive(entry.action, entry.sequence);
                    restored.insert(static_cast<int>(entry.action), true);
                }
            }
            for (int index : cycle) {
                HotkeyRegistrationResult &result = results[index].registration;
                const int id = static_cast<int>(requests.at(index).action);
                result.registered = false;
                result.unchanged = false;
                if (!result.error.has_value()) {
                    result.error = HotkeyError{std::nullopt,
                        QStringLiteral("Shortcut replacement was rolled back because another shortcut in this cycle could not be registered.")};
                }
                result.previousRestored = restored.value(id, false);
                if (recoveryErrors.contains(id)) {
                    result.recoveryError = recoveryErrors.value(id);
                }
                processed[index] = true;
            }
        }

        std::function<void(int)> processAcyclic = [&](int index) {
            if (processed.at(index)) {
                return;
            }
            const int owner = dependency.at(index);
            if (owner >= 0) {
                processAcyclic(owner);
                if (!results.at(owner).registration.registered) {
                    results[index].registration = {
                        false, false,
                        HotkeyError{std::nullopt,
                            QStringLiteral("Shortcut could not move because the action that owns its requested shortcut did not move.")},
                        true};
                    processed[index] = true;
                    return;
                }
            }
            results[index].registration = registerShortcut(requests.at(index).action,
                                                           requests.at(index).sequence);
            processed[index] = true;
        };
        for (qsizetype index = 0; index < requests.size(); ++index) {
            if (changed.at(index)) {
                processAcyclic(index);
            }
        }
        return results;
    }

    void unregisterAll() override {
        ++unregisterAllCount;
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
    QVector<QVector<HotkeyRegistrationRequest>> batchRequests;
    QVector<ShortcutAction> unregisteredActions;
    QVector<Registration> rejectedRegistrations;
    QVector<Rejection> rejections;
    QHash<int, QKeySequence> activeBindings;
    int unregisterAllCount = 0;

private:
    static bool isValid(const QKeySequence &sequence) {
        return !sequence.isEmpty() && sequence.count() == 1
            && sequence[0].key() != Qt::Key_unknown;
    }

    std::optional<HotkeyError> attemptError(ShortcutAction action, const QKeySequence &sequence) {
        attempts.append({action, sequence});
        if (const auto rejection = rejectionFor(action, sequence); rejection.has_value()) {
            return HotkeyError{rejection->nativeCode, rejection->message};
        }
        for (auto active = activeBindings.cbegin(); active != activeBindings.cend(); ++active) {
            if (active.key() != static_cast<int>(action) && active.value() == sequence) {
                return HotkeyError{std::nullopt, QStringLiteral("Shortcut is already registered.")};
            }
        }
        return std::nullopt;
    }

    void addActive(ShortcutAction action, const QKeySequence &sequence) {
        activeBindings.insert(static_cast<int>(action), sequence);
        registrations.append({action, sequence});
    }

    void unregisterAction(ShortcutAction action) {
        activeBindings.remove(static_cast<int>(action));
        removeRegistration(action);
        unregisteredActions.append(action);
    }

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
