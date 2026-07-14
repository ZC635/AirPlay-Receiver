#include "app/RecordingStartupCleanup.h"

#include "app/AppSettings.h"
#include "backend/RecordingFileTransaction.h"

QStringList cleanupRecordingDirectoryAtStartup(const AppSettings &settings) {
    return RecordingFileTransaction::cleanupStaleTemporaryFiles(
        settings.recordingOutputDirectory());
}
