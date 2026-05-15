#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QImage>
#include <QSemaphore>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <cstring>

#include "backend/VideoFrameBridge.h"
#include "backend/GstAppSinkFrameSource.h"

class FakeAppSinkFrameSource : public AppSinkFrameSource {
public:
    void setNextSample(VideoFrameSample sample) { m_nextSample = std::move(sample); }
    void setFrameAvailableCallback(std::function<void()> callback) override {
        m_frameAvailableCallback = std::move(callback);
    }
    std::optional<VideoFrameSample> pullSample() override {
        if (!m_nextSample) return std::nullopt;
        auto s = std::move(*m_nextSample);
        m_nextSample.reset();
        return s;
    }
    void start() override { m_startCount++; }
    int startCount() const { return m_startCount; }
    void emitFrameAvailable() {
        if (m_frameAvailableCallback) {
            m_frameAvailableCallback();
        }
    }

private:
    std::optional<VideoFrameSample> m_nextSample;
    std::function<void()> m_frameAvailableCallback;
    int m_startCount = 0;
};

static QByteArray makeRGBA(int width, int height, int stride) {
    QByteArray data(stride * height, Qt::Uninitialized);
    auto *pixels = reinterpret_cast<uint8_t *>(data.data());
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int offset = y * stride + x * 4;
            pixels[offset + 0] = static_cast<uint8_t>(x * 25);
            pixels[offset + 1] = static_cast<uint8_t>(y * 60);
            pixels[offset + 2] = 128;
            pixels[offset + 3] = 255;
        }
    }
    return data;
}

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

class VideoFrameBridgeTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        gst_init(nullptr, nullptr);
    }

    void emitsCopiedRGBAFrameFromValidSample() {
        const int width = 10, height = 10, stride = 40;
        QByteArray pixels = makeRGBA(width, height, stride);

        FakeAppSinkFrameSource fakeSource;
        fakeSource.setNextSample({pixels, width, height, stride, "RGBA"});

        VideoFrameBridge bridge(&fakeSource);
        QSignalSpy spy(&bridge, &VideoFrameBridge::frameReady);

        bridge.processFrame();

        QCOMPARE(spy.count(), 1);
        QImage frame = spy.at(0).at(0).value<QImage>();
        QVERIFY(!frame.isNull());
        QCOMPARE(frame.width(), width);
        QCOMPARE(frame.height(), height);
        QCOMPARE(frame.format(), QImage::Format_RGBA8888);

        const uint8_t *frameBits = frame.constBits();
        QVERIFY(memcmp(frameBits, pixels.constData(), pixels.size()) == 0);
    }

    void rejectsNonRGBAFormat() {
        QByteArray pixels(4, Qt::Uninitialized);
        FakeAppSinkFrameSource fakeSource;
        fakeSource.setNextSample({pixels, 1, 1, 4, "I420"});

        VideoFrameBridge bridge(&fakeSource);
        QSignalSpy spy(&bridge, &VideoFrameBridge::frameReady);

        bridge.processFrame();

        QCOMPARE(spy.count(), 0);
    }

    void usesProvidedStride() {
        const int stride = 64, width = 10, height = 4;
        QByteArray pixels(stride * height, Qt::Uninitialized);
        auto *raw = reinterpret_cast<uint8_t *>(pixels.data());
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int offset = y * stride + x * 4;
                raw[offset + 0] = static_cast<uint8_t>(x * 25);
                raw[offset + 1] = static_cast<uint8_t>(y * 60);
                raw[offset + 2] = 128;
                raw[offset + 3] = 255;
            }
        }

        FakeAppSinkFrameSource fakeSource;
        fakeSource.setNextSample({pixels, width, height, stride, "RGBA"});

        VideoFrameBridge bridge(&fakeSource);
        QSignalSpy spy(&bridge, &VideoFrameBridge::frameReady);

        bridge.processFrame();

        QCOMPARE(spy.count(), 1);
        QImage frame = spy.at(0).at(0).value<QImage>();
        QCOMPARE(frame.width(), width);
        QCOMPARE(frame.height(), height);

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int srcOffset = y * stride + x * 4;
                const uint8_t *srcPixel = raw + srcOffset;
                QRgb dstPixel = frame.pixel(x, y);
                QCOMPARE(qRed(dstPixel), static_cast<int>(srcPixel[0]));
                QCOMPARE(qGreen(dstPixel), static_cast<int>(srcPixel[1]));
                QCOMPARE(qBlue(dstPixel), static_cast<int>(srcPixel[2]));
                QCOMPARE(qAlpha(dstPixel), static_cast<int>(srcPixel[3]));
            }
        }
    }

    void rejectsMissingSampleData() {
        FakeAppSinkFrameSource fakeSource;

        VideoFrameBridge bridge(&fakeSource);
        QSignalSpy spy(&bridge, &VideoFrameBridge::frameReady);

        bridge.processFrame();

        QCOMPARE(spy.count(), 0);
    }

    void rejectsEmptyBytes() {
        FakeAppSinkFrameSource fakeSource;
        fakeSource.setNextSample({QByteArray(), 10, 10, 40, "RGBA"});

        VideoFrameBridge bridge(&fakeSource);
        QSignalSpy spy(&bridge, &VideoFrameBridge::frameReady);

        bridge.processFrame();

        QCOMPARE(spy.count(), 0);
    }

    void rejectsZeroDimensions() {
        QByteArray pixels(16, Qt::Uninitialized);
        FakeAppSinkFrameSource fakeSource;
        fakeSource.setNextSample({pixels, 0, 10, 40, "RGBA"});

        VideoFrameBridge bridge(&fakeSource);
        QSignalSpy spy(&bridge, &VideoFrameBridge::frameReady);

        bridge.processFrame();

        QCOMPARE(spy.count(), 0);
    }

    void rejectsInsufficientBytesPerLine() {
        QByteArray pixels(16, Qt::Uninitialized);
        FakeAppSinkFrameSource fakeSource;
        fakeSource.setNextSample({pixels, 4, 4, 8, "RGBA"}); // 4*4=16 > 8

        VideoFrameBridge bridge(&fakeSource);
        QSignalSpy spy(&bridge, &VideoFrameBridge::frameReady);

        bridge.processFrame();

        QCOMPARE(spy.count(), 0);
    }

    void emittedFrameIsDeepCopy() {
        const int width = 4, height = 4, stride = width * 4;
        QByteArray pixels = makeRGBA(width, height, stride);

        FakeAppSinkFrameSource fakeSource;
        fakeSource.setNextSample({pixels, width, height, stride, "RGBA"});

        VideoFrameBridge bridge(&fakeSource);
        QSignalSpy spy(&bridge, &VideoFrameBridge::frameReady);

        bridge.processFrame();

        QCOMPARE(spy.count(), 1);
        QImage frame = spy.at(0).at(0).value<QImage>();
        QVERIFY(!frame.isNull());

        // Emitted QImage is an independent deep copy; modifying or
        // discarding the source sample must not affect pixel data.
        fakeSource.setNextSample({QByteArray(), 0, 0, 0, ""});
        const uint8_t *frameBits = frame.constBits();
        QVERIFY(memcmp(frameBits, pixels.constData(), pixels.size()) == 0);
    }

    void startDelegatesToSourceOnce() {
        FakeAppSinkFrameSource fakeSource;
        VideoFrameBridge bridge(&fakeSource);

        QCOMPARE(fakeSource.startCount(), 0);
        bridge.start();
        QCOMPARE(fakeSource.startCount(), 1);

        bridge.start();
        QCOMPARE(fakeSource.startCount(), 1);
    }

    void startReceivesFrameFromSourceEvent() {
        const int width = 2, height = 2, stride = width * 4;
        QByteArray pixels = makeRGBA(width, height, stride);

        FakeAppSinkFrameSource fakeSource;
        fakeSource.setNextSample({pixels, width, height, stride, "RGBA"});

        VideoFrameBridge bridge(&fakeSource);
        QSignalSpy spy(&bridge, &VideoFrameBridge::frameReady);

        bridge.start();
        fakeSource.emitFrameAvailable();

        QCOMPARE(spy.count(), 1);
        QImage frame = spy.at(0).at(0).value<QImage>();
        QCOMPARE(frame.width(), width);
        QCOMPARE(frame.height(), height);
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

QTEST_GUILESS_MAIN(VideoFrameBridgeTest)
#include "VideoFrameBridgeTest.moc"
