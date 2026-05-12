#include <QtTest/QtTest>
#include "backend/ReceiverStatePolicy.h"

class ReceiverStatePolicyTest : public QObject {
    Q_OBJECT

private slots:
    void receiverNameUnchangedNoop() {
        const auto result = decideReceiverNameChange(
            ReceiverState::Discoverable, QStringLiteral("MyReceiver"),
            QStringLiteral("MyReceiver"));
        QCOMPARE(result.action, ReceiverPolicyAction::Noop);
    }

    void receiverNameRejectedWhenEmptyAfterTrim() {
        {
            const auto result = decideReceiverNameChange(
                ReceiverState::Idle, QStringLiteral("Old"), QStringLiteral(""));
            QCOMPARE(result.action, ReceiverPolicyAction::Reject);
        }
        {
            const auto result = decideReceiverNameChange(
                ReceiverState::Idle, QStringLiteral("Old"), QStringLiteral("   "));
            QCOMPARE(result.action, ReceiverPolicyAction::Reject);
        }
    }

    void receiverNameInIdleStoresWithoutRestart() {
        const auto result = decideReceiverNameChange(
            ReceiverState::Idle, QStringLiteral("OldName"),
            QStringLiteral("NewName"));
        QCOMPARE(result.action, ReceiverPolicyAction::StoreOnly);
        QCOMPARE(result.normalizedName, QStringLiteral("NewName"));
    }

    void receiverNameInDiscoverableRequiresDiscoveryRestartWithRollback() {
        const auto result = decideReceiverNameChange(
            ReceiverState::Discoverable, QStringLiteral("OldName"),
            QStringLiteral("NewName"));
        QCOMPARE(result.action, ReceiverPolicyAction::RestartDiscovery);
        QCOMPARE(result.normalizedName, QStringLiteral("NewName"));
        QCOMPARE(result.rollbackName, QStringLiteral("OldName"));
    }

    void receiverNameInConnectedRequiresDiscoveryRestartConnected() {
        {
            const auto result = decideReceiverNameChange(
                ReceiverState::Connecting, QStringLiteral("Old"),
                QStringLiteral("New"));
            QCOMPARE(result.action, ReceiverPolicyAction::RestartDiscoveryConnected);
            QCOMPARE(result.rollbackName, QStringLiteral("Old"));
        }
        {
            const auto result = decideReceiverNameChange(
                ReceiverState::Connected, QStringLiteral("Old"),
                QStringLiteral("New"));
            QCOMPARE(result.action, ReceiverPolicyAction::RestartDiscoveryConnected);
            QCOMPARE(result.rollbackName, QStringLiteral("Old"));
        }
    }

    void receiverNameInErrorOrStartingRestartsDiscovery() {
        {
            const auto result = decideReceiverNameChange(
                ReceiverState::Error, QStringLiteral("Old"),
                QStringLiteral("New"));
            QCOMPARE(result.action, ReceiverPolicyAction::RestartDiscovery);
        }
        {
            const auto result = decideReceiverNameChange(
                ReceiverState::Starting, QStringLiteral("Old"),
                QStringLiteral("New"));
            QCOMPARE(result.action, ReceiverPolicyAction::RestartDiscovery);
        }
    }

    void receiverNameTrimsBeforeChecking() {
        const auto result = decideReceiverNameChange(
            ReceiverState::Idle, QStringLiteral("Old"),
            QStringLiteral("  Trimmed  "));
        QCOMPARE(result.action, ReceiverPolicyAction::StoreOnly);
        QCOMPARE(result.normalizedName, QStringLiteral("Trimmed"));
    }

    void videoQualityUnchangedNoop() {
        const VideoQualitySettings current{VideoResolution::P1080, VideoFrameRate::Fps30};
        const VideoQualitySettings requested{VideoResolution::P1080, VideoFrameRate::Fps30};
        const auto result = decideVideoQualityChange(
            ReceiverState::Discoverable, current, requested);
        QCOMPARE(result.action, ReceiverPolicyAction::Noop);
    }

    void videoQualityInIdleStoresOnly() {
        const VideoQualitySettings current{VideoResolution::P1080, VideoFrameRate::Fps30};
        const VideoQualitySettings requested{VideoResolution::P720, VideoFrameRate::Fps15};
        const auto result = decideVideoQualityChange(
            ReceiverState::Idle, current, requested);
        QCOMPARE(result.action, ReceiverPolicyAction::StoreOnly);
    }

    void videoQualityInErrorOrStartingRejects() {
        const VideoQualitySettings current{VideoResolution::P1080, VideoFrameRate::Fps30};
        const VideoQualitySettings requested{VideoResolution::P720, VideoFrameRate::Fps15};
        {
            const auto result = decideVideoQualityChange(
                ReceiverState::Error, current, requested);
            QCOMPARE(result.action, ReceiverPolicyAction::Reject);
        }
        {
            const auto result = decideVideoQualityChange(
                ReceiverState::Starting, current, requested);
            QCOMPARE(result.action, ReceiverPolicyAction::Reject);
        }
    }

    void videoQualityInDiscoverableRequiresDiscoveryRestart() {
        const VideoQualitySettings current{VideoResolution::P1080, VideoFrameRate::Fps30};
        const VideoQualitySettings requested{VideoResolution::P720, VideoFrameRate::Fps15};
        const auto result = decideVideoQualityChange(
            ReceiverState::Discoverable, current, requested);
        QCOMPARE(result.action, ReceiverPolicyAction::RestartDiscovery);
    }

    void videoQualityInConnectedOrConnectingRequiresReceiverRestart() {
        const VideoQualitySettings current{VideoResolution::P1080, VideoFrameRate::Fps30};
        const VideoQualitySettings requested{VideoResolution::P720, VideoFrameRate::Fps15};
        {
            const auto result = decideVideoQualityChange(
                ReceiverState::Connecting, current, requested);
            QCOMPARE(result.action, ReceiverPolicyAction::RestartReceiver);
        }
        {
            const auto result = decideVideoQualityChange(
                ReceiverState::Connected, current, requested);
            QCOMPARE(result.action, ReceiverPolicyAction::RestartReceiver);
        }
    }
};

QTEST_GUILESS_MAIN(ReceiverStatePolicyTest)
#include "ReceiverStatePolicyTest.moc"
