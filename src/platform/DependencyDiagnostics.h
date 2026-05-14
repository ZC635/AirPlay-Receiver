#pragma once

#include <QString>
#include <QStringList>

struct DiagnosticResult {
    bool ok;
    QString message;
};

class DependencyDiagnostics {
public:
    static DiagnosticResult checkExecutable(const QString &name);
    static DiagnosticResult checkEnvironmentVariable(const QString &name);
    static QStringList checkRuntimeBasics();
    static bool shouldCheckStandaloneRuntime();
    static QStringList checkStandaloneRuntime(const QString &directory);
};
