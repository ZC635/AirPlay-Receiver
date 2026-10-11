#include <QtTest/QtTest>
#include <QSemaphore>
#include <QProcess>
#include <QProcessEnvironment>
#include <cstdio>
#include <atomic>
#include <memory>
#include <thread>
#include <utility>

#include "backend/GstAppSinkFrameSource.h"

class ScopedThread {
public:
    explicit ScopedThread(std::thread thread) : m_thread(std::move(thread)) {}
    ~ScopedThread() { join(); }

    void join() {
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

private:
    std::thread m_thread;
};

class GstAppSinkFrameSourceTest : public QObject {
    Q_OBJECT

private:
    struct ChildResult {
        bool started = false;
        bool finished = false;
        bool reaped = false;
        bool blockedCallbackEntered = false;
        int exitCode = -1;
        QProcess::ExitStatus exitStatus = QProcess::CrashExit;
        QByteArray output;
    };

    static ChildResult runIsolated(const QString &test, bool blockCallback = false) {
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("AIRPLAY_CALLBACK_TEST_CHILD", test);
        if (blockCallback)
            environment.insert("AIRPLAY_CALLBACK_TEST_BLOCK", "1");
        else
            environment.remove("AIRPLAY_CALLBACK_TEST_BLOCK");
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {test});
        ChildResult result;
        result.started = child.waitForStarted(1000);
        if (result.started) {
            if (blockCallback) {
                QElapsedTimer deadline;
                deadline.start();
                while (deadline.elapsed() < 5000 && child.state() != QProcess::NotRunning) {
                    const auto remaining = 5000 - deadline.elapsed();
                    if (remaining <= 0) break;
                    child.waitForReadyRead(static_cast<int>(remaining));
                    result.output += child.readAll();
                    if (result.output.contains("CALLBACK_BLOCK_ENTERED")) {
                        result.blockedCallbackEntered = true;
                        break;
                    }
                }
            }
            // A genuine callback deadlock cannot be cancelled safely in-process.
            // Fail the isolated test process; never detach a stack-owning worker.
            result.finished = child.waitForFinished(blockCallback ? 1000 : 5000);
            if (!result.finished) {
                child.kill();
                result.reaped = child.waitForFinished(1000);
            } else {
                result.reaped = true;
            }
            result.exitCode = child.exitCode();
            result.exitStatus = child.exitStatus();
        }
        result.output += child.readAll();
        return result;
    }

    static bool isIsolatedChild() {
        return qEnvironmentVariable("AIRPLAY_CALLBACK_TEST_CHILD") ==
               QString::fromLatin1(QTest::currentTestFunction());
    }

private slots:
    void callbackTimeoutIsContainedAndNextRunSucceeds() {
        const QString scenario = QStringLiteral(
            "clearingGstSourceCallbackFromCallbackDoesNotDeadlock");
        const auto blocked = runIsolated(scenario, true);
        QVERIFY2(blocked.started, blocked.output.constData());
        QVERIFY2(blocked.blockedCallbackEntered, blocked.output.constData());
        QVERIFY2(!blocked.finished, "The deliberately blocked callback must time out");
        QVERIFY2(blocked.reaped, "The timed-out child must exit before another test starts");
        const auto healthy = runIsolated(scenario);
        QVERIFY2(healthy.started && healthy.finished && healthy.reaped,
                 healthy.output.constData());
        QCOMPARE(healthy.exitStatus, QProcess::NormalExit);
        QCOMPARE(healthy.exitCode, 0);
    }

    void initTestCase() {
        gst_init(nullptr, nullptr);
    }

    void gstSourceHoldsAppsinkReference() {
        GstElement *appsink = gst_element_factory_make("appsink", nullptr);
        QVERIFY(appsink != nullptr);

        const int initialRefCount = GST_OBJECT_REFCOUNT_VALUE(appsink);
        {
            GstAppSinkFrameSource source(appsink);
            QCOMPARE(GST_OBJECT_REFCOUNT_VALUE(appsink), initialRefCount + 1);
        }
        QCOMPARE(GST_OBJECT_REFCOUNT_VALUE(appsink), initialRefCount);

        gst_object_unref(appsink);
    }

    void clearingGstSourceCallbackWaitsForInFlightCallback() {
        GstElement *appsink = gst_element_factory_make("appsink", nullptr);
        QVERIFY(appsink != nullptr);

        GstAppSinkFrameSource source(appsink);
        QSemaphore callbackEntered;
        QSemaphore clearStarted;
        QSemaphore allowCallbackReturn;
        std::atomic_bool clearReturned = false;

        source.setFrameAvailableCallback([&] {
            callbackEntered.release();
            allowCallbackReturn.acquire();
        });
        source.start();

        ScopedThread emitter(std::thread([&] {
            GstFlowReturn result = GST_FLOW_OK;
            g_signal_emit_by_name(appsink, "new-sample", &result);
        }));
        const bool entered = callbackEntered.tryAcquire(1, 1000);

        ScopedThread clearer(std::thread([&] {
            clearStarted.release();
            source.setFrameAvailableCallback({});
            clearReturned.store(true);
        }));
        const bool clearReachedSetCallback = clearStarted.tryAcquire(1, 1000);

        QTest::qWait(50);
        const bool returnedBeforeCallbackFinished = clearReturned.load();

        allowCallbackReturn.release();
        clearer.join();
        emitter.join();
        QVERIFY(entered);
        QVERIFY(clearReachedSetCallback);
        QVERIFY(!returnedBeforeCallbackFinished);
        QVERIFY(clearReturned.load());

        gst_object_unref(appsink);
    }

    void clearingGstSourceCallbackFromCallbackDoesNotDeadlock() {
        if (!isIsolatedChild()) {
            const auto child = runIsolated(QString::fromLatin1(QTest::currentTestFunction()));
            QVERIFY2(child.started && child.finished && child.reaped, child.output.constData());
            QCOMPARE(child.exitStatus, QProcess::NormalExit);
            QCOMPARE(child.exitCode, 0);
            return;
        }
        GstElement *appsink = gst_element_factory_make("appsink", nullptr);
        QVERIFY(appsink != nullptr);

        auto source = std::make_unique<GstAppSinkFrameSource>(appsink);
        std::atomic_bool callbackReturned = false;

        source->setFrameAvailableCallback([&] {
            if (qEnvironmentVariableIsSet("AIRPLAY_CALLBACK_TEST_BLOCK")) {
                std::fputs("CALLBACK_BLOCK_ENTERED\n", stdout);
                std::fflush(stdout);
                QSemaphore neverReleased;
                neverReleased.acquire();
            }
            source->setFrameAvailableCallback({});
            callbackReturned.store(true);
        });
        source->start();

        std::thread emitter([&] {
            GstFlowReturn result = GST_FLOW_OK;
            g_signal_emit_by_name(appsink, "new-sample", &result);
        });

        for (int i = 0; i < 100 && !callbackReturned.load(); i++) {
            QTest::qWait(10);
        }

        const bool returnedInTime = callbackReturned.load();
        // On deadlock the parent's watchdog fails this child; all captured locals
        // remain alive here until it exits instead of leaking into the next test.
        emitter.join();
        QVERIFY(returnedInTime);
        source.reset();
        gst_object_unref(appsink);
    }

    void clearingGstSourceCallbackFromCallbackWaitsForOtherCallbacks() {
        GstElement *appsink = gst_element_factory_make("appsink", nullptr);
        QVERIFY(appsink != nullptr);

        GstAppSinkFrameSource source(appsink);
        QSemaphore blockedCallbackEntered;
        QSemaphore allowBlockedCallbackReturn;
        std::atomic_int callbackCount = 0;
        std::atomic_bool clearReturned = false;

        source.setFrameAvailableCallback([&] {
            const int callbackIndex = callbackCount.fetch_add(1);
            if (callbackIndex == 0) {
                blockedCallbackEntered.release();
                allowBlockedCallbackReturn.acquire();
                return;
            }

            blockedCallbackEntered.release();
            source.setFrameAvailableCallback({});
            clearReturned.store(true);
        });
        source.start();

        ScopedThread blockedEmitter(std::thread([&] {
            GstFlowReturn result = GST_FLOW_OK;
            g_signal_emit_by_name(appsink, "new-sample", &result);
        }));
        const bool blockedEntered = blockedCallbackEntered.tryAcquire(1, 1000);

        ScopedThread clearingEmitter(std::thread([&] {
            GstFlowReturn result = GST_FLOW_OK;
            g_signal_emit_by_name(appsink, "new-sample", &result);
        }));
        const bool clearingReachedSetCallback = blockedCallbackEntered.tryAcquire(1, 1000);

        QTest::qWait(50);
        const bool returnedBeforeOtherCallbackFinished = clearReturned.load();

        allowBlockedCallbackReturn.release();
        clearingEmitter.join();
        blockedEmitter.join();

        QVERIFY(blockedEntered);
        QVERIFY(clearingReachedSetCallback);
        QCOMPARE(callbackCount.load(), 2);
        QVERIFY(!returnedBeforeOtherCallbackFinished);
        QVERIFY(clearReturned.load());

        gst_object_unref(appsink);
    }

    void concurrentInCallbackClearsBothReturn() {
        if (!isIsolatedChild()) {
            const auto child = runIsolated(QString::fromLatin1(QTest::currentTestFunction()));
            QVERIFY2(child.started && child.finished && child.reaped, child.output.constData());
            QCOMPARE(child.exitStatus, QProcess::NormalExit);
            QCOMPARE(child.exitCode, 0);
            return;
        }
        GstElement *appsink = gst_element_factory_make("appsink", nullptr);
        QVERIFY(appsink != nullptr);

        auto source = std::make_unique<GstAppSinkFrameSource>(appsink);
        QSemaphore callbacksEntered;
        QSemaphore allowClear;
        QSemaphore clearReady;
        QSemaphore clearGo;
        std::atomic_int callbacksReturned = 0;

        source->setFrameAvailableCallback([&] {
            callbacksEntered.release();
            allowClear.acquire();
            clearReady.release();
            clearGo.acquire();
            source->setFrameAvailableCallback({});
            callbacksReturned.fetch_add(1);
        });
        source->start();

        std::thread firstEmitter([&] {
            GstFlowReturn result = GST_FLOW_OK;
            g_signal_emit_by_name(appsink, "new-sample", &result);
        });
        std::thread secondEmitter([&] {
            GstFlowReturn result = GST_FLOW_OK;
            g_signal_emit_by_name(appsink, "new-sample", &result);
        });

        const bool bothCallbacksEntered = callbacksEntered.tryAcquire(2, 1000);
        allowClear.release(2);
        const bool bothCallbacksReadyToClear = clearReady.tryAcquire(2, 1000);
        clearGo.release(2);

        for (int i = 0; i < 100 && callbacksReturned.load() != 2; i++) {
            QTest::qWait(10);
        }

        const bool returnedInTime = callbacksReturned.load() == 2;
        firstEmitter.join();
        secondEmitter.join();
        source.reset();
        QVERIFY(returnedInTime);
        QVERIFY(bothCallbacksEntered);
        QVERIFY(bothCallbacksReadyToClear);
        QCOMPARE(callbacksReturned.load(), 2);
        gst_object_unref(appsink);
    }
};

QTEST_GUILESS_MAIN(GstAppSinkFrameSourceTest)
#include "GstAppSinkFrameSourceTest.moc"
