#include "platform/DependencyDiagnostics.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <limits>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

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

QStringList requiredUxPlayPlugins() {
    return {"app", "libav", "playback", "autodetect", "videoparsersbad"};
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

RuntimePathCompatibility DependencyDiagnostics::checkRuntimePathCompatibility(
    const QString &path,
    quint32 ansiCodePage,
    const std::function<bool(const QString &, quint32)> &roundTrips) {
    RuntimePathCompatibility result;
    result.hasNonAscii = std::any_of(path.cbegin(), path.cend(), [](QChar codeUnit) {
        return codeUnit.unicode() > 0x7f;
    });
    result.ansiCodePage = ansiCodePage;
    result.pathLength = path.size();
    result.compatible = path.isEmpty()
        || (roundTrips && roundTrips(path, ansiCodePage));
    return result;
}

RuntimePathCompatibility DependencyDiagnostics::checkRuntimePathCompatibility(
    const QString &path) {
#if defined(Q_OS_WIN)
    return checkRuntimePathCompatibility(path, GetACP());
#else
    return checkRuntimePathCompatibility(path, 0);
#endif
}

RuntimePathCompatibility DependencyDiagnostics::checkRuntimePathCompatibility(
    const QString &path,
    quint32 ansiCodePage) {
#if defined(Q_OS_WIN)
    if (path.size() > std::numeric_limits<int>::max()) {
        return checkRuntimePathCompatibility(path, ansiCodePage, {});
    }

    return checkRuntimePathCompatibility(path, ansiCodePage,
                                         [](const QString &originalPath,
                                            quint32 ansiCodePage) {
        const DWORD encodeFlags = ansiCodePage == CP_UTF8
            ? WC_ERR_INVALID_CHARS
            : WC_NO_BEST_FIT_CHARS;
        BOOL usedDefaultChar = FALSE;
        LPBOOL usedDefaultCharPointer = ansiCodePage == CP_UTF8
            ? nullptr
            : &usedDefaultChar;
        const int encodedLength = WideCharToMultiByte(
            ansiCodePage,
            encodeFlags,
            reinterpret_cast<LPCWCH>(originalPath.utf16()),
            static_cast<int>(originalPath.size()),
            nullptr,
            0,
            nullptr,
            usedDefaultCharPointer);
        if (encodedLength == 0 || usedDefaultChar) {
            return false;
        }

        QByteArray encodedPath(encodedLength, Qt::Uninitialized);
        if (WideCharToMultiByte(
                ansiCodePage,
                encodeFlags,
                reinterpret_cast<LPCWCH>(originalPath.utf16()),
                static_cast<int>(originalPath.size()),
                encodedPath.data(),
                encodedLength,
                nullptr,
                usedDefaultCharPointer) == 0
            || usedDefaultChar) {
            return false;
        }

        const DWORD decodeFlags = ansiCodePage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
        const int decodedLength = MultiByteToWideChar(
            ansiCodePage,
            decodeFlags,
            encodedPath.constData(),
            encodedLength,
            nullptr,
            0);
        if (decodedLength == 0) {
            return false;
        }

        QString decodedPath(decodedLength, Qt::Uninitialized);
        return MultiByteToWideChar(
                   ansiCodePage,
                   decodeFlags,
                   encodedPath.constData(),
                   encodedLength,
                   reinterpret_cast<wchar_t *>(decodedPath.data()),
                   decodedLength) != 0
            && decodedPath == originalPath;
    });
#else
    return checkRuntimePathCompatibility(path, ansiCodePage, {});
#endif
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

bool DependencyDiagnostics::configurePackageLocalGStreamerEnvironment(
    const QString &packageDirectory, const QString &privateRegistry) {
    if (privateRegistry.isEmpty() || !QDir::isAbsolutePath(privateRegistry)
        || !QDir(QDir(packageDirectory).filePath("gstreamer-plugins")).exists()) return false;
    const QString package=QDir(packageDirectory).absolutePath();
    const auto setPath=[](const char *name,const QString &path) { qputenv(name,QDir::toNativeSeparators(path).toUtf8()); };
    // An internal worker must not borrow host Gst DLLs, plugin directories or a default registry.
    const auto windows=qEnvironmentVariable("SystemRoot");
    setPath("PATH",package+QDir::listSeparator()+QDir(windows).filePath("System32")+QDir::listSeparator()+windows);
    for (const char *name:{"GST_PLUGIN_PATH","GST_PLUGIN_PATH_1_0"}) setPath(name,QDir(package).filePath("gstreamer-plugins"));
    for (const char *name:{"GST_PLUGIN_SYSTEM_PATH","GST_PLUGIN_SYSTEM_PATH_1_0"}) qputenv(name,"");
    for (const char *name:{"GST_REGISTRY","GST_REGISTRY_1_0"}) setPath(name,privateRegistry);
    for (const char *name:{"GST_PLUGIN_SCANNER","GST_PLUGIN_SCANNER_1_0"}) setPath(name,QDir(package).filePath("libexec/gstreamer-1.0/gst-plugin-scanner.exe"));
    return true;
}
GStreamerPluginReadiness DependencyDiagnostics::checkPackageGStreamerPluginReadiness(const QString &packageDirectory) {
#if AIRPLAY_WITH_UXPLAY
    GError *error=nullptr;
    if (!gst_init_check(nullptr,nullptr,&error)) {
        GStreamerPluginReadiness result;
        result.initializationError=error && error->message?QString::fromUtf8(error->message):QStringLiteral("GStreamer initialization failed");
        g_clear_error(&error); return result;
    }
    const auto directory=QDir(packageDirectory).filePath("gstreamer-plugins");
    return checkGStreamerPluginReadiness([&](const QString &name) {
        auto plugin=gst_plugin_load_by_name(name.toUtf8().constData());
        if (!plugin) return false;
        const auto source=gst_plugin_get_filename(plugin);
        const auto actual=source?QFileInfo(QString::fromUtf8(source)).canonicalFilePath():QString{};
        const bool local=!actual.isEmpty() && QFileInfo(actual).absolutePath().compare(QFileInfo(directory).canonicalFilePath(),Qt::CaseInsensitive)==0;
        gst_object_unref(plugin); return local;
    });
#else
    Q_UNUSED(packageDirectory)
    GStreamerPluginReadiness result; result.initializationError=QStringLiteral("GStreamer support is not built"); return result;
#endif
}

GStreamerPluginReadiness DependencyDiagnostics::checkGStreamerPluginReadiness(
    const std::function<bool(const QString &)> &pluginAvailable) {
    GStreamerPluginReadiness result;
    for (const QString &plugin : requiredUxPlayPlugins()) {
        if (!pluginAvailable || !pluginAvailable(plugin)) {
            result.missingPlugins.append(plugin);
        }
    }
    result.ready = result.missingPlugins.isEmpty();
    return result;
}

GStreamerPluginReadiness DependencyDiagnostics::checkGStreamerPluginReadiness() {
#if AIRPLAY_WITH_UXPLAY
    GError *error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        GStreamerPluginReadiness result;
        result.initializationError = error && error->message
            ? QString::fromUtf8(error->message)
            : QStringLiteral("GStreamer initialization failed");
        g_clear_error(&error);
        return result;
    }
    return checkGStreamerPluginReadiness([](const QString &name) {
        GstPlugin *plugin = gst_registry_find_plugin(
            gst_registry_get(), name.toUtf8().constData());
        if (!plugin) {
            return false;
        }
        gst_object_unref(plugin);
        return true;
    });
#else
    GStreamerPluginReadiness result;
    result.initializationError = QStringLiteral("GStreamer support is not built");
    return result;
#endif
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
