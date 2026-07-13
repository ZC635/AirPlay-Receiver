#include "backend/GstRecordingPipeline.h"
#include "backend/RecordingFileTransaction.h"

#include <QDir>
#include <QFile>
#include <QStringList>

#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>

#include <chrono>
#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

bool realFactoryExists(const QString &name)
{
    GstElementFactory *factory = gst_element_factory_find(name.toUtf8().constData());
    if (!factory) return false;
    gst_object_unref(factory);
    return true;
}

bool realFactoryReady(const QString &name, QString *error)
{
    GstElement *element = gst_element_factory_make(name.toUtf8().constData(), nullptr);
    if (!element) {
        if (error) *error = QStringLiteral("factory create failed");
        return false;
    }
    const GstStateChangeReturn state = gst_element_set_state(element, GST_STATE_READY);
    gst_element_set_state(element, GST_STATE_NULL);
    gst_object_unref(element);
    if (state == GST_STATE_CHANGE_FAILURE) {
        if (error) *error = QStringLiteral("READY state failed");
        return false;
    }
    return true;
}

GstRecordingPipelineHooks completedHooks(GstRecordingPipelineHooks hooks)
{
    if (!hooks.factoryExists) hooks.factoryExists = realFactoryExists;
    if (!hooks.factoryReady) hooks.factoryReady = realFactoryReady;
    if (!hooks.busTimedPop) {
        hooks.busTimedPop = [](GstBus *bus, GstClockTime timeout) {
            return gst_bus_timed_pop_filtered(
                bus, timeout,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        };
    }
    if (!hooks.labeledBusTimedPop) {
        const auto legacyWait = hooks.busTimedPop;
        hooks.labeledBusTimedPop = [legacyWait](const QString &, GstBus *bus,
                                                GstClockTime timeout) {
            return legacyWait(bus, timeout);
        };
    }
    if (!hooks.monotonicMilliseconds) {
        hooks.monotonicMilliseconds = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };
    }
    if (!hooks.encoderFactoryAlias) {
        hooks.encoderFactoryAlias = [](const QString &logicalFactory) {
            return logicalFactory;
        };
    }
    return hooks;
}

struct RequiredFactory {
    const char *name;
    const char *classification;
};

constexpr RequiredFactory kRequiredFactories[] = {
    {"matroskamux", "Matroska spool"},
    {"matroskademux", "Matroska spool"},
    {"mp4mux", "MP4 muxer"},
    {"h264parse", "H.264 parser"},
    {"avenc_aac", "AAC encoder"},
    {"aacparse", "AAC parser"},
    {"appsrc", "appsrc"},
    {"appsink", "appsink"},
    {"videoconvert", "videoconvert"},
    {"audioconvert", "audioconvert"},
    {"audioresample", "audioresample"},
    {"capsfilter", "capsfilter"},
    {"filesrc", "filesrc"},
    {"filesink", "filesink"},
    {"identity", "identity"},
};

struct CapabilityProbeDetails {
    GstRecordingCapabilityResult result;
    QStringList readyEncoders;
};

CapabilityProbeDetails probeCapabilityDetails(GstRecordingPipelineHooks hooks)
{
    hooks = completedHooks(std::move(hooks));
    for (const RequiredFactory &required : kRequiredFactories) {
        if (!hooks.factoryExists(QString::fromLatin1(required.name))) {
            return {{false, {},
                     QStringLiteral("%1 unavailable: required factory %2 is missing")
                         .arg(QString::fromLatin1(required.classification),
                              QString::fromLatin1(required.name))},
                    {}};
        }
    }

    QStringList encoderFailures;
    QStringList readyEncoders;
    for (const QString &encoder : {QStringLiteral("mfh264enc"),
                                   QStringLiteral("openh264enc")}) {
        const QString actualEncoder = hooks.encoderFactoryAlias(encoder);
        if (!hooks.factoryExists(actualEncoder)) {
            encoderFailures.append(QStringLiteral("%1 missing").arg(encoder));
            continue;
        }
        QString readyError;
        if (hooks.factoryReady(actualEncoder, &readyError)) {
            readyEncoders.append(encoder);
        } else {
            encoderFailures.append(QStringLiteral("%1 READY failed: %2").arg(encoder, readyError));
        }
    }
    if (!readyEncoders.isEmpty()) {
        return {{true, readyEncoders.constFirst(), {}}, readyEncoders};
    }
    return {{false, {},
             QStringLiteral("H.264 encoder unavailable: %1")
                 .arg(encoderFailures.join(QStringLiteral("; ")))},
            {}};
}

void unrefElements(std::initializer_list<GstElement *> elements)
{
    for (GstElement *element : elements) {
        if (element) gst_object_unref(element);
    }
}

QString capsText(const char *text)
{
    return QString::fromLatin1(text).remove(QLatin1Char(' '));
}

bool setEnumProperty(GstElement *element, const char *property,
                     const char *value, QString *error)
{
    if (!g_object_class_find_property(G_OBJECT_GET_CLASS(element), property)) {
        if (error) {
            *error = QStringLiteral("%1 property %2 is missing")
                         .arg(QString::fromUtf8(GST_ELEMENT_NAME(element)),
                              QString::fromLatin1(property));
        }
        return false;
    }
    gst_util_set_object_arg(G_OBJECT(element), property, value);
    return true;
}

bool setUnsignedProperty(GstElement *element, const char *property,
                         guint value, QString *error)
{
    GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(element), property);
    if (!spec) {
        if (error) {
            *error = QStringLiteral("%1 property %2 is missing")
                         .arg(QString::fromUtf8(GST_ELEMENT_NAME(element)),
                              QString::fromLatin1(property));
        }
        return false;
    }
    g_object_set(element, property, value, nullptr);
    return true;
}

bool sampleHasVideoCaps(GstSample *sample,
                        const RecordingVideoDescription &expected,
                        int *stride,
                        gsize *bufferSize,
                        QString *error)
{
    if (!sample || !gst_sample_get_caps(sample) || !gst_sample_get_buffer(sample)) {
        if (error) *error = QStringLiteral("Video sample, caps, or buffer is missing");
        return false;
    }
    const GstStructure *structure = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
    const char *format = gst_structure_get_string(structure, "format");
    int width = 0;
    int height = 0;
    if (g_strcmp0(gst_structure_get_name(structure), "video/x-raw") != 0 ||
        g_strcmp0(format, "RGBA") != 0 ||
        !gst_structure_get_int(structure, "width", &width) ||
        !gst_structure_get_int(structure, "height", &height)) {
        if (error) *error = QStringLiteral("Video sample caps must be RGBA video/x-raw with dimensions");
        return false;
    }
    if (width != expected.width || height != expected.height) {
        if (error) {
            *error = QStringLiteral("Video dimensions changed from %1x%2 to %3x%4")
                         .arg(expected.width).arg(expected.height).arg(width).arg(height);
        }
        return false;
    }
    const GstVideoMeta *meta = gst_buffer_get_video_meta(gst_sample_get_buffer(sample));
    const int detectedStride = meta && meta->stride[0] > 0 ? meta->stride[0] : width * 4;
    const gsize size = gst_buffer_get_size(gst_sample_get_buffer(sample));
    if (detectedStride < width * 4 || size < static_cast<gsize>(detectedStride) * height) {
        if (error) *error = QStringLiteral("RGBA video buffer is smaller than its dimensions and stride");
        return false;
    }
    if (stride) *stride = detectedStride;
    if (bufferSize) *bufferSize = size;
    return true;
}

QString busMessageError(GstMessage *message)
{
    GError *error = nullptr;
    gchar *debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    const QString text = QStringLiteral("%1: %2; debug=%3")
                             .arg(QString::fromUtf8(GST_OBJECT_NAME(message->src)),
                                  QString::fromUtf8(error ? error->message : "unknown"),
                                  QString::fromUtf8(debug ? debug : "none"));
    g_clear_error(&error);
    g_free(debug);
    return text;
}

class HiddenFileLease {
public:
    explicit HiddenFileLease(QString path)
        : m_path(std::move(path))
    {
    }

    bool makeVisible(QString *error)
    {
#ifdef Q_OS_WIN
        const std::wstring nativePath = QDir::toNativeSeparators(m_path).toStdWString();
        m_originalAttributes = GetFileAttributesW(nativePath.c_str());
        if (m_originalAttributes == INVALID_FILE_ATTRIBUTES) {
            if (error) {
                *error = QStringLiteral("Could not read owned placeholder attributes for %1 (Windows error %2)")
                             .arg(m_path).arg(GetLastError());
            }
            return false;
        }
        DWORD visibleAttributes = m_originalAttributes & ~FILE_ATTRIBUTE_HIDDEN;
        if (visibleAttributes == 0) visibleAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileAttributesW(nativePath.c_str(), visibleAttributes)) {
            if (error) {
                *error = QStringLiteral("Could not make owned placeholder visible for writer %1 (Windows error %2)")
                             .arg(m_path).arg(GetLastError());
            }
            return false;
        }
        m_changed = true;
#else
        if (!QFile::exists(m_path)) {
            if (error) *error = QStringLiteral("Owned placeholder is missing: %1").arg(m_path);
            return false;
        }
#endif
        return true;
    }

    bool restore(QString *error)
    {
#ifdef Q_OS_WIN
        if (!m_changed) return true;
        const std::wstring nativePath = QDir::toNativeSeparators(m_path).toStdWString();
        if (!SetFileAttributesW(nativePath.c_str(), m_originalAttributes)) {
            if (error) {
                *error = QStringLiteral("Could not restore HIDDEN on owned recording file %1 (Windows error %2)")
                             .arg(m_path).arg(GetLastError());
            }
            return false;
        }
        m_changed = false;
#else
        Q_UNUSED(error);
#endif
        return true;
    }

    ~HiddenFileLease()
    {
        QString ignored;
        restore(&ignored);
    }

private:
    QString m_path;
#ifdef Q_OS_WIN
    DWORD m_originalAttributes = INVALID_FILE_ATTRIBUTES;
    bool m_changed = false;
#endif
};

struct DemuxLinkTarget {
    GstElement *sink = nullptr;
    const char *capsName = nullptr;
};

void linkDemuxPad(GstElement *, GstPad *pad, gpointer data)
{
    auto *target = static_cast<DemuxLinkTarget *>(data);
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    const char *name = caps && !gst_caps_is_empty(caps)
        ? gst_structure_get_name(gst_caps_get_structure(caps, 0)) : nullptr;
    if (g_strcmp0(name, target->capsName) == 0) {
        GstPad *sinkPad = gst_element_get_static_pad(target->sink, "sink");
        if (!gst_pad_is_linked(sinkPad)) gst_pad_link(pad, sinkPad);
        gst_object_unref(sinkPad);
    }
    if (caps) gst_caps_unref(caps);
}

} // namespace

class GstRecordingPipeline::Impl {
public:
    explicit Impl(GstRecordingPipelineHooks suppliedHooks)
        : hooks(completedHooks(std::move(suppliedHooks)))
    {
    }

    ~Impl() { cleanupAll(); }

    GstElement *create(const char *factory, const char *name = nullptr)
    {
        GstElement *element = gst_element_factory_make(factory, name);
        if (element && hooks.elementCreated) hooks.elementCreated(QString::fromLatin1(factory));
        return element;
    }

    void observe(const QString &element, const QString &key, const QString &value)
    {
        if (hooks.configurationObserved) hooks.configurationObserved(element, key, value);
    }

    bool stageAllowed(const QString &encoder, const QString &stage, QString *error)
    {
        if (!hooks.encoderStageAllowed || hooks.encoderStageAllowed(encoder, stage, error)) {
            return true;
        }
        if (error && error->isEmpty()) {
            *error = QStringLiteral("Injected %1 failure for %2").arg(stage, encoder);
        }
        return false;
    }

    void destroyPipeline(const QString &label, GstElement *pipeline)
    {
        if (!pipeline) return;
        gst_element_set_state(pipeline, GST_STATE_NULL);
        if (hooks.pipelineStateObserved) hooks.pipelineStateObserved(label, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }

    void cleanupVideo()
    {
        if (videoPipeline) {
            destroyPipeline(QStringLiteral("video-spool"), videoPipeline);
        }
        videoPipeline = nullptr;
        videoSource = nullptr;
        started = false;
    }

    void cleanupAudio()
    {
        if (audioPipeline) {
            destroyPipeline(QStringLiteral("audio-spool"), audioPipeline);
        }
        audioPipeline = nullptr;
        audioSource = nullptr;
    }

    void cleanupAll()
    {
        cleanupAudio();
        cleanupVideo();
    }

    bool buildVideo(const QString &encoderName, QString *error)
    {
        cleanupVideo();
        const QString actualEncoderName = hooks.encoderFactoryAlias(encoderName);
        const GstRecordingEncoderConfiguration encoderConfiguration =
            GstRecordingPipeline::encoderConfiguration(
                actualEncoderName, config.videoBitrateBitsPerSecond);
        GstElement *pipeline = gst_pipeline_new("recording_video_spool");
        GstElement *source = gst_element_factory_make("appsrc", "recording_video_source");
        GstElement *convert = gst_element_factory_make("videoconvert", "recording_video_convert");
        GstElement *inputCapsFilter = gst_element_factory_make("capsfilter", "recording_video_input_caps");
        GstElement *encoder = gst_element_factory_make(actualEncoderName.toUtf8().constData(), "recording_video_encoder");
        GstElement *parser = gst_element_factory_make("h264parse", "recording_video_parser");
        GstElement *outputCapsFilter = gst_element_factory_make("capsfilter", "recording_video_output_caps");
        GstElement *mux = gst_element_factory_make("matroskamux", "recording_video_mux");
        GstElement *sink = gst_element_factory_make("filesink", "recording_video_sink");
        if (!pipeline || !source || !convert || !inputCapsFilter || !encoder || !parser ||
            !outputCapsFilter || !mux || !sink) {
            if (error) *error = QStringLiteral("Video spool factory creation failed for %1").arg(encoderName);
            unrefElements({source, convert, inputCapsFilter, encoder, parser,
                           outputCapsFilter, mux, sink});
            destroyPipeline(QStringLiteral("video-spool"), pipeline);
            return false;
        }
        if (!stageAllowed(encoderName, QStringLiteral("factory"), error)) {
            unrefElements({source, convert, inputCapsFilter, encoder, parser,
                           outputCapsFilter, mux, sink});
            destroyPipeline(QStringLiteral("video-spool"), pipeline);
            return false;
        }

        const QByteArray inputCapsString = QStringLiteral(
            "video/x-raw,format=RGBA,width=%1,height=%2,framerate=%3/%4,pixel-aspect-ratio=1/1")
            .arg(config.video.width).arg(config.video.height)
            .arg(config.video.fpsNumerator).arg(config.video.fpsDenominator)
            .toLatin1();
        GstCaps *sourceCaps = gst_caps_from_string(inputCapsString.constData());
        GstCaps *encoderInputCaps = gst_caps_new_simple(
            "video/x-raw", "format", G_TYPE_STRING,
            encoderConfiguration.inputFormat.toUtf8().constData(), nullptr);
        GstCaps *outputCaps = gst_caps_from_string(
            "video/x-h264,stream-format=avc,alignment=au");
        if (!sourceCaps || !encoderInputCaps || !outputCaps) {
            if (error) *error = QStringLiteral("Video spool caps creation failed for %1").arg(encoderName);
            if (sourceCaps) gst_caps_unref(sourceCaps);
            if (encoderInputCaps) gst_caps_unref(encoderInputCaps);
            if (outputCaps) gst_caps_unref(outputCaps);
            unrefElements({source, convert, inputCapsFilter, encoder, parser,
                           outputCapsFilter, mux, sink});
            destroyPipeline(QStringLiteral("video-spool"), pipeline);
            return false;
        }

        g_object_set(source, "block", FALSE, "is-live", FALSE,
                     "format", GST_FORMAT_TIME, nullptr);
        gst_app_src_set_caps(GST_APP_SRC(source), sourceCaps);
        g_object_set(inputCapsFilter, "caps", encoderInputCaps, nullptr);
        g_object_set(outputCapsFilter, "caps", outputCaps, nullptr);
        g_object_set(mux, "offset-to-zero", TRUE, nullptr);
        g_object_set(sink, "location", config.videoSpoolPath.toUtf8().constData(), nullptr);
        gst_caps_unref(sourceCaps);
        gst_caps_unref(encoderInputCaps);
        gst_caps_unref(outputCaps);

        if (actualEncoderName == QStringLiteral("mfh264enc")) {
            if (!setUnsignedProperty(encoder, "bitrate",
                                     static_cast<guint>(encoderConfiguration.bitratePropertyValue), error) ||
                !setEnumProperty(encoder, "rc-mode",
                                 encoderConfiguration.rateControl.toUtf8().constData(), error)) {
                unrefElements({source, convert, inputCapsFilter, encoder, parser,
                               outputCapsFilter, mux, sink});
                destroyPipeline(QStringLiteral("video-spool"), pipeline);
                return false;
            }
            observe(actualEncoderName, QStringLiteral("bitrate"),
                    QString::number(encoderConfiguration.bitratePropertyValue));
            observe(actualEncoderName, QStringLiteral("rc-mode"), encoderConfiguration.rateControl);
            observe(actualEncoderName, QStringLiteral("input-format"), encoderConfiguration.inputFormat);
        } else {
            if (!setUnsignedProperty(encoder, "bitrate",
                                     static_cast<guint>(encoderConfiguration.bitratePropertyValue), error) ||
                !setEnumProperty(encoder, "rate-control",
                                 encoderConfiguration.rateControl.toUtf8().constData(), error) ||
                !setEnumProperty(encoder, "usage-type",
                                 encoderConfiguration.usageType.toUtf8().constData(), error)) {
                unrefElements({source, convert, inputCapsFilter, encoder, parser,
                               outputCapsFilter, mux, sink});
                destroyPipeline(QStringLiteral("video-spool"), pipeline);
                return false;
            }
            observe(actualEncoderName, QStringLiteral("bitrate"),
                    QString::number(encoderConfiguration.bitratePropertyValue));
            observe(actualEncoderName, QStringLiteral("rate-control"), encoderConfiguration.rateControl);
            observe(actualEncoderName, QStringLiteral("usage-type"), encoderConfiguration.usageType);
            observe(actualEncoderName, QStringLiteral("input-format"), encoderConfiguration.inputFormat);
        }
        observe(QStringLiteral("video-appsrc"), QStringLiteral("caps"),
                capsText(inputCapsString.constData()));
        observe(QStringLiteral("h264parse"), QStringLiteral("caps"),
                QStringLiteral("video/x-h264,stream-format=avc,alignment=au"));
        observe(QStringLiteral("video-graph"), QStringLiteral("converters"),
                QStringLiteral("videoconvert"));

        if (!stageAllowed(encoderName, QStringLiteral("caps"), error)) {
            unrefElements({source, convert, inputCapsFilter, encoder, parser,
                           outputCapsFilter, mux, sink});
            destroyPipeline(QStringLiteral("video-spool"), pipeline);
            return false;
        }

        gst_bin_add_many(GST_BIN(pipeline), source, convert, inputCapsFilter, encoder,
                         parser, outputCapsFilter, mux, sink, nullptr);
        if (!gst_element_link_many(source, convert, inputCapsFilter, encoder, parser,
                                   outputCapsFilter, mux, sink, nullptr)) {
            if (error && error->isEmpty()) {
                *error = QStringLiteral("Video spool link/caps failure for %1").arg(encoderName);
            }
            destroyPipeline(QStringLiteral("video-spool"), pipeline);
            return false;
        }
        if (!stageAllowed(encoderName, QStringLiteral("link"), error)) {
            destroyPipeline(QStringLiteral("video-spool"), pipeline);
            return false;
        }
        HiddenFileLease visibility(config.videoSpoolPath);
        if (!visibility.makeVisible(error)) {
            destroyPipeline(QStringLiteral("video-spool"), pipeline);
            return false;
        }
        const GstStateChangeReturn ready = gst_element_set_state(pipeline, GST_STATE_READY);
        QString restoreError;
        const bool restored = visibility.restore(&restoreError);
        if (ready == GST_STATE_CHANGE_FAILURE || !restored) {
            if (error) {
                *error = !restored
                    ? restoreError
                    : QStringLiteral("Video spool READY state failure for %1").arg(encoderName);
            }
            destroyPipeline(QStringLiteral("video-spool"), pipeline);
            return false;
        }
        if (!stageAllowed(encoderName, QStringLiteral("ready"), error)) {
            destroyPipeline(QStringLiteral("video-spool"), pipeline);
            return false;
        }
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            if (error) *error = QStringLiteral("Video spool PLAYING state failure for %1").arg(encoderName);
            destroyPipeline(QStringLiteral("video-spool"), pipeline);
            return false;
        }
        if (!stageAllowed(encoderName, QStringLiteral("playing"), error)) {
            destroyPipeline(QStringLiteral("video-spool"), pipeline);
            return false;
        }
        videoPipeline = pipeline;
        videoSource = source;
        return true;
    }

    bool pushVideoBuffer(GstSample *sample, qint64 normalizedPtsNs, QString *error)
    {
        if (!videoSource || normalizedPtsNs < 0) {
            if (error) *error = QStringLiteral("Video pipeline is not active or PTS is invalid");
            return false;
        }
        int sampleStride = 0;
        gsize sampleSize = 0;
        if (!sampleHasVideoCaps(sample, config.video, &sampleStride, &sampleSize, error)) return false;
        GstBuffer *copy = gst_buffer_copy_deep(gst_sample_get_buffer(sample));
        if (!copy) {
            if (error) *error = QStringLiteral("Could not copy RGBA video buffer");
            return false;
        }
        GST_BUFFER_PTS(copy) = static_cast<GstClockTime>(normalizedPtsNs);
        GST_BUFFER_DTS(copy) = GST_CLOCK_TIME_NONE;
        if (hooks.bufferPushed) hooks.bufferPushed(QStringLiteral("video"), copy);
        const GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(videoSource), copy);
        if (flow != GST_FLOW_OK) {
            if (error) *error = QStringLiteral("Video appsrc push failed: %1").arg(flow);
            return false;
        }
        stride = sampleStride;
        blackBufferSize = sampleSize;
        return true;
    }

    bool settleVideoStart(QString *error)
    {
        GstState state = GST_STATE_VOID_PENDING;
        GstState pending = GST_STATE_VOID_PENDING;
        const GstStateChangeReturn settled =
            gst_element_get_state(videoPipeline, &state, &pending, 5 * GST_SECOND);
        GstBus *bus = gst_element_get_bus(videoPipeline);
        GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        gst_object_unref(bus);
        if (message) {
            if (error) *error = QStringLiteral("Video spool startup error: %1").arg(busMessageError(message));
            gst_message_unref(message);
            return false;
        }
        if (settled == GST_STATE_CHANGE_FAILURE || state != GST_STATE_PLAYING) {
            if (error) {
                *error = QStringLiteral("Video spool did not settle in PLAYING (state=%1 pending=%2 result=%3)")
                             .arg(state).arg(pending).arg(settled);
            }
            return false;
        }
        return true;
    }

    bool validateAudioSample(GstSample *sample, QString *error) const
    {
        if (!sample || !gst_sample_get_caps(sample) || !gst_sample_get_buffer(sample)) {
            if (error) *error = QStringLiteral("Audio sample, caps, or buffer is missing");
            return false;
        }
        const GstStructure *structure = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
        const char *format = gst_structure_get_string(structure, "format");
        const char *layout = gst_structure_get_string(structure, "layout");
        int rate = 0;
        int channels = 0;
        if (g_strcmp0(gst_structure_get_name(structure), "audio/x-raw") != 0 ||
            g_strcmp0(format, "S16LE") != 0 ||
            g_strcmp0(layout, "interleaved") != 0 ||
            !gst_structure_get_int(structure, "rate", &rate) || rate != 44100 ||
            !gst_structure_get_int(structure, "channels", &channels) || channels != 2) {
            if (error) *error = QStringLiteral("Audio sample caps must be interleaved S16LE 44100 Hz stereo");
            return false;
        }
        return true;
    }

    bool buildAudio(QString *error)
    {
        cleanupAudio();
        GstElement *pipeline = gst_pipeline_new("recording_audio_spool");
        GstElement *source = create("appsrc", "recording_audio_source");
        GstElement *convert = create("audioconvert", "recording_audio_convert");
        GstElement *resample = create("audioresample", "recording_audio_resample");
        GstElement *rawCapsFilter = create("capsfilter", "recording_audio_raw_caps");
        GstElement *encoder = create("avenc_aac", "recording_audio_encoder");
        GstElement *parser = create("aacparse", "recording_audio_parser");
        GstElement *aacCapsFilter = create("capsfilter", "recording_audio_aac_caps");
        GstElement *mux = create("matroskamux", "recording_audio_mux");
        GstElement *sink = create("filesink", "recording_audio_sink");
        if (!pipeline || !source || !convert || !resample || !rawCapsFilter || !encoder ||
            !parser || !aacCapsFilter || !mux || !sink) {
            if (error) *error = QStringLiteral("Audio spool factory creation failed");
            unrefElements({source, convert, resample, rawCapsFilter, encoder,
                           parser, aacCapsFilter, mux, sink});
            if (pipeline) gst_object_unref(pipeline);
            return false;
        }
        GstCaps *sourceCaps = gst_caps_from_string(
            "audio/x-raw,format=S16LE,rate=44100,channels=2,layout=interleaved");
        GstCaps *encoderCaps = gst_caps_from_string(
            "audio/x-raw,format=F32LE,rate=44100,channels=2,layout=interleaved");
        GstCaps *aacCaps = gst_caps_from_string(
            "audio/mpeg,mpegversion=4,stream-format=raw");
        if (!sourceCaps || !encoderCaps || !aacCaps) {
            if (error) *error = QStringLiteral("Audio spool caps creation failed");
            if (sourceCaps) gst_caps_unref(sourceCaps);
            if (encoderCaps) gst_caps_unref(encoderCaps);
            if (aacCaps) gst_caps_unref(aacCaps);
            unrefElements({source, convert, resample, rawCapsFilter, encoder,
                           parser, aacCapsFilter, mux, sink});
            gst_object_unref(pipeline);
            return false;
        }
        g_object_set(source, "block", FALSE, "is-live", FALSE,
                     "format", GST_FORMAT_TIME, nullptr);
        gst_app_src_set_caps(GST_APP_SRC(source), sourceCaps);
        g_object_set(rawCapsFilter, "caps", encoderCaps, nullptr);
        g_object_set(aacCapsFilter, "caps", aacCaps, nullptr);
        gst_caps_unref(sourceCaps);
        gst_caps_unref(encoderCaps);
        gst_caps_unref(aacCaps);
        if (!setUnsignedProperty(encoder, "bitrate", 192000, error)) {
            unrefElements({source, convert, resample, rawCapsFilter, encoder,
                           parser, aacCapsFilter, mux, sink});
            gst_object_unref(pipeline);
            return false;
        }
        observe(QStringLiteral("avenc_aac"), QStringLiteral("bitrate"), QStringLiteral("192000"));
        g_object_set(mux, "offset-to-zero", TRUE, nullptr);
        g_object_set(sink, "location", config.audioSpoolPath.toUtf8().constData(), nullptr);
        gst_bin_add_many(GST_BIN(pipeline), source, convert, resample, rawCapsFilter,
                         encoder, parser, aacCapsFilter, mux, sink, nullptr);
        if (!gst_element_link_many(source, convert, resample, rawCapsFilter, encoder,
                                   parser, aacCapsFilter, mux, sink, nullptr)) {
            if (error) *error = QStringLiteral("Audio spool link/caps failure");
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            return false;
        }
        HiddenFileLease visibility(config.audioSpoolPath);
        if (!visibility.makeVisible(error)) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            return false;
        }
        const GstStateChangeReturn ready = gst_element_set_state(pipeline, GST_STATE_READY);
        QString restoreError;
        const bool restored = visibility.restore(&restoreError);
        if (ready == GST_STATE_CHANGE_FAILURE || !restored) {
            if (error) *error = restored ? QStringLiteral("Audio spool READY state failure") : restoreError;
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            return false;
        }
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            if (error) *error = QStringLiteral("Audio spool PLAYING state failure");
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            return false;
        }
        audioPipeline = pipeline;
        audioSource = source;
        return true;
    }

    bool pushAudioBuffer(GstSample *sample, qint64 localPtsNs, QString *error)
    {
        GstBuffer *copy = gst_buffer_copy_deep(gst_sample_get_buffer(sample));
        if (!copy) {
            if (error) *error = QStringLiteral("Could not copy S16LE audio buffer");
            return false;
        }
        GST_BUFFER_PTS(copy) = static_cast<GstClockTime>(localPtsNs);
        GST_BUFFER_DTS(copy) = GST_CLOCK_TIME_NONE;
        if (hooks.bufferPushed) hooks.bufferPushed(QStringLiteral("audio"), copy);
        const GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(audioSource), copy);
        if (flow != GST_FLOW_OK) {
            if (error) *error = QStringLiteral("Audio appsrc push failed: %1").arg(flow);
            return false;
        }
        return true;
    }

    bool waitForEos(GstElement *pipeline, int deadlineMs, qint64 deadlineStartMs,
                    const std::atomic_bool &cancelled, const QString &label,
                    QString *error)
    {
        GstBus *bus = gst_element_get_bus(pipeline);
        while (hooks.monotonicMilliseconds() - deadlineStartMs < deadlineMs) {
            if (cancelled.load(std::memory_order_relaxed)) {
                *error = QStringLiteral("%1 cancelled").arg(label);
                gst_object_unref(bus);
                return false;
            }
            const qint64 remainingMs =
                deadlineMs - (hooks.monotonicMilliseconds() - deadlineStartMs);
            if (remainingMs <= 0) break;
            const GstClockTime slice = static_cast<GstClockTime>(qMin<qint64>(remainingMs, 10)) * GST_MSECOND;
            GstMessage *message = hooks.labeledBusTimedPop(label, bus, slice);
            if (!message) continue;
            const GstMessageType type = GST_MESSAGE_TYPE(message);
            if (type == GST_MESSAGE_EOS) {
                gst_message_unref(message);
                gst_object_unref(bus);
                return true;
            }
            if (type == GST_MESSAGE_ERROR) {
                *error = QStringLiteral("%1 error: %2").arg(label, busMessageError(message));
                gst_message_unref(message);
                gst_object_unref(bus);
                return false;
            }
            gst_message_unref(message);
        }
        gst_object_unref(bus);
        *error = QStringLiteral("%1 deadline exceeded after %2 ms").arg(label).arg(deadlineMs);
        return false;
    }

    bool remux(int deadlineMs, qint64 deadlineStartMs,
               const std::atomic_bool &cancelled, QString *error)
    {
        GstElement *pipeline = gst_pipeline_new("recording_spool_remux");
        GstElement *videoFile = create("filesrc", "recording_remux_video_file");
        GstElement *videoDemux = create("matroskademux", "recording_remux_video_demux");
        GstElement *videoParse = create("h264parse", "recording_remux_video_parse");
        GstElement *videoIdentity = create("identity", "recording_remux_video_identity");
        GstElement *videoCapsFilter = create("capsfilter", "recording_remux_video_caps");
        GstElement *mux = create("mp4mux", "recording_remux_mux");
        GstElement *sink = create("filesink", "recording_remux_sink");
        GstElement *audioFile = hasAudio ? create("filesrc", "recording_remux_audio_file") : nullptr;
        GstElement *audioDemux = hasAudio ? create("matroskademux", "recording_remux_audio_demux") : nullptr;
        GstElement *audioParse = hasAudio ? create("aacparse", "recording_remux_audio_parse") : nullptr;
        GstElement *audioIdentity = hasAudio ? create("identity", "recording_remux_audio_identity") : nullptr;
        GstElement *audioCapsFilter = hasAudio ? create("capsfilter", "recording_remux_audio_caps") : nullptr;
        if (!pipeline || !videoFile || !videoDemux || !videoParse || !videoIdentity ||
            !videoCapsFilter || !mux || !sink ||
            (hasAudio && (!audioFile || !audioDemux || !audioParse ||
                          !audioIdentity || !audioCapsFilter))) {
            if (error) *error = QStringLiteral("Remux factory creation failed");
            unrefElements({videoFile, videoDemux, videoParse, videoIdentity, videoCapsFilter,
                           mux, sink, audioFile, audioDemux, audioParse, audioIdentity,
                           audioCapsFilter});
            if (pipeline) gst_object_unref(pipeline);
            return false;
        }

        g_object_set(videoFile, "location", config.videoSpoolPath.toUtf8().constData(), nullptr);
        g_object_set(sink, "location", config.temporaryMp4Path.toUtf8().constData(), nullptr);
        GstCaps *videoCaps = gst_caps_from_string(
            "video/x-h264,stream-format=avc,alignment=au");
        GstCaps *audioCaps = hasAudio
            ? gst_caps_from_string("audio/mpeg,mpegversion=4,stream-format=raw") : nullptr;
        if (!videoCaps || (hasAudio && !audioCaps)) {
            if (error) *error = QStringLiteral("Remux caps creation failed");
            if (videoCaps) gst_caps_unref(videoCaps);
            if (audioCaps) gst_caps_unref(audioCaps);
            unrefElements({videoFile, videoDemux, videoParse, videoIdentity, videoCapsFilter,
                           mux, sink, audioFile, audioDemux, audioParse, audioIdentity,
                           audioCapsFilter});
            gst_object_unref(pipeline);
            return false;
        }
        g_object_set(videoCapsFilter, "caps", videoCaps, nullptr);
        gst_caps_unref(videoCaps);
        if (hasAudio) {
            g_object_set(audioFile, "location", config.audioSpoolPath.toUtf8().constData(), nullptr);
            g_object_set(audioCapsFilter, "caps", audioCaps, nullptr);
            gst_caps_unref(audioCaps);
        }
        GstPad *videoOffsetPad = gst_element_get_static_pad(videoIdentity, "src");
        gst_pad_set_offset(videoOffsetPad, 0);
        gst_object_unref(videoOffsetPad);
        if (hasAudio) {
            GstPad *audioOffsetPad = gst_element_get_static_pad(audioIdentity, "src");
            gst_pad_set_offset(audioOffsetPad, audioOriginPts);
            gst_object_unref(audioOffsetPad);
        }

        if (hasAudio) {
            gst_bin_add_many(GST_BIN(pipeline), videoFile, videoDemux, videoParse,
                             videoIdentity, videoCapsFilter, audioFile, audioDemux,
                             audioParse, audioIdentity, audioCapsFilter, mux, sink, nullptr);
        } else {
            gst_bin_add_many(GST_BIN(pipeline), videoFile, videoDemux, videoParse,
                             videoIdentity, videoCapsFilter, mux, sink, nullptr);
        }
        auto releaseRequestPad = [&](GstPad *pad, const QString &name) {
            if (!pad) return;
            gst_element_release_request_pad(mux, pad);
            if (hooks.requestPadObserved) hooks.requestPadObserved(name, false);
            gst_object_unref(pad);
        };
        auto failOwnedPipeline = [&](const QString &failure, GstPad *videoPad, GstPad *audioPad) {
            if (error) *error = failure;
            gst_element_set_state(pipeline, GST_STATE_NULL);
            if (hooks.pipelineStateObserved) {
                hooks.pipelineStateObserved(QStringLiteral("mp4-remux"), GST_STATE_NULL);
            }
            releaseRequestPad(videoPad, QStringLiteral("video_%u"));
            releaseRequestPad(audioPad, QStringLiteral("audio_%u"));
            gst_object_unref(pipeline);
            return false;
        };
        if (!gst_element_link(videoFile, videoDemux) ||
            !gst_element_link_many(videoParse, videoIdentity, videoCapsFilter, nullptr) ||
            !gst_element_link(mux, sink) ||
            (hasAudio && (!gst_element_link(audioFile, audioDemux) ||
                          !gst_element_link_many(audioParse, audioIdentity,
                                                 audioCapsFilter, nullptr)))) {
            return failOwnedPipeline(QStringLiteral("Remux static link failure"), nullptr, nullptr);
        }
        DemuxLinkTarget videoTarget{videoParse, "video/x-h264"};
        DemuxLinkTarget audioTarget{audioParse, "audio/mpeg"};
        g_signal_connect(videoDemux, "pad-added", G_CALLBACK(linkDemuxPad), &videoTarget);
        if (hasAudio) {
            g_signal_connect(audioDemux, "pad-added", G_CALLBACK(linkDemuxPad), &audioTarget);
        }

        GstPad *videoMuxPad = gst_element_request_pad_simple(mux, "video_%u");
        if (videoMuxPad && hooks.requestPadObserved) {
            hooks.requestPadObserved(QStringLiteral("video_%u"), true);
        }
        if (hooks.elementCreated) hooks.elementCreated(QStringLiteral("video_%u"));
        GstPad *audioMuxPad = hasAudio ? gst_element_request_pad_simple(mux, "audio_%u") : nullptr;
        if (audioMuxPad && hooks.requestPadObserved) {
            hooks.requestPadObserved(QStringLiteral("audio_%u"), true);
        }
        if (hasAudio && hooks.elementCreated) hooks.elementCreated(QStringLiteral("audio_%u"));
        GstPad *videoSrcPad = gst_element_get_static_pad(videoCapsFilter, "src");
        const GstPadLinkReturn videoLink = videoMuxPad && videoSrcPad
            ? gst_pad_link(videoSrcPad, videoMuxPad) : GST_PAD_LINK_REFUSED;
        if (videoSrcPad) gst_object_unref(videoSrcPad);
        GstPadLinkReturn audioLink = GST_PAD_LINK_OK;
        if (hasAudio) {
            GstPad *audioSrcPad = gst_element_get_static_pad(audioCapsFilter, "src");
            audioLink = audioMuxPad && audioSrcPad
                ? gst_pad_link(audioSrcPad, audioMuxPad) : GST_PAD_LINK_REFUSED;
            if (audioSrcPad) gst_object_unref(audioSrcPad);
        }
        if (!videoMuxPad || videoLink != GST_PAD_LINK_OK ||
            (hasAudio && (!audioMuxPad || audioLink != GST_PAD_LINK_OK))) {
            return failOwnedPipeline(
                QStringLiteral("Remux request-pad link failure (video=%1 audio=%2)")
                    .arg(videoLink).arg(audioLink), videoMuxPad, audioMuxPad);
        }

        HiddenFileLease visibility(config.temporaryMp4Path);
        if (!visibility.makeVisible(error)) {
            return failOwnedPipeline(*error, videoMuxPad, audioMuxPad);
        }
        const GstStateChangeReturn ready = gst_element_set_state(pipeline, GST_STATE_READY);
        QString restoreError;
        const bool restored = visibility.restore(&restoreError);
        if (ready == GST_STATE_CHANGE_FAILURE || !restored) {
            return failOwnedPipeline(
                restored ? QStringLiteral("Remux READY state failure") : restoreError,
                videoMuxPad, audioMuxPad);
        }
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            return failOwnedPipeline(QStringLiteral("Remux PLAYING state failure"),
                                     videoMuxPad, audioMuxPad);
        }
        QString waitError;
        const bool completed = waitForEos(pipeline, deadlineMs, deadlineStartMs, cancelled,
                                          QStringLiteral("MP4 remux"), &waitError);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        if (hooks.pipelineStateObserved) {
            hooks.pipelineStateObserved(QStringLiteral("mp4-remux"), GST_STATE_NULL);
        }
        releaseRequestPad(videoMuxPad, QStringLiteral("video_%u"));
        releaseRequestPad(audioMuxPad, QStringLiteral("audio_%u"));
        gst_object_unref(pipeline);
        if (!completed) {
            if (error) *error = waitError;
            return false;
        }
        return true;
    }

    GstRecordingPipelineHooks hooks;
    GstRecordingPipelineConfig config;
    GstElement *videoPipeline = nullptr;
    GstElement *videoSource = nullptr;
    GstElement *audioPipeline = nullptr;
    GstElement *audioSource = nullptr;
    QString encoderName;
    qint64 audioOriginPts = -1;
    bool hasAudio = false;
    int stride = 0;
    gsize blackBufferSize = 0;
    bool started = false;
};

GstRecordingPipeline::GstRecordingPipeline(GstRecordingPipelineHooks hooks)
    : m_impl(std::make_unique<Impl>(std::move(hooks)))
{
}

GstRecordingPipeline::~GstRecordingPipeline() = default;

GstRecordingCapabilityResult GstRecordingPipeline::probeCapabilities()
{
    return probeCapabilities({});
}

GstRecordingCapabilityResult GstRecordingPipeline::probeCapabilities(
    const GstRecordingPipelineHooks &suppliedHooks)
{
    return probeCapabilityDetails(suppliedHooks).result;
}

GstRecordingEncoderConfiguration GstRecordingPipeline::encoderConfiguration(
    const QString &factory, int bitrateBitsPerSecond)
{
    if (factory == QStringLiteral("mfh264enc")) {
        return {QStringLiteral("NV12"), bitrateBitsPerSecond / 1000,
                QStringLiteral("cbr"), {}};
    }
    if (factory == QStringLiteral("openh264enc")) {
        return {QStringLiteral("I420"), bitrateBitsPerSecond,
                QStringLiteral("bitrate"), QStringLiteral("screen")};
    }
    return {};
}

bool GstRecordingPipeline::start(const GstRecordingPipelineConfig &config,
                                 GstSample *firstVideoSample,
                                 QString *error)
{
    if (error) error->clear();
    if (m_impl->started || config.videoSpoolPath.isEmpty() ||
        config.audioSpoolPath.isEmpty() || config.temporaryMp4Path.isEmpty() ||
        config.video.width <= 0 || config.video.height <= 0 ||
        config.video.fpsNumerator <= 0 || config.video.fpsDenominator <= 0 ||
        config.videoBitrateBitsPerSecond <= 0) {
        if (error) *error = QStringLiteral("Invalid or duplicate recording pipeline start");
        return false;
    }
    const CapabilityProbeDetails capabilities = probeCapabilityDetails(m_impl->hooks);
    if (!capabilities.result.available) {
        if (error) *error = capabilities.result.error;
        return false;
    }
    int stride = 0;
    gsize size = 0;
    if (!sampleHasVideoCaps(firstVideoSample, config.video, &stride, &size, error)) return false;
    m_impl->config = config;
    m_impl->hasAudio = false;
    m_impl->audioOriginPts = -1;
    m_impl->stride = stride;
    m_impl->blackBufferSize = size;
    QStringList candidates;
    if (capabilities.readyEncoders.contains(config.preferredEncoder)) {
        candidates.append(config.preferredEncoder);
    }
    for (const QString &encoder : {QStringLiteral("mfh264enc"),
                                   QStringLiteral("openh264enc")}) {
        if (capabilities.readyEncoders.contains(encoder) && !candidates.contains(encoder)) {
            candidates.append(encoder);
        }
    }
    QStringList failures;
    for (const QString &encoder : candidates) {
        if (m_impl->hooks.encoderAttempted) m_impl->hooks.encoderAttempted(encoder);
        QString attemptError;
        if (m_impl->hooks.encoderBuildAllowed &&
            !m_impl->hooks.encoderBuildAllowed(encoder, &attemptError)) {
            failures.append(QStringLiteral("%1: %2").arg(encoder, attemptError));
            continue;
        }
        if (!m_impl->buildVideo(encoder, &attemptError)) {
            failures.append(QStringLiteral("%1: %2").arg(encoder, attemptError));
            continue;
        }
        if (!m_impl->pushVideoBuffer(firstVideoSample, 0, &attemptError)) {
            failures.append(QStringLiteral("%1: %2").arg(encoder, attemptError));
            m_impl->cleanupVideo();
            continue;
        }
        if (!m_impl->settleVideoStart(&attemptError)) {
            failures.append(QStringLiteral("%1: %2").arg(encoder, attemptError));
            m_impl->cleanupVideo();
            continue;
        }
        m_impl->encoderName = encoder;
        m_impl->started = true;
        return true;
    }
    m_impl->cleanupVideo();
    if (error) {
        *error = QStringLiteral("H.264 encoder fallback exhausted: %1")
                     .arg(failures.join(QStringLiteral("; ")));
    }
    return false;
}

bool GstRecordingPipeline::pushVideo(GstSample *sample,
                                     qint64 normalizedPtsNs,
                                     QString *error)
{
    return m_impl->pushVideoBuffer(sample, normalizedPtsNs, error);
}

bool GstRecordingPipeline::pushBlackFrame(qint64 normalizedPtsNs, QString *error)
{
    if (error) error->clear();
    if (!m_impl->started || !m_impl->videoSource || normalizedPtsNs < 0 ||
        m_impl->stride <= 0 || m_impl->blackBufferSize == 0) {
        if (error) *error = QStringLiteral("Black frame requires an active video pipeline and valid locked stride");
        return false;
    }
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, m_impl->blackBufferSize, nullptr);
    if (!buffer) {
        if (error) *error = QStringLiteral("Could not allocate black RGBA frame");
        return false;
    }
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        if (error) *error = QStringLiteral("Could not map black RGBA frame");
        return false;
    }
    std::memset(map.data, 0, map.size);
    gst_buffer_unmap(buffer, &map);
    gsize offsets[GST_VIDEO_MAX_PLANES]{};
    gint strides[GST_VIDEO_MAX_PLANES]{};
    strides[0] = m_impl->stride;
    gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE,
                                   GST_VIDEO_FORMAT_RGBA,
                                   m_impl->config.video.width,
                                   m_impl->config.video.height,
                                   1, offsets, strides);
    GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(normalizedPtsNs);
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(
        GST_SECOND, static_cast<guint64>(m_impl->config.video.fpsDenominator),
        static_cast<guint64>(m_impl->config.video.fpsNumerator));
    if (m_impl->hooks.bufferPushed) {
        m_impl->hooks.bufferPushed(QStringLiteral("black"), buffer);
    }
    const GstFlowReturn flow =
        gst_app_src_push_buffer(GST_APP_SRC(m_impl->videoSource), buffer);
    if (flow != GST_FLOW_OK) {
        if (error) *error = QStringLiteral("Black-frame appsrc push failed: %1").arg(flow);
        return false;
    }
    return true;
}

bool GstRecordingPipeline::pushAudio(GstSample *sample,
                                     qint64 normalizedPtsNs,
                                     QString *error)
{
    if (error) error->clear();
    if (!m_impl->started || normalizedPtsNs < 0) {
        if (error) *error = QStringLiteral("Audio push requires an active pipeline and non-negative PTS");
        return false;
    }
    if (!m_impl->validateAudioSample(sample, error)) return false;
    if (!m_impl->hasAudio) {
        const QString claimError =
            RecordingFileTransaction::claimOptionalAudioSpool(m_impl->config.audioSpoolPath);
        if (!claimError.isEmpty()) {
            if (error) *error = QStringLiteral("Could not claim optional audio spool: %1").arg(claimError);
            return false;
        }
        if (!m_impl->buildAudio(error)) return false;
        m_impl->audioOriginPts = normalizedPtsNs;
        m_impl->hasAudio = true;
        m_impl->observe(QStringLiteral("audio-appsrc"), QStringLiteral("first-local-pts"),
                        QStringLiteral("0"));
    }
    if (normalizedPtsNs < m_impl->audioOriginPts) {
        if (error) *error = QStringLiteral("Audio PTS precedes the first accepted audio sample");
        return false;
    }
    return m_impl->pushAudioBuffer(sample,
                                   normalizedPtsNs - m_impl->audioOriginPts,
                                   error);
}

GstRecordingFinalizeResult GstRecordingPipeline::finalize(
    int deadlineMs, const std::atomic_bool &cancelled)
{
    if (!m_impl->started || deadlineMs <= 0) {
        return {false, QStringLiteral("Finalize requires an active pipeline and positive deadline")};
    }
    const qint64 deadlineStartMs = m_impl->hooks.monotonicMilliseconds();
    if (cancelled.load(std::memory_order_relaxed)) {
        m_impl->cleanupAll();
        return {false, QStringLiteral("Recording finalize cancelled")};
    }
    const GstFlowReturn videoEos = gst_app_src_end_of_stream(GST_APP_SRC(m_impl->videoSource));
    const GstFlowReturn audioEos = m_impl->hasAudio
        ? gst_app_src_end_of_stream(GST_APP_SRC(m_impl->audioSource)) : GST_FLOW_OK;
    if (videoEos != GST_FLOW_OK || audioEos != GST_FLOW_OK) {
        m_impl->cleanupAll();
        return {false,
                QStringLiteral("Spool EOS injection failed (video=%1 audio=%2)")
                    .arg(videoEos).arg(audioEos)};
    }
    QString error;
    if (!m_impl->waitForEos(m_impl->videoPipeline, deadlineMs, deadlineStartMs, cancelled,
                            QStringLiteral("Video spool"), &error)) {
        m_impl->cleanupAll();
        return {false, error};
    }
    if (m_impl->hasAudio &&
        !m_impl->waitForEos(m_impl->audioPipeline, deadlineMs, deadlineStartMs, cancelled,
                            QStringLiteral("Audio spool"), &error)) {
        m_impl->cleanupAll();
        return {false, error};
    }
    m_impl->cleanupAll();
    if (!m_impl->remux(deadlineMs, deadlineStartMs, cancelled, &error)) {
        return {false, error};
    }
    return {true, {}};
}

void GstRecordingPipeline::abort()
{
    m_impl->cleanupAll();
    m_impl->encoderName.clear();
    m_impl->hasAudio = false;
    m_impl->audioOriginPts = -1;
}

QString GstRecordingPipeline::encoderFactoryName() const
{
    return m_impl->encoderName;
}
