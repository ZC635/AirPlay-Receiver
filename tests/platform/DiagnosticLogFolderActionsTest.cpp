#include <QtTest/QtTest>

#include "platform/DiagnosticLogFolderActions.h"
#include "app/LanguageManager.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {

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

} // namespace

class DiagnosticLogFolderActionsTest : public QObject {
    Q_OBJECT

private slots:
    void createsAndOpensOnlyThePackageLocalLogFolder() {
        FakeLogFolderOperations fake;

        DiagnosticLogFolderActions actions("C:/package", fake.operation());

        QVERIFY(actions.ensureAndOpen().isEmpty());
        const QString expectedPath = logPathFor("C:/package");
        QCOMPARE(fake.createdTargets, QStringList{expectedPath});
        QCOMPARE(fake.openedTargets, QStringList{expectedPath});
        QVERIFY(!fake.createdTargets.join('\n').contains("AppData", Qt::CaseInsensitive));
        QVERIFY(!fake.createdTargets.join('\n').contains("Temp", Qt::CaseInsensitive));
        QVERIFY(!fake.openedTargets.join('\n').contains("AppData", Qt::CaseInsensitive));
        QVERIFY(!fake.openedTargets.join('\n').contains("Temp", Qt::CaseInsensitive));
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
};

QTEST_MAIN(DiagnosticLogFolderActionsTest)
#include "DiagnosticLogFolderActionsTest.moc"
