#pragma once

#include <QString>
#include "app/AppSettings.h"

class AppSettingsStore {
public:
    explicit AppSettingsStore(QString path);

    AppSettings loadOrDefaults() const;
    bool save(const AppSettings &settings) const;

private:
    QString path_;
};
