#include "app/AppSettingsStore.h"
#include "app/ShortcutActionKey.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <utility>

namespace {
QString resolutionToString(VideoResolution res) {
    switch (res) {
    case VideoResolution::P540: return "540p";
    case VideoResolution::P720: return "720p";
    default: return "1080p";
    }
}

VideoResolution resolutionFromString(const QString &s) {
    if (s == "540p") return VideoResolution::P540;
    if (s == "720p") return VideoResolution::P720;
    if (s == "1080p") return VideoResolution::P1080;
    return VideoResolution::P1080;
}

int frameRateToInt(VideoFrameRate fr) {
    switch (fr) {
    case VideoFrameRate::Fps15: return 15;
    case VideoFrameRate::Fps60: return 60;
    default: return 30;
    }
}

VideoFrameRate frameRateFromInt(int value) {
    if (value == 15) return VideoFrameRate::Fps15;
    if (value == 60) return VideoFrameRate::Fps60;
    if (value == 30) return VideoFrameRate::Fps30;
    return VideoFrameRate::Fps30;
}
}

AppSettingsStore::AppSettingsStore(QString path)
    : path_(std::move(path)) {}

AppSettings AppSettingsStore::loadOrDefaults() const {
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) {
        return AppSettings::defaults();
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isNull() || !doc.isObject()) {
        return AppSettings::defaults();
    }

    const QJsonObject root = doc.object();
    const QJsonObject shortcuts = root.value("shortcuts").toObject();
    AppSettings settings = AppSettings::defaults();
    const QString receiverName = root.value("receiverName").toString().trimmed();
    if (!receiverName.isEmpty()) {
        settings.setReceiverName(receiverName);
    }
    for (const ShortcutBinding &binding : settings.shortcuts()) {
        const QString value = shortcuts.value(shortcutActionKey(binding.action)).toString();
        if (!value.isEmpty()) {
            const QKeySequence parsed = QKeySequence::fromString(value, QKeySequence::PortableText);
            if (!parsed.toString(QKeySequence::PortableText).isEmpty() && parsed.count() == 1) {
                settings.setShortcut(binding.action, parsed);
            }
        }
    }
    const QJsonValue volume = root.value("volume");
    if (volume.isDouble()) {
        settings.setVolume(volume.toInt());
    }
    const QJsonValue aspectLock = root.value("aspectRatioLock");
    if (aspectLock.isBool()) {
        settings.setAspectRatioLock(aspectLock.toBool());
    }
    const QJsonValue videoFit = root.value("videoFitMode");
    if (videoFit.isBool()) {
        settings.setVideoFitMode(videoFit.toBool());
    }
    const QJsonValue videoQualityVal = root.value("videoQuality");
    if (videoQualityVal.isObject()) {
        const QJsonObject vqObj = videoQualityVal.toObject();
        VideoQualitySettings vq = settings.videoQuality();
        const QJsonValue resolutionVal = vqObj.value("resolution");
        if (resolutionVal.isString()) {
            vq.resolution = resolutionFromString(resolutionVal.toString());
        }
        const QJsonValue frameRateVal = vqObj.value("frameRate");
        if (frameRateVal.isDouble()) {
            vq.frameRate = frameRateFromInt(frameRateVal.toInt());
        }
        settings.setVideoQuality(vq);
    }
    return settings;
}

bool AppSettingsStore::save(const AppSettings &settings) const {
    QJsonObject shortcuts;
    for (const ShortcutBinding &binding : settings.shortcuts()) {
        shortcuts.insert(shortcutActionKey(binding.action), binding.sequence.toString(QKeySequence::PortableText));
    }

    QJsonObject videoQuality;
    videoQuality.insert("resolution", resolutionToString(settings.videoQuality().resolution));
    videoQuality.insert("frameRate", frameRateToInt(settings.videoQuality().frameRate));

    QJsonObject root;
    root.insert("receiverName", settings.receiverName());
    root.insert("shortcuts", shortcuts);
    root.insert("volume", settings.volume());
    root.insert("aspectRatioLock", settings.aspectRatioLock());
    root.insert("videoFitMode", settings.videoFitMode());
    root.insert("videoQuality", videoQuality);

    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        return false;
    }
    return file.commit();
}
