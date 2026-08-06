#include <QtTest/QtTest>

#include "backend/ReceiverConfigurationChange.h"

class ReceiverConfigurationChangeTest : public QObject {
    Q_OBJECT

private slots:
    void simultaneousChangesStoreBothAndRestartOnce() {
        ReceiverConfigurationBatchRequest request;
        request.receiverNameChanged = true;
        request.resolutionChanged = true;
        request.frameRateChanged = true;
        request.requestedReceiverName = QStringLiteral("Requested");
        request.rollbackReceiverName = QStringLiteral("Current");
        request.requestedVideoQuality = {VideoResolution::P720, VideoFrameRate::Fps60};
        request.rollbackVideoQuality = {VideoResolution::P1080, VideoFrameRate::Fps30};

        QString storedName;
        VideoQualitySettings storedQuality;
        int restartCount = 0;
        ReceiverConfigurationBatchOperations operations;
        operations.storeReceiverName = [&](const QString &name) { storedName = name; };
        operations.storeVideoQuality = [&](const VideoQualitySettings &quality) { storedQuality = quality; };
        operations.restartWithRequestedConfiguration = [&] {
            ++restartCount;
            return ReceiverOperationResult{true, {}};
        };

        const auto result = applyReceiverConfigurationBatch(request, operations);

        QCOMPARE(result.status, ReceiverConfigurationBatchStatus::Applied);
        QCOMPARE(storedName, request.requestedReceiverName);
        QCOMPARE(storedQuality, request.requestedVideoQuality);
        QCOMPARE(restartCount, 1);
        QCOMPARE(result.knownRuntimeReceiverName, request.requestedReceiverName);
        QCOMPARE(result.knownRuntimeVideoQuality, request.requestedVideoQuality);
    }

    void frameRateOnlyPreservesRollbackResolution() {
        ReceiverConfigurationBatchRequest request;
        request.frameRateChanged = true;
        request.requestedReceiverName = QStringLiteral("Ignored requested name");
        request.rollbackReceiverName = QStringLiteral("Current");
        request.requestedVideoQuality = {VideoResolution::P720, VideoFrameRate::Fps60};
        request.rollbackVideoQuality = {VideoResolution::P1080, VideoFrameRate::Fps30};

        int nameStoreCount = 0;
        int qualityStoreCount = 0;
        int restartCount = 0;
        VideoQualitySettings storedQuality;
        ReceiverConfigurationBatchOperations operations;
        operations.storeReceiverName = [&](const QString &) { ++nameStoreCount; };
        operations.storeVideoQuality = [&](const VideoQualitySettings &quality) {
            ++qualityStoreCount;
            storedQuality = quality;
        };
        operations.restartWithRequestedConfiguration = [&] {
            ++restartCount;
            return ReceiverOperationResult{true, {}};
        };

        const auto result = applyReceiverConfigurationBatch(request, operations);

        QCOMPARE(result.status, ReceiverConfigurationBatchStatus::Applied);
        QCOMPARE(nameStoreCount, 0);
        QCOMPARE(qualityStoreCount, 1);
        QCOMPARE(storedQuality.resolution, VideoResolution::P1080);
        QCOMPARE(storedQuality.frameRate, VideoFrameRate::Fps60);
        QCOMPARE(restartCount, 1);
        QCOMPARE(result.knownRuntimeReceiverName, request.rollbackReceiverName);
    }

    void resolutionOnlyPreservesRollbackFrameRate() {
        ReceiverConfigurationBatchRequest request;
        request.resolutionChanged = true;
        request.requestedVideoQuality = {VideoResolution::P720, VideoFrameRate::Fps60};
        request.rollbackVideoQuality = {VideoResolution::P1080, VideoFrameRate::Fps30};

        VideoQualitySettings storedQuality;
        ReceiverConfigurationBatchOperations operations;
        operations.storeVideoQuality = [&](const VideoQualitySettings &quality) { storedQuality = quality; };
        operations.restartWithRequestedConfiguration = [] { return ReceiverOperationResult{true, {}}; };

        const auto result = applyReceiverConfigurationBatch(request, operations);

        QCOMPARE(result.status, ReceiverConfigurationBatchStatus::Applied);
        QCOMPARE(storedQuality.resolution, VideoResolution::P720);
        QCOMPARE(storedQuality.frameRate, VideoFrameRate::Fps30);
        QCOMPARE(result.knownRuntimeVideoQuality, storedQuality);
    }

    void requestedRestartFailureRestoresAllChangedValues() {
        ReceiverConfigurationBatchRequest request;
        request.receiverNameChanged = true;
        request.resolutionChanged = true;
        request.requestedReceiverName = QStringLiteral("Requested");
        request.rollbackReceiverName = QStringLiteral("Current");
        request.requestedVideoQuality = {VideoResolution::P720, VideoFrameRate::Fps30};
        request.rollbackVideoQuality = {VideoResolution::P1080, VideoFrameRate::Fps30};

        int requestedNameStores = 0;
        int rollbackNameStores = 0;
        int requestedQualityStores = 0;
        int rollbackQualityStores = 0;
        int requestedRestartCount = 0;
        int rollbackRestartCount = 0;
        ReceiverConfigurationBatchOperations operations;
        operations.storeReceiverName = [&](const QString &name) {
            name == request.requestedReceiverName ? ++requestedNameStores : ++rollbackNameStores;
        };
        operations.storeVideoQuality = [&](const VideoQualitySettings &quality) {
            quality == request.requestedVideoQuality ? ++requestedQualityStores : ++rollbackQualityStores;
        };
        operations.restartWithRequestedConfiguration = [&] {
            ++requestedRestartCount;
            return ReceiverOperationResult{false, QStringLiteral("requested restart failed")};
        };
        operations.restartWithRollbackConfiguration = [&] {
            ++rollbackRestartCount;
            return ReceiverOperationResult{true, {}};
        };

        const auto result = applyReceiverConfigurationBatch(request, operations);

        QCOMPARE(result.status, ReceiverConfigurationBatchStatus::ApplyFailedRolledBack);
        QCOMPARE(result.applyError, QStringLiteral("requested restart failed"));
        QCOMPARE(requestedNameStores, 1);
        QCOMPARE(rollbackNameStores, 1);
        QCOMPARE(requestedQualityStores, 1);
        QCOMPARE(rollbackQualityStores, 1);
        QCOMPARE(requestedRestartCount, 1);
        QCOMPARE(rollbackRestartCount, 1);
        QCOMPARE(result.knownRuntimeReceiverName, request.rollbackReceiverName);
        QCOMPARE(result.knownRuntimeVideoQuality, request.rollbackVideoQuality);
    }

    void restorationFailureKeepsErrorsDistinct() {
        ReceiverConfigurationBatchRequest request;
        request.receiverNameChanged = true;
        request.requestedReceiverName = QStringLiteral("Requested");
        request.rollbackReceiverName = QStringLiteral("Current");
        request.requestedVideoQuality = {VideoResolution::P720, VideoFrameRate::Fps60};
        request.rollbackVideoQuality = {VideoResolution::P1080, VideoFrameRate::Fps30};

        ReceiverConfigurationBatchOperations operations;
        operations.storeReceiverName = [](const QString &) {};
        operations.restartWithRequestedConfiguration = [] {
            return ReceiverOperationResult{false, QStringLiteral("apply failure")};
        };
        operations.restartWithRollbackConfiguration = [] {
            return ReceiverOperationResult{false, QStringLiteral("recovery failure")};
        };

        const auto result = applyReceiverConfigurationBatch(request, operations);

        QCOMPARE(result.status, ReceiverConfigurationBatchStatus::RecoveryFailed);
        QCOMPARE(result.applyError, QStringLiteral("apply failure"));
        QCOMPARE(result.recoveryError, QStringLiteral("recovery failure"));
        QCOMPARE(result.knownRuntimeReceiverName, request.rollbackReceiverName);
        QCOMPARE(result.knownRuntimeVideoQuality, request.rollbackVideoQuality);
    }

    void noChangesInvokesNoCallbacks() {
        ReceiverConfigurationBatchRequest request;
        request.requestedReceiverName = QStringLiteral("Requested");
        request.rollbackReceiverName = QStringLiteral("Current");
        request.requestedVideoQuality = {VideoResolution::P720, VideoFrameRate::Fps60};
        request.rollbackVideoQuality = {VideoResolution::P1080, VideoFrameRate::Fps30};

        int callbackCount = 0;
        ReceiverConfigurationBatchOperations operations;
        operations.storeReceiverName = [&](const QString &) { ++callbackCount; };
        operations.storeVideoQuality = [&](const VideoQualitySettings &) { ++callbackCount; };
        operations.restartWithRequestedConfiguration = [&] { ++callbackCount; return ReceiverOperationResult{true, {}}; };
        operations.restartWithRollbackConfiguration = [&] { ++callbackCount; return ReceiverOperationResult{true, {}}; };

        const auto result = applyReceiverConfigurationBatch(request, operations);

        QCOMPARE(result.status, ReceiverConfigurationBatchStatus::Applied);
        QCOMPARE(callbackCount, 0);
        QCOMPARE(result.knownRuntimeReceiverName, request.rollbackReceiverName);
        QCOMPARE(result.knownRuntimeVideoQuality, request.rollbackVideoQuality);
    }

    void unchangedFieldsRemainAtRollbackSnapshot() {
        ReceiverConfigurationBatchRequest request;
        request.resolutionChanged = true;
        request.requestedReceiverName = QStringLiteral("Incorrect requested name");
        request.rollbackReceiverName = QStringLiteral("Current");
        request.requestedVideoQuality = {VideoResolution::P720, VideoFrameRate::Fps60};
        request.rollbackVideoQuality = {VideoResolution::P1080, VideoFrameRate::Fps30};

        ReceiverConfigurationBatchOperations operations;
        operations.storeVideoQuality = [](const VideoQualitySettings &) {};
        operations.restartWithRequestedConfiguration = [] { return ReceiverOperationResult{true, {}}; };

        const auto result = applyReceiverConfigurationBatch(request, operations);

        QCOMPARE(result.status, ReceiverConfigurationBatchStatus::Applied);
        QCOMPARE(result.knownRuntimeReceiverName, request.rollbackReceiverName);
        QCOMPARE(result.knownRuntimeVideoQuality.resolution, VideoResolution::P720);
        QCOMPARE(result.knownRuntimeVideoQuality.frameRate, VideoFrameRate::Fps30);
    }

    void missingRequestedRestartIsStructuredFailure() {
        ReceiverConfigurationBatchRequest request;
        request.receiverNameChanged = true;
        request.requestedReceiverName = QStringLiteral("Requested");
        request.rollbackReceiverName = QStringLiteral("Current");

        ReceiverConfigurationBatchOperations operations;
        operations.storeReceiverName = [](const QString &) {};

        const auto result = applyReceiverConfigurationBatch(request, operations);

        QCOMPARE(result.status, ReceiverConfigurationBatchStatus::ApplyFailedRolledBack);
        QCOMPARE(result.applyError, QStringLiteral("Missing requested configuration restart operation"));
        QCOMPARE(result.knownRuntimeReceiverName, request.rollbackReceiverName);
    }
};

QTEST_GUILESS_MAIN(ReceiverConfigurationChangeTest)
#include "ReceiverConfigurationChangeTest.moc"
