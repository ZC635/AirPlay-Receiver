#include <QtTest/QtTest>
#include <QTimer>

#include "backend/DiscoveryRestartController.h"

class DiscoveryRestartControllerTest : public QObject {
    Q_OBJECT

private:
    struct FakeOps {
        bool canRestartResult = true;
        bool canContinueResult = true;
        bool createResult = true;
        bool startHttpdResult = true;
        bool registerResult = true;

        int stopHttpdCount = 0;
        int unregisterCount = 0;
        int destroyCount = 0;
        int createCount = 0;
        int startHttpdCountOp = 0;
        int registerCount = 0;
        int restoreCount = 0;
        int failCount = 0;
        int canContinueCheckCount = 0;
        QString lastFailMessage;
        QString lastRecoveryName;

        DiscoveryRestartOperations build() {
            DiscoveryRestartOperations ops;
            ops.canRestart = [this] { return canRestartResult; };
            ops.canContinue = [this] { ++canContinueCheckCount; return canContinueResult; };
            ops.stopHttpdIfStarted = [this] { ++stopHttpdCount; };
            ops.unregisterBroadcast = [this] { ++unregisterCount; };
            ops.destroyBroadcast = [this] { ++destroyCount; };
            ops.createBroadcast = [this, createResult = createResult] () mutable {
                ++createCount;
                return createResult;
            };
            ops.startHttpdIfNeeded = [this, startHttpdResult = startHttpdResult] () mutable {
                ++startHttpdCountOp;
                return startHttpdResult;
            };
            ops.registerBroadcast = [this, registerResult = registerResult] () mutable {
                ++registerCount;
                return registerResult;
            };
            ops.restoreReceiverName = [this](QString name) {
                ++restoreCount;
                lastRecoveryName = name;
            };
            ops.fail = [this](QString msg) {
                ++failCount;
                lastFailMessage = msg;
            };
            return ops;
        }
    };

private slots:
    void scheduleSendsGoodbyeAndStartsDelay() {
        DiscoveryRestartController controller(0);
        FakeOps fake;
        auto ops = fake.build();

        QVERIFY(controller.schedule("MyOldName", true, std::move(ops)));

        QCOMPARE(fake.stopHttpdCount, 1);
        QCOMPARE(fake.unregisterCount, 1);
        QVERIFY(controller.pending());
        QCOMPARE(controller.recoveryName(), QString("MyOldName"));
    }

    void secondScheduleWhilePendingCoalesces() {
        DiscoveryRestartController controller(0);
        FakeOps fake1;
        QVERIFY(controller.schedule("FirstRecovery", true, fake1.build()));

        FakeOps fake2;
        QVERIFY(controller.schedule("SecondRecovery", false, fake2.build()));

        QCOMPARE(fake2.stopHttpdCount, 0);
        QCOMPARE(fake2.unregisterCount, 0);
        QCOMPARE(controller.recoveryName(), QString("FirstRecovery"));
    }

    void timeoutDestroysCreatesStartsHttpdRegisters() {
        DiscoveryRestartController controller(0);
        FakeOps fake;
        auto ops = fake.build();
        QVERIFY(controller.schedule(QString(), true, std::move(ops)));
        QVERIFY(controller.pending());

        QTest::qWait(10);

        QCOMPARE(fake.destroyCount, 1);
        QCOMPARE(fake.canContinueCheckCount, 1);
        QCOMPARE(fake.createCount, 1);
        QCOMPARE(fake.startHttpdCountOp, 1);
        QCOMPARE(fake.registerCount, 1);
        QVERIFY(!controller.pending());
        QVERIFY(controller.recoveryName().isEmpty());
    }

    void timeoutDestroysCreatesRegistersWithoutHttpd() {
        DiscoveryRestartController controller(0);
        FakeOps fake;
        auto ops = fake.build();
        QVERIFY(controller.schedule(QString(), false, std::move(ops)));
        QVERIFY(controller.pending());

        QTest::qWait(10);

        QCOMPARE(fake.destroyCount, 1);
        QCOMPARE(fake.createCount, 1);
        QCOMPARE(fake.startHttpdCountOp, 0);
        QCOMPARE(fake.registerCount, 1);
        QVERIFY(!controller.pending());
    }

    void createFailureUsesRecoveryNameWhenAvailable() {
        DiscoveryRestartController controller(0);
        FakeOps fake;
        fake.createResult = false;
        auto ops = fake.build();
        QVERIFY(controller.schedule("RecoveryName", true, std::move(ops)));

        QTest::qWait(10);

        QCOMPARE(fake.destroyCount, 1);
        QCOMPARE(fake.createCount, 2);
        QCOMPARE(fake.restoreCount, 1);
        QCOMPARE(fake.lastRecoveryName, QString("RecoveryName"));
        QCOMPARE(fake.failCount, 1);
        QVERIFY(!controller.pending());
    }

    void createFailureWithoutRecoveryCallsFail() {
        DiscoveryRestartController controller(0);
        FakeOps fake;
        fake.createResult = false;
        auto ops = fake.build();
        QVERIFY(controller.schedule(QString(), true, std::move(ops)));

        QTest::qWait(10);

        QCOMPARE(fake.destroyCount, 1);
        QCOMPARE(fake.createCount, 1);
        QCOMPARE(fake.restoreCount, 0);
        QCOMPARE(fake.failCount, 1);
        QVERIFY(!controller.pending());
    }

    void startHttpdFailureCleansUpAndCallsFail() {
        DiscoveryRestartController controller(0);
        FakeOps fake;
        fake.startHttpdResult = false;
        auto ops = fake.build();
        QVERIFY(controller.schedule(QString(), true, std::move(ops)));

        QTest::qWait(10);

        QCOMPARE(fake.destroyCount, 2);
        QCOMPARE(fake.stopHttpdCount, 2);
        QCOMPARE(fake.createCount, 1);
        QCOMPARE(fake.startHttpdCountOp, 1);
        QCOMPARE(fake.registerCount, 0);
        QCOMPARE(fake.failCount, 1);
        QVERIFY(!controller.pending());
    }

    void registerFailureWithoutRecoveryCleansUpAndCallsFail() {
        DiscoveryRestartController controller(0);
        FakeOps fake;
        fake.registerResult = false;
        auto ops = fake.build();
        QVERIFY(controller.schedule(QString(), true, std::move(ops)));

        QTest::qWait(10);

        QCOMPARE(fake.destroyCount, 2);
        QCOMPARE(fake.stopHttpdCount, 2);
        QCOMPARE(fake.createCount, 1);
        QCOMPARE(fake.registerCount, 1);
        QCOMPARE(fake.restoreCount, 0);
        QCOMPARE(fake.failCount, 1);
        QVERIFY(!controller.pending());
    }

    void registerFailureUsesRecoveryNameWhenAvailable() {
        DiscoveryRestartController controller(0);
        FakeOps fake;
        int registerAttempts = 0;
        auto ops = fake.build();
        ops.registerBroadcast = [&] {
            ++fake.registerCount;
            return ++registerAttempts > 1;
        };
        QVERIFY(controller.schedule("RollbackName", true, std::move(ops)));

        QTest::qWait(10);

        QCOMPARE(fake.destroyCount, 2);
        QCOMPARE(fake.stopHttpdCount, 2);
        QCOMPARE(fake.createCount, 2);
        QCOMPARE(fake.restoreCount, 1);
        QCOMPARE(fake.lastRecoveryName, QString("RollbackName"));
        QCOMPARE(fake.startHttpdCountOp, 2);
        QCOMPARE(fake.registerCount, 2);
        QVERIFY(!controller.pending());
        QVERIFY(controller.recoveryName().isEmpty());
    }

    void registerFailureRecoveryStartHttpdFailsCleansUpAndCallsFail() {
        DiscoveryRestartController controller(0);
        FakeOps fake;
        int startHttpdAttempts = 0;
        fake.registerResult = false;
        auto ops = fake.build();
        ops.startHttpdIfNeeded = [&] {
            ++fake.startHttpdCountOp;
            ++startHttpdAttempts;
            return startHttpdAttempts == 1;
        };
        QVERIFY(controller.schedule("RollbackName", true, std::move(ops)));

        QTest::qWait(10);

        QCOMPARE(fake.destroyCount, 3);
        QCOMPARE(fake.stopHttpdCount, 2);
        QCOMPARE(fake.createCount, 2);
        QCOMPARE(fake.restoreCount, 1);
        QCOMPARE(fake.startHttpdCountOp, 2);
        QCOMPARE(fake.registerCount, 1);
        QCOMPARE(fake.failCount, 1);
        QVERIFY(controller.recoveryName() == "RollbackName");
        QVERIFY(!controller.pending());
    }

    void registerFailureRecoveryRegisterFailsCleansUpAndCallsFail() {
        DiscoveryRestartController controller(0);
        FakeOps fake;
        int registerAttempts = 0;
        auto ops = fake.build();
        ops.registerBroadcast = [&] {
            ++fake.registerCount;
            ++registerAttempts;
            return false;
        };
        QVERIFY(controller.schedule("RollbackName", true, std::move(ops)));

        QTest::qWait(10);

        QCOMPARE(fake.destroyCount, 3);
        QCOMPARE(fake.stopHttpdCount, 3);
        QCOMPARE(fake.createCount, 2);
        QCOMPARE(fake.restoreCount, 1);
        QCOMPARE(fake.startHttpdCountOp, 2);
        QCOMPARE(fake.registerCount, 2);
        QCOMPARE(fake.failCount, 1);
        QVERIFY(controller.recoveryName() == "RollbackName");
        QVERIFY(!controller.pending());
    }

    void cancelClearsPendingAndRecoveryName() {
        DiscoveryRestartController controller(0);
        FakeOps fake;
        QVERIFY(controller.schedule("CancelTest", true, fake.build()));
        QVERIFY(controller.pending());
        QCOMPARE(controller.recoveryName(), QString("CancelTest"));

        controller.cancel();

        QVERIFY(!controller.pending());
        QVERIFY(controller.recoveryName().isEmpty());
    }

    void cancelStopsTimer() {
        DiscoveryRestartController controller(100);
        FakeOps fake;
        auto ops = fake.build();
        QVERIFY(controller.schedule(QString(), true, std::move(ops)));
        QVERIFY(controller.pending());

        controller.cancel();

        QCOMPARE(fake.destroyCount, 0);
        QCOMPARE(fake.createCount, 0);
        QVERIFY(!controller.pending());
    }

    void createFailureRecoverySuccessThenRegisters() {
        DiscoveryRestartController controller(0);
        FakeOps fake;
        int createAttempts = 0;
        auto ops = fake.build();
        ops.createBroadcast = [&] {
            ++fake.createCount;
            return ++createAttempts > 1;
        };
        QVERIFY(controller.schedule("SavedName", true, std::move(ops)));

        QTest::qWait(10);

        QCOMPARE(fake.destroyCount, 1);
        QCOMPARE(fake.createCount, 2);
        QCOMPARE(fake.restoreCount, 1);
        QCOMPARE(fake.lastRecoveryName, QString("SavedName"));
        QCOMPARE(fake.startHttpdCountOp, 1);
        QCOMPARE(fake.registerCount, 1);
        QCOMPARE(fake.failCount, 0);
        QVERIFY(!controller.pending());
    }

    void canRestartFailureCallsStopHttpdAndFail() {
        DiscoveryRestartController controller(0);
        FakeOps fake;
        fake.canRestartResult = false;
        auto ops = fake.build();

        QVERIFY(!controller.schedule(QString(), true, std::move(ops)));

        QCOMPARE(fake.stopHttpdCount, 1);
        QCOMPARE(fake.failCount, 1);
        QVERIFY(!controller.pending());
        QVERIFY(!fake.lastFailMessage.isEmpty());
    }

    void canRestartFailureDoesNotUnregister() {
        DiscoveryRestartController controller(0);
        FakeOps fake;
        fake.canRestartResult = false;
        auto ops = fake.build();

        QVERIFY(!controller.schedule(QString(), true, std::move(ops)));

        QCOMPARE(fake.unregisterCount, 0);
        QCOMPARE(fake.destroyCount, 0);
    }

    void initiallyNotPending() {
        DiscoveryRestartController controller(0);
        QVERIFY(!controller.pending());
        QVERIFY(controller.recoveryName().isEmpty());
    }

    void canContinueFalseExitsWithoutOperations() {
        DiscoveryRestartController controller(0);
        FakeOps fake;
        fake.canContinueResult = false;
        auto ops = fake.build();
        QVERIFY(controller.schedule(QString(), true, std::move(ops)));
        QVERIFY(controller.pending());

        QTest::qWait(10);

        QCOMPARE(fake.canContinueCheckCount, 1);
        QCOMPARE(fake.destroyCount, 0);
        QCOMPARE(fake.createCount, 0);
        QCOMPARE(fake.failCount, 0);
        QVERIFY(!controller.pending());
    }

    void canContinueTrueAllowsNormalTimeout() {
        DiscoveryRestartController controller(0);
        FakeOps fake;
        auto ops = fake.build();
        QVERIFY(controller.schedule(QString(), true, std::move(ops)));

        QTest::qWait(10);

        QCOMPARE(fake.canContinueCheckCount, 1);
        QCOMPARE(fake.destroyCount, 1);
        QCOMPARE(fake.registerCount, 1);
        QVERIFY(!controller.pending());
    }
};

QTEST_GUILESS_MAIN(DiscoveryRestartControllerTest)
#include "DiscoveryRestartControllerTest.moc"
