#include "platform/DiagnosticLogFolderActions.h"

#include <QDesktopServices>
#include <QDir>

#include <utility>

namespace {

QString createDirectory(const QString &path) {
    if (QDir(path).mkpath(".")) {
        return {};
    }
    return QStringLiteral("Could not create diagnostic log folder: %1").arg(path);
}

QString openUrl(const QUrl &url) {
    if (QDesktopServices::openUrl(url)) {
        return {};
    }
    return QStringLiteral("Could not open diagnostic log folder: %1").arg(url.toLocalFile());
}

} // namespace

DiagnosticLogFolderActions::DiagnosticLogFolderActions(
    QString applicationDirectory, DiagnosticLogFolderOperations operations)
    : applicationDirectory_(std::move(applicationDirectory)), operations_(std::move(operations)) {
    if (!operations_.createDirectory) {
        operations_.createDirectory = createDirectory;
    }
    if (!operations_.openUrl) {
        operations_.openUrl = openUrl;
    }
}

QString DiagnosticLogFolderActions::ensureAndOpen() {
    const QString logDirectory = QDir(applicationDirectory_).filePath("logs");
    if (const QString error = operations_.createDirectory(logDirectory); !error.isEmpty()) {
        return error;
    }
    return operations_.openUrl(QUrl::fromLocalFile(logDirectory));
}
