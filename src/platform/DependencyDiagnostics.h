#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

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

struct GStreamerPluginReadiness {
    bool ready = false;
    QStringList missingPlugins;
    QString initializationError;
};

struct StandaloneRuntimeSnapshot {
    bool complete = false;
    QStringList relativePaths;
    QStringList missingRelativePaths;
};

struct RuntimePathCompatibility {
    bool compatible = false;
    bool hasNonAscii = false;
    quint32 ansiCodePage = 0;
    qsizetype pathLength = 0;
};

class DependencyDiagnostics {
public:
    static DiagnosticResult checkExecutable(const QString &name);
    static DiagnosticResult checkEnvironmentVariable(const QString &name);
    static QStringList checkRuntimeBasics();
    static RuntimePathCompatibility checkRuntimePathCompatibility(const QString &path);
    static RuntimePathCompatibility checkRuntimePathCompatibility(
        const QString &path,
        quint32 ansiCodePage);
    static RuntimePathCompatibility checkRuntimePathCompatibility(
        const QString &path,
        quint32 ansiCodePage,
        const std::function<bool(const QString &, quint32)> &roundTrips);
    static bool shouldCheckStandaloneRuntime();
    static StandaloneRuntimeSnapshot standaloneRuntimeSnapshot(const QString &directory);
    static QStringList checkStandaloneRuntime(const QString &directory);
    static bool configurePackageLocalGStreamerEnvironment(
        const QString &applicationDirectory);
    static bool configurePackageLocalGStreamerEnvironment(
        const QString &packageDirectory, const QString &privateRegistry);
    static GStreamerPluginReadiness checkPackageGStreamerPluginReadiness(const QString &packageDirectory);
    static GStreamerPluginReadiness checkGStreamerPluginReadiness();
    static GStreamerPluginReadiness checkGStreamerPluginReadiness(
        const std::function<bool(const QString &)> &pluginAvailable);
    static RecordingCapabilityDiagnostics checkRecordingCapabilities(
        bool requireBothEncoders);
    static RecordingCapabilityDiagnostics checkRecordingCapabilities(
        bool requireBothEncoders,
        const std::function<bool(const QString &)> &factoryAvailable);
};
