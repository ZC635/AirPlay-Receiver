#include "diagnostics/QtDiagnosticMessageBridge.h"

#include "diagnostics/DiagnosticLogSink.h"
#include "diagnostics/DiagnosticSanitizer.h"

#include <QMutex>
#include <QWaitCondition>

namespace {

QMutex bridgeMutex;
QWaitCondition bridgeIdle;
DiagnosticLogSink *activeSink = nullptr;
QtMessageHandler previousHandler = nullptr;
int inFlightHandlers = 0;
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

    QMutexLocker locker(&bridgeMutex);
    const QtMessageHandler savedHandler = qInstallMessageHandler(nullptr);
    qt_message_output(type, context, message);
    qInstallMessageHandler(savedHandler);
}

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

    if (sink) {
        if (const auto event = eventForMessage(type, context, message)) {
            try {
                sink->record(*event);
            } catch (...) {
            }
        }
    }

    forwardToPrevious(handler, type, context, message);

    if (sink) {
        QMutexLocker locker(&bridgeMutex);
        --inFlightHandlers;
        if (inFlightHandlers == 0)
            bridgeIdle.wakeAll();
    }
}
