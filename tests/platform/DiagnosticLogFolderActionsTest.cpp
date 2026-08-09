#include <QtTest/QtTest>

#include "platform/DiagnosticLogFolderActions.h"

#include <QDir>

namespace {

class FakeLogFolderOperations {
public:
    DiagnosticLogFolderOperations operation() {
        return {
            [this](const QString &path) {
                createdTargets.append(path);
                return createError;
            },
            [this](const QUrl &url) {
                openedTargets.append(url.toLocalFile());
                return openError;
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

        QCOMPARE(actions.ensureAndOpen(), QString());
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

        QCOMPARE(actions.ensureAndOpen(), QString("access denied"));
        QCOMPARE(fake.createdTargets, QStringList{"C:/package/logs"});
        QVERIFY(fake.openedTargets.isEmpty());
        QVERIFY(!fake.createdTargets.join('\n').contains("AppData", Qt::CaseInsensitive));
        QVERIFY(!fake.createdTargets.join('\n').contains("Temp", Qt::CaseInsensitive));
    }
};

QTEST_MAIN(DiagnosticLogFolderActionsTest)
#include "DiagnosticLogFolderActionsTest.moc"
