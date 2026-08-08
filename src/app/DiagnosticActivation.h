#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

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
