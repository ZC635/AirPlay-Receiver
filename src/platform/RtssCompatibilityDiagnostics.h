#pragma once
#include <QString>
#include <QStringList>

enum class RtssPresence { Unknown, NotRunning, Running };
enum class RtssDllPathRisk { Unknown, NotObserved, KnownLengthRisk };

struct RtssCompatibilitySnapshot {
    RtssPresence rtssPresence = RtssPresence::Unknown;
    RtssDllPathRisk dllPathRisk = RtssDllPathRisk::Unknown;
    QString executableFileName;
    QString runtimeDllPath;
    QString dllPathSource;
    int ansiPathBytes = -1;
    QStringList detectionErrors;
    bool shouldWarn() const;
};

namespace RtssCompatibilityDiagnostics {
RtssCompatibilitySnapshot inspectCurrentProcess();
RtssDllPathRisk classifyVulkanDllPath(const QString &fullPath);
}