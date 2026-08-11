#pragma once

#include "diagnostics/DiagnosticEvent.h"

#include <QLoggingCategory>
#include <QStringView>

#include <optional>

class DiagnosticLogSink;

class QtDiagnosticMessageBridge final {
public:
    explicit QtDiagnosticMessageBridge(DiagnosticLogSink *sink);
    ~QtDiagnosticMessageBridge();

    QtDiagnosticMessageBridge(const QtDiagnosticMessageBridge &) = delete;
    QtDiagnosticMessageBridge &operator=(const QtDiagnosticMessageBridge &) = delete;

    static std::optional<DiagnosticEvent> eventForMessage(
        QtMsgType type, const QMessageLogContext &context, QStringView message);

private:
    static void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message);

    bool m_installed = false;
};
