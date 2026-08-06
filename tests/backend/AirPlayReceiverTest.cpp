#include <QtTest/QtTest>
#include "backend/FakeAirPlayReceiver.h"

class AirPlayReceiverTest : public QObject {
    Q_OBJECT

private slots:
    void emitsStateChanges() {
        FakeAirPlayReceiver receiver;
        QSignalSpy spy(&receiver, &AirPlayReceiver::stateChanged);
        receiver.start();
        QCOMPARE(receiver.state(), ReceiverState::Discoverable);
        QCOMPARE(spy.count(), 1);
    }

    void emitsVideoSizeChanged() {
        FakeAirPlayReceiver receiver;
        QSignalSpy spy(&receiver, &AirPlayReceiver::videoSizeChanged);
        receiver.emitVideoSize(1170, 2532);
        QCOMPARE(spy.count(), 1);
        QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.at(0).toInt(), 1170);
        QCOMPARE(args.at(1).toInt(), 2532);
    }

    void fakeRejectsVideoQualityWhileStartingOrError() {
        FakeAirPlayReceiver receiver;
        const VideoQualitySettings quality{VideoResolution::P720, VideoFrameRate::Fps60};
        const VideoQualitySettings defaultQuality;

        receiver.forceState(ReceiverState::Starting);

        QVERIFY(!receiver.applyVideoQuality(quality));
        QCOMPARE(receiver.lastAppliedVideoQuality, defaultQuality);

        receiver.forceState(ReceiverState::Error);

        QVERIFY(!receiver.applyVideoQuality(quality));
        QCOMPARE(receiver.lastAppliedVideoQuality, defaultQuality);

        receiver.forceState(ReceiverState::Discoverable);

        QVERIFY(receiver.applyVideoQuality(quality));
        QCOMPARE(receiver.lastAppliedVideoQuality, quality);
    }
    void unchangedVideoQualityReturnsTrueEvenIfRejected() {
        FakeAirPlayReceiver receiver;
        const VideoQualitySettings defaultQuality;
        const VideoQualitySettings otherQuality{VideoResolution::P720, VideoFrameRate::Fps60};

        QVERIFY(receiver.applyVideoQuality(otherQuality));
        QCOMPARE(receiver.lastAppliedVideoQuality, otherQuality);

        receiver.rejectedVideoQualities.append(otherQuality);

        QVERIFY(receiver.applyVideoQuality(otherQuality));
        QCOMPARE(receiver.lastAppliedVideoQuality, otherQuality);
    }

    void fakeBatchFailureRollsBackWithExactError() {
        FakeAirPlayReceiver receiver;
        receiver.forceState(ReceiverState::Discoverable);
        receiver.requestedConfigurationRestartError = QStringLiteral("requested restart failed");

        ReceiverConfigurationBatchRequest request;
        request.receiverNameChanged = true;
        request.resolutionChanged = true;
        request.requestedReceiverName = QStringLiteral("Updated Receiver");
        request.rollbackReceiverName = receiver.receiverName();
        request.requestedVideoQuality = {VideoResolution::P720, VideoFrameRate::Fps30};
        request.rollbackVideoQuality = receiver.videoQuality();

        const auto result = receiver.applyConfigurationBatch(request);

        QCOMPARE(result.status, ReceiverConfigurationBatchStatus::ApplyFailedRolledBack);
        QCOMPARE(result.applyError, QStringLiteral("requested restart failed"));
        QCOMPARE(receiver.configurationBatchCount, 1);
        QCOMPARE(receiver.configurationRestartCount, 2);
        QCOMPARE(receiver.receiverName(), request.rollbackReceiverName);
        QCOMPARE(receiver.videoQuality(), request.rollbackVideoQuality);
    }

    void fakeBatchRecoveryFailureRetainsDistinctError() {
        FakeAirPlayReceiver receiver;
        receiver.forceState(ReceiverState::Discoverable);
        receiver.requestedConfigurationRestartError = QStringLiteral("requested restart failed");
        receiver.rollbackConfigurationRestartError = QStringLiteral("rollback restart failed");

        ReceiverConfigurationBatchRequest request;
        request.receiverNameChanged = true;
        request.requestedReceiverName = QStringLiteral("Updated Receiver");
        request.rollbackReceiverName = receiver.receiverName();
        request.requestedVideoQuality = receiver.videoQuality();
        request.rollbackVideoQuality = receiver.videoQuality();

        const auto result = receiver.applyConfigurationBatch(request);

        QCOMPARE(result.status, ReceiverConfigurationBatchStatus::RecoveryFailed);
        QCOMPARE(result.applyError, QStringLiteral("requested restart failed"));
        QCOMPARE(result.recoveryError, QStringLiteral("rollback restart failed"));
        QCOMPARE(receiver.receiverName(), request.rollbackReceiverName);
        QCOMPARE(receiver.configurationRestartCount, 2);
    }

    void fakeIdlePartialBatchMergesQualityAndResult() {
        FakeAirPlayReceiver receiver;
        const VideoQualitySettings current{VideoResolution::P1080, VideoFrameRate::Fps30};
        QCOMPARE(receiver.videoQuality(), current);

        ReceiverConfigurationBatchRequest request;
        request.resolutionChanged = true;
        request.requestedReceiverName = receiver.receiverName();
        request.rollbackReceiverName = receiver.receiverName();
        request.requestedVideoQuality = {VideoResolution::P720, VideoFrameRate::Fps60};
        request.rollbackVideoQuality = current;

        const auto result = receiver.applyConfigurationBatch(request);

        const VideoQualitySettings expected{VideoResolution::P720, VideoFrameRate::Fps30};
        QCOMPARE(result.status, ReceiverConfigurationBatchStatus::Applied);
        QCOMPARE(receiver.videoQuality(), expected);
        QCOMPARE(result.knownRuntimeVideoQuality, receiver.videoQuality());
        QCOMPARE(receiver.configurationRestartCount, 0);
    }

    void fakeStartsAndStopsRecording() {
        FakeAirPlayReceiver receiver;
        QSignalSpy availabilitySpy(&receiver, &AirPlayReceiver::recordingAvailabilityChanged);
        QSignalSpy stateSpy(&receiver, &AirPlayReceiver::recordingStateChanged);
        QSignalSpy finishedSpy(&receiver, &AirPlayReceiver::recordingFinished);

        QVERIFY(!receiver.recordingAvailable());
        QCOMPARE(receiver.recordingState(), RecordingState::Idle);

        receiver.setRecordingAvailableForTest(true);

        QVERIFY(receiver.recordingAvailable());
        QCOMPARE(availabilitySpy.count(), 1);

        const RecordingOptions options{QStringLiteral("recordings"), RecordingFormat::Mp4};
        const RecordingStartResult startResult = receiver.startRecording(options);

        QVERIFY(startResult.accepted);
        QCOMPARE(receiver.startRecordingCount, 1);
        QCOMPARE(receiver.lastRecordingOptions.outputDirectory, options.outputDirectory);
        QCOMPARE(receiver.lastRecordingOptions.format, options.format);
        QCOMPARE(receiver.recordingState(), RecordingState::Recording);
        QCOMPARE(stateSpy.count(), 1);

        const RecordingStartResult duplicateStartResult = receiver.startRecording(options);

        QVERIFY(!duplicateStartResult.accepted);
        QCOMPARE(receiver.startRecordingCount, 1);

        receiver.stopRecording();

        QCOMPARE(receiver.stopRecordingCount, 1);
        QCOMPARE(receiver.recordingState(), RecordingState::Finalizing);
        QCOMPARE(stateSpy.count(), 2);

        receiver.stopRecording();

        QCOMPARE(receiver.stopRecordingCount, 1);
        QCOMPARE(receiver.recordingState(), RecordingState::Finalizing);

        receiver.completeRecordingForTest({QStringLiteral("saved.mp4"), {}});

        QCOMPARE(receiver.recordingState(), RecordingState::Idle);
        QCOMPARE(stateSpy.count(), 3);
        QCOMPARE(finishedSpy.count(), 1);
        const RecordingResult result = qvariant_cast<RecordingResult>(finishedSpy.takeFirst().at(0));
        QCOMPARE(result.finalPath, QStringLiteral("saved.mp4"));
    }

    void fakeRejectsStartWithoutAvailability() {
        FakeAirPlayReceiver receiver;

        const RecordingStartResult result = receiver.startRecording({QStringLiteral("recordings")});

        QVERIFY(!result.accepted);
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(receiver.startRecordingCount, 0);
        QCOMPARE(receiver.recordingState(), RecordingState::Idle);
    }

    void fakeFinalizingIgnoresDuplicateStop() {
        FakeAirPlayReceiver receiver;
        receiver.setRecordingAvailableForTest(true);
        QVERIFY(receiver.startRecording({QStringLiteral("recordings")}).accepted);

        receiver.stopRecording();
        receiver.stopRecording();

        QCOMPARE(receiver.stopRecordingCount, 1);
        QCOMPARE(receiver.recordingState(), RecordingState::Finalizing);
    }

    void fakeDiscardReturnsIdleWithoutFinishedSignal() {
        FakeAirPlayReceiver receiver;
        QSignalSpy finishedSpy(&receiver, &AirPlayReceiver::recordingFinished);
        receiver.setRecordingAvailableForTest(true);
        QVERIFY(receiver.startRecording({QStringLiteral("recordings")}).accepted);

        receiver.discardRecording();

        QCOMPARE(receiver.discardRecordingCount, 1);
        QCOMPARE(receiver.recordingState(), RecordingState::Idle);
        QCOMPARE(finishedSpy.count(), 0);

        receiver.discardRecording();

        QCOMPARE(receiver.discardRecordingCount, 1);
        QCOMPARE(receiver.recordingState(), RecordingState::Idle);
        QCOMPARE(finishedSpy.count(), 0);
    }

    void fakeFailureFromRecordingReturnsIdleBeforeFailureSignal() {
        FakeAirPlayReceiver receiver;
        QSignalSpy stateSpy(&receiver, &AirPlayReceiver::recordingStateChanged);
        QSignalSpy failedSpy(&receiver, &AirPlayReceiver::recordingFailed);
        QStringList events;
        connect(&receiver, &AirPlayReceiver::recordingStateChanged, &receiver,
                [&](RecordingState) { events.append(QStringLiteral("state")); });
        connect(&receiver, &AirPlayReceiver::recordingFailed, &receiver,
                [&](const QString &) { events.append(QStringLiteral("failed")); });
        receiver.setRecordingAvailableForTest(true);
        QVERIFY(receiver.startRecording({QStringLiteral("recordings")}).accepted);
        stateSpy.clear();
        events.clear();

        receiver.failRecordingForTest(QStringLiteral("capture failed"));

        QCOMPARE(receiver.recordingState(), RecordingState::Idle);
        QCOMPARE(stateSpy.count(), 1);
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(failedSpy.at(0).at(0).toString(), QStringLiteral("capture failed"));
        const QStringList expectedEvents{QStringLiteral("state"), QStringLiteral("failed")};
        QCOMPARE(events, expectedEvents);

        receiver.failRecordingForTest(QStringLiteral("ignored"));

        QCOMPARE(receiver.recordingState(), RecordingState::Idle);
        QCOMPARE(stateSpy.count(), 1);
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(events, expectedEvents);
    }

    void fakeFailureFromFinalizingReturnsIdleBeforeFailureSignal() {
        FakeAirPlayReceiver receiver;
        QSignalSpy stateSpy(&receiver, &AirPlayReceiver::recordingStateChanged);
        QSignalSpy failedSpy(&receiver, &AirPlayReceiver::recordingFailed);
        QStringList events;
        connect(&receiver, &AirPlayReceiver::recordingStateChanged, &receiver,
                [&](RecordingState) { events.append(QStringLiteral("state")); });
        connect(&receiver, &AirPlayReceiver::recordingFailed, &receiver,
                [&](const QString &) { events.append(QStringLiteral("failed")); });
        receiver.setRecordingAvailableForTest(true);
        QVERIFY(receiver.startRecording({QStringLiteral("recordings")}).accepted);
        receiver.stopRecording();
        stateSpy.clear();
        events.clear();

        receiver.failRecordingForTest(QStringLiteral("finalize failed"));

        QCOMPARE(receiver.recordingState(), RecordingState::Idle);
        QCOMPARE(stateSpy.count(), 1);
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(failedSpy.at(0).at(0).toString(), QStringLiteral("finalize failed"));
        const QStringList expectedEvents{QStringLiteral("state"), QStringLiteral("failed")};
        QCOMPARE(events, expectedEvents);

        receiver.failRecordingForTest(QStringLiteral("ignored"));

        QCOMPARE(receiver.recordingState(), RecordingState::Idle);
        QCOMPARE(stateSpy.count(), 1);
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(events, expectedEvents);
    }

    void fakeCompletionIsIgnoredUntilFinalizing() {
        FakeAirPlayReceiver receiver;
        QSignalSpy stateSpy(&receiver, &AirPlayReceiver::recordingStateChanged);
        QSignalSpy finishedSpy(&receiver, &AirPlayReceiver::recordingFinished);

        receiver.completeRecordingForTest({QStringLiteral("idle.mp4"), {}});

        QCOMPARE(receiver.recordingState(), RecordingState::Idle);
        QCOMPARE(stateSpy.count(), 0);
        QCOMPARE(finishedSpy.count(), 0);

        receiver.setRecordingAvailableForTest(true);
        QVERIFY(receiver.startRecording({QStringLiteral("recordings")}).accepted);
        stateSpy.clear();

        receiver.completeRecordingForTest({QStringLiteral("recording.mp4"), {}});

        QCOMPARE(receiver.recordingState(), RecordingState::Recording);
        QCOMPARE(stateSpy.count(), 0);
        QCOMPARE(finishedSpy.count(), 0);
    }

    void fakeDiscardFromFinalizingReturnsIdleWithoutFinishedSignal() {
        FakeAirPlayReceiver receiver;
        QSignalSpy finishedSpy(&receiver, &AirPlayReceiver::recordingFinished);
        receiver.setRecordingAvailableForTest(true);
        QVERIFY(receiver.startRecording({QStringLiteral("recordings")}).accepted);
        receiver.stopRecording();
        QCOMPARE(receiver.recordingState(), RecordingState::Finalizing);

        receiver.discardRecording();

        QCOMPARE(receiver.discardRecordingCount, 1);
        QCOMPARE(receiver.recordingState(), RecordingState::Idle);
        QCOMPARE(finishedSpy.count(), 0);

        receiver.discardRecording();

        QCOMPARE(receiver.discardRecordingCount, 1);
        QCOMPARE(receiver.recordingState(), RecordingState::Idle);
        QCOMPARE(finishedSpy.count(), 0);
    }
};

QTEST_MAIN(AirPlayReceiverTest)
#include "AirPlayReceiverTest.moc"
