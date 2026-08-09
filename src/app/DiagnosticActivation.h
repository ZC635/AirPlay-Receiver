#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

#include <functional>

class DiagnosticSession;

enum class DiagnosticActivationSource { None, CommandArgument, EnvironmentVariable };

struct DiagnosticActivation {
    bool enabled = false;
    DiagnosticActivationSource source = DiagnosticActivationSource::None;
    qint64 parentPid = 0;
    QString readyToken;
    QString argumentError;

    QString sourceName() const;
    QMap<QString, QString> publicFields() const;
    bool isCoordinatedChild() const { return parentPid > 0 && !readyToken.isEmpty(); }

    static DiagnosticActivation parse(const QStringList &arguments, const QByteArray &environmentValue);
};

enum class DiagnosticChildGateResult { ContinueStartup, ExitChild };

struct DiagnosticChildGateOperations {
    std::function<void *(qint64, QString *)> openParentForWait;
    std::function<bool(const QString &, const QByteArray &, QString *)> sendReadyLine;
    std::function<bool(void *, int, QString *)> waitForParentExit;
    std::function<void(void *)> closeParentHandle;
};

DiagnosticChildGateResult runDiagnosticChildGate(
    const DiagnosticActivation &, DiagnosticSession *, const QString &sessionCreationError,
    DiagnosticChildGateOperations operations);
