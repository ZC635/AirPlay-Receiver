#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QTemporaryDir>
#include <QTextStream>

#include "platform/DependencyDiagnostics.h"

namespace {
QStringList requiredStandaloneRuntimePaths() {
    const QString relativeManifestPath = "config/portable-runtime-manifest.txt";
    const QString appDirPath = QCoreApplication::applicationDirPath();
    const QString currentDirPath = QDir::currentPath();
    const QStringList candidatePaths = {
        QDir(appDirPath).filePath(relativeManifestPath),
        QDir(appDirPath).filePath("../" + relativeManifestPath),
        QDir(appDirPath).filePath("../../" + relativeManifestPath),
        QDir(currentDirPath).filePath(relativeManifestPath),
        QDir(currentDirPath).filePath("../" + relativeManifestPath),
        QDir(currentDirPath).filePath("../../" + relativeManifestPath)
    };

    for (const QString &candidatePath : candidatePaths) {
        QFile file(candidatePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }

        QStringList paths;
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            QString line = stream.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#')) {
                continue;
            }
            paths.append(line.replace('\\', '/'));
        }
        if (!paths.isEmpty()) {
            return paths;
        }
    }

    return {};
}

QStringList playbackStandaloneRuntimePaths() {
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

class EnvironmentSnapshot {
public:
    explicit EnvironmentSnapshot(QStringList names) {
        for (const QString &name : names) {
            values_.insert(name, qEnvironmentVariable(name.toUtf8().constData()));
            wasSet_.insert(name, qEnvironmentVariableIsSet(name.toUtf8().constData()));
        }
    }

    ~EnvironmentSnapshot() {
        for (auto it = values_.cbegin(); it != values_.cend(); ++it) {
            const QByteArray name = it.key().toUtf8();
            if (wasSet_.value(it.key())) {
                qputenv(name.constData(), it.value().toUtf8());
            } else {
                qunsetenv(name.constData());
            }
        }
    }

private:
    QHash<QString, QString> values_;
    QHash<QString, bool> wasSet_;
};

void createStandaloneRuntimeFixture(const QString &root, const QStringList &requiredPaths) {
    for (const QString &relativePath : requiredPaths) {
        const QString fullPath = QDir(root).filePath(relativePath);
        QVERIFY(QDir().mkpath(QFileInfo(fullPath).absolutePath()));
        QFile file(fullPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
}
}

class DependencyDiagnosticsTest : public QObject {
    Q_OBJECT

private slots:
    void reportsMissingExecutable() {
        const auto result = DependencyDiagnostics::checkExecutable("definitely-not-installed-airplay-tool");
        QVERIFY(!result.ok);
        QVERIFY(result.message.contains("not found"));
    }

    void reportsEnvironmentVariableState() {
        qputenv("AIRPLAY_DIAGNOSTICS_TEST_VARIABLE", "1");
        const auto present = DependencyDiagnostics::checkEnvironmentVariable("AIRPLAY_DIAGNOSTICS_TEST_VARIABLE");
        QVERIFY(present.ok);

        qunsetenv("AIRPLAY_DIAGNOSTICS_TEST_VARIABLE");
        const auto missing = DependencyDiagnostics::checkEnvironmentVariable("AIRPLAY_DIAGNOSTICS_TEST_VARIABLE");
        QVERIFY(!missing.ok);
        QVERIFY(missing.message.contains("not set"));
    }

    void reportsRuntimeBasicsHints() {
        const auto messages = DependencyDiagnostics::checkRuntimeBasics();
        QVERIFY(messages.join('\n').contains("GStreamer"));
        QVERIFY(messages.join('\n').contains("QMdnsEngine"));
        QVERIFY(messages.join('\n').contains("UxPlay"));
    }

    void reportsMissingStandaloneRuntimeFiles() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const auto missing = DependencyDiagnostics::checkStandaloneRuntime(dir.path());

        QCOMPARE(missing, playbackStandaloneRuntimePaths());
    }

    void acceptsCompleteStandaloneRuntimeFiles() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QStringList requiredPaths = playbackStandaloneRuntimePaths();
        createStandaloneRuntimeFixture(dir.path(), requiredPaths);

        QVERIFY(DependencyDiagnostics::checkStandaloneRuntime(dir.path()).isEmpty());
    }

    void reportsMissingGStreamerRegistryInStandaloneRuntime() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QStringList requiredPaths = playbackStandaloneRuntimePaths();
        QVERIFY(requiredPaths.removeOne("gstreamer-1.0/registry.x86_64.bin"));
        createStandaloneRuntimeFixture(dir.path(), requiredPaths);

        const auto missing = DependencyDiagnostics::checkStandaloneRuntime(dir.path());

        QCOMPARE(missing, QStringList({"gstreamer-1.0/registry.x86_64.bin"}));
    }

    void skipsStandaloneRuntimeCheckInMsys2PathMode() {
        qunsetenv("AIRPLAY_MSYS2_PATH_MODE");
        QVERIFY(DependencyDiagnostics::shouldCheckStandaloneRuntime());

        qputenv("AIRPLAY_MSYS2_PATH_MODE", "1");
        QVERIFY(!DependencyDiagnostics::shouldCheckStandaloneRuntime());
        qunsetenv("AIRPLAY_MSYS2_PATH_MODE");
    }

    void normalRecordingCapabilityAcceptsEitherEncoder() {
        const QStringList available{
            "matroskamux", "matroskademux", "mp4mux", "h264parse",
            "avenc_aac", "aacparse", "appsrc", "appsink", "videoconvert",
            "audioconvert", "audioresample", "capsfilter", "filesrc",
            "filesink", "identity", "openh264enc"};

        const RecordingCapabilityDiagnostics result =
            DependencyDiagnostics::checkRecordingCapabilities(
                false, [&](const QString &factory) {
                    return available.contains(factory);
                });

        QVERIFY(result.canRecord);
        QCOMPARE(result.selectedEncoder, QString("openh264enc"));
        QVERIFY(result.missingFactories.isEmpty());
    }

    void strictRecordingCapabilityRequiresBothEncoders() {
        const QStringList available{
            "matroskamux", "matroskademux", "mp4mux", "h264parse",
            "avenc_aac", "aacparse", "appsrc", "appsink", "videoconvert",
            "audioconvert", "audioresample", "capsfilter", "filesrc",
            "filesink", "identity", "openh264enc"};

        const RecordingCapabilityDiagnostics result =
            DependencyDiagnostics::checkRecordingCapabilities(
                true, [&](const QString &factory) {
                    return available.contains(factory);
                });

        QVERIFY(!result.canRecord);
        QCOMPARE(result.selectedEncoder, QString("openh264enc"));
        QCOMPARE(result.missingFactories, QStringList{"mfh264enc"});
    }

    void recordingCapabilityReportsMissingMuxerSeparatelyFromPlayback() {
        const RecordingCapabilityDiagnostics result =
            DependencyDiagnostics::checkRecordingCapabilities(
                false, [](const QString &factory) {
                    return factory != "mp4mux";
                });

        QVERIFY(!result.canRecord);
        QVERIFY(result.missingFactories.contains("mp4mux"));
    }

    void manifestListsRecordingPluginsAndCodecRuntime() {
        const QStringList manifest = requiredStandaloneRuntimePaths();
        const QStringList recordingPaths{
            "gstreamer-plugins/libgstcoreelements.dll",
            "gstreamer-plugins/libgstapp.dll",
            "gstreamer-plugins/libgstvideoconvertscale.dll",
            "gstreamer-plugins/libgstaudioconvert.dll",
            "gstreamer-plugins/libgstaudioresample.dll",
            "gstreamer-plugins/libgstvideoparsersbad.dll",
            "gstreamer-plugins/libgstaudioparsers.dll",
            "gstreamer-plugins/libgstisomp4.dll",
            "gstreamer-plugins/libgstmatroska.dll",
            "gstreamer-plugins/libgstmediafoundation.dll",
            "gstreamer-plugins/libgstopenh264.dll",
            "gstreamer-plugins/libgstlibav.dll",
            "libopenh264-7.dll",
        };
        for (const QString &path : recordingPaths) {
            QVERIFY2(manifest.contains(path), qPrintable(path));
        }
    }

    void packageLocalGStreamerEnvironmentOverridesExternalPaths() {
        const QStringList names{
            "GST_PLUGIN_PATH", "GST_PLUGIN_PATH_1_0",
            "GST_PLUGIN_SYSTEM_PATH", "GST_PLUGIN_SYSTEM_PATH_1_0",
            "GST_REGISTRY", "GST_REGISTRY_1_0",
            "GST_PLUGIN_SCANNER", "GST_PLUGIN_SCANNER_1_0"};
        EnvironmentSnapshot restore(names);
        for (const QString &name : names) {
            qputenv(name.toUtf8().constData(), "C:/external-msys2");
        }
        QTemporaryDir package;
        QVERIFY(package.isValid());
        QVERIFY(QDir().mkpath(package.filePath("gstreamer-plugins")));

        QVERIFY(DependencyDiagnostics::configurePackageLocalGStreamerEnvironment(
            package.path()));

        const QString plugins = QDir::toNativeSeparators(
            package.filePath("gstreamer-plugins"));
        const QString registry = QDir::toNativeSeparators(
            package.filePath("gstreamer-1.0/registry.x86_64.bin"));
        const QString scanner = QDir::toNativeSeparators(
            package.filePath("libexec/gstreamer-1.0/gst-plugin-scanner.exe"));
        QCOMPARE(qEnvironmentVariable("GST_PLUGIN_PATH"), plugins);
        QCOMPARE(qEnvironmentVariable("GST_PLUGIN_PATH_1_0"), plugins);
        QCOMPARE(qEnvironmentVariable("GST_PLUGIN_SYSTEM_PATH"), plugins);
        QCOMPARE(qEnvironmentVariable("GST_PLUGIN_SYSTEM_PATH_1_0"), plugins);
        QCOMPARE(qEnvironmentVariable("GST_REGISTRY"), registry);
        QCOMPARE(qEnvironmentVariable("GST_REGISTRY_1_0"), registry);
        QCOMPARE(qEnvironmentVariable("GST_PLUGIN_SCANNER"), scanner);
        QCOMPARE(qEnvironmentVariable("GST_PLUGIN_SCANNER_1_0"), scanner);
    }

    void developmentModeWithoutPackagePluginDirectoryPreservesEnvironment() {
        const QStringList names{
            "GST_PLUGIN_PATH", "GST_PLUGIN_PATH_1_0",
            "GST_PLUGIN_SYSTEM_PATH", "GST_PLUGIN_SYSTEM_PATH_1_0",
            "GST_REGISTRY", "GST_REGISTRY_1_0",
            "GST_PLUGIN_SCANNER", "GST_PLUGIN_SCANNER_1_0"};
        EnvironmentSnapshot restore(names);
        for (const QString &name : names) {
            qputenv(name.toUtf8().constData(), QByteArray("sentinel-") + name.toUtf8());
        }
        QTemporaryDir developmentBuild;
        QVERIFY(developmentBuild.isValid());

        QVERIFY(!DependencyDiagnostics::configurePackageLocalGStreamerEnvironment(
            developmentBuild.path()));

        for (const QString &name : names) {
            QCOMPARE(qEnvironmentVariable(name.toUtf8().constData()),
                     QString("sentinel-%1").arg(name));
        }
    }
};

QTEST_GUILESS_MAIN(DependencyDiagnosticsTest)
#include "DependencyDiagnosticsTest.moc"
