#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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

        const QStringList requiredPaths = requiredStandaloneRuntimePaths();
        QVERIFY(!requiredPaths.isEmpty());
        QCOMPARE(missing, requiredPaths);
    }

    void acceptsCompleteStandaloneRuntimeFiles() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QStringList requiredPaths = requiredStandaloneRuntimePaths();
        QVERIFY(!requiredPaths.isEmpty());
        createStandaloneRuntimeFixture(dir.path(), requiredPaths);

        QVERIFY(DependencyDiagnostics::checkStandaloneRuntime(dir.path()).isEmpty());
    }

    void reportsMissingGStreamerRegistryInStandaloneRuntime() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QStringList requiredPaths = requiredStandaloneRuntimePaths();
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
};

QTEST_GUILESS_MAIN(DependencyDiagnosticsTest)
#include "DependencyDiagnosticsTest.moc"
