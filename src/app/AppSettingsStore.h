#pragma once

#include "app/AppSettings.h"

#include <QFileDevice>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

enum class AppSettingsSaveStage {
    Open,
    Write,
    Commit,
};

struct AppSettingsSaveResult {
    bool success = false;
    QString targetPath;
    std::optional<AppSettingsSaveStage> failureStage;
    QFileDevice::FileError fileError = QFileDevice::NoError;
    QString errorString;
};

class SettingsSaveDevice {
public:
    virtual ~SettingsSaveDevice() = default;

    virtual bool open() = 0;
    virtual qint64 write(const QByteArray &data) = 0;
    virtual bool commit() = 0;
    virtual QFileDevice::FileError error() const = 0;
    virtual QString errorString() const = 0;
};

using SettingsSaveDeviceFactory = std::function<std::unique_ptr<SettingsSaveDevice>(const QString &path)>;

class SettingsPersistence {
public:
    virtual ~SettingsPersistence() = default;

    virtual AppSettingsSaveResult save(const AppSettings &settings) const = 0;
};

class AppSettingsStore final : public SettingsPersistence {
public:
    explicit AppSettingsStore(QString path, SettingsSaveDeviceFactory deviceFactory = {});

    AppSettings loadOrDefaults() const;
    AppSettingsSaveResult save(const AppSettings &settings) const override;

private:
    QString path_;
    SettingsSaveDeviceFactory deviceFactory_;
};
