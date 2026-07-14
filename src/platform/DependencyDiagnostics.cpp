#include "platform/DependencyDiagnostics.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#if AIRPLAY_WITH_UXPLAY
#include <gst/gst.h>
#endif

namespace {
QStringList playbackRuntimePaths() {
    return {
        "config/portable-runtime-manifest.txt",
        "airplay_receiver.exe",
        "Qt6Core.dll",
        "Qt6Gui.dll",
        "Qt6Widgets.dll",
        "platforms/qwindows.dll",
        "libgcc_s_seh-1.dll",
        "libstdc++-6.dll",
        "libwinpthread-1.dll",
        "libgstreamer-1.0-0.dll",
        "gstreamer-plugins/libgstapp.dll",
        "gstreamer-plugins/libgstplayback.dll",
        "gstreamer-plugins/libgstautodetect.dll",
        "gstreamer-plugins/libgstvideoparsersbad.dll",
        "gstreamer-plugins/libgstlibav.dll",
        "gstreamer-plugins/libgstd3d11.dll",
        "gstreamer-plugins/libgstwasapi.dll",
        "gstreamer-1.0/registry.x86_64.bin",
        "libqmdnsengine.dll",
    };
}

QStringList requiredRecordingFactories() {
    return {
        "matroskamux", "matroskademux", "mp4mux", "h264parse",
        "avenc_aac", "aacparse", "appsrc", "appsink", "videoconvert",
        "audioconvert", "audioresample", "capsfilter", "filesrc",
        "filesink", "identity",
    };
}
}

DiagnosticResult DependencyDiagnostics::checkExecutable(const QString &name) {
    const QString path = QStandardPaths::findExecutable(name);
    if (path.isEmpty()) {
        return {false, QString("%1 not found. Install it or add it to PATH.").arg(name)};
    }
    return {true, QString("%1 found at %2.").arg(name, path)};
}

DiagnosticResult DependencyDiagnostics::checkEnvironmentVariable(const QString &name) {
    if (!qEnvironmentVariableIsSet(name.toUtf8().constData())) {
        return {false, QString("%1 not set. Set it before launching AirPlay Receiver.").arg(name)};
    }
    return {true, QString("%1 is set.").arg(name)};
}

QStringList DependencyDiagnostics::checkRuntimeBasics() {
    return {
        "GStreamer: install runtime plugins and ensure binaries are on PATH.",
        "QMdnsEngine: install qmdnsengine runtime for mDNS discovery.",
        "UxPlay: build/runtime dependencies must be available for AirPlay receiver support."
    };
}

bool DependencyDiagnostics::shouldCheckStandaloneRuntime() {
    return !qEnvironmentVariableIsSet("AIRPLAY_MSYS2_PATH_MODE");
}

QStringList DependencyDiagnostics::checkStandaloneRuntime(const QString &directory) {
    const QStringList requiredPaths = playbackRuntimePaths();

    QStringList missing;
    const QDir baseDir(directory);
    for (const QString &relativePath : requiredPaths) {
        if (!QFileInfo::exists(baseDir.filePath(relativePath))) {
            missing.append(relativePath);
        }
    }
    return missing;
}

RecordingCapabilityDiagnostics DependencyDiagnostics::checkRecordingCapabilities(
    bool requireBothEncoders,
    const std::function<bool(const QString &)> &factoryAvailable) {
    RecordingCapabilityDiagnostics result;
    for (const QString &factory : requiredRecordingFactories()) {
        if (!factoryAvailable(factory)) {
            result.missingFactories.append(factory);
        }
    }

    QStringList availableEncoders;
    for (const QString &encoder : {QString("mfh264enc"), QString("openh264enc")}) {
        if (factoryAvailable(encoder)) {
            availableEncoders.append(encoder);
        } else if (requireBothEncoders) {
            result.missingFactories.append(encoder);
        }
    }
    if (!availableEncoders.isEmpty()) {
        result.selectedEncoder = availableEncoders.constFirst();
    }
    const bool encoderRequirementMet = requireBothEncoders
        ? availableEncoders.size() == 2
        : !availableEncoders.isEmpty();
    result.canRecord = result.missingFactories.isEmpty() && encoderRequirementMet;
    if (!encoderRequirementMet && !requireBothEncoders) {
        result.missingFactories.append("mfh264enc");
        result.missingFactories.append("openh264enc");
    }
    return result;
}

RecordingCapabilityDiagnostics DependencyDiagnostics::checkRecordingCapabilities(
    bool requireBothEncoders) {
#if AIRPLAY_WITH_UXPLAY
    GError *error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        RecordingCapabilityDiagnostics result;
        result.missingFactories.append(
            error && error->message
                ? QString("GStreamer initialization: %1").arg(error->message)
                : QString("GStreamer initialization"));
        g_clear_error(&error);
        return result;
    }
    return checkRecordingCapabilities(requireBothEncoders, [](const QString &name) {
        GstElementFactory *factory = gst_element_factory_find(name.toUtf8().constData());
        if (factory == nullptr) {
            return false;
        }
        GstElement *element = gst_element_factory_create(factory, nullptr);
        gst_object_unref(factory);
        if (element == nullptr) {
            return false;
        }
        gst_object_unref(element);
        return true;
    });
#else
    Q_UNUSED(requireBothEncoders);
    RecordingCapabilityDiagnostics result;
    result.missingFactories.append("GStreamer recording support");
    return result;
#endif
}
