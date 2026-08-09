#include "app/DiagnosticActivation.h"

#include "diagnostics/DiagnosticEvent.h"
#include "diagnostics/DiagnosticSanitizer.h"
#include "diagnostics/DiagnosticSession.h"

#include <QLocalSocket>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

constexpr auto diagnosticLog = "--diagnostic-log";
constexpr auto parentPidPrefix = "--diagnostic-parent-pid=";
constexpr auto readyTokenPrefix = "--diagnostic-ready-token=";
constexpr int parentExitTimeoutMs = 30000;
constexpr qsizetype maximumChildResponseBytes = 1024;

QString childFailureText(QString text) {
    text = DiagnosticSanitizer::sanitizeText(text);
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text.trimmed();
}

QByteArray errorLine(QString text) {
    QByteArray payload = childFailureText(std::move(text)).toUtf8();
    payload.truncate(maximumChildResponseBytes - QByteArrayLiteral("ERROR\t\n").size());
    return QByteArrayLiteral("ERROR\t") + payload + '\n';
}

void recordHandoffFailure(DiagnosticSession *session, const QString &reason) {
    if (session && session->isActive()) {
        session->record(makeDiagnosticEvent(DiagnosticSeverity::Error, QStringLiteral("startup"),
                                            QStringLiteral("child_handoff_failed"),
                                            {{QStringLiteral("reason"), reason},
                                             {QStringLiteral("result"), QStringLiteral("failed")}}, true));
    }
}

DiagnosticChildGateOperations defaultChildGateOperations() {
    return {
        [](qint64 parentPid, QString *error) -> void * {
            HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parentPid));
            if (handle == nullptr && error)
                *error = QStringLiteral("parent handle unavailable");
            return handle;
        },
        [](const QString &token, const QByteArray &line, QString *error) {
            QLocalSocket socket;
            socket.connectToServer(token);
            if (!socket.waitForConnected(1000)) {
                if (error)
                    *error = QStringLiteral("private handoff channel unavailable");
                return false;
            }
            if (socket.write(line) != line.size() || !socket.flush() ||
                !socket.waitForBytesWritten(1000)) {
                if (error)
                    *error = QStringLiteral("private handoff channel unavailable");
                socket.disconnectFromServer();
                return false;
            }
            socket.disconnectFromServer();
            return true;
        },
        [](void *rawHandle, int timeoutMs, QString *error) {
            const DWORD result = WaitForSingleObject(static_cast<HANDLE>(rawHandle), timeoutMs);
            if (result == WAIT_OBJECT_0)
                return true;
            if (error) {
                *error = result == WAIT_TIMEOUT ? QStringLiteral("parent exit timed out")
                                                : QStringLiteral("parent exit wait failed");
            }
            return false;
        },
        [](void *rawHandle) { CloseHandle(static_cast<HANDLE>(rawHandle)); },
    };
}

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

DiagnosticChildGateResult runDiagnosticChildGate(
    const DiagnosticActivation &activation, DiagnosticSession *session,
    const QString &sessionCreationError, DiagnosticChildGateOperations operations) {
    if (!activation.isCoordinatedChild())
        return DiagnosticChildGateResult::ContinueStartup;

    const DiagnosticChildGateOperations defaults = defaultChildGateOperations();
    if (!operations.openParentForWait)
        operations.openParentForWait = defaults.openParentForWait;
    if (!operations.sendReadyLine)
        operations.sendReadyLine = defaults.sendReadyLine;
    if (!operations.waitForParentExit)
        operations.waitForParentExit = defaults.waitForParentExit;
    if (!operations.closeParentHandle)
        operations.closeParentHandle = defaults.closeParentHandle;

    if (session == nullptr || !session->isActive() || !sessionCreationError.isEmpty()) {
        QString ignored;
        operations.sendReadyLine(activation.readyToken,
                                 errorLine(sessionCreationError.isEmpty()
                                               ? QStringLiteral("Diagnostic logging could not be initialized.")
                                               : sessionCreationError),
                                 &ignored);
        return DiagnosticChildGateResult::ExitChild;
    }

    QString ignored;
    void *parentHandle = operations.openParentForWait(activation.parentPid, &ignored);
    if (parentHandle == nullptr) {
        operations.sendReadyLine(activation.readyToken,
                                 errorLine(QStringLiteral("Diagnostic parent process is unavailable.")),
                                 &ignored);
        recordHandoffFailure(session, QStringLiteral("parent_handle_unavailable"));
        return DiagnosticChildGateResult::ExitChild;
    }

    if (!operations.sendReadyLine(activation.readyToken, QByteArrayLiteral("READY\n"), &ignored)) {
        operations.closeParentHandle(parentHandle);
        recordHandoffFailure(session, QStringLiteral("ready_send_failed"));
        return DiagnosticChildGateResult::ExitChild;
    }

    const bool exited = operations.waitForParentExit(parentHandle, parentExitTimeoutMs, &ignored);
    operations.closeParentHandle(parentHandle);
    if (!exited) {
        recordHandoffFailure(session, QStringLiteral("parent_exit_wait_failed"));
        return DiagnosticChildGateResult::ExitChild;
    }
    return DiagnosticChildGateResult::ContinueStartup;
}
