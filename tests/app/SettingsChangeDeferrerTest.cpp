#include <QtTest/QtTest>
#include "app/SettingsChangeDeferrer.h"
#include "backend/VideoQualitySettings.h"

class SettingsChangeDeferrerTest : public QObject {
    Q_OBJECT

private slots:
    void initiallyHasNoPendingChanges() {
        SettingsChangeDeferrer deferrer;
        QVERIFY(!deferrer.isReceiverNamePending(QString()));
        QVERIFY(!deferrer.isReceiverNamePending("AnyName"));
        QVERIFY(!deferrer.isVideoQualityPending(VideoQualitySettings{}));
    }

    void storesDeferredReceiverNameAndEmitsWhenSessionEnds() {
        SettingsChangeDeferrer deferrer;
        QString emittedName;
        connect(&deferrer, &SettingsChangeDeferrer::receiverNameReady,
                [&](const QString &name) { emittedName = name; });

        deferrer.receiverNameChanged("NewName", "OldName");
        QVERIFY(deferrer.isReceiverNamePending("NewName"));
        QVERIFY(!deferrer.isReceiverNamePending("OtherName"));

        deferrer.receiverSessionChanged(true, false);
        QCOMPARE(emittedName, QString("NewName"));
    }

    void clearsDeferredReceiverNameWhenChangedBackToActiveName() {
        SettingsChangeDeferrer deferrer;

        deferrer.receiverNameChanged("NewName", "OldName");
        QVERIFY(deferrer.isReceiverNamePending("NewName"));

        deferrer.receiverNameChanged("OldName", "OldName");
        QVERIFY(!deferrer.isReceiverNamePending("NewName"));
    }

    void doesNotEmitReceiverNameWhenNoPending() {
        SettingsChangeDeferrer deferrer;
        int emitCount = 0;
        connect(&deferrer, &SettingsChangeDeferrer::receiverNameReady,
                [&](const QString &) { ++emitCount; });

        deferrer.receiverSessionChanged(true, false);
        QCOMPARE(emitCount, 0);
    }

    void marksReceiverNameAppliedClearsPending() {
        SettingsChangeDeferrer deferrer;

        deferrer.receiverNameChanged("NewName", "OldName");
        QVERIFY(deferrer.isReceiverNamePending("NewName"));

        deferrer.markReceiverNameApplied("NewName");
        QVERIFY(!deferrer.isReceiverNamePending("NewName"));
    }

    void marksReceiverNameAppliedWithDifferentNameDoesNotClear() {
        SettingsChangeDeferrer deferrer;

        deferrer.receiverNameChanged("NewName", "OldName");
        QVERIFY(deferrer.isReceiverNamePending("NewName"));

        deferrer.markReceiverNameApplied("OtherName");
        QVERIFY(deferrer.isReceiverNamePending("NewName"));
    }

    void storesDeferredVideoQualityAndEmitsWhenSessionEnds() {
        SettingsChangeDeferrer deferrer;
        const VideoQualitySettings pending{VideoResolution::P720, VideoFrameRate::Fps60};
        const VideoQualitySettings active{VideoResolution::P1080, VideoFrameRate::Fps30};
        std::optional<VideoQualitySettings> emittedQuality;
        connect(&deferrer, &SettingsChangeDeferrer::videoQualityReady,
                [&](const VideoQualitySettings &q) { emittedQuality = q; });

        deferrer.videoQualityChanged(pending, active);
        QVERIFY(deferrer.isVideoQualityPending(pending));

        deferrer.receiverSessionChanged(true, false);
        QVERIFY(emittedQuality.has_value());
        QVERIFY(emittedQuality->resolution == pending.resolution);
        QVERIFY(emittedQuality->frameRate == pending.frameRate);
    }

    void pendingVideoQualityRetriesOnSessionChangeUntilApplied() {
        SettingsChangeDeferrer deferrer;
        const VideoQualitySettings pending{VideoResolution::P720, VideoFrameRate::Fps60};
        const VideoQualitySettings active{VideoResolution::P1080, VideoFrameRate::Fps30};
        int emitCount = 0;
        connect(&deferrer, &SettingsChangeDeferrer::videoQualityReady,
                [&](const VideoQualitySettings &) { ++emitCount; });

        deferrer.videoQualityChanged(pending, active);
        QVERIFY(deferrer.isVideoQualityPending(pending));

        deferrer.receiverSessionChanged(true, false);
        QCOMPARE(emitCount, 1);
        QVERIFY(deferrer.isVideoQualityPending(pending));

        deferrer.receiverSessionChanged(false, false);
        QCOMPARE(emitCount, 2);
        QVERIFY(deferrer.isVideoQualityPending(pending));

        deferrer.markVideoQualityApplied(pending);
        QVERIFY(!deferrer.isVideoQualityPending(pending));

        deferrer.receiverSessionChanged(false, false);
        QCOMPARE(emitCount, 2);
    }

    void clearsDeferredVideoQualityWhenChangedBackToActive() {
        SettingsChangeDeferrer deferrer;
        const VideoQualitySettings newQuality{VideoResolution::P720, VideoFrameRate::Fps60};
        const VideoQualitySettings activeQuality{VideoResolution::P1080, VideoFrameRate::Fps30};

        deferrer.videoQualityChanged(newQuality, activeQuality);
        QVERIFY(deferrer.isVideoQualityPending(newQuality));

        deferrer.videoQualityChanged(activeQuality, activeQuality);
        QVERIFY(!deferrer.isVideoQualityPending(newQuality));
    }

    void doesNotRepeatedlySetSamePendingReceiverName() {
        SettingsChangeDeferrer deferrer;

        deferrer.receiverNameChanged("NewName", "OldName");
        QVERIFY(deferrer.isReceiverNamePending("NewName"));

        deferrer.receiverNameChanged("NewName", "OldName");
        QVERIFY(deferrer.isReceiverNamePending("NewName"));
    }

    void doesNotRepeatedlySetSamePendingVideoQuality() {
        SettingsChangeDeferrer deferrer;
        const VideoQualitySettings newQuality{VideoResolution::P720, VideoFrameRate::Fps60};
        const VideoQualitySettings activeQuality{VideoResolution::P1080, VideoFrameRate::Fps30};

        deferrer.videoQualityChanged(newQuality, activeQuality);
        QVERIFY(deferrer.isVideoQualityPending(newQuality));

        deferrer.videoQualityChanged(newQuality, activeQuality);
        QVERIFY(deferrer.isVideoQualityPending(newQuality));
    }

    void videoQualityNotEmittedWhileSessionActive() {
        SettingsChangeDeferrer deferrer;
        const VideoQualitySettings pending{VideoResolution::P720, VideoFrameRate::Fps60};
        const VideoQualitySettings active{VideoResolution::P1080, VideoFrameRate::Fps30};
        int emitCount = 0;
        connect(&deferrer, &SettingsChangeDeferrer::videoQualityReady,
                [&](const VideoQualitySettings &) { ++emitCount; });

        deferrer.videoQualityChanged(pending, active);
        QVERIFY(deferrer.isVideoQualityPending(pending));

        deferrer.receiverSessionChanged(false, true);
        QCOMPARE(emitCount, 0);
        QVERIFY(deferrer.isVideoQualityPending(pending));
    }

    void receiverNameNotEmittedWhenSessionBecomesActive() {
        SettingsChangeDeferrer deferrer;
        QString emittedName;
        connect(&deferrer, &SettingsChangeDeferrer::receiverNameReady,
                [&](const QString &name) { emittedName = name; });

        deferrer.receiverNameChanged("NewName", "OldName");
        QVERIFY(deferrer.isReceiverNamePending("NewName"));

        deferrer.receiverSessionChanged(false, true);
        QVERIFY(emittedName.isEmpty());
        QVERIFY(deferrer.isReceiverNamePending("NewName"));
    }

    void receiverNameNotEmittedWhenSessionStaysInactive() {
        SettingsChangeDeferrer deferrer;
        QString emittedName;
        connect(&deferrer, &SettingsChangeDeferrer::receiverNameReady,
                [&](const QString &name) { emittedName = name; });

        deferrer.receiverNameChanged("NewName", "OldName");
        QVERIFY(deferrer.isReceiverNamePending("NewName"));

        deferrer.receiverSessionChanged(false, false);
        QVERIFY(emittedName.isEmpty());
        QVERIFY(deferrer.isReceiverNamePending("NewName"));
    }

    void changingPendingReceiverNameToAnotherReplacesPending() {
        SettingsChangeDeferrer deferrer;
        QString emittedName;
        connect(&deferrer, &SettingsChangeDeferrer::receiverNameReady,
                [&](const QString &name) { emittedName = name; });

        deferrer.receiverNameChanged("NameA", "OldName");
        QVERIFY(deferrer.isReceiverNamePending("NameA"));

        deferrer.receiverNameChanged("NameB", "OldName");
        QVERIFY(deferrer.isReceiverNamePending("NameB"));
        QVERIFY(!deferrer.isReceiverNamePending("NameA"));

        deferrer.receiverSessionChanged(true, false);
        QCOMPARE(emittedName, QString("NameB"));
    }

    void changingPendingVideoQualityToAnotherReplacesPending() {
        SettingsChangeDeferrer deferrer;
        const VideoQualitySettings active{VideoResolution::P1080, VideoFrameRate::Fps30};
        const VideoQualitySettings first{VideoResolution::P540, VideoFrameRate::Fps30};
        const VideoQualitySettings second{VideoResolution::P720, VideoFrameRate::Fps60};
        std::optional<VideoQualitySettings> emittedQuality;
        connect(&deferrer, &SettingsChangeDeferrer::videoQualityReady,
                [&](const VideoQualitySettings &q) { emittedQuality = q; });

        deferrer.videoQualityChanged(first, active);
        QVERIFY(deferrer.isVideoQualityPending(first));

        deferrer.videoQualityChanged(second, active);
        QVERIFY(deferrer.isVideoQualityPending(second));

        deferrer.receiverSessionChanged(true, false);
        QVERIFY(emittedQuality.has_value());
        QVERIFY(emittedQuality->resolution == second.resolution);
        QVERIFY(emittedQuality->frameRate == second.frameRate);
    }

    void acceptedReceiverNameRestartWaitsForRecordingIdle() {
        SettingsChangeDeferrer deferrer;
        QStringList emittedNames;
        connect(&deferrer, &SettingsChangeDeferrer::receiverNameReady,
                [&](const QString &name) { emittedNames.append(name); });

        deferrer.deferReceiverNameUntilRecordingIdle("Desk Receiver");
        deferrer.recordingStateChanged(RecordingState::Recording,
                                       RecordingState::Finalizing);
        QVERIFY(emittedNames.isEmpty());

        deferrer.recordingStateChanged(RecordingState::Finalizing,
                                       RecordingState::Idle);
        QCOMPARE(emittedNames, QStringList{"Desk Receiver"});

        deferrer.recordingStateChanged(RecordingState::Finalizing,
                                       RecordingState::Idle);
        QCOMPARE(emittedNames.size(), 1);
    }

    void acceptedVideoQualityRestartRunsAfterFailedFinalizeReturnsIdle() {
        SettingsChangeDeferrer deferrer;
        const VideoQualitySettings requested{VideoResolution::P720,
                                             VideoFrameRate::Fps60};
        QVector<VideoQualitySettings> emittedQualities;
        connect(&deferrer, &SettingsChangeDeferrer::videoQualityReady,
                [&](const VideoQualitySettings &quality) {
                    emittedQualities.append(quality);
                });

        deferrer.deferVideoQualityUntilRecordingIdle(requested);
        deferrer.recordingStateChanged(RecordingState::Recording,
                                       RecordingState::Finalizing);
        QVERIFY(emittedQualities.isEmpty());

        deferrer.recordingStateChanged(RecordingState::Finalizing,
                                       RecordingState::Idle);
        QCOMPARE(emittedQualities, QVector<VideoQualitySettings>{requested});
    }

    void discardTransitionDoesNotApplyRestartDeferredForSuccessfulSave() {
        SettingsChangeDeferrer deferrer;
        int emitCount = 0;
        connect(&deferrer, &SettingsChangeDeferrer::receiverNameReady,
                [&](const QString &) { ++emitCount; });
        deferrer.deferReceiverNameUntilRecordingIdle("Desk Receiver");

        deferrer.recordingStateChanged(RecordingState::Recording,
                                       RecordingState::Idle);

        QCOMPARE(emitCount, 0);
        QVERIFY(deferrer.isReceiverNamePending("Desk Receiver"));
    }

    void replacingRecordingDeferredValueWithSessionDeferredValueClearsGate() {
        SettingsChangeDeferrer deferrer;
        QString emittedName;
        connect(&deferrer, &SettingsChangeDeferrer::receiverNameReady,
                [&](const QString &name) { emittedName = name; });
        deferrer.deferReceiverNameUntilRecordingIdle("Recording Name");
        deferrer.receiverNameChanged("Session Name", "Active Name");

        deferrer.receiverSessionChanged(true, false);

        QCOMPARE(emittedName, QString("Session Name"));
    }

    void recordingReadySignalCannotReenterThroughSessionTransition() {
        SettingsChangeDeferrer deferrer;
        int emitCount = 0;
        connect(&deferrer, &SettingsChangeDeferrer::receiverNameReady,
                [&](const QString &) {
                    ++emitCount;
                    if (emitCount == 1) {
                        deferrer.receiverSessionChanged(true, false);
                    }
                });
        deferrer.deferReceiverNameUntilRecordingIdle("Desk Receiver");

        deferrer.recordingStateChanged(RecordingState::Finalizing,
                                       RecordingState::Idle);

        QCOMPARE(emitCount, 1);
        QVERIFY(deferrer.isReceiverNamePending("Desk Receiver"));
        deferrer.receiverSessionChanged(true, false);
        QCOMPARE(emitCount, 2);
    }
};

QTEST_MAIN(SettingsChangeDeferrerTest)
#include "SettingsChangeDeferrerTest.moc"
