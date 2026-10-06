#include "backend/RecordingFileTransaction.h"
#include "platform/FileSystemPath.h"

#include <QDir>
#include <QHash>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
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

QSet<QString> &claimedAudioRegistry() {
    static QSet<QString> registry;
    return registry;
}

QString normalizedLockKey(const QString &path) {
    QString key = FileSystemPath::absolute(path);
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
    if (QFileInfo::exists(FileSystemPath::forIo(path))) {
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
    auto lock = QSharedPointer<QLockFile>::create(FileSystemPath::forIo(path));
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

QString releaseReservationLockLocked(const QString &path) {
    if (path.isEmpty()) return {};
    const auto lock = lockRegistry().take(normalizedLockKey(path));
    if (!lock) return {};
    lock->unlock();
    if (QFileInfo::exists(FileSystemPath::forIo(path))) {
        return QStringLiteral("recording lock cleanup failed; lock file remains: %1").arg(path);
    }
    return {};
}

QString releaseReservationLock(const QString &path) {
    QMutexLocker guard(&lockRegistryMutex());
    return releaseReservationLockLocked(path);
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

const QRegularExpression &optionalAudioFilePattern() {
    static const QRegularExpression pattern(QStringLiteral(
        R"(^\.airplay-recording-([0-9a-f]{32})\.audio\.mka\.part$)"));
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
    const std::wstring nativePath = FileSystemPath::forIo(path).toStdWString();
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
    QFile file(FileSystemPath::forIo(path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return {false, false,
                QStringLiteral("Could not exclusively create temporary recording file \"%1\": %2")
                    .arg(path, file.errorString())};
    }
    file.close();

#ifdef Q_OS_WIN
    const std::wstring nativePath = FileSystemPath::forIo(path).toStdWString();
    const DWORD attributes = GetFileAttributesW(nativePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !SetFileAttributesW(nativePath.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN)) {
        const DWORD error = GetLastError();
        const bool removed = QFile::remove(FileSystemPath::forIo(path));
        return {!removed, true,
                QStringLiteral("Could not hide temporary recording file \"%1\" (Windows error %2)")
                    .arg(path)
                    .arg(error)};
    }
#endif
    return {true, false, {}};
}

void removeExistingFile(const QString &path) {
    const QFileInfo info(FileSystemPath::forIo(path));
    if (info.exists() || info.isSymLink()) QFile::remove(FileSystemPath::forIo(path));
}

QString finalRecordingPath(const QDir &directory, const QDateTime &localNow) {
    const QString stem = QStringLiteral("AirPlay Recording %1")
                             .arg(localNow.toString(QStringLiteral("yyyy-MM-dd HH-mm-ss")));
    QString candidate = directory.filePath(stem + QStringLiteral(".mp4"));
    for (int suffix = 2; QFileInfo::exists(FileSystemPath::forIo(candidate)); ++suffix) {
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
    const QString outputPath = FileSystemPath::absolute(directory);
    if (outputPath.isEmpty()) {
        return {{}, QStringLiteral("Unsupported recording output directory: \"%1\"").arg(directory)};
    }
    if (!QDir().mkpath(FileSystemPath::forIo(outputPath)) || !QFileInfo(FileSystemPath::forIo(outputPath)).isDir()) {
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

    const QFileInfo optionalAudioInfo(FileSystemPath::forIo(reservation.audioSpoolPath));
    if (optionalAudioInfo.exists() || optionalAudioInfo.isSymLink()) {
        QString error = QStringLiteral("Optional audio spool path is already occupied: \"%1\"")
                            .arg(reservation.audioSpoolPath);
        const QString lockCleanupError = releaseReservationLock(transactionLockPath);
        if (!lockCleanupError.isEmpty()) error += QStringLiteral("; ") + lockCleanupError;
        return {{}, error};
    }

    QStringList created;
    for (const QString &path : {reservation.videoSpoolPath, reservation.temporaryMp4Path}) {
        const TemporaryFileCreationResult creation = createTemporaryFile(path);
        if (!creation.error.isEmpty()) {
            if (creation.ownedFileRemains) created.append(path);
            QStringList cleanupFailures;
            for (const QString &createdPath : created) {
                const QFileInfo info(FileSystemPath::forIo(createdPath));
                if ((info.exists() || info.isSymLink()) && !QFile::remove(FileSystemPath::forIo(createdPath))) {
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
            if (QFileInfo::exists(FileSystemPath::forIo(createdPath)) && !QFile::remove(FileSystemPath::forIo(createdPath))) {
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

QString RecordingFileTransaction::claimOptionalAudioSpool(const QString &audioSpoolPath) {
    const QString logicalAudioPath = FileSystemPath::absolute(audioSpoolPath);
    if (logicalAudioPath.isEmpty()) {
        return QStringLiteral("Optional audio spool path is not a supported file-system path");
    }
    const QFileInfo audioInfo(FileSystemPath::forIo(logicalAudioPath));
    const QRegularExpressionMatch match =
        optionalAudioFilePattern().match(audioInfo.fileName());
    if (!match.hasMatch()) {
        return QStringLiteral("Optional audio spool path is not an exact recording audio path: \"%1\"")
            .arg(logicalAudioPath);
    }

    const QString audioKey = normalizedLockKey(logicalAudioPath);
    const QString transactionLockPath = lockPath(audioInfo.absolutePath(), match.captured(1));
    QMutexLocker guard(&lockRegistryMutex());
    if (!lockRegistry().contains(normalizedLockKey(transactionLockPath))) {
        return QStringLiteral("Optional audio spool requires a matching active reservation lock: \"%1\"")
            .arg(logicalAudioPath);
    }
    if (claimedAudioRegistry().contains(audioKey)) {
        return QStringLiteral("Optional audio spool is already claimed: \"%1\"")
            .arg(logicalAudioPath);
    }

    const TemporaryFileCreationResult creation = createTemporaryFile(logicalAudioPath);
    if (!creation.error.isEmpty()) {
        QString error = creation.error;
        if (creation.ownedFileRemains) {
            claimedAudioRegistry().insert(audioKey);
            error += QStringLiteral("; owned audio spool remains claimed for cleanup: %1")
                         .arg(logicalAudioPath);
        } else if (creation.hideFailed) {
            error += QStringLiteral("; temporary file cleanup succeeded");
        }
        return error;
    }

    claimedAudioRegistry().insert(audioKey);
    return {};
}

QString RecordingFileTransaction::commit(const RecordingFileReservation &reservation) {
    const QFileInfo mp4Info(FileSystemPath::forIo(reservation.temporaryMp4Path));
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
    if (QFileInfo::exists(FileSystemPath::forIo(reservation.finalPath))) {
        return QStringLiteral("Final recording path already exists: \"%1\"")
            .arg(reservation.finalPath);
    }

    const QFileInfo videoInfo(FileSystemPath::forIo(reservation.videoSpoolPath));
    if ((videoInfo.exists() || videoInfo.isSymLink()) &&
        !QFile::remove(FileSystemPath::forIo(reservation.videoSpoolPath))) {
        return QStringLiteral("Temporary spool cleanup failed before commit: %1")
            .arg(reservation.videoSpoolPath);
    }

    QMutexLocker registryGuard(&lockRegistryMutex());
    const QString audioKey = normalizedLockKey(reservation.audioSpoolPath);
    if (claimedAudioRegistry().contains(audioKey)) {
        const QFileInfo audioInfo(FileSystemPath::forIo(reservation.audioSpoolPath));
        if ((audioInfo.exists() || audioInfo.isSymLink()) &&
            !QFile::remove(FileSystemPath::forIo(reservation.audioSpoolPath))) {
            return QStringLiteral("Temporary spool cleanup failed before commit: %1")
                .arg(reservation.audioSpoolPath);
        }
        claimedAudioRegistry().remove(audioKey);
    }

#ifdef Q_OS_WIN
    const std::wstring nativeTemporaryPath =
        FileSystemPath::forIo(reservation.temporaryMp4Path).toStdWString();
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

    QFile mp4(FileSystemPath::forIo(reservation.temporaryMp4Path));
    if (!mp4.rename(FileSystemPath::forIo(reservation.finalPath))) {
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
    const QString lockCleanupError =
        releaseReservationLockLocked(reservationLockPath(reservation));
    if (!lockCleanupError.isEmpty()) qWarning().noquote() << lockCleanupError;
    return {};
}

void RecordingFileTransaction::discard(const RecordingFileReservation &reservation) {
    removeExistingFile(reservation.videoSpoolPath);
    removeExistingFile(reservation.temporaryMp4Path);
    QMutexLocker registryGuard(&lockRegistryMutex());
    const QString audioKey = normalizedLockKey(reservation.audioSpoolPath);
    if (claimedAudioRegistry().contains(audioKey)) {
        const QFileInfo audioInfo(FileSystemPath::forIo(reservation.audioSpoolPath));
        if ((audioInfo.exists() || audioInfo.isSymLink()) &&
            !QFile::remove(FileSystemPath::forIo(reservation.audioSpoolPath))) {
            qWarning().noquote()
                << QStringLiteral("Could not discard claimed optional audio spool: %1")
                       .arg(reservation.audioSpoolPath);
        }
        claimedAudioRegistry().remove(audioKey);
    }
    const QString lockCleanupError =
        releaseReservationLockLocked(reservationLockPath(reservation));
    if (!lockCleanupError.isEmpty()) qWarning().noquote() << lockCleanupError;
}

QStringList RecordingFileTransaction::cleanupStaleTemporaryFiles(const QString &directory) {
    if (directory.trimmed().isEmpty()) return {};
    const QString outputPath = FileSystemPath::absolute(directory);
    if (outputPath.isEmpty()) return {};
    const QDir outputDirectory(FileSystemPath::forIo(outputPath));
    QMap<QString, QStringList> pathsById;
    const QFileInfoList entries = outputDirectory.entryInfoList(
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QRegularExpressionMatch match = temporaryFilePattern().match(entry.fileName());
        if (!entry.isSymLink() && entry.isFile() && match.hasMatch()) {
            pathsById[match.captured(1)].append(FileSystemPath::absolute(entry.absoluteFilePath()));
            continue;
        }
        const QRegularExpressionMatch lockMatch = lockFilePattern().match(entry.fileName());
        if (!entry.isSymLink() && entry.isFile() && lockMatch.hasMatch()) {
            pathsById[lockMatch.captured(1)];
        }
    }

    QStringList deleted;
    for (auto group = pathsById.cbegin(); group != pathsById.cend(); ++group) {
        const QString transactionLockPath = lockPath(outputPath, group.key());
        QLockFile lock(FileSystemPath::forIo(transactionLockPath));
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
            if (QFile::remove(FileSystemPath::forIo(path))) {
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
