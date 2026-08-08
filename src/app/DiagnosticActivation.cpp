#include "app/DiagnosticActivation.h"

namespace {

constexpr auto diagnosticLog = "--diagnostic-log";
constexpr auto parentPidPrefix = "--diagnostic-parent-pid=";
constexpr auto readyTokenPrefix = "--diagnostic-ready-token=";

} // namespace

QString DiagnosticActivation::sourceName() const {
    switch (source) {
    case DiagnosticActivationSource::CommandArgument: return QStringLiteral("command_argument");
    case DiagnosticActivationSource::EnvironmentVariable: return QStringLiteral("environment_variable");
    case DiagnosticActivationSource::None: return QStringLiteral("none");
    }
    return QStringLiteral("none");
}

QMap<QString, QString> DiagnosticActivation::publicFields() const {
    if (!enabled)
        return {};
    return {{QStringLiteral("activation_source"), sourceName()}};
}

DiagnosticActivation DiagnosticActivation::parse(
    const QStringList &arguments, const QByteArray &environmentValue) {
    DiagnosticActivation result;
    bool commandArgument = false;
    bool parentPidSeen = false;
    bool readyTokenSeen = false;

    for (const QString &argument : arguments) {
        if (argument == QLatin1String(diagnosticLog)) {
            commandArgument = true;
            continue;
        }
        if (argument.startsWith(QLatin1String(parentPidPrefix))) {
            if (parentPidSeen) {
                result.argumentError = QStringLiteral("duplicate_diagnostic_parent_pid");
                return result;
            }
            parentPidSeen = true;
            bool converted = false;
            const qint64 parentPid = argument.mid(qstrlen(parentPidPrefix)).toLongLong(&converted, 10);
            if (!converted || parentPid <= 0) {
                result.argumentError = QStringLiteral("invalid_diagnostic_parent_pid");
                return result;
            }
            result.parentPid = parentPid;
            continue;
        }
        if (argument.startsWith(QLatin1String(readyTokenPrefix))) {
            if (readyTokenSeen) {
                result.argumentError = QStringLiteral("duplicate_diagnostic_ready_token");
                return result;
            }
            readyTokenSeen = true;
            result.readyToken = argument.mid(qstrlen(readyTokenPrefix));
            if (result.readyToken.isEmpty()) {
                result.argumentError = QStringLiteral("invalid_diagnostic_ready_token");
                return result;
            }
        }
    }

    if (parentPidSeen != readyTokenSeen) {
        result.argumentError = QStringLiteral("incomplete_diagnostic_coordination");
        return result;
    }

    if (commandArgument) {
        result.enabled = true;
        result.source = DiagnosticActivationSource::CommandArgument;
    } else if (!environmentValue.isEmpty()) {
        result.enabled = true;
        result.source = DiagnosticActivationSource::EnvironmentVariable;
    }
    if (parentPidSeen && !result.enabled) {
        result.parentPid = 0;
        result.readyToken.clear();
        result.argumentError = QStringLiteral("diagnostic_coordination_requires_activation");
    }
    return result;
}
