#pragma once

#include "backend/RecordingTypes.h"
#include "backend/ReceiverConfigurationChange.h"
#include "backend/VideoQualitySettings.h"

#include <QObject>
#include <QString>
#include <optional>

class SettingsChangeDeferrer : public QObject {
    Q_OBJECT

public:
    explicit SettingsChangeDeferrer(QObject *parent = nullptr);

    void deferReceiverConfiguration(ReceiverConfigurationBatchRequest batch,
                                    bool waitForSessionEnd,
                                    bool waitForRecordingIdle);
    void cancelPendingReceiverConfiguration();
    bool hasPendingReceiverConfiguration() const;

    // Compatibility API for the legacy independent name and quality flows.
    // These remain separate from unified receiver configuration batches until
    // their callers migrate.
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
    void receiverConfigurationReady(ReceiverConfigurationBatchRequest batch);
    void receiverNameReady(QString name);
    void videoQualityReady(VideoQualitySettings quality);

private:
    void emitReceiverConfigurationIfReady();

    std::optional<ReceiverConfigurationBatchRequest> pendingReceiverConfiguration_;
    bool receiverConfigurationWaitsForSessionEnd_ = false;
    bool receiverConfigurationWaitsForRecordingIdle_ = false;

    std::optional<QString> pendingReceiverName_;
    std::optional<VideoQualitySettings> pendingVideoQuality_;
    bool receiverNameWaitsForRecordingIdle_ = false;
    bool videoQualityWaitsForRecordingIdle_ = false;
};
