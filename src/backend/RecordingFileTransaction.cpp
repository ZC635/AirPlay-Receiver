#include "backend/RecordingFileTransaction.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <string>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

QString createTemporaryFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return QStringLiteral("Could not exclusively create temporary recording file \"%1\": %2")
            .arg(path, file.errorString());
    }
    file.close();

#ifdef Q_OS_WIN
    const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();
    const DWORD attributes = GetFileAttributesW(nativePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !SetFileAttributesW(nativePath.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN)) {
        const DWORD error = GetLastError();
        QFile::remove(path);
        return QStringLiteral("Could not hide temporary recording file \"%1\" (Windows error %2)")
            .arg(path)
            .arg(error);
    }
#endif
    return {};
}

void removeExistingFile(const QString &path) {
    const QFileInfo info(path);
    if (info.exists() || info.isSymLink()) QFile::remove(path);
}

QString finalRecordingPath(const QDir &directory, const QDateTime &localNow) {
    const QString stem = QStringLiteral("AirPlay Recording %1")
                             .arg(localNow.toString(QStringLiteral("yyyy-MM-dd HH-mm-ss")));
    QString candidate = directory.filePath(stem + QStringLiteral(".mp4"));
    for (int suffix = 2; QFileInfo::exists(candidate); ++suffix) {
        candidate = directory.filePath(
            QStringLiteral("%1-%2.mp4").arg(stem).arg(suffix));
    }
    return QDir::cleanPath(candidate);
}

} // namespace

RecordingFileReservationResult RecordingFileTransaction::reserve(
    const QString &directory, const QDateTime &localNow, const QUuid &uuid) {
    if (directory.trimmed().isEmpty()) {
        return {{}, QStringLiteral("Recording output directory is empty")};
    }
    const QString outputPath = QDir(directory).absolutePath();
    if (!QDir().mkpath(outputPath) || !QFileInfo(outputPath).isDir()) {
        return {{}, QStringLiteral("Could not create recording output directory \"%1\"")
                        .arg(outputPath)};
    }

    const QDir outputDirectory(outputPath);
    const QString id = uuid.toString(QUuid::Id128).toLower();
    RecordingFileReservation reservation{
        outputDirectory.filePath(
            QStringLiteral(".airplay-recording-%1.video.mkv.part").arg(id)),
        outputDirectory.filePath(
            QStringLiteral(".airplay-recording-%1.audio.mka.part").arg(id)),
        outputDirectory.filePath(
            QStringLiteral(".airplay-recording-%1.mp4.part").arg(id)),
        finalRecordingPath(outputDirectory, localNow),
    };

    QStringList created;
    for (const QString &path : {reservation.videoSpoolPath, reservation.audioSpoolPath,
                                reservation.temporaryMp4Path}) {
        const QString error = createTemporaryFile(path);
        if (!error.isEmpty()) {
            for (const QString &createdPath : created) QFile::remove(createdPath);
            return {{}, error};
        }
        created.append(path);
    }
    return {reservation, {}};
}

QString RecordingFileTransaction::commit(const RecordingFileReservation &reservation) {
    const QFileInfo mp4Info(reservation.temporaryMp4Path);
    if (!mp4Info.exists() || !mp4Info.isFile() || mp4Info.isSymLink()) {
        return QStringLiteral("Temporary MP4 result is missing or is not a regular file: \"%1\"")
            .arg(reservation.temporaryMp4Path);
    }
    if (mp4Info.size() <= 0) {
        return QStringLiteral("Temporary MP4 result is empty: \"%1\"")
            .arg(reservation.temporaryMp4Path);
    }
    if (QFileInfo::exists(reservation.finalPath)) {
        return QStringLiteral("Final recording path already exists: \"%1\"")
            .arg(reservation.finalPath);
    }

    QFile mp4(reservation.temporaryMp4Path);
    if (!mp4.rename(reservation.finalPath)) {
        return QStringLiteral("Could not commit recording to \"%1\": %2")
            .arg(reservation.finalPath, mp4.errorString());
    }

    QStringList cleanupFailures;
    for (const QString &path : {reservation.videoSpoolPath, reservation.audioSpoolPath}) {
        const QFileInfo info(path);
        if ((info.exists() || info.isSymLink()) && !QFile::remove(path)) cleanupFailures.append(path);
    }
    if (!cleanupFailures.isEmpty()) {
        return QStringLiteral("Recording committed to \"%1\", but temporary spool cleanup failed: %2")
            .arg(reservation.finalPath, cleanupFailures.join(QStringLiteral(", ")));
    }
    return {};
}

void RecordingFileTransaction::discard(const RecordingFileReservation &reservation) {
    const bool uncommittedMp4Exists = QFileInfo::exists(reservation.temporaryMp4Path);
    removeExistingFile(reservation.videoSpoolPath);
    removeExistingFile(reservation.audioSpoolPath);
    removeExistingFile(reservation.temporaryMp4Path);
    if (!uncommittedMp4Exists) removeExistingFile(reservation.finalPath);
}

QStringList RecordingFileTransaction::cleanupStaleTemporaryFiles(const QString &directory) {
    static const QRegularExpression pattern(QStringLiteral(
        R"(^\.airplay-recording-[0-9a-f]{32}\.(?:video\.mkv|audio\.mka|mp4)\.part$)"));
    const QDir outputDirectory(directory);
    QStringList deleted;
    const QFileInfoList entries = outputDirectory.entryInfoList(
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (entry.isSymLink() || !entry.isFile() ||
            !pattern.match(entry.fileName()).hasMatch()) {
            continue;
        }
        const QString path = QDir::cleanPath(entry.absoluteFilePath());
        if (QFile::remove(path)) deleted.append(path);
    }
    deleted.sort();
    return deleted;
}
