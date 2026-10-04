#include "platform/DiagnosticLogFolderActions.h"

#include "diagnostics/DiagnosticLogSink.h"

#include <QDesktopServices>
#include <QCoreApplication>
#include <QDir>

#include <utility>

namespace {

UiMessage createDirectory(const QString &path) {
    if (QDir(path).mkpath(".")) {
        return {};
    }
    return UiMessage::translated("DiagnosticLogFolderActions",
        QT_TRANSLATE_NOOP("DiagnosticLogFolderActions", "Could not create diagnostic log folder: %1"),
        {path});
}

UiMessage openUrl(const QUrl &url) {
    if (QDesktopServices::openUrl(url)) {
        return {};
    }
    return UiMessage::translated("DiagnosticLogFolderActions",
        QT_TRANSLATE_NOOP("DiagnosticLogFolderActions", "Could not open diagnostic log folder: %1"),
        {url.toLocalFile()});
}

void recordDirectoryActionFailure(DiagnosticLogSink *sink, bool creationFailed) {
    if (!sink->isActive()) {
        return;
    }
    sink->record(makeDiagnosticEvent(DiagnosticSeverity::Warning, QStringLiteral("ui"),
        QStringLiteral("directory_action_failed"),
        {{QStringLiteral("area"), QStringLiteral("diagnostic_logs")},
         {QStringLiteral("operation"), creationFailed ? QStringLiteral("create_directory")
                                                     : QStringLiteral("open_directory")},
         {QStringLiteral("reason"), creationFailed ? QStringLiteral("create_failed")
                                                  : QStringLiteral("launcher_failed")}}, true));
}

} // namespace

DiagnosticLogFolderActions::DiagnosticLogFolderActions(
    QString applicationDirectory, DiagnosticLogFolderOperations operations, DiagnosticLogSink *sink)
    : applicationDirectory_(std::move(applicationDirectory)), operations_(std::move(operations)),
      sink_(sink ? sink : &nullDiagnosticLogSink()) {
    if (!operations_.createDirectory) {
        operations_.createDirectory = createDirectory;
    }
    if (!operations_.openUrl) {
        operations_.openUrl = openUrl;
    }
}

UiMessage DiagnosticLogFolderActions::ensureAndOpen() {
    const QString logDirectory = QDir(applicationDirectory_).filePath("logs");
    if (const UiMessage error = operations_.createDirectory(logDirectory); !error.isEmpty()) {
        recordDirectoryActionFailure(sink_, true);
        return error;
    }
    const UiMessage error = operations_.openUrl(QUrl::fromLocalFile(logDirectory));
    if (!error.isEmpty()) {
        recordDirectoryActionFailure(sink_, false);
    }
    return error;
}
