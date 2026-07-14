#pragma once

#include <QStringList>

class AppSettings;

QStringList cleanupRecordingDirectoryAtStartup(const AppSettings &settings);
