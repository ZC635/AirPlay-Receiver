#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <functional>
#include <optional>

struct RecordingFileReservation {
    QString videoSpoolPath;
    QString audioSpoolPath;
    QString temporaryMp4Path;
    QString finalPath;
};

struct RecordingFileReservationResult {
    std::optional<RecordingFileReservation> reservation;
    QString error;
};

class RecordingFileTransaction {
public:
    static RecordingFileReservationResult reserve(const QString &directory,
                                                  const QDateTime &localNow,
                                                  const QUuid &uuid);
    static QString claimOptionalAudioSpool(const QString &audioSpoolPath);
    static QString commit(const RecordingFileReservation &reservation);
    static void discard(const RecordingFileReservation &reservation);
    static QStringList cleanupStaleTemporaryFiles(const QString &directory);

private:
    friend class RecordingFileTransactionTest;
    // Return zero on success or the native file-attribute error.
    using FileHider = std::function<unsigned long(const QString &path)>;
    static RecordingFileReservationResult reserveWithFileHider(
        const QString &directory, const QDateTime &localNow, const QUuid &uuid, const FileHider &hide);
};
