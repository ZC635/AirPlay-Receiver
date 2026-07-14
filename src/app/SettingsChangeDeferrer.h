#pragma once

#include "backend/RecordingTypes.h"
#include "backend/VideoQualitySettings.h"

#include <QObject>
#include <QString>
#include <optional>

class SettingsChangeDeferrer : public QObject {
    Q_OBJECT

public:
    explicit SettingsChangeDeferrer(QObject *parent = nullptr);

    bool isReceiverNamePending(QString name) const;
    bool isVideoQualityPending(VideoQualitySettings quality) const;

    void receiverNameChanged(QString requestedName, QString activeName);
    void videoQualityChanged(VideoQualitySettings requestedQuality, VideoQualitySettings activeQuality);
    void recordingStateChanged(RecordingState previous, RecordingState current);
    void deferReceiverNameUntilRecordingIdle(QString name);
    void deferVideoQualityUntilRecordingIdle(VideoQualitySettings quality);
    void receiverSessionChanged(bool wasSessionActive, bool sessionActive);
    void markReceiverNameApplied(QString name);
    void markVideoQualityApplied(VideoQualitySettings quality);

signals:
    void receiverNameReady(QString name);
    void videoQualityReady(VideoQualitySettings quality);

private:
    std::optional<QString> pendingReceiverName_;
    std::optional<VideoQualitySettings> pendingVideoQuality_;
    bool receiverNameWaitsForRecordingIdle_ = false;
    bool videoQualityWaitsForRecordingIdle_ = false;
};
