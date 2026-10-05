#include <QtTest>
#include <QMessageBox>
#include <QTimer>
#include <QTemporaryDir>

#include "support/MemoryDiagnosticStorage.h"

#define main diagnosticFailurePresentationTestMain
#include "app/main.cpp"
#undef main

namespace {
using DiagnosticTestSupport::MemoryStorage;

DiagnosticLifecycleDependencies memoryDependencies(const std::shared_ptr<MemoryStorage> &storage) {
    DiagnosticLifecycleDependencies dependencies;
    dependencies.sessionStorage = storage;
    return dependencies;
}

DiagnosticLifecycleStart enabledStart(const QString &directory, int &runtimeReports) {
    DiagnosticLifecycleStart start;
    start.applicationDirectory = directory;
    start.activation = {true, DiagnosticActivationSource::CommandArgument};
    start.reportFailure = [&runtimeReports](DiagnosticFailure failure) {
        ++runtimeReports;
        showDiagnosticFailureWarning(failure);
    };
    return start;
}

// A repeating timer observes and acknowledges the real modal box, including any
// unexpected duplicate. It lives through late queue processing in each test.
class DialogObserver final {
public:
    DialogObserver() {
        QObject::connect(&timer, &QTimer::timeout, [&] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                auto *box = qobject_cast<QMessageBox *>(widget);
                if (!box || !box->isVisible())
                    continue;
                titles.append(box->windowTitle());
                messages.append(box->text());
                icons.append(box->icon());
                qInfo().noquote() << "Observed dialog" << messages.size()
                                  << "title:" << box->windowTitle() << "body:" << box->text();
                box->accept();
            }
        });
        timer.start(1);
    }
    QStringList titles;
    QStringList messages;
    QList<QMessageBox::Icon> icons;
private:
    QTimer timer;
};
}

class DiagnosticFailurePresentationTest final : public QObject {
    Q_OBJECT
private slots:
    void startupAbortCombinesUnreportedFailure() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto storage = std::make_shared<MemoryStorage>();
        int runtimeReports = 0;
        DiagnosticLifecycle diagnostics(qApp, memoryDependencies(storage));
        QVERIFY(diagnostics.start(enabledStart(directory.path(), runtimeReports)).creationError.isEmpty());
        storage->file->failWrites = true;
        qWarning("startup warning through the real Qt diagnostic bridge");
        DialogObserver dialogs;
        const QString mainError = QStringLiteral("Required runtime file missing: libgstapp.dll");
        showStartupFailure(diagnostics, "missing_runtime", "Dependencies missing", mainError);
        QCOMPARE(dialogs.messages.size(), 1);
        QCOMPARE(dialogs.titles.front(), QStringLiteral("Dependencies missing"));
        QCOMPARE(dialogs.icons.front(), QMessageBox::Critical);
        QVERIFY(dialogs.messages.front().contains(mainError));
        QVERIFY(dialogs.messages.front().contains("Diagnostic log may be incomplete: disk full"));
        QCoreApplication::processEvents();
        QCOMPARE(dialogs.messages.size(), 1);
        QCOMPARE(runtimeReports, 0);
        QCOMPARE(diagnostics.sink(), &nullDiagnosticLogSink());
        QVERIFY(!storage->file->bytes.contains("normal_exit=yes"));
    }

    void abortWriteFailureAppearsInSameDialog() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto storage = std::make_shared<MemoryStorage>();
        int runtimeReports = 0;
        DiagnosticLifecycle diagnostics(qApp, memoryDependencies(storage));
        QVERIFY(diagnostics.start(enabledStart(directory.path(), runtimeReports)).creationError.isEmpty());
        // The abort marker itself is the first failing write.
        storage->file->failWriteAt = storage->file->writes + 1;
        DialogObserver dialogs;
        showStartupFailure(diagnostics, "gstreamer_plugin_load_failure", "Plugins unavailable",
                           "Required plugins: app, libav");
        QCOMPARE(dialogs.messages.size(), 1);
        QVERIFY(dialogs.messages.front().contains("Required plugins: app, libav"));
        QVERIFY(dialogs.messages.front().contains("disk full"));
        QCoreApplication::processEvents();
        QCOMPARE(dialogs.messages.size(), 1);
        QCOMPARE(runtimeReports, 0);
        QVERIFY(!storage->file->bytes.contains("normal_exit=yes"));
    }

    void creationFailureCombinesWithStartupError() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto storage = std::make_shared<MemoryStorage>();
        storage->file->ensureError = QStringLiteral("access denied");
        int runtimeReports = 0;
        DiagnosticLifecycle diagnostics(qApp, memoryDependencies(storage));
        QCOMPARE(diagnostics.start(enabledStart(directory.path(), runtimeReports)).creationError,
                 QStringLiteral("access denied"));
        DialogObserver dialogs;
        showStartupFailure(diagnostics, "unsupported_application_path", "Unsupported application path",
                           "Move AirPlay to C:\\AirPlay");
        QCOMPARE(dialogs.messages.size(), 1);
        QVERIFY(dialogs.messages.front().contains("Move AirPlay to C:\\AirPlay"));
        QVERIFY(dialogs.messages.front().contains("Diagnostic logging could not be started: access denied"));
        QCoreApplication::processEvents();
        QCOMPARE(dialogs.messages.size(), 1);
        QCOMPARE(runtimeReports, 0);
    }

    void normalExitShowsOneWarningWithoutQueuePump_data() {
        QTest::addColumn<bool>("chinese");
        QTest::newRow("English") << false;
        QTest::newRow("Chinese") << true;
    }

    void normalExitShowsOneWarningWithoutQueuePump() {
        QFETCH(bool, chinese);
        LanguageManager language(qApp);
        QVERIFY(language.apply(chinese ? QStringLiteral("zh-CN") : QStringLiteral("en"), QLocale("en-US")));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto storage = std::make_shared<MemoryStorage>();
        int runtimeReports = 0;
        DiagnosticLifecycle diagnostics(qApp, memoryDependencies(storage));
        QVERIFY(diagnostics.start(enabledStart(directory.path(), runtimeReports)).creationError.isEmpty());
        DiagnosticLogSink *borrowed = diagnostics.sink();
        DialogObserver dialogs;
        bool receiverStopped = false;
        finishDiagnosticSessionForExit(diagnostics, [&] {
            receiverStopped = true;
            // shutdown_started succeeded; failure occurs during the final flush.
            storage->file->failFlush = true;
        });
        QVERIFY(receiverStopped);
        QCOMPARE(dialogs.messages.size(), 1);
        QCOMPARE(dialogs.titles.front(), chinese ? QString::fromUtf8(u8"诊断日志已停止")
                                                : QStringLiteral("Diagnostic logging stopped"));
        QCOMPARE(dialogs.icons.front(), QMessageBox::Warning);
        QCOMPARE(dialogs.messages.front(), chinese ? QString::fromUtf8(u8"诊断日志未能完整保存：disk full")
            : QStringLiteral("Diagnostic log could not be fully saved: disk full"));
        QCOMPARE(runtimeReports, 0);
        QCOMPARE(diagnostics.sink(), borrowed);
        QVERIFY(!borrowed->isActive());
        QCoreApplication::processEvents();
        QCOMPARE(dialogs.messages.size(), 1);
    }

    void lateQueueAfterDialogDoesNotRepeat() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto storage = std::make_shared<MemoryStorage>();
        int runtimeReports = 0;
        DiagnosticLifecycle diagnostics(qApp, memoryDependencies(storage));
        QVERIFY(diagnostics.start(enabledStart(directory.path(), runtimeReports)).creationError.isEmpty());
        diagnostics.enableFailureReporting();
        storage->file->failWrites = true;
        qWarning("pending runtime warning through the real Qt diagnostic bridge");
        DialogObserver dialogs;
        finishDiagnosticSessionForExit(diagnostics, {});
        QCOMPARE(dialogs.messages.size(), 1);
        QVERIFY(dialogs.messages.front().contains("disk full"));
        QCoreApplication::processEvents();
        QTest::qWait(10);
        finishDiagnosticSessionForExit(diagnostics, {});
        QCOMPARE(dialogs.messages.size(), 1);
        QCOMPARE(runtimeReports, 0);
    }

    void mergedFailurePreservesMainErrorAndLocalizedDetails_data() {
        QTest::addColumn<bool>("writeFailure");
        QTest::newRow("creation") << false;
        QTest::newRow("write") << true;
    }

    void mergedFailurePreservesMainErrorAndLocalizedDetails() {
        QFETCH(bool, writeFailure);
        LanguageManager language(qApp);
        QVERIFY(language.apply(QStringLiteral("zh-CN"), QLocale("en-US")));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto storage = std::make_shared<MemoryStorage>();
        const QString detail = QString::fromUtf8(u8"拒绝访问 C:/日志/失败.log\n第二行：原始错误");
        if (!writeFailure)
            storage->file->ensureError = detail;
        int runtimeReports = 0;
        DiagnosticLifecycle diagnostics(qApp, memoryDependencies(storage));
        QCOMPARE(diagnostics.start(enabledStart(directory.path(), runtimeReports)).creationError,
                 writeFailure ? QString{} : detail);
        if (writeFailure)
            storage->file->failWrites = true;
        DialogObserver dialogs;
        const QString mainError = QString::fromUtf8(u8"缺少运行文件：\nC:/应用/libgstapp.dll\n请重新解压。");
        const QString title = QString::fromUtf8(u8"依赖项缺失");
        showStartupFailure(diagnostics, "missing_runtime", title, mainError);
        QCOMPARE(dialogs.messages.size(), 1);
        QCOMPARE(dialogs.titles.front(), title);
        QCOMPARE(dialogs.messages.front(), mainError + "\n\n" + (writeFailure
            ? QString::fromUtf8(u8"诊断日志可能不完整：disk full")
            : QString::fromUtf8(u8"无法启动诊断日志：") + detail));
        QCoreApplication::processEvents();
        QCOMPARE(dialogs.messages.size(), 1);
        QCOMPARE(runtimeReports, 0);
    }
};

QTEST_MAIN(DiagnosticFailurePresentationTest)
#include "DiagnosticFailurePresentationTest.moc"
