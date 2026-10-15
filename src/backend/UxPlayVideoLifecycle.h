#pragma once

#include <QObject>
#include <QImage>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <gst/gst.h>

class VideoFrameBridge;
class VideoObservation;

// Owns video resources and ordering, not receiver policy. The receiver serializes
// calls with its shared renderer lock and drains recording before reset/shutdown.
class UxPlayVideoLifecycle final : public QObject {
public:
    struct SampleTap {
        void *context = nullptr;
        // The receiver owns callback routing; release drains and retires that
        // context after the native callback has been unregistered.
        std::function<void()> release;
    };
    struct Options {
        std::shared_ptr<VideoObservation> observation;
        void (*sampleCallback)(GstSample *, void *) = nullptr;
        std::function<SampleTap()> createSampleTap;
        std::function<void(const QString &)> callObserver;
        std::function<int(bool, bool)> codecChooser;
    };
    enum class ResetResult { Ready, InitializationFailed, CodecSelectionFailed };

    explicit UxPlayVideoLifecycle(Options options, QObject *parent);
    ~UxPlayVideoLifecycle() override;
    bool initialize(void *logger, const QString &sink);
    void start();
    ResetResult reset(void *logger, const QString &sink, bool fitMode);
    bool stopForDisconnect();
    void resumeAfterDisconnect();
    void shutdown();

    int selectCodec(bool h265);
    void attachSelectedCodec(bool h265);
    void setFrameCallback(std::function<void(QImage)> callback);
    void setFitMode(bool enabled);

private:
    void registerSampleTap();
    void clearSampleTap();
    void resetFrameBridge();
    void attachFrameBridge();
    int chooseCodec(bool h265) const;
    void observeCall(const QString &call) const;
    void observeBoundary(const char *name, int result = 0, bool hasResult = false) const;

    Options m_options;
    SampleTap m_tap;
    bool m_initialized = false;
    std::atomic_bool m_stopped = false;
    bool m_h265 = false;
    VideoFrameBridge *m_bridge = nullptr;
    void *m_observedBridgePipeline = nullptr;
    std::function<void(QImage)> m_frameCallback;
};
