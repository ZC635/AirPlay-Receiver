#include <QtTest>

#include "app/AppSettings.h"
#include "app/RecordingStartupCleanup.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {
void writeFile(const QString &path) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("fixture");
}
}

class RecordingStartupCleanupTest : public QObject {
    Q_OBJECT

private slots:
    void removesOnlyExactAppTemporaryFilesFromConfiguredDirectory() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const QString staleVideo = directory.filePath(
            QString(".airplay-recording-%1.video.mkv.part").arg(id));
        const QString staleMp4 = directory.filePath(
            QString(".airplay-recording-%1.mp4.part").arg(id));
        const QString unrelated = directory.filePath("unrelated.mp4.part");
        writeFile(staleVideo);
        writeFile(staleMp4);
        writeFile(unrelated);
        AppSettings settings = AppSettings::defaults();
        settings.setRecordingOutputDirectory(directory.path());

        QStringList deleted = cleanupRecordingDirectoryAtStartup(settings);
        deleted.sort();
        QStringList expected{QDir::cleanPath(staleVideo), QDir::cleanPath(staleMp4)};
        expected.sort();

        QCOMPARE(deleted, expected);
        QVERIFY(QFileInfo::exists(unrelated));
    }
};

QTEST_GUILESS_MAIN(RecordingStartupCleanupTest)
#include "RecordingStartupCleanupTest.moc"
