#include "platform/RecordingPathActions.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QProcess>

#include <utility>

namespace {
bool launchDetached(const QString &program, const QStringList &arguments) {
    return QProcess::startDetached(program, arguments);
}
}

WindowsRecordingPathActions::WindowsRecordingPathActions(ProcessLauncher processLauncher)
    : processLauncher_(processLauncher ? std::move(processLauncher) : launchDetached) {}

QString WindowsRecordingPathActions::chooseExistingDirectory(
    QWidget *parent, const QString &initialDirectory) {
    return QFileDialog::getExistingDirectory(
        parent, "Choose Recording Directory", initialDirectory);
}

QString WindowsRecordingPathActions::ensureAndOpenDirectory(const QString &directory) {
    const QString absoluteDirectory = QFileInfo(directory).absoluteFilePath();
    if (!QDir().mkpath(absoluteDirectory)) {
        return QString("Could not create recording directory: %1")
            .arg(QDir::toNativeSeparators(absoluteDirectory));
    }

    const QString nativeDirectory = QDir::toNativeSeparators(absoluteDirectory);
    if (!processLauncher_("explorer.exe", QStringList{nativeDirectory})) {
        return QString("Could not open recording directory: %1").arg(nativeDirectory);
    }
    return {};
}

QString WindowsRecordingPathActions::revealFile(const QString &filePath) {
    const QString absoluteFilePath = QDir::toNativeSeparators(
        QFileInfo(filePath).absoluteFilePath());
    if (!processLauncher_("explorer.exe", QStringList{"/select,", absoluteFilePath})) {
        return QString("Could not reveal recording file: %1").arg(absoluteFilePath);
    }
    return {};
}
