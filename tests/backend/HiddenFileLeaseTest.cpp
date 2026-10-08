#include <QtTest>
#include "backend/HiddenFileLease.h"
#include "backend/GstFileLocation.h"
#include "platform/FileSystemPath.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <memory>

// QTemporaryDir reserves an exclusive directory; only that reservation is removed.
class OwnedFixtures {
    QString parent;
public:
    QString error;
    explicit OwnedFixtures(const QString &supplied)
        : parent(QDir::cleanPath(QFileInfo(supplied).absoluteFilePath())) {
        // The reserved basename has exactly 8 units (t- plus six random units).
        const int required = parent.size() + 1 + 8 + 1 + QStringLiteral("中文 空格").size() + 1 + 5;
        if (supplied.isEmpty() || required > 213) {
            error = QStringLiteral("Runtime parent cannot fit exact shortest target 213: parent units=%1, maximum=192; no fixture writes").arg(parent.size());
            return;
        }
        if (!QDir().mkpath(parent)) {
            error = QStringLiteral("Cannot create runtime parent: %1").arg(parent);
            return;
        }
        QTemporaryDir owned(parent + QStringLiteral("/t-XXXXXX"));
        owned.setAutoRemove(false);
        ownedPath = owned.path();
        if (!owned.isValid()) error = owned.errorString();
        else qInfo().noquote() << "owned_fixture=" + ownedPath;
    }
    QString ownedPath;
    QString path() const { return ownedPath; }
    bool cleanup() {
        if (ownedPath.isEmpty()) return true;
        const QFileInfo info(ownedPath);
        if (info.isSymLink() || info.absolutePath() != parent ||
            !info.fileName().startsWith(QStringLiteral("t-")) || info.fileName().size() != 8)
            return false;
        const bool removed = QDir(ownedPath).removeRecursively();
        if (removed) ownedPath.clear();
        return removed;
    }
    ~OwnedFixtures() { if (!cleanup()) qWarning("Owned fixture cleanup failed boundary validation or removal"); }
};


class HiddenFileLeaseTest : public QObject {
 Q_OBJECT
 std::unique_ptr<OwnedFixtures> fixtures;
private slots:
 void initTestCase() {
   fixtures = std::make_unique<OwnedFixtures>(qEnvironmentVariable("AIRPLAY_BOUNDARY_RUNTIME"));
   QVERIFY2(fixtures->error.isEmpty(), qPrintable(fixtures->error));
 }
 void cleanupTestCase() { QVERIFY(fixtures->cleanup()); }

 void fileBoundaryRejectsInvalidAndPreservesErrors() {
#ifdef Q_OS_WIN
   QString disk=QStringLiteral("C:/data/中文 空格/file.raw");
   QString unc=QStringLiteral("//server/share/中文/file.raw");
   for(const QString &path:{disk,unc}) {
     QString native=FileSystemPath::forIo(path);
     QVERIFY(!native.isEmpty());
     QString raw=QStringLiteral("error: %1; debug: %1; detail stays").arg(native);
     QCOMPARE(GstFileLocation::logicalError(raw,{path}),QStringLiteral("error: %1; debug: %1; detail stays").arg(path));
   }
   for(const QString &path:{QString(),QStringLiteral("//./NUL"),QStringLiteral("//?/GLOBALROOT/device"),QString(QChar(0))}) {
     QString error; QVERIFY(GstFileLocation::forIo(path,&error).isEmpty()); QVERIFY(!error.isEmpty());
   }
   QCOMPARE(GstFileLocation::logicalError(QStringLiteral("untouched warning/debug"),{disk}),QStringLiteral("untouched warning/debug"));
#endif
 }
 void longPathAttributes() {
#ifdef Q_OS_WIN
    QString root=fixtures->path(); QVERIFY(!root.isEmpty());
    for(int length:{213,259,260,275,325}) {
      QString dir=root+QStringLiteral("/属性 空格");
      while(dir.size()+70<length) dir+="/"+QString(40,'d');
      QVERIFY(QDir().mkpath(dir));
      QString path=dir+"/"+QString(length-dir.size()-5,'a')+".raw"; QCOMPARE(path.size(),length);
      QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write("sentinel"),qint64(8)); f.close();
      auto native=FileSystemPath::forIo(path).toStdWString();
      DWORD original=FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_ARCHIVE;
      QVERIFY(SetFileAttributesW(native.c_str(),original));
      {
        HiddenFileLease lease(path); QString error;
        QVERIFY2(lease.makeVisible(&error),qPrintable(error));
        QCOMPARE(GetFileAttributesW(native.c_str()),DWORD(FILE_ATTRIBUTE_ARCHIVE));
        QVERIFY2(lease.restore(&error),qPrintable(error));
        QCOMPARE(GetFileAttributesW(native.c_str()),original);
        QVERIFY(lease.restore(&error)); QCOMPARE(GetFileAttributesW(native.c_str()),original);
      }
      {
        HiddenFileLease lease(path); QString error; QVERIFY2(lease.makeVisible(&error),qPrintable(error));
      }
      QCOMPARE(GetFileAttributesW(native.c_str()),original);
      QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(),QByteArray("sentinel")); f.close();
      QVERIFY(QFile::remove(path));
    }
#endif
 }
 void rejectsInvalidAndMissing() {
   for(const QString &path:{QString(),QStringLiteral("//./NUL"),fixtures->path()+"/missing.raw"}) {
     HiddenFileLease lease(path); QString error; QVERIFY(!lease.makeVisible(&error)); QVERIFY(!error.isEmpty());
     QVERIFY(!error.contains(QStringLiteral("\\\\?\\"))); QVERIFY(lease.restore(&error));
   }
 }
 void restoreFailureRetainsLogicalPath() {
#ifdef Q_OS_WIN
   QString path=fixtures->path()+QStringLiteral("/restore 中文.raw");
   QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.close();
   auto native=FileSystemPath::forIo(path).toStdWString(); QVERIFY(SetFileAttributesW(native.c_str(),FILE_ATTRIBUTE_HIDDEN));
   HiddenFileLease lease(path); QString error; QVERIFY(lease.makeVisible(&error)); QVERIFY(QFile::remove(path));
   QVERIFY(!lease.restore(&error)); QVERIFY(error.contains(path)); QVERIFY(!error.contains(QStringLiteral("\\\\?\\")));
   QVERIFY(f.open(QIODevice::WriteOnly)); f.close(); QVERIFY(lease.restore(&error));
   QCOMPARE(GetFileAttributesW(native.c_str()),DWORD(FILE_ATTRIBUTE_HIDDEN)); QVERIFY(QFile::remove(path));
#endif
 }
};
QTEST_GUILESS_MAIN(HiddenFileLeaseTest)
#include "HiddenFileLeaseTest.moc"
