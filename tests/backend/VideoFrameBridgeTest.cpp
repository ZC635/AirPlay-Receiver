#include <QtTest/QtTest>
#include <QSignalSpy>
#include <QImage>
#include <QStringList>
#include <functional>
#include <memory>
#include <utility>
#include <cstring>

#include "backend/VideoFrameBridge.h"
#include "backend/AppSinkFrameSource.h"

struct FrameSourceLifetimeState {
    bool destroyed = false;
    QStringList events;
};

class FakeAppSinkFrameSource : public AppSinkFrameSource {
public:
    explicit FakeAppSinkFrameSource(std::shared_ptr<FrameSourceLifetimeState> state = {})
        : m_lifetimeState(std::move(state)) {}
    ~FakeAppSinkFrameSource() override {
        if (m_lifetimeState) {
            m_lifetimeState->destroyed = true;
            m_lifetimeState->events.append(QStringLiteral("destroyed"));
        }
    }
    void setNextSample(VideoFrameSample sample) { m_nextSample = std::move(sample); }
    void setFrameAvailableCallback(std::function<void()> callback) override {
        if (!callback && m_lifetimeState) {
            m_lifetimeState->events.append(QStringLiteral("cleared"));
        }
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
    std::shared_ptr<FrameSourceLifetimeState> m_lifetimeState;
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

class VideoFrameBridgeTest : public QObject {
    Q_OBJECT

private slots:
    void ownedSourceClearsCallbackBeforeDestruction() {
        auto state = std::make_shared<FrameSourceLifetimeState>();
        {
            VideoFrameBridge bridge(std::make_unique<FakeAppSinkFrameSource>(state));
            QVERIFY(!state->destroyed);
            bridge.start();
            QVERIFY(state->events.isEmpty());
        }
        QVERIFY(state->destroyed);
        QCOMPARE(state->events, QStringList({"cleared", "destroyed"}));
    }

    void borrowedSourceSurvivesBridge() {
        auto state = std::make_shared<FrameSourceLifetimeState>();
        {
            FakeAppSinkFrameSource source(state);
            {
                VideoFrameBridge bridge(&source);
                bridge.start();
                QVERIFY(!state->destroyed);
            }
            QVERIFY(!state->destroyed);
            QCOMPARE(state->events, QStringList({"cleared"}));
        }
        QVERIFY(state->destroyed);
        QCOMPARE(state->events, QStringList({"cleared", "destroyed"}));
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

};

QTEST_GUILESS_MAIN(VideoFrameBridgeTest)
#include "VideoFrameBridgeTest.moc"
