#pragma once

#include "diagnostics/DiagnosticEvent.h"

#include <QDeadlineTimer>
#include <QStringList>
#include <QVector>

#include <functional>

enum class DiagnosticFactStatus { Available, Unavailable, TimedOut };

struct DiagnosticFact {
    DiagnosticFactStatus status = DiagnosticFactStatus::Unavailable;
    QString value;

    static DiagnosticFact available(QString value);
    static DiagnosticFact unavailable();
    static DiagnosticFact timedOut();
};

struct NetworkAdapterFact {
    int sessionIndex = 0;
    QString type;
    bool enabled = false;
    bool up = false;
    QString physicalClassification;
    int routeMetric = -1;
    bool ownsDefaultRoute = false;
    QStringList prefixes;
};

struct NetworkEnvironmentFact {
    QVector<NetworkAdapterFact> adapters;
    DiagnosticFact category;
    DiagnosticFact firewallProfiles;
    DiagnosticFact executableFirewallRule;
};

template<typename T>
struct DiagnosticValue {
    DiagnosticFactStatus status = DiagnosticFactStatus::Unavailable;
    T value{};

    static DiagnosticValue available(T value) {
        return {DiagnosticFactStatus::Available, std::move(value)};
    }
    static DiagnosticValue unavailable() { return {}; }
    static DiagnosticValue timedOut() { return {DiagnosticFactStatus::TimedOut, {}}; }
};

struct EnvironmentSnapshot {
    DiagnosticFact operatingSystem;
    DiagnosticFact cpuArchitecture;
    DiagnosticFact processElevation;
    DiagnosticValue<NetworkEnvironmentFact> network;
};

struct EnvironmentDiagnosticProviders {
    std::function<DiagnosticFact(QDeadlineTimer)> operatingSystem;
    std::function<DiagnosticFact(QDeadlineTimer)> cpuArchitecture;
    std::function<DiagnosticFact(QDeadlineTimer)> processElevation;
    std::function<DiagnosticValue<NetworkEnvironmentFact>(QDeadlineTimer)> network;
};

class EnvironmentDiagnostics {
public:
    static EnvironmentSnapshot collect(const EnvironmentDiagnosticProviders &providers,
                                       int totalTimeoutMs);
    static QList<DiagnosticEvent> events(const EnvironmentSnapshot &snapshot);
};
