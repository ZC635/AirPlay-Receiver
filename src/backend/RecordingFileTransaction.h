#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QUuid>

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
    static QString commit(const RecordingFileReservation &reservation);
    static void discard(const RecordingFileReservation &reservation);
    static QStringList cleanupStaleTemporaryFiles(const QString &directory);
};
