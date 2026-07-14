#include <QtTest/QtTest>

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include "platform/RecordingPathActions.h"

class RecordingPathActionsTest : public QObject {
    Q_OBJECT

private slots:
    void ensureAndOpenDirectoryCreatesDirectoryAndUsesSeparatePathArgument() {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QString directory = QDir(temporaryDirectory.path()).filePath("new/nested folder");
        QString launchedProgram;
        QStringList launchedArguments;
        WindowsRecordingPathActions actions(
            [&](const QString &program, const QStringList &arguments) {
                launchedProgram = program;
                launchedArguments = arguments;
                return true;
            });

        const QString error = actions.ensureAndOpenDirectory(directory);

        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QDir(directory).exists());
        QCOMPARE(launchedProgram, QString("explorer.exe"));
        QCOMPARE(launchedArguments,
                 QStringList({QDir::toNativeSeparators(
                     QFileInfo(directory).absoluteFilePath())}));
    }

    void revealFileUsesSelectSwitchAndAbsolutePathAsSeparateArguments() {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QString filePath = QDir(temporaryDirectory.path()).filePath("finished video.mp4");
        QString launchedProgram;
        QStringList launchedArguments;
        WindowsRecordingPathActions actions(
            [&](const QString &program, const QStringList &arguments) {
                launchedProgram = program;
                launchedArguments = arguments;
                return true;
            });

        const QString error = actions.revealFile(filePath);

        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(launchedProgram, QString("explorer.exe"));
        QCOMPARE(launchedArguments,
                 QStringList({"/select,", QDir::toNativeSeparators(
                     QFileInfo(filePath).absoluteFilePath())}));
    }

    void processLaunchFailureReturnsVisibleErrorText() {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        WindowsRecordingPathActions actions(
            [](const QString &, const QStringList &) { return false; });

        const QString openError = actions.ensureAndOpenDirectory(
            QDir(temporaryDirectory.path()).filePath("recordings"));
        const QString revealError = actions.revealFile(
            QDir(temporaryDirectory.path()).filePath("finished.mp4"));

        QVERIFY(!openError.isEmpty());
        QVERIFY(!revealError.isEmpty());
    }
};

QTEST_MAIN(RecordingPathActionsTest)
#include "RecordingPathActionsTest.moc"
