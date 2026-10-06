#include "platform/FileSystemPath.h"

#include <QDir>
#include <QFileInfo>

QString FileSystemPath::absolute(const QString &path) {
    if (path.isEmpty()) return {};
    QString logical = path;
#ifdef Q_OS_WIN
    // Inspect namespaces before Qt normalizes separators and removes //?/.
    logical.replace(QChar('\\'), QChar('/'));
    if (logical.startsWith(QStringLiteral("//?/UNC/"), Qt::CaseInsensitive)) {
        logical = QStringLiteral("//") + logical.mid(8);
    } else if (logical.startsWith(QStringLiteral("//?/"))) {
        if (logical.size() < 7 || !logical.at(4).isLetter()
            || logical.at(5) != QChar(':') || logical.at(6) != QChar('/')) return {};
        logical = logical.mid(4);
    }
    if (logical.startsWith(QStringLiteral("//./"))) return {};
#endif
    return QDir::cleanPath(QFileInfo(logical).absoluteFilePath());
}

QString FileSystemPath::forIo(const QString &path) {
    const QString logical = absolute(path);
    if (logical.isEmpty()) return {};
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(logical);
    if (native.startsWith(QStringLiteral("\\\\"))) {
        return QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    }
    if (native.size() >= 3 && native.at(0).isLetter()
        && native.at(1) == QChar(':') && native.at(2) == QChar('\\')) {
        return QStringLiteral("\\\\?\\") + native;
    }
    return {};
#else
    return logical;
#endif
}
