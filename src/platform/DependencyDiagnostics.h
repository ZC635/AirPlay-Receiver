#pragma once

#include <QString>
#include <QStringList>

#include <functional>

struct DiagnosticResult {
    bool ok;
    QString message;
};

struct RecordingCapabilityDiagnostics {
    bool canRecord = false;
    QString selectedEncoder;
    QStringList missingFactories;
};

class DependencyDiagnostics {
public:
    static DiagnosticResult checkExecutable(const QString &name);
    static DiagnosticResult checkEnvironmentVariable(const QString &name);
    static QStringList checkRuntimeBasics();
    static bool shouldCheckStandaloneRuntime();
    static QStringList checkStandaloneRuntime(const QString &directory);
    static bool configurePackageLocalGStreamerEnvironment(
        const QString &applicationDirectory);
    static RecordingCapabilityDiagnostics checkRecordingCapabilities(
        bool requireBothEncoders);
    static RecordingCapabilityDiagnostics checkRecordingCapabilities(
        bool requireBothEncoders,
        const std::function<bool(const QString &)> &factoryAvailable);
};
