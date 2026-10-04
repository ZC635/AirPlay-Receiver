#include <QtTest/QtTest>

#include "platform/DiagnosticLogFolderActions.h"
#include "app/LanguageManager.h"
#include "diagnostics/DiagnosticSession.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {

class InactiveDiagnosticSink final : public DiagnosticLogSink {
public:
    void record(DiagnosticEvent event) override { events.append(std::move(event)); }
    bool isActive() const override { return false; }
    QList<DiagnosticEvent> events;
};

class FakeLogFolderOperations {
public:
    DiagnosticLogFolderOperations operation() {
        return {
            [this](const QString &path) {
                createdTargets.append(path);
                return UiMessage::raw(createError);
            },
            [this](const QUrl &url) {
                openedTargets.append(url.toLocalFile());
                return UiMessage::raw(openError);
            },
        };
    }

    QStringList createdTargets;
    QStringList openedTargets;
    QString createError;
    QString openError;
};

QString logPathFor(const QString &packagePath) {
    return QDir(packagePath).filePath("logs");
}

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

class DiagnosticLogFolderActionsTest : public QObject {
    Q_OBJECT

private slots:
    void createsAndOpensOnlyThePackageLocalLogFolder() {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        auto created = DiagnosticSession::create(sessionOptions(temporaryDirectory.path()));
        QVERIFY2(created.session != nullptr, qPrintable(created.error));
        FakeLogFolderOperations fake;

        DiagnosticLogFolderActions actions("C:/package", fake.operation(), created.session.get());

        QVERIFY(actions.ensureAndOpen().isEmpty());
        const QString expectedPath = logPathFor("C:/package");
        QCOMPARE(fake.createdTargets, QStringList{expectedPath});
        QCOMPARE(fake.openedTargets, QStringList{expectedPath});
        QVERIFY(!fake.createdTargets.join('\n').contains("AppData", Qt::CaseInsensitive));
        QVERIFY(!fake.createdTargets.join('\n').contains("Temp", Qt::CaseInsensitive));
        QVERIFY(!fake.openedTargets.join('\n').contains("AppData", Qt::CaseInsensitive));
        QVERIFY(!fake.openedTargets.join('\n').contains("Temp", Qt::CaseInsensitive));
        QCOMPARE(readLog(created.session->filePath()).count('\n'), 1);
    }

    void reportsCreationFailureWithoutOpeningOrFallback() {
        FakeLogFolderOperations fake;
        fake.createError = "access denied";

        DiagnosticLogFolderActions actions("C:/package", fake.operation());

        QCOMPARE(actions.ensureAndOpen().render(), QString("access denied"));
        QCOMPARE(fake.createdTargets, QStringList{"C:/package/logs"});
        QVERIFY(fake.openedTargets.isEmpty());
        QVERIFY(!fake.createdTargets.join('\n').contains("AppData", Qt::CaseInsensitive));
        QVERIFY(!fake.createdTargets.join('\n').contains("Temp", Qt::CaseInsensitive));
    }

    void cachedDefaultCreationFailureRetranslatesWithoutRetry() {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        QFile blocker(temporaryDirectory.filePath("blocker"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        int openAttempts = 0;
        DiagnosticLogFolderOperations operations;
        operations.openUrl = [&](const QUrl &) {
            ++openAttempts;
            return UiMessage{};
        };
        DiagnosticLogFolderActions actions(blocker.fileName(), operations);
        const QString logPath = blocker.fileName() + "/logs";
        const UiMessage error = actions.ensureAndOpen();
        QCOMPARE(error.render(), QString("Could not create diagnostic log folder: %1").arg(logPath));
        QVERIFY(QFile::remove(blocker.fileName()));

        LanguageManager language(QCoreApplication::instance());
        QVERIFY(language.apply("zh-CN", QLocale("en-US")));
        QCOMPARE(error.render(), QString::fromUtf8(u8"无法创建诊断日志文件夹：%1").arg(logPath));
        QVERIFY(language.apply("en", QLocale("en-US")));
        QCOMPARE(error.render(), QString("Could not create diagnostic log folder: %1").arg(logPath));
        QCOMPARE(openAttempts, 0);
        QVERIFY(!QDir(logPath).exists());
    }

    void rawOperationFailuresRemainUntranslated_data() {
        QTest::addColumn<bool>("creationFails");
        QTest::newRow("create") << true;
        QTest::newRow("open") << false;
    }

    void rawOperationFailuresRemainUntranslated() {
        QFETCH(bool, creationFails);
        const QString rawError = "Could not open diagnostic log folder: %1";
        FakeLogFolderOperations fake;
        if (creationFails) {
            fake.createError = rawError;
        } else {
            fake.openError = rawError;
        }
        DiagnosticLogFolderActions actions("C:/package", fake.operation());
        const UiMessage error = actions.ensureAndOpen();
        QCOMPARE(error.render(), rawError);

        LanguageManager language(QCoreApplication::instance());
        QVERIFY(language.apply("zh-CN", QLocale("en-US")));
        QCOMPARE(error.render(), rawError);
        QVERIFY(language.apply("en", QLocale("en-US")));
        QCOMPARE(error.render(), rawError);
        QCOMPARE(fake.createdTargets, QStringList{"C:/package/logs"});
        QCOMPARE(fake.openedTargets.size(), creationFails ? 0 : 1);
    }

    void failuresWriteOnePathFreeWarning_data() {
        QTest::addColumn<QString>("stage");
        QTest::newRow("default-create") << "default_create";
        QTest::newRow("callback-create") << "callback_create";
        QTest::newRow("callback-open") << "callback_open";
    }

    void failuresWriteOnePathFreeWarning() {
        QFETCH(QString, stage);
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        auto created = DiagnosticSession::create(sessionOptions(temporaryDirectory.path()));
        QVERIFY2(created.session != nullptr, qPrintable(created.error));
        QString applicationDirectory = temporaryDirectory.filePath("PrivateDiagnosticTarget");
        if (stage == "default_create") {
            QFile blocker(temporaryDirectory.filePath("PrivateDiagnosticBlocker"));
            QVERIFY(blocker.open(QIODevice::WriteOnly));
            blocker.close();
            applicationDirectory = blocker.fileName();
        }
        const QString rawError = "UnfilteredPrivateCallbackDetail: " + applicationDirectory;
        int createAttempts = 0;
        int openAttempts = 0;
        DiagnosticLogFolderOperations operations;
        if (stage != "default_create") {
            operations.createDirectory = [&](const QString &) {
                ++createAttempts;
                return stage == "callback_create" ? UiMessage::raw(rawError) : UiMessage{};
            };
        }
        operations.openUrl = [&](const QUrl &) {
            ++openAttempts;
            return stage == "callback_open" ? UiMessage::raw(rawError) : UiMessage{};
        };
        DiagnosticLogFolderActions actions(applicationDirectory, operations, created.session.get());

        const UiMessage error = actions.ensureAndOpen();

        QVERIFY(!error.isEmpty());
        const bool creationFailed = stage != "callback_open";
        QCOMPARE(createAttempts, stage == "default_create" ? 0 : 1);
        QCOMPARE(openAttempts, creationFailed ? 0 : 1);
        if (stage != "default_create") {
            QCOMPARE(error.render(), rawError);
        }
        const QByteArray bytes = readLog(created.session->filePath());
        const QByteArray expected = QString(" WARN ui directory_action_failed area=diagnostic_logs operation=%1 reason=%2\n")
                                        .arg(creationFailed ? "create_directory" : "open_directory",
                                             creationFailed ? "create_failed" : "launcher_failed").toUtf8();
        QVERIFY2(bytes.contains(expected), bytes.constData());
        QCOMPARE(bytes.count(" ui directory_action_failed "), 1);
        QCOMPARE(bytes.count('\n'), 2);
        QVERIFY(!bytes.contains("PrivateDiagnosticTarget"));
        QVERIFY(!bytes.contains("PrivateDiagnosticBlocker"));
        QVERIFY(!bytes.contains("UnfilteredPrivateCallbackDetail"));
        QVERIFY(!bytes.contains(applicationDirectory.toUtf8()));
        QVERIFY(!bytes.contains(QDir::toNativeSeparators(applicationDirectory).toUtf8()));
        QVERIFY(!bytes.contains("Could not"));

        LanguageManager language(QCoreApplication::instance());
        QVERIFY(language.apply("zh-CN", QLocale("en-US")));
        if (stage != "default_create") {
            QCOMPARE(error.render(), rawError);
        } else {
            QVERIFY(!error.render().isEmpty());
        }
        QCOMPARE(readLog(created.session->filePath()), bytes);
        QVERIFY(language.apply("en", QLocale("en-US")));
        QVERIFY(!error.render().isEmpty());
        QCOMPARE(readLog(created.session->filePath()), bytes);
        QCOMPARE(createAttempts, stage == "default_create" ? 0 : 1);
        QCOMPARE(openAttempts, creationFailed ? 0 : 1);
    }

    void inactiveSinkDoesNotReceiveDirectoryFailures_data() {
        QTest::addColumn<bool>("creationFails");
        QTest::newRow("create") << true;
        QTest::newRow("open") << false;
    }

    void inactiveSinkDoesNotReceiveDirectoryFailures() {
        QFETCH(bool, creationFails);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString applicationDirectory = directory.filePath("package");
        if (creationFails) {
            QFile blocker(directory.filePath("blocker"));
            QVERIFY(blocker.open(QIODevice::WriteOnly));
            blocker.close();
            applicationDirectory = blocker.fileName();
        }
        InactiveDiagnosticSink sink;
        int openAttempts = 0;
        DiagnosticLogFolderOperations operations;
        operations.openUrl = [&openAttempts](const QUrl &) {
            ++openAttempts;
            return UiMessage::raw(QStringLiteral("open failure"));
        };
        DiagnosticLogFolderActions actions(applicationDirectory, operations, &sink);

        const UiMessage error = actions.ensureAndOpen();

        QVERIFY(!error.isEmpty());
        QCOMPARE(openAttempts, creationFails ? 0 : 1);
        QVERIFY(sink.events.isEmpty());
    }
};

QTEST_MAIN(DiagnosticLogFolderActionsTest)
#include "DiagnosticLogFolderActionsTest.moc"
