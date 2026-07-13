#pragma once

#include <QMetaType>
#include <QString>

enum class RecordingState {
    Idle,
    Recording,
    Finalizing
};

enum class RecordingFormat {
    Mp4
};

struct RecordingOptions {
    QString outputDirectory;
    RecordingFormat format = RecordingFormat::Mp4;
};

struct RecordingStartResult {
    bool accepted = false;
    QString error;
};

struct RecordingResult {
    QString finalPath;
    QString warning;
};

Q_DECLARE_METATYPE(RecordingState)
Q_DECLARE_METATYPE(RecordingResult)
