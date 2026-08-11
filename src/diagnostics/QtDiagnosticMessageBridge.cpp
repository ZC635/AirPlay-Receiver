#include "diagnostics/QtDiagnosticMessageBridge.h"

#include "diagnostics/DiagnosticLogSink.h"
#include "diagnostics/DiagnosticSanitizer.h"

#include <QMutex>
#include <QWaitCondition>

#include <atomic>
#include <cstdio>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace {

QMutex bridgeMutex;
QWaitCondition bridgeIdle;
DiagnosticLogSink *activeSink = nullptr;
QtMessageHandler previousHandler = nullptr;
std::atomic_uint handlerEntries = 0;
bool tearingDown = false;

QString asciiSanitizedText(QStringView value) {
    const QString sanitized = DiagnosticSanitizer::sanitizeText(value);
    QString ascii;
    ascii.reserve(sanitized.size());
    for (const QChar character : sanitized)
        ascii.append(character.unicode() >= 0x20 && character.unicode() <= 0x7e
                         ? character : QLatin1Char('_'));
    return ascii;
}

void forwardToPrevious(QtMessageHandler handler, QtMsgType type,
                       const QMessageLogContext &context, const QString &message) {
    if (handler) {
        handler(type, context, message);
        return;
    }

    // Qt performs fatal termination after this message handler returns.
    const QString formatted = qFormatLogMessage(type, context, message);
#if defined(Q_OS_WIN)
    if (!GetConsoleWindow()) {
        OutputDebugStringW(reinterpret_cast<LPCWSTR>((formatted + QLatin1Char('\n')).utf16()));
        return;
    }
#endif
    const QByteArray output = formatted.toLocal8Bit();
    std::fputs(output.constData(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

} // namespace

namespace {

class HandlerEntry final {
public:
    HandlerEntry() { handlerEntries.fetch_add(1, std::memory_order_acq_rel); }
    ~HandlerEntry() {
        if (handlerEntries.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            QMutexLocker locker(&bridgeMutex);
            bridgeIdle.wakeAll();
        }
    }
};

} // namespace

QtDiagnosticMessageBridge::QtDiagnosticMessageBridge(DiagnosticLogSink *sink) {
    QMutexLocker locker(&bridgeMutex);
    if (activeSink || tearingDown)
        return;
    activeSink = sink ? sink : &nullDiagnosticLogSink();
    previousHandler = qInstallMessageHandler(&QtDiagnosticMessageBridge::messageHandler);
    m_installed = true;
}

QtDiagnosticMessageBridge::~QtDiagnosticMessageBridge() {
    QMutexLocker locker(&bridgeMutex);
    if (!m_installed)
        return;
    qInstallMessageHandler(previousHandler);
    activeSink = nullptr;
    tearingDown = true;
    while (handlerEntries.load(std::memory_order_acquire) != 0)
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
    HandlerEntry entry;
    DiagnosticLogSink *sink = nullptr;
    QtMessageHandler handler = nullptr;
    {
        QMutexLocker locker(&bridgeMutex);
        if (activeSink) {
            sink = activeSink;
        }
        handler = previousHandler;
    }
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
