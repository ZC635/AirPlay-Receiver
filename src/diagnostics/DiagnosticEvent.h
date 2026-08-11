#pragma once

#include <QDateTime>
#include <QMap>
#include <QString>

#include <utility>

enum class DiagnosticSeverity { Debug, Info, Warning, Error, Critical };

struct DiagnosticEvent {
    DiagnosticSeverity severity = DiagnosticSeverity::Info;
    QString component;
    QString name;
    QMap<QString, QString> fields;
    QDateTime timeUtc;
    bool flushImmediately = false;
};

inline DiagnosticEvent makeDiagnosticEvent(
    DiagnosticSeverity severity, QString component, QString name,
    QMap<QString, QString> fields = {}, bool flushImmediately = false,
    QDateTime time = QDateTime::currentDateTimeUtc()) {
    return {severity, std::move(component), std::move(name), std::move(fields),
            time.toUTC(), flushImmediately};
}
