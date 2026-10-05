#include <QtTest/QtTest>
#include <QSemaphore>
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

private slots:
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
        GstElement *appsink = gst_element_factory_make("appsink", nullptr);
        QVERIFY(appsink != nullptr);

        auto source = std::make_unique<GstAppSinkFrameSource>(appsink);
        std::atomic_bool callbackReturned = false;

        source->setFrameAvailableCallback([&] {
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

        if (!callbackReturned.load()) {
            emitter.detach();
            source.release();
            QFAIL("Clearing the callback from inside the active callback deadlocked");
        }

        emitter.join();
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

        if (callbacksReturned.load() != 2) {
            firstEmitter.detach();
            secondEmitter.detach();
            source.release();
            gst_object_unref(appsink);
            QFAIL("Concurrent in-callback clears deadlocked");
        }

        firstEmitter.join();
        secondEmitter.join();
        source.reset();
        QVERIFY(bothCallbacksEntered);
        QVERIFY(bothCallbacksReadyToClear);
        QCOMPARE(callbacksReturned.load(), 2);
        gst_object_unref(appsink);
    }
};

QTEST_GUILESS_MAIN(GstAppSinkFrameSourceTest)
#include "GstAppSinkFrameSourceTest.moc"
