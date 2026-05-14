#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "platform/DependencyDiagnostics.h"

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

        QVERIFY(missing.contains("airplay_receiver.exe"));
        QVERIFY(missing.contains("Qt6Core.dll"));
        QVERIFY(missing.contains("gstreamer-plugins/libgstapp.dll"));
        QVERIFY(missing.contains("libqmdnsengine.dll"));
    }

    void acceptsCompleteStandaloneRuntimeFiles() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QStringList requiredPaths = {
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
            "libqmdnsengine.dll"
        };

        for (const QString &relativePath : requiredPaths) {
            const QString fullPath = QDir(dir.path()).filePath(relativePath);
            QVERIFY(QDir().mkpath(QFileInfo(fullPath).absolutePath()));
            QFile file(fullPath);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }

        QVERIFY(DependencyDiagnostics::checkStandaloneRuntime(dir.path()).isEmpty());
    }

    void skipsStandaloneRuntimeCheckInMsys2PathMode() {
        qunsetenv("AIRPLAY_MSYS2_PATH_MODE");
        QVERIFY(DependencyDiagnostics::shouldCheckStandaloneRuntime());

        qputenv("AIRPLAY_MSYS2_PATH_MODE", "1");
        QVERIFY(!DependencyDiagnostics::shouldCheckStandaloneRuntime());
        qunsetenv("AIRPLAY_MSYS2_PATH_MODE");
    }
};

QTEST_GUILESS_MAIN(DependencyDiagnosticsTest)
#include "DependencyDiagnosticsTest.moc"
