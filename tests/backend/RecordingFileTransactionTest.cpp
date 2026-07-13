#include <QtTest/QtTest>

#include "backend/RecordingFileTransaction.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <string>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#endif

namespace {

const QDateTime kLocalNow(QDate(2026, 7, 13), QTime(9, 8, 7));
const QUuid kUuid(QStringLiteral("01234567-89ab-cdef-0123-456789abcdef"));
const QString kId = QStringLiteral("0123456789abcdef0123456789abcdef");

void writeBytes(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(file.errorString()));
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();
}

QByteArray readBytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

RecordingFileReservation reserveOrFail(const QString &directory, const QUuid &uuid = kUuid) {
    const RecordingFileReservationResult result =
        RecordingFileTransaction::reserve(directory, kLocalNow, uuid);
    if (!result.reservation.has_value()) {
        QTest::qFail(qPrintable(result.error), __FILE__, __LINE__);
        return {};
    }
    return *result.reservation;
}

#ifdef Q_OS_WIN
class DirectoryWriteDeny {
public:
    explicit DirectoryWriteDeny(const QString &path) : m_path(QDir::toNativeSeparators(path).toStdWString()) {
        const DWORD query = GetNamedSecurityInfoW(
            m_path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &m_originalDacl, nullptr, &m_descriptor);
        if (query != ERROR_SUCCESS) {
            m_error = QStringLiteral("GetNamedSecurityInfoW failed: %1").arg(query);
            return;
        }
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        if (!GetSecurityDescriptorControl(m_descriptor, &control, &revision)) {
            m_error = QStringLiteral("GetSecurityDescriptorControl failed: %1").arg(GetLastError());
            return;
        }
        m_wasProtected = (control & SE_DACL_PROTECTED) != 0;

        SID_IDENTIFIER_AUTHORITY worldAuthority = SECURITY_WORLD_SID_AUTHORITY;
        if (!AllocateAndInitializeSid(&worldAuthority, 1, SECURITY_WORLD_RID,
                                     0, 0, 0, 0, 0, 0, 0, &m_worldSid)) {
            m_error = QStringLiteral("AllocateAndInitializeSid failed: %1").arg(GetLastError());
            return;
        }
        EXPLICIT_ACCESSW deny{};
        deny.grfAccessPermissions = FILE_ADD_FILE;
        deny.grfAccessMode = DENY_ACCESS;
        deny.grfInheritance = NO_INHERITANCE;
        deny.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        deny.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        deny.Trustee.ptstrName = static_cast<LPWSTR>(m_worldSid);
        const DWORD aclResult = SetEntriesInAclW(1, &deny, m_originalDacl, &m_deniedDacl);
        if (aclResult != ERROR_SUCCESS) {
            m_error = QStringLiteral("SetEntriesInAclW failed: %1").arg(aclResult);
            return;
        }
        const DWORD setResult = SetNamedSecurityInfoW(
            m_path.data(), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, m_deniedDacl, nullptr);
        if (setResult != ERROR_SUCCESS) {
            m_error = QStringLiteral("SetNamedSecurityInfoW deny failed: %1").arg(setResult);
            return;
        }
        m_active = true;
    }

    ~DirectoryWriteDeny() {
        restore();
        if (m_deniedDacl) LocalFree(m_deniedDacl);
        if (m_worldSid) FreeSid(m_worldSid);
        if (m_descriptor) LocalFree(m_descriptor);
    }

    bool active() const { return m_active; }
    QString error() const { return m_error; }

    void restore() {
        if (!m_active) return;
        const SECURITY_INFORMATION protection =
            m_wasProtected ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
        SetNamedSecurityInfoW(m_path.data(), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | protection,
                              nullptr, nullptr, m_originalDacl, nullptr);
        m_active = false;
    }

private:
    std::wstring m_path;
    PSECURITY_DESCRIPTOR m_descriptor = nullptr;
    PACL m_originalDacl = nullptr;
    PACL m_deniedDacl = nullptr;
    PSID m_worldSid = nullptr;
    bool m_wasProtected = false;
    bool m_active = false;
    QString m_error;
};
#endif

} // namespace

class RecordingFileTransactionTest : public QObject {
    Q_OBJECT

private slots:
    void reserveUsesTimestampAndNextAvailableSuffixWithoutTouchingExistingFiles() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString base = directory.filePath(QStringLiteral("AirPlay Recording 2026-07-13 09-08-07.mp4"));
        const QString second = directory.filePath(QStringLiteral("AirPlay Recording 2026-07-13 09-08-07-2.mp4"));
        writeBytes(base, "base bytes");
        writeBytes(second, "second bytes");

        const RecordingFileReservation reservation = reserveOrFail(directory.path());

        QCOMPARE(reservation.finalPath,
                 directory.filePath(QStringLiteral("AirPlay Recording 2026-07-13 09-08-07-3.mp4")));
        QCOMPARE(readBytes(base), QByteArray("base bytes"));
        QCOMPARE(readBytes(second), QByteArray("second bytes"));
        QVERIFY(!QFileInfo::exists(reservation.finalPath));
        RecordingFileTransaction::discard(reservation);
    }

    void reserveCreatesDirectoryAndThreeExclusiveTemporaryFiles() {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString output = parent.filePath(QStringLiteral("new/nested/output"));

        const RecordingFileReservation reservation = reserveOrFail(output);

        QCOMPARE(QFileInfo(reservation.videoSpoolPath).absolutePath(), QDir(output).absolutePath());
        QCOMPARE(QFileInfo(reservation.audioSpoolPath).absolutePath(), QDir(output).absolutePath());
        QCOMPARE(QFileInfo(reservation.temporaryMp4Path).absolutePath(), QDir(output).absolutePath());
        QCOMPARE(QFileInfo(reservation.videoSpoolPath).fileName(),
                 QStringLiteral(".airplay-recording-%1.video.mkv.part").arg(kId));
        QCOMPARE(QFileInfo(reservation.audioSpoolPath).fileName(),
                 QStringLiteral(".airplay-recording-%1.audio.mka.part").arg(kId));
        QCOMPARE(QFileInfo(reservation.temporaryMp4Path).fileName(),
                 QStringLiteral(".airplay-recording-%1.mp4.part").arg(kId));
        for (const QString &path : {reservation.videoSpoolPath, reservation.audioSpoolPath,
                                    reservation.temporaryMp4Path}) {
            const QFileInfo info(path);
            QVERIFY(info.isFile());
            QCOMPARE(info.size(), 0);
#ifdef Q_OS_WIN
            const DWORD attributes = GetFileAttributesW(QDir::toNativeSeparators(path).toStdWString().c_str());
            QVERIFY(attributes != INVALID_FILE_ATTRIBUTES);
            QVERIFY(attributes & FILE_ATTRIBUTE_HIDDEN);
#endif
        }
        RecordingFileTransaction::discard(reservation);
    }

    void reserveFailsWhenParentIsFile() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString parentFile = directory.filePath(QStringLiteral("not-a-directory"));
        writeBytes(parentFile, "keep");

        const RecordingFileReservationResult result = RecordingFileTransaction::reserve(
            QDir(parentFile).filePath(QStringLiteral("child")), kLocalNow, kUuid);

        QVERIFY(!result.reservation.has_value());
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(readBytes(parentFile), QByteArray("keep"));
    }

    void reserveFailsInActuallyUnwritableDirectory() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
#ifdef Q_OS_WIN
        DirectoryWriteDeny deny(directory.path());
        QVERIFY2(deny.active(), qPrintable(deny.error()));
        const RecordingFileReservationResult result =
            RecordingFileTransaction::reserve(directory.path(), kLocalNow, kUuid);
        deny.restore();

        QVERIFY(!result.reservation.has_value());
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size(), 0);
#else
        const QFileDevice::Permissions original = QFileInfo(directory.path()).permissions();
        QVERIFY(QFile::setPermissions(directory.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const RecordingFileReservationResult result =
            RecordingFileTransaction::reserve(directory.path(), kLocalNow, kUuid);
        QFile::setPermissions(directory.path(), original);
        QVERIFY(!result.reservation.has_value());
        QVERIFY(!result.error.isEmpty());
#endif
    }

    void reserveCleansPartialCreationAndPreservesBlockingFile() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString video = directory.filePath(
            QStringLiteral(".airplay-recording-%1.video.mkv.part").arg(kId));
        const QString audio = directory.filePath(
            QStringLiteral(".airplay-recording-%1.audio.mka.part").arg(kId));
        const QString mp4 = directory.filePath(
            QStringLiteral(".airplay-recording-%1.mp4.part").arg(kId));
        writeBytes(audio, "foreign");

        const RecordingFileReservationResult result =
            RecordingFileTransaction::reserve(directory.path(), kLocalNow, kUuid);

        QVERIFY(!result.reservation.has_value());
        QVERIFY(!result.error.isEmpty());
        QVERIFY(!QFileInfo::exists(video));
        QCOMPARE(readBytes(audio), QByteArray("foreign"));
        QVERIFY(!QFileInfo::exists(mp4));
    }

    void commitRejectsMissingEmptyAndNonRegularMp4Results() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RecordingFileReservation empty = reserveOrFail(directory.path());
        QVERIFY(!RecordingFileTransaction::commit(empty).isEmpty());
        QVERIFY(QFileInfo::exists(empty.temporaryMp4Path));
        RecordingFileTransaction::discard(empty);

        RecordingFileReservation missing = reserveOrFail(
            directory.path(), QUuid(QStringLiteral("11111111-1111-1111-1111-111111111111")));
        QVERIFY(QFile::remove(missing.temporaryMp4Path));
        QVERIFY(!RecordingFileTransaction::commit(missing).isEmpty());
        QVERIFY(!QFileInfo::exists(missing.finalPath));
        RecordingFileTransaction::discard(missing);

        RecordingFileReservation nonRegular = reserveOrFail(
            directory.path(), QUuid(QStringLiteral("22222222-2222-2222-2222-222222222222")));
        QVERIFY(QFile::remove(nonRegular.temporaryMp4Path));
        QVERIFY(QDir().mkdir(nonRegular.temporaryMp4Path));
        QVERIFY(!RecordingFileTransaction::commit(nonRegular).isEmpty());
        QVERIFY(!QFileInfo::exists(nonRegular.finalPath));
        QDir(nonRegular.temporaryMp4Path).removeRecursively();
        RecordingFileTransaction::discard(nonRegular);
    }

    void commitRenamesWithoutOverwriteAndDeletesSpools() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const RecordingFileReservation reservation = reserveOrFail(directory.path());
        writeBytes(reservation.videoSpoolPath, "video spool");
        writeBytes(reservation.audioSpoolPath, "audio spool");
        writeBytes(reservation.temporaryMp4Path, "final mp4");

        QCOMPARE(RecordingFileTransaction::commit(reservation), QString());

        QCOMPARE(readBytes(reservation.finalPath), QByteArray("final mp4"));
        QVERIFY(!QFileInfo::exists(reservation.temporaryMp4Path));
        QVERIFY(!QFileInfo::exists(reservation.videoSpoolPath));
        QVERIFY(!QFileInfo::exists(reservation.audioSpoolPath));

        RecordingFileTransaction::discard(reservation);
        QVERIFY(!QFileInfo::exists(reservation.finalPath));
    }

    void externalFinalCollisionPreservesBothFilesAndDiscardDoesNotDeleteExternalFinal() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const RecordingFileReservation reservation = reserveOrFail(directory.path());
        writeBytes(reservation.temporaryMp4Path, "new mp4");
        writeBytes(reservation.finalPath, "external mp4");

        const QString error = RecordingFileTransaction::commit(reservation);

        QVERIFY(!error.isEmpty());
        QCOMPARE(readBytes(reservation.finalPath), QByteArray("external mp4"));
        QCOMPARE(readBytes(reservation.temporaryMp4Path), QByteArray("new mp4"));

        RecordingFileTransaction::discard(reservation);
        QCOMPARE(readBytes(reservation.finalPath), QByteArray("external mp4"));
        QVERIFY(!QFileInfo::exists(reservation.temporaryMp4Path));
        QVERIFY(!QFileInfo::exists(reservation.videoSpoolPath));
        QVERIFY(!QFileInfo::exists(reservation.audioSpoolPath));
    }

    void commitReportsSpoolCleanupFailureWithoutUndoingFinal() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const RecordingFileReservation reservation = reserveOrFail(directory.path());
        writeBytes(reservation.temporaryMp4Path, "committed");
        QVERIFY(QFile::remove(reservation.videoSpoolPath));
        QVERIFY(QDir().mkdir(reservation.videoSpoolPath));

        const QString error = RecordingFileTransaction::commit(reservation);

        QVERIFY(!error.isEmpty());
        QCOMPARE(readBytes(reservation.finalPath), QByteArray("committed"));
        QVERIFY(!QFileInfo::exists(reservation.temporaryMp4Path));
        QVERIFY(QFileInfo(reservation.videoSpoolPath).isDir());
        QDir(reservation.videoSpoolPath).removeRecursively();
        RecordingFileTransaction::discard(reservation);
    }

    void discardRemovesAllOwnedTemporaryFiles() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const RecordingFileReservation reservation = reserveOrFail(directory.path());

        RecordingFileTransaction::discard(reservation);

        QVERIFY(!QFileInfo::exists(reservation.videoSpoolPath));
        QVERIFY(!QFileInfo::exists(reservation.audioSpoolPath));
        QVERIFY(!QFileInfo::exists(reservation.temporaryMp4Path));
        QVERIFY(!QFileInfo::exists(reservation.finalPath));
    }

    void cleanupDeletesOnlyExactRegularTemporaryFilePatternsInSortedOrder() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString id = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        const QStringList exactNames{
            QStringLiteral(".airplay-recording-%1.video.mkv.part").arg(id),
            QStringLiteral(".airplay-recording-%1.audio.mka.part").arg(id),
            QStringLiteral(".airplay-recording-%1.mp4.part").arg(id),
        };
        for (const QString &name : exactNames) writeBytes(directory.filePath(name), "stale");
        const QStringList keepNames{
            QStringLiteral("ordinary.mp4"),
            QStringLiteral("other.part"),
            QStringLiteral(".airplay-recording-BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB.mp4.part"),
            QStringLiteral(".airplay-recording-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.mp4.part"),
            QStringLiteral(".airplay-recording-%1.video.mp4.part").arg(id),
            QStringLiteral(".airplay-recording-%1.audio.mka.part.bak").arg(id),
        };
        for (const QString &name : keepNames) writeBytes(directory.filePath(name), "keep");
        const QString exactDirectory = directory.filePath(exactNames.first());
        QVERIFY(QFile::remove(exactDirectory));
        QVERIFY(QDir().mkdir(exactDirectory));

        QStringList expectedDeleted{
            QDir::cleanPath(directory.filePath(exactNames.at(1))),
            QDir::cleanPath(directory.filePath(exactNames.at(2))),
        };
        expectedDeleted.sort();
        const QStringList deleted = RecordingFileTransaction::cleanupStaleTemporaryFiles(directory.path());

        QCOMPARE(deleted, expectedDeleted);
        QVERIFY(QFileInfo(exactDirectory).isDir());
        for (const QString &name : keepNames) {
            QVERIFY2(readBytes(directory.filePath(name)) == QByteArray("keep"), qPrintable(name));
        }
    }
};

QTEST_GUILESS_MAIN(RecordingFileTransactionTest)
#include "RecordingFileTransactionTest.moc"
