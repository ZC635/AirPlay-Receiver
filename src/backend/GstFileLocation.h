#pragma once
#include "platform/FileSystemPath.h"
#include <QByteArray>
#include <QStringList>
namespace GstFileLocation {
inline QString logicalError(QString text, const QStringList &logicalPaths) {
    for (const QString &path : logicalPaths) {
        const QString ioPath = FileSystemPath::forIo(path);
        if (!ioPath.isEmpty()) text.replace(ioPath, FileSystemPath::absolute(path));
    }
    return text;
}
// Internal filesrc/filesink boundary; callers retain logical paths for identity/errors.
inline QByteArray forIo(const QString &path, QString *error = nullptr) {
    const QString ioPath = FileSystemPath::forIo(path);
    if (ioPath.isEmpty() || ioPath.contains(QChar(0))) {
        if (error) *error = QStringLiteral("Invalid recording file path: %1").arg(path);
        return {};
    }
    return ioPath.toUtf8();
}
}