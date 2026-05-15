#include "platform/DependencyDiagnostics.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

namespace {
QStringList readPortableRuntimeManifest() {
    const QString relativeManifestPath = "config/portable-runtime-manifest.txt";
    const QString appDirPath = QCoreApplication::applicationDirPath();
    const QString currentDirPath = QDir::currentPath();
    const QStringList candidatePaths = {
        QDir(appDirPath).filePath(relativeManifestPath),
        QDir(appDirPath).filePath("../" + relativeManifestPath),
        QDir(appDirPath).filePath("../../" + relativeManifestPath),
        QDir(currentDirPath).filePath(relativeManifestPath),
        QDir(currentDirPath).filePath("../" + relativeManifestPath),
        QDir(currentDirPath).filePath("../../" + relativeManifestPath)
    };

    for (const QString &candidatePath : candidatePaths) {
        QFile file(candidatePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }

        QStringList paths;
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            QString line = stream.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#')) {
                continue;
            }
            paths.append(line.replace('\\', '/'));
        }
        if (!paths.isEmpty()) {
            return paths;
        }
    }

    return {"config/portable-runtime-manifest.txt"};
}
}

DiagnosticResult DependencyDiagnostics::checkExecutable(const QString &name) {
    const QString path = QStandardPaths::findExecutable(name);
    if (path.isEmpty()) {
        return {false, QString("%1 not found. Install it or add it to PATH.").arg(name)};
    }
    return {true, QString("%1 found at %2.").arg(name, path)};
}

DiagnosticResult DependencyDiagnostics::checkEnvironmentVariable(const QString &name) {
    if (!qEnvironmentVariableIsSet(name.toUtf8().constData())) {
        return {false, QString("%1 not set. Set it before launching AirPlay Receiver.").arg(name)};
    }
    return {true, QString("%1 is set.").arg(name)};
}

QStringList DependencyDiagnostics::checkRuntimeBasics() {
    return {
        "GStreamer: install runtime plugins and ensure binaries are on PATH.",
        "QMdnsEngine: install qmdnsengine runtime for mDNS discovery.",
        "UxPlay: build/runtime dependencies must be available for AirPlay receiver support."
    };
}

bool DependencyDiagnostics::shouldCheckStandaloneRuntime() {
    return !qEnvironmentVariableIsSet("AIRPLAY_MSYS2_PATH_MODE");
}

QStringList DependencyDiagnostics::checkStandaloneRuntime(const QString &directory) {
    const QStringList requiredPaths = readPortableRuntimeManifest();

    QStringList missing;
    const QDir baseDir(directory);
    for (const QString &relativePath : requiredPaths) {
        if (!QFileInfo::exists(baseDir.filePath(relativePath))) {
            missing.append(relativePath);
        }
    }
    return missing;
}
