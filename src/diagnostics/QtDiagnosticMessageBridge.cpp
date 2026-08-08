#include "diagnostics/QtDiagnosticMessageBridge.h"

#include "diagnostics/DiagnosticLogSink.h"
#include "diagnostics/DiagnosticSanitizer.h"

#include <QMutex>
#include <QWaitCondition>

#include <atomic>
#include <cstdio>

namespace {

QMutex bridgeMutex;
QWaitCondition bridgeIdle;
DiagnosticLogSink *activeSink = nullptr;
QtMessageHandler previousHandler = nullptr;
int inFlightHandlers = 0;
bool tearingDown = false;
using InstallObserver = void (*)(QtMessageHandler);
std::atomic<InstallObserver> installObserver = nullptr;

QString asciiSanitizedText(QStringView value) {
    const QString sanitized = DiagnosticSanitizer::sanitizeText(value);
    QString ascii;
    ascii.reserve(sanitized.size());
    for (const QChar character : sanitized)
        ascii.append(character.unicode() >= 0x20 && character.unicode() <= 0x7e
                         ? character : QLatin1Char('_'));
    return ascii;
}

QtMessageHandler installMessageHandler(QtMessageHandler handler) {
    if (const InstallObserver observer = installObserver.load(std::memory_order_relaxed))
        observer(handler);
    return qInstallMessageHandler(handler);
}

void forwardToPrevious(QtMessageHandler handler, QtMsgType type,
                       const QMessageLogContext &context, const QString &message) {
    if (handler) {
        handler(type, context, message);
        return;
    }

    const QByteArray formatted = qFormatLogMessage(type, context, message).toLocal8Bit();
    std::fputs(formatted.constData(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace

void setQtDiagnosticMessageBridgeInstallObserverForTests(void (*observer)(QtMessageHandler)) {
    installObserver.store(observer, std::memory_order_relaxed);
}

void forwardQtDiagnosticMessageBridgeNullPreviousForTests() {
    forwardToPrevious(nullptr, QtWarningMsg, QMessageLogContext(),
                      QStringLiteral("default forwarding test"));
}

namespace {

class HandlerFlight final {
public:
    explicit HandlerFlight(bool active) : m_active(active) {}
    ~HandlerFlight() {
        if (!m_active)
            return;
        QMutexLocker locker(&bridgeMutex);
        --inFlightHandlers;
        if (inFlightHandlers == 0)
            bridgeIdle.wakeAll();
    }

private:
    bool m_active;
};

} // namespace

QtDiagnosticMessageBridge::QtDiagnosticMessageBridge(DiagnosticLogSink *sink) {
    QMutexLocker locker(&bridgeMutex);
    if (activeSink || tearingDown)
        return;
    activeSink = sink ? sink : &nullDiagnosticLogSink();
    previousHandler = installMessageHandler(&QtDiagnosticMessageBridge::messageHandler);
    m_installed = true;
}

QtDiagnosticMessageBridge::~QtDiagnosticMessageBridge() {
    QMutexLocker locker(&bridgeMutex);
    if (!m_installed)
        return;
    installMessageHandler(previousHandler);
    activeSink = nullptr;
    tearingDown = true;
    while (inFlightHandlers != 0)
        bridgeIdle.wait(&bridgeMutex);
    previousHandler = nullptr;
    tearingDown = false;
}

std::optional<DiagnosticEvent> QtDiagnosticMessageBridge::eventForMessage(
    QtMsgType type, const QMessageLogContext &context, QStringView message) {
    DiagnosticSeverity severity;
    bool flushImmediately = false;
    switch (type) {
    case QtDebugMsg: return std::nullopt;
    case QtInfoMsg:
        if (!context.category || !QString::fromLatin1(context.category).startsWith(QStringLiteral("airplay.")))
            return std::nullopt;
        severity = DiagnosticSeverity::Info;
        break;
    case QtWarningMsg:
        severity = DiagnosticSeverity::Warning;
        flushImmediately = true;
        break;
    case QtCriticalMsg:
        severity = DiagnosticSeverity::Error;
        flushImmediately = true;
        break;
    case QtFatalMsg:
        severity = DiagnosticSeverity::Critical;
        flushImmediately = true;
        break;
    }

    QMap<QString, QString> fields;
    fields.insert(QStringLiteral("message"), asciiSanitizedText(message));
    if (context.category)
        fields.insert(QStringLiteral("category"), asciiSanitizedText(QString::fromLatin1(context.category)));
    return makeDiagnosticEvent(severity, QStringLiteral("qt"), QStringLiteral("message"),
                               std::move(fields), flushImmediately);
}

void QtDiagnosticMessageBridge::messageHandler(
    QtMsgType type, const QMessageLogContext &context, const QString &message) {
    DiagnosticLogSink *sink = nullptr;
    QtMessageHandler handler = nullptr;
    {
        QMutexLocker locker(&bridgeMutex);
        if (activeSink) {
            ++inFlightHandlers;
            sink = activeSink;
        }
        handler = previousHandler;
    }
    HandlerFlight flight(sink != nullptr);

    if (sink) {
        try {
            if (const auto event = eventForMessage(type, context, message))
                sink->record(*event);
        } catch (...) {
        }
    }

    try {
        forwardToPrevious(handler, type, context, message);
    } catch (...) {
    }
}
