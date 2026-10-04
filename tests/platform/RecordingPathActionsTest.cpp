#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "app/LanguageManager.h"
#include "diagnostics/DiagnosticSession.h"
#include "platform/RecordingPathActions.h"

namespace {

class InactiveDiagnosticSink final : public DiagnosticLogSink {
public:
    void record(DiagnosticEvent event) override { events.append(std::move(event)); }
    bool isActive() const override { return false; }
    QList<DiagnosticEvent> events;
};

QByteArray readLog(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

DiagnosticSessionOptions sessionOptions(const QString &applicationDirectory) {
    DiagnosticSessionOptions options;
    options.applicationDirectory = applicationDirectory;
    options.activationSource = QStringLiteral("command_argument");
    return options;
}

} // namespace

class RecordingPathActionsTest : public QObject {
    Q_OBJECT

private slots:
    void ensureAndOpenDirectoryCreatesDirectoryAndUsesSeparatePathArgument() {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        auto created = DiagnosticSession::create(sessionOptions(temporaryDirectory.path()));
        QVERIFY2(created.session != nullptr, qPrintable(created.error));
        const QString directory = QDir(temporaryDirectory.path()).filePath("new/nested folder");
        QString launchedProgram;
        QStringList launchedArguments;
        WindowsRecordingPathActions actions(
            [&](const QString &program, const QStringList &arguments) {
                launchedProgram = program;
                launchedArguments = arguments;
                return true;
            }, created.session.get());

        const UiMessage error = actions.ensureAndOpenDirectory(directory);

        QVERIFY2(error.isEmpty(), qPrintable(error.render()));
        QVERIFY(QDir(directory).exists());
        QCOMPARE(launchedProgram, QString("explorer.exe"));
        QCOMPARE(launchedArguments,
                 QStringList({QDir::toNativeSeparators(
                     QFileInfo(directory).absoluteFilePath())}));
        QCOMPARE(readLog(created.session->filePath()).count('\n'), 1);
    }

    void revealFileUsesSelectSwitchAndAbsolutePathAsSeparateArguments() {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        auto created = DiagnosticSession::create(sessionOptions(temporaryDirectory.path()));
        QVERIFY2(created.session != nullptr, qPrintable(created.error));
        const QString filePath = QDir(temporaryDirectory.path()).filePath("finished video.mp4");
        QString launchedProgram;
        QStringList launchedArguments;
        WindowsRecordingPathActions actions(
            [&](const QString &program, const QStringList &arguments) {
                launchedProgram = program;
                launchedArguments = arguments;
                return true;
            }, created.session.get());

        const UiMessage error = actions.revealFile(filePath);

        QVERIFY2(error.isEmpty(), qPrintable(error.render()));
        QCOMPARE(launchedProgram, QString("explorer.exe"));
        QCOMPARE(launchedArguments,
                 QStringList({"/select,", QDir::toNativeSeparators(
                     QFileInfo(filePath).absoluteFilePath())}));
        QCOMPARE(readLog(created.session->filePath()).count('\n'), 1);
    }

    void processLaunchFailureReturnsVisibleErrorText() {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        WindowsRecordingPathActions actions(
            [](const QString &, const QStringList &) { return false; });

        const UiMessage openError = actions.ensureAndOpenDirectory(
            QDir(temporaryDirectory.path()).filePath("recordings"));
        const UiMessage revealError = actions.revealFile(
            QDir(temporaryDirectory.path()).filePath("finished.mp4"));

        QVERIFY(!openError.isEmpty());
        QVERIFY(!revealError.isEmpty());
    }

    void failuresWriteOnePathFreeWarning_data() {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<QString>("reason");
        QTest::newRow("create") << "create_directory" << "create_failed";
        QTest::newRow("open") << "open_directory" << "launcher_failed";
        QTest::newRow("reveal") << "reveal_recording" << "launcher_failed";
    }

    void failuresWriteOnePathFreeWarning() {
        QFETCH(QString, operation);
        QFETCH(QString, reason);
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        auto created = DiagnosticSession::create(sessionOptions(temporaryDirectory.path()));
        QVERIFY2(created.session != nullptr, qPrintable(created.error));
        QString target = temporaryDirectory.filePath("PrivateRecordingTarget/finished.mp4");
        if (operation == "create_directory") {
            QFile blocker(temporaryDirectory.filePath("PrivateRecordingBlocker"));
            QVERIFY(blocker.open(QIODevice::WriteOnly));
            blocker.close();
            target = blocker.fileName() + "/PrivateRecordingTarget";
        }
        int launchAttempts = 0;
        WindowsRecordingPathActions actions(
            [&](const QString &, const QStringList &) {
                ++launchAttempts;
                return false;
            }, created.session.get());

        const UiMessage error = operation == "reveal_recording"
            ? actions.revealFile(target) : actions.ensureAndOpenDirectory(target);

        QVERIFY(!error.isEmpty());
        QCOMPARE(launchAttempts, operation == "create_directory" ? 0 : 1);
        const QByteArray bytes = readLog(created.session->filePath());
        const QByteArray expected = QString(" WARN ui directory_action_failed area=recording operation=%1 reason=%2\n")
                                        .arg(operation, reason).toUtf8();
        QVERIFY2(bytes.contains(expected), bytes.constData());
        QCOMPARE(bytes.count(" ui directory_action_failed "), 1);
        QCOMPARE(bytes.count('\n'), 2);
        QVERIFY(!bytes.contains("PrivateRecordingTarget"));
        QVERIFY(!bytes.contains("PrivateRecordingBlocker"));
        QVERIFY(!bytes.contains(target.toUtf8()));
        QVERIFY(!bytes.contains(QDir::toNativeSeparators(target).toUtf8()));
        QVERIFY(!bytes.contains("Could not"));

        LanguageManager language(QCoreApplication::instance());
        QVERIFY(language.apply("zh-CN", QLocale("en-US")));
        QVERIFY(!error.render().isEmpty());
        QCOMPARE(readLog(created.session->filePath()), bytes);
        QVERIFY(language.apply("en", QLocale("en-US")));
        QVERIFY(!error.render().isEmpty());
        QCOMPARE(readLog(created.session->filePath()), bytes);
        QCOMPARE(launchAttempts, operation == "create_directory" ? 0 : 1);
    }

    void inactiveSinkDoesNotReceiveDirectoryFailures_data() {
        QTest::addColumn<QString>("operation");
        QTest::newRow("create") << "create_directory";
        QTest::newRow("open") << "open_directory";
        QTest::newRow("reveal") << "reveal_recording";
    }

    void inactiveSinkDoesNotReceiveDirectoryFailures() {
        QFETCH(QString, operation);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString target = directory.filePath("recordings/finished.mp4");
        if (operation == "create_directory") {
            QFile blocker(directory.filePath("blocker"));
            QVERIFY(blocker.open(QIODevice::WriteOnly));
            blocker.close();
            target = blocker.fileName() + "/recordings";
        }
        InactiveDiagnosticSink sink;
        int launchAttempts = 0;
        WindowsRecordingPathActions actions([&launchAttempts](const QString &, const QStringList &) {
            ++launchAttempts;
            return false;
        }, &sink);

        const UiMessage error = operation == "reveal_recording"
            ? actions.revealFile(target) : actions.ensureAndOpenDirectory(target);

        QVERIFY(!error.isEmpty());
        QCOMPARE(launchAttempts, operation == "create_directory" ? 0 : 1);
        QVERIFY(sink.events.isEmpty());
    }
};

QTEST_MAIN(RecordingPathActionsTest)
#include "RecordingPathActionsTest.moc"
