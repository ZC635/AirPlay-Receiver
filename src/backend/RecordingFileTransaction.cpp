#include "backend/RecordingFileTransaction.h"

#include <QDir>
#include <QHash>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSharedPointer>

#include <string>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

QMutex &lockRegistryMutex() {
    static QMutex mutex;
    return mutex;
}

QHash<QString, QSharedPointer<QLockFile>> &lockRegistry() {
    static QHash<QString, QSharedPointer<QLockFile>> registry;
    return registry;
}

QString normalizedLockKey(const QString &path) {
    QString key = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    key = key.toLower();
#endif
    return key;
}

QString lockPath(const QString &directory, const QString &id) {
    return QDir(directory).filePath(QStringLiteral(".airplay-recording-%1.lock").arg(id));
}

void configureLock(QLockFile *lock) {
    // Never expire a live long-running recording based on file age. QLockFile
    // still identifies locks whose owning PID no longer exists as stale.
    lock->setStaleLockTime(0);
}

void warnIfLockFileRemains(const QString &path) {
    if (QFileInfo::exists(path)) {
        qWarning().noquote() << QStringLiteral("Could not remove recording lock file: %1").arg(path);
    }
}

bool acquireReservationLock(const QString &path, QString *error) {
    const QString key = normalizedLockKey(path);
    QMutexLocker guard(&lockRegistryMutex());
    if (lockRegistry().contains(key)) {
        *error = QStringLiteral("Recording transaction is already active: \"%1\"").arg(path);
        return false;
    }
    auto lock = QSharedPointer<QLockFile>::create(path);
    configureLock(lock.data());
    if (!lock->tryLock(0)) {
        *error = QStringLiteral("Could not acquire recording transaction lock \"%1\" (error %2)")
                     .arg(path)
                     .arg(static_cast<int>(lock->error()));
        return false;
    }
    lockRegistry().insert(key, lock);
    return true;
}

QString releaseReservationLock(const QString &path) {
    if (path.isEmpty()) return {};
    QMutexLocker guard(&lockRegistryMutex());
    const auto lock = lockRegistry().take(normalizedLockKey(path));
    if (!lock) return {};
    lock->unlock();
    if (QFileInfo::exists(path)) {
        return QStringLiteral("recording lock cleanup failed; lock file remains: %1").arg(path);
    }
    return {};
}

const QRegularExpression &temporaryFilePattern() {
    static const QRegularExpression pattern(QStringLiteral(
        R"(^\.airplay-recording-([0-9a-f]{32})\.(?:video\.mkv|audio\.mka|mp4)\.part$)"));
    return pattern;
}

const QRegularExpression &lockFilePattern() {
    static const QRegularExpression pattern(QStringLiteral(
        R"(^\.airplay-recording-([0-9a-f]{32})\.lock$)"));
    return pattern;
}

QString reservationLockPath(const RecordingFileReservation &reservation) {
    for (const QString &path : {reservation.videoSpoolPath, reservation.audioSpoolPath,
                                reservation.temporaryMp4Path}) {
        const QRegularExpressionMatch match =
            temporaryFilePattern().match(QFileInfo(path).fileName());
        if (match.hasMatch()) return lockPath(QFileInfo(path).absolutePath(), match.captured(1));
    }
    return {};
}

QString hideReservationLock(const QString &path) {
#ifdef Q_OS_WIN
    const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();
    const DWORD attributes = GetFileAttributesW(nativePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !SetFileAttributesW(nativePath.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN)) {
        return QStringLiteral("Could not hide recording transaction lock \"%1\" (Windows error %2)")
            .arg(path)
            .arg(GetLastError());
    }
#else
    Q_UNUSED(path);
#endif
    return {};
}

struct TemporaryFileCreationResult {
    bool ownedFileRemains = false;
    bool hideFailed = false;
    QString error;
};

TemporaryFileCreationResult createTemporaryFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return {false, false,
                QStringLiteral("Could not exclusively create temporary recording file \"%1\": %2")
                    .arg(path, file.errorString())};
    }
    file.close();

#ifdef Q_OS_WIN
    const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();
    const DWORD attributes = GetFileAttributesW(nativePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !SetFileAttributesW(nativePath.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN)) {
        const DWORD error = GetLastError();
        const bool removed = QFile::remove(path);
        return {!removed, true,
                QStringLiteral("Could not hide temporary recording file \"%1\" (Windows error %2)")
                    .arg(path)
                    .arg(error)};
    }
#endif
    return {true, false, {}};
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
    const QString transactionLockPath = lockPath(outputPath, id);
    QString lockError;
    if (!acquireReservationLock(transactionLockPath, &lockError)) return {{}, lockError};
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
        const TemporaryFileCreationResult creation = createTemporaryFile(path);
        if (!creation.error.isEmpty()) {
            if (creation.ownedFileRemains) created.append(path);
            QStringList cleanupFailures;
            for (const QString &createdPath : created) {
                const QFileInfo info(createdPath);
                if ((info.exists() || info.isSymLink()) && !QFile::remove(createdPath)) {
                    cleanupFailures.append(createdPath);
                }
            }
            QString error = creation.error;
            if (creation.hideFailed && cleanupFailures.isEmpty()) {
                error += QStringLiteral("; temporary file cleanup succeeded");
            } else if (!cleanupFailures.isEmpty()) {
                error += QStringLiteral("; temporary file cleanup also failed; owned temporary files remain: %1")
                             .arg(cleanupFailures.join(QStringLiteral(", ")));
            }
            const QString lockCleanupError = releaseReservationLock(transactionLockPath);
            if (!lockCleanupError.isEmpty()) error += QStringLiteral("; ") + lockCleanupError;
            return {{}, error};
        }
        created.append(path);
    }
    const QString lockHideError = hideReservationLock(transactionLockPath);
    if (!lockHideError.isEmpty()) {
        QStringList cleanupFailures;
        for (const QString &createdPath : created) {
            if (QFileInfo::exists(createdPath) && !QFile::remove(createdPath)) {
                cleanupFailures.append(createdPath);
            }
        }
        QString error = lockHideError;
        if (cleanupFailures.isEmpty()) {
            error += QStringLiteral("; temporary file cleanup succeeded");
        } else {
            error += QStringLiteral("; temporary file cleanup also failed; owned temporary files remain: %1")
                         .arg(cleanupFailures.join(QStringLiteral(", ")));
        }
        const QString lockCleanupError = releaseReservationLock(transactionLockPath);
        if (!lockCleanupError.isEmpty()) error += QStringLiteral("; ") + lockCleanupError;
        return {{}, error};
    }
    return {reservation, {}};
}

QString RecordingFileTransaction::commit(const RecordingFileReservation &reservation) {
    const QFileInfo mp4Info(reservation.temporaryMp4Path);
    if (mp4Info.isSymLink() || (mp4Info.exists() && !mp4Info.isFile())) {
        return QStringLiteral("Temporary MP4 result is not a regular file: \"%1\"")
            .arg(reservation.temporaryMp4Path);
    }
    if (!mp4Info.exists()) {
        return QStringLiteral("Temporary MP4 result is missing: \"%1\"")
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

    QStringList cleanupFailures;
    for (const QString &path : {reservation.videoSpoolPath, reservation.audioSpoolPath}) {
        const QFileInfo info(path);
        if ((info.exists() || info.isSymLink()) && !QFile::remove(path)) cleanupFailures.append(path);
    }
    if (!cleanupFailures.isEmpty()) {
        return QStringLiteral("Temporary spool cleanup failed before commit: %1")
            .arg(cleanupFailures.join(QStringLiteral(", ")));
    }

#ifdef Q_OS_WIN
    const std::wstring nativeTemporaryPath =
        QDir::toNativeSeparators(reservation.temporaryMp4Path).toStdWString();
    const DWORD originalAttributes = GetFileAttributesW(nativeTemporaryPath.c_str());
    if (originalAttributes == INVALID_FILE_ATTRIBUTES) {
        return QStringLiteral("Could not read temporary MP4 attributes before commit: \"%1\" (Windows error %2)")
            .arg(reservation.temporaryMp4Path)
            .arg(GetLastError());
    }
    const bool wasHidden = (originalAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
    if (wasHidden) {
        DWORD visibleAttributes = originalAttributes & ~FILE_ATTRIBUTE_HIDDEN;
        if (visibleAttributes == 0) visibleAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileAttributesW(nativeTemporaryPath.c_str(), visibleAttributes)) {
            return QStringLiteral("Could not make temporary MP4 visible before commit: \"%1\" (Windows error %2)")
                .arg(reservation.temporaryMp4Path)
                .arg(GetLastError());
        }
    }
#endif

    QFile mp4(reservation.temporaryMp4Path);
    if (!mp4.rename(reservation.finalPath)) {
        QString error = QStringLiteral("Could not commit recording to \"%1\": %2")
                            .arg(reservation.finalPath, mp4.errorString());
#ifdef Q_OS_WIN
        if (wasHidden) {
            const DWORD currentAttributes = GetFileAttributesW(nativeTemporaryPath.c_str());
            if (currentAttributes == INVALID_FILE_ATTRIBUTES ||
                !SetFileAttributesW(nativeTemporaryPath.c_str(),
                                    currentAttributes | FILE_ATTRIBUTE_HIDDEN)) {
                error += QStringLiteral("; failed to restore HIDDEN on temporary MP4 \"%1\" (Windows error %2)")
                             .arg(reservation.temporaryMp4Path)
                             .arg(GetLastError());
            }
        }
#endif
        return error;
    }
    const QString lockCleanupError = releaseReservationLock(reservationLockPath(reservation));
    if (!lockCleanupError.isEmpty()) qWarning().noquote() << lockCleanupError;
    return {};
}

void RecordingFileTransaction::discard(const RecordingFileReservation &reservation) {
    removeExistingFile(reservation.videoSpoolPath);
    removeExistingFile(reservation.audioSpoolPath);
    removeExistingFile(reservation.temporaryMp4Path);
    const QString lockCleanupError = releaseReservationLock(reservationLockPath(reservation));
    if (!lockCleanupError.isEmpty()) qWarning().noquote() << lockCleanupError;
}

QStringList RecordingFileTransaction::cleanupStaleTemporaryFiles(const QString &directory) {
    if (directory.trimmed().isEmpty()) return {};
    const QDir outputDirectory(directory);
    QMap<QString, QStringList> pathsById;
    const QFileInfoList entries = outputDirectory.entryInfoList(
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QRegularExpressionMatch match = temporaryFilePattern().match(entry.fileName());
        if (!entry.isSymLink() && entry.isFile() && match.hasMatch()) {
            pathsById[match.captured(1)].append(QDir::cleanPath(entry.absoluteFilePath()));
            continue;
        }
        const QRegularExpressionMatch lockMatch = lockFilePattern().match(entry.fileName());
        if (!entry.isSymLink() && entry.isFile() && lockMatch.hasMatch()) {
            pathsById[lockMatch.captured(1)];
        }
    }

    QStringList deleted;
    for (auto group = pathsById.cbegin(); group != pathsById.cend(); ++group) {
        const QString transactionLockPath = lockPath(outputDirectory.absolutePath(), group.key());
        QLockFile lock(transactionLockPath);
        configureLock(&lock);
        if (!lock.tryLock(0)) {
            if (lock.error() != QLockFile::LockFailedError) {
                qWarning().noquote()
                    << QStringLiteral("Could not inspect recording lock \"%1\" during stale cleanup (error %2)")
                           .arg(transactionLockPath)
                           .arg(static_cast<int>(lock.error()));
            }
            continue;
        }
        const QString lockHideError = hideReservationLock(transactionLockPath);
        if (!lockHideError.isEmpty()) qWarning().noquote() << lockHideError;
        for (const QString &path : group.value()) {
            if (QFile::remove(path)) {
                deleted.append(path);
            } else {
                qWarning().noquote()
                    << QStringLiteral("Could not remove stale recording temporary file: %1").arg(path);
            }
        }
        lock.unlock();
        warnIfLockFileRemains(transactionLockPath);
    }
    deleted.sort();
    return deleted;
}
