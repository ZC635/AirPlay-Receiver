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
        "gstreamer-plugins/libgstcoreelements.dll",
        "gstreamer-plugins/libgstplayback.dll",
        "gstreamer-plugins/libgstautodetect.dll",
        "gstreamer-plugins/libgstvideoconvertscale.dll",
        "gstreamer-plugins/libgstaudioconvert.dll",
        "gstreamer-plugins/libgstaudioresample.dll",
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
    return standaloneRuntimeSnapshot(directory).missingRelativePaths;
}

StandaloneRuntimeSnapshot DependencyDiagnostics::standaloneRuntimeSnapshot(const QString &directory) {
    StandaloneRuntimeSnapshot snapshot;
    snapshot.relativePaths = playbackRuntimePaths();
    const QDir baseDir(directory);
    for (const QString &relativePath : snapshot.relativePaths) {
        if (!QFileInfo::exists(baseDir.filePath(relativePath))) {
            snapshot.missingRelativePaths.append(relativePath);
        }
    }
    snapshot.complete = snapshot.missingRelativePaths.isEmpty();
    return snapshot;
}

bool DependencyDiagnostics::configurePackageLocalGStreamerEnvironment(
    const QString &applicationDirectory) {
    const QString applicationPath = QDir::toNativeSeparators(
        QDir::cleanPath(QDir(applicationDirectory).absolutePath()));
    const QDir applicationDir(applicationPath);
    const QString pluginDirectory = applicationDir.filePath("gstreamer-plugins");
    if (!QDir(pluginDirectory).exists()) {
        return false;
    }

    QStringList pathEntries;
    const QString currentPath = qEnvironmentVariable("PATH");
    if (!currentPath.isEmpty()) {
        pathEntries = currentPath.split(QDir::listSeparator(), Qt::KeepEmptyParts);
    }
    for (qsizetype index = pathEntries.size() - 1; index >= 0; --index) {
        const QString candidate = QDir::toNativeSeparators(
            QDir::cleanPath(pathEntries.at(index).trimmed()));
        if (candidate.compare(applicationPath, Qt::CaseInsensitive) == 0) {
            pathEntries.removeAt(index);
        }
    }
    pathEntries.prepend(applicationPath);
    qputenv("PATH", pathEntries.join(QDir::listSeparator()).toUtf8());

    const auto setPath = [](const char *name, const QString &path) {
        qputenv(name, QDir::toNativeSeparators(path).toUtf8());
    };
    setPath("GST_PLUGIN_PATH", pluginDirectory);
    setPath("GST_PLUGIN_PATH_1_0", pluginDirectory);
    setPath("GST_PLUGIN_SYSTEM_PATH", pluginDirectory);
    setPath("GST_PLUGIN_SYSTEM_PATH_1_0", pluginDirectory);
    const QString registry = applicationDir.filePath(
        "gstreamer-1.0/registry.x86_64.bin");
    setPath("GST_REGISTRY", registry);
    setPath("GST_REGISTRY_1_0", registry);
    const QString scanner = applicationDir.filePath(
        "libexec/gstreamer-1.0/gst-plugin-scanner.exe");
    setPath("GST_PLUGIN_SCANNER", scanner);
    setPath("GST_PLUGIN_SCANNER_1_0", scanner);
    return true;
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
