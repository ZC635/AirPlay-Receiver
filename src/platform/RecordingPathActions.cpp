#include "platform/RecordingPathActions.h"

#include "diagnostics/DiagnosticLogSink.h"

#include <QDir>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QProcess>

#include <utility>

namespace {
QString localized(const char *source) {
    return QCoreApplication::translate("RecordingPathActions", source);
}

bool launchDetached(const QString &program, const QStringList &arguments) {
    return QProcess::startDetached(program, arguments);
}

void recordDirectoryActionFailure(DiagnosticLogSink *sink, const QString &operation,
                                  const QString &reason) {
    if (!sink->isActive()) {
        return;
    }
    sink->record(makeDiagnosticEvent(DiagnosticSeverity::Warning, QStringLiteral("ui"),
        QStringLiteral("directory_action_failed"),
        {{QStringLiteral("area"), QStringLiteral("recording")},
         {QStringLiteral("operation"), operation},
         {QStringLiteral("reason"), reason}}, true));
}
}

WindowsRecordingPathActions::WindowsRecordingPathActions(ProcessLauncher processLauncher,
                                                       DiagnosticLogSink *sink)
    : processLauncher_(processLauncher ? std::move(processLauncher) : launchDetached),
      sink_(sink ? sink : &nullDiagnosticLogSink()) {}

QString WindowsRecordingPathActions::chooseExistingDirectory(
    QWidget *parent, const QString &initialDirectory) {
    return QFileDialog::getExistingDirectory(
        parent, localized(QT_TRANSLATE_NOOP("RecordingPathActions", "Choose Recording Directory")), initialDirectory);
}

UiMessage WindowsRecordingPathActions::ensureAndOpenDirectory(const QString &directory) {
    const QString absoluteDirectory = QFileInfo(directory).absoluteFilePath();
    if (!QDir().mkpath(absoluteDirectory)) {
        recordDirectoryActionFailure(sink_, QStringLiteral("create_directory"),
                                     QStringLiteral("create_failed"));
        return UiMessage::translated("RecordingPathActions",
            QT_TRANSLATE_NOOP("RecordingPathActions", "Could not create recording directory: %1"),
            {QDir::toNativeSeparators(absoluteDirectory)});
    }

    const QString nativeDirectory = QDir::toNativeSeparators(absoluteDirectory);
    if (!processLauncher_("explorer.exe", QStringList{nativeDirectory})) {
        recordDirectoryActionFailure(sink_, QStringLiteral("open_directory"),
                                     QStringLiteral("launcher_failed"));
        return UiMessage::translated("RecordingPathActions",
            QT_TRANSLATE_NOOP("RecordingPathActions", "Could not open recording directory: %1"),
            {nativeDirectory});
    }
    return {};
}

UiMessage WindowsRecordingPathActions::revealFile(const QString &filePath) {
    const QString absoluteFilePath = QDir::toNativeSeparators(
        QFileInfo(filePath).absoluteFilePath());
    if (!processLauncher_("explorer.exe", QStringList{"/select,", absoluteFilePath})) {
        recordDirectoryActionFailure(sink_, QStringLiteral("reveal_recording"),
                                     QStringLiteral("launcher_failed"));
        return UiMessage::translated("RecordingPathActions",
            QT_TRANSLATE_NOOP("RecordingPathActions", "Could not reveal recording file: %1"),
            {absoluteFilePath});
    }
    return {};
}
