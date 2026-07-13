#include <QtTest/QtTest>

#include "backend/RecordingFileTransaction.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextStream>

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

QString lockPath(const QString &directory, const QString &id = kId) {
    return QDir(directory).filePath(QStringLiteral(".airplay-recording-%1.lock").arg(id));
}

QStringList temporaryPaths(const QString &directory, const QString &id) {
    return {
        QDir(directory).filePath(QStringLiteral(".airplay-recording-%1.video.mkv.part").arg(id)),
        QDir(directory).filePath(QStringLiteral(".airplay-recording-%1.mp4.part").arg(id)),
    };
}

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
class DirectoryAccessDeny {
public:
    DirectoryAccessDeny(const QString &path, DWORD permissions, DWORD inheritance)
        : m_path(QDir::toNativeSeparators(path).toStdWString()) {
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
        deny.grfAccessPermissions = permissions;
        deny.grfAccessMode = DENY_ACCESS;
        deny.grfInheritance = inheritance;
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

    ~DirectoryAccessDeny() {
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
    void reserveUsesBaseNameInEmptyDirectory() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const RecordingFileReservation reservation = reserveOrFail(directory.path());

        QCOMPARE(reservation.finalPath,
                 directory.filePath(QStringLiteral("AirPlay Recording 2026-07-13 09-08-07.mp4")));
        RecordingFileTransaction::discard(reservation);
    }

    void reserveUsesSecondNameWhenBaseExistsWithoutTouchingIt() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString base = directory.filePath(QStringLiteral("AirPlay Recording 2026-07-13 09-08-07.mp4"));
        writeBytes(base, "base bytes");

        const RecordingFileReservation reservation = reserveOrFail(directory.path());

        QCOMPARE(reservation.finalPath,
                 directory.filePath(QStringLiteral("AirPlay Recording 2026-07-13 09-08-07-2.mp4")));
        QCOMPARE(readBytes(base), QByteArray("base bytes"));
        RecordingFileTransaction::discard(reservation);
        QCOMPARE(readBytes(base), QByteArray("base bytes"));
    }

    void reserveUsesThirdNameWhenBaseAndSecondExistWithoutTouchingThem() {
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
        QCOMPARE(readBytes(base), QByteArray("base bytes"));
        QCOMPARE(readBytes(second), QByteArray("second bytes"));
    }

    void reserveCreatesOnlyRequiredVideoAndMp4TemporaryFiles() {
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
        for (const QString &path : {reservation.videoSpoolPath, reservation.temporaryMp4Path}) {
            const QFileInfo info(path);
            QVERIFY(info.isFile());
            QCOMPARE(info.size(), 0);
#ifdef Q_OS_WIN
            const DWORD attributes = GetFileAttributesW(QDir::toNativeSeparators(path).toStdWString().c_str());
            QVERIFY(attributes != INVALID_FILE_ATTRIBUTES);
            QVERIFY(attributes & FILE_ATTRIBUTE_HIDDEN);
#endif
        }
        QVERIFY(!QFileInfo::exists(reservation.audioSpoolPath));
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
        DirectoryAccessDeny deny(directory.path(), FILE_ADD_FILE, NO_INHERITANCE);
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

    void reserveReportsRealHideFailureAndCleansAllCreatedFiles() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
#ifdef Q_OS_WIN
        DirectoryAccessDeny deny(directory.path(), FILE_WRITE_ATTRIBUTES,
                                 SUB_OBJECTS_ONLY_INHERIT);
        QVERIFY2(deny.active(), qPrintable(deny.error()));
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral("Could not remove our own lock file.*")));

        const RecordingFileReservationResult result =
            RecordingFileTransaction::reserve(directory.path(), kLocalNow, kUuid);
        deny.restore();

        QVERIFY(!result.reservation.has_value());
        QVERIFY2(result.error.contains(QStringLiteral("hide"), Qt::CaseInsensitive),
                 qPrintable(result.error));
        QVERIFY2(result.error.contains(QStringLiteral("cleanup succeeded"), Qt::CaseInsensitive),
                 qPrintable(result.error));
        QVERIFY2(result.error.contains(
                     QStringLiteral(".airplay-recording-%1.video.mkv.part").arg(kId)),
                 qPrintable(result.error));
        QVERIFY2(result.error.contains(QStringLiteral("lock cleanup failed"), Qt::CaseInsensitive),
                 qPrintable(result.error));
        QStringList temporaryFiles = QDir(directory.path()).entryList(
            {QStringLiteral("*.part")}, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
        QVERIFY(temporaryFiles.isEmpty());
        QVERIFY(QFileInfo::exists(lockPath(directory.path())));
        QVERIFY(QFile::remove(lockPath(directory.path())));
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
        writeBytes(mp4, "foreign mp4");

        const RecordingFileReservationResult result =
            RecordingFileTransaction::reserve(directory.path(), kLocalNow, kUuid);

        QVERIFY(!result.reservation.has_value());
        QVERIFY(!result.error.isEmpty());
        QVERIFY(!QFileInfo::exists(video));
        QVERIFY(!QFileInfo::exists(audio));
        QCOMPARE(readBytes(mp4), QByteArray("foreign mp4"));
        QVERIFY(!QFileInfo::exists(lockPath(directory.path())));
    }

    void reserveRejectsForeignAudioPathWithoutTouchingForeignObject() {
        QTemporaryDir fileDirectory;
        QVERIFY(fileDirectory.isValid());
        const QString foreignAudio = fileDirectory.filePath(
            QStringLiteral(".airplay-recording-%1.audio.mka.part").arg(kId));
        writeBytes(foreignAudio, "foreign audio bytes");

        const RecordingFileReservationResult fileResult =
            RecordingFileTransaction::reserve(fileDirectory.path(), kLocalNow, kUuid);

        QVERIFY(!fileResult.reservation.has_value());
        QVERIFY2(fileResult.error.contains(foreignAudio), qPrintable(fileResult.error));
        QCOMPARE(readBytes(foreignAudio), QByteArray("foreign audio bytes"));
        for (const QString &path : temporaryPaths(fileDirectory.path(), kId)) {
            QVERIFY(!QFileInfo::exists(path));
        }
        QVERIFY(!QFileInfo::exists(lockPath(fileDirectory.path())));

        QTemporaryDir directoryDirectory;
        QVERIFY(directoryDirectory.isValid());
        const QString foreignAudioDirectory = directoryDirectory.filePath(
            QStringLiteral(".airplay-recording-%1.audio.mka.part").arg(kId));
        QVERIFY(QDir().mkdir(foreignAudioDirectory));

        const RecordingFileReservationResult directoryResult =
            RecordingFileTransaction::reserve(directoryDirectory.path(), kLocalNow, kUuid);

        QVERIFY(!directoryResult.reservation.has_value());
        QVERIFY2(directoryResult.error.contains(foreignAudioDirectory),
                 qPrintable(directoryResult.error));
        QVERIFY(QFileInfo(foreignAudioDirectory).isDir());
        for (const QString &path : temporaryPaths(directoryDirectory.path(), kId)) {
            QVERIFY(!QFileInfo::exists(path));
        }
        QVERIFY(!QFileInfo::exists(lockPath(directoryDirectory.path())));
    }

    void commitRejectsEmptyMp4ResultWithSpecificPathAndReason() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RecordingFileReservation empty = reserveOrFail(directory.path());
        const QString error = RecordingFileTransaction::commit(empty);
        QVERIFY2(error.contains(QStringLiteral("empty"), Qt::CaseInsensitive), qPrintable(error));
        QVERIFY2(error.contains(empty.temporaryMp4Path), qPrintable(error));
        QVERIFY(QFileInfo::exists(empty.temporaryMp4Path));
        RecordingFileTransaction::discard(empty);
    }

    void commitRejectsMissingMp4ResultWithSpecificPathAndReason() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        RecordingFileReservation missing = reserveOrFail(
            directory.path(), QUuid(QStringLiteral("11111111-1111-1111-1111-111111111111")));
        QVERIFY(QFile::remove(missing.temporaryMp4Path));
        const QString error = RecordingFileTransaction::commit(missing);
        QVERIFY2(error.contains(QStringLiteral("missing"), Qt::CaseInsensitive), qPrintable(error));
        QVERIFY2(!error.contains(QStringLiteral("not a regular file"), Qt::CaseInsensitive),
                 qPrintable(error));
        QVERIFY2(error.contains(missing.temporaryMp4Path), qPrintable(error));
        QVERIFY(!QFileInfo::exists(missing.finalPath));
        RecordingFileTransaction::discard(missing);
    }

    void commitRejectsNonRegularMp4ResultWithSpecificPathAndReason() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        RecordingFileReservation nonRegular = reserveOrFail(
            directory.path(), QUuid(QStringLiteral("22222222-2222-2222-2222-222222222222")));
        QVERIFY(QFile::remove(nonRegular.temporaryMp4Path));
        QVERIFY(QDir().mkdir(nonRegular.temporaryMp4Path));
        const QString error = RecordingFileTransaction::commit(nonRegular);
        QVERIFY2(error.contains(QStringLiteral("not a regular file"), Qt::CaseInsensitive),
                 qPrintable(error));
        QVERIFY2(!error.contains(QStringLiteral("missing"), Qt::CaseInsensitive), qPrintable(error));
        QVERIFY2(error.contains(nonRegular.temporaryMp4Path), qPrintable(error));
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
#ifdef Q_OS_WIN
        const std::wstring nativeTemporary =
            QDir::toNativeSeparators(reservation.temporaryMp4Path).toStdWString();
        const DWORD temporaryAttributes = GetFileAttributesW(nativeTemporary.c_str());
        QVERIFY(temporaryAttributes != INVALID_FILE_ATTRIBUTES);
        QVERIFY(SetFileAttributesW(nativeTemporary.c_str(),
                                   temporaryAttributes | FILE_ATTRIBUTE_HIDDEN |
                                       FILE_ATTRIBUTE_ARCHIVE));
#endif

        QCOMPARE(RecordingFileTransaction::commit(reservation), QString());

        QCOMPARE(readBytes(reservation.finalPath), QByteArray("final mp4"));
#ifdef Q_OS_WIN
        const DWORD finalAttributes = GetFileAttributesW(
            QDir::toNativeSeparators(reservation.finalPath).toStdWString().c_str());
        QVERIFY(finalAttributes != INVALID_FILE_ATTRIBUTES);
        QCOMPARE(finalAttributes & FILE_ATTRIBUTE_HIDDEN, DWORD(0));
        QVERIFY(finalAttributes & FILE_ATTRIBUTE_ARCHIVE);
#endif
        QVERIFY(!QFileInfo::exists(reservation.temporaryMp4Path));
        QVERIFY(!QFileInfo::exists(reservation.videoSpoolPath));
        QVERIFY(!QFileInfo::exists(reservation.audioSpoolPath));

        RecordingFileTransaction::discard(reservation);
        QCOMPARE(readBytes(reservation.finalPath), QByteArray("final mp4"));
    }

    void externalFinalCollisionPreservesBothFilesAndDiscardDoesNotDeleteExternalFinal() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const RecordingFileReservation reservation = reserveOrFail(directory.path());
        writeBytes(reservation.temporaryMp4Path, "new mp4");
        writeBytes(reservation.finalPath, "external mp4");

        const QString error = RecordingFileTransaction::commit(reservation);

        QVERIFY2(error.contains(reservation.finalPath), qPrintable(error));
        QCOMPARE(readBytes(reservation.finalPath), QByteArray("external mp4"));
        QCOMPARE(readBytes(reservation.temporaryMp4Path), QByteArray("new mp4"));

        RecordingFileTransaction::discard(reservation);
        QCOMPARE(readBytes(reservation.finalPath), QByteArray("external mp4"));
        QVERIFY(!QFileInfo::exists(reservation.temporaryMp4Path));
        QVERIFY(!QFileInfo::exists(reservation.videoSpoolPath));
        QVERIFY(!QFileInfo::exists(reservation.audioSpoolPath));
    }

    void discardWithMissingTempNeverDeletesExternalFinal() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const RecordingFileReservation reservation = reserveOrFail(directory.path());
        QVERIFY(QFile::remove(reservation.temporaryMp4Path));
        writeBytes(reservation.finalPath, "external survives");

        RecordingFileTransaction::discard(reservation);

        QCOMPARE(readBytes(reservation.finalPath), QByteArray("external survives"));
        QVERIFY(!QFileInfo::exists(reservation.videoSpoolPath));
        QVERIFY(!QFileInfo::exists(reservation.audioSpoolPath));
    }

    void commitCleanupFailureDoesNotPublishFinalAndCanBeRetried() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const RecordingFileReservation reservation = reserveOrFail(directory.path());
        writeBytes(reservation.temporaryMp4Path, "committed");
        QVERIFY(QFile::remove(reservation.videoSpoolPath));
        QVERIFY(QDir().mkdir(reservation.videoSpoolPath));

        const QString error = RecordingFileTransaction::commit(reservation);

        QVERIFY2(error.contains(QStringLiteral("cleanup"), Qt::CaseInsensitive), qPrintable(error));
        QVERIFY2(error.contains(QStringLiteral("failed"), Qt::CaseInsensitive), qPrintable(error));
        QVERIFY2(error.contains(reservation.videoSpoolPath), qPrintable(error));
        QVERIFY(!QFileInfo::exists(reservation.finalPath));
        QCOMPARE(readBytes(reservation.temporaryMp4Path), QByteArray("committed"));
#ifdef Q_OS_WIN
        const DWORD temporaryAttributes = GetFileAttributesW(
            QDir::toNativeSeparators(reservation.temporaryMp4Path).toStdWString().c_str());
        QVERIFY(temporaryAttributes != INVALID_FILE_ATTRIBUTES);
        QVERIFY(temporaryAttributes & FILE_ATTRIBUTE_HIDDEN);
#endif
        QVERIFY(QFileInfo(reservation.videoSpoolPath).isDir());
        QVERIFY(!QFileInfo::exists(reservation.audioSpoolPath));
        QDir(reservation.videoSpoolPath).removeRecursively();

        QCOMPARE(RecordingFileTransaction::commit(reservation), QString());
        QCOMPARE(readBytes(reservation.finalPath), QByteArray("committed"));
    }

    void renameFailureRestoresHiddenTemporaryMp4() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RecordingFileReservation reservation = reserveOrFail(directory.path());
        writeBytes(reservation.temporaryMp4Path, "retryable");
        reservation.finalPath = directory.filePath(QStringLiteral("missing/final.mp4"));

        const QString error = RecordingFileTransaction::commit(reservation);

        QVERIFY2(error.contains(reservation.finalPath), qPrintable(error));
        QVERIFY(!QFileInfo::exists(reservation.finalPath));
        QCOMPARE(readBytes(reservation.temporaryMp4Path), QByteArray("retryable"));
#ifdef Q_OS_WIN
        const DWORD attributes = GetFileAttributesW(
            QDir::toNativeSeparators(reservation.temporaryMp4Path).toStdWString().c_str());
        QVERIFY(attributes != INVALID_FILE_ATTRIBUTES);
        QVERIFY(attributes & FILE_ATTRIBUTE_HIDDEN);
#endif
        RecordingFileTransaction::discard(reservation);
    }

    void discardRemovesAllOwnedTemporaryFiles() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const RecordingFileReservation reservation = reserveOrFail(directory.path());
        writeBytes(reservation.audioSpoolPath, "optional audio");

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

    void cleanupEmptyOrWhitespaceDirectoryIsNoOpInsteadOfUsingCurrentDirectory() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString previousCurrent = QDir::currentPath();
        const QString stale = directory.filePath(QStringLiteral(
            ".airplay-recording-eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee.mp4.part"));
        writeBytes(stale, "must stay");
        QVERIFY(QDir::setCurrent(directory.path()));

        const QStringList emptyResult =
            RecordingFileTransaction::cleanupStaleTemporaryFiles(QString());
        const QStringList whitespaceResult =
            RecordingFileTransaction::cleanupStaleTemporaryFiles(QStringLiteral("   "));
        const bool restored = QDir::setCurrent(previousCurrent);

        QVERIFY(restored);
        QVERIFY(emptyResult.isEmpty());
        QVERIFY(whitespaceResult.isEmpty());
        QCOMPARE(readBytes(stale), QByteArray("must stay"));
    }

    void cleanupSkipsReservationHeldByCurrentProcessAndDiscardReleasesLock() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const RecordingFileReservation reservation = reserveOrFail(directory.path());

        const QStringList deleted =
            RecordingFileTransaction::cleanupStaleTemporaryFiles(directory.path());
        const bool allRequiredTempsRemain =
            QFileInfo::exists(reservation.videoSpoolPath) &&
            QFileInfo::exists(reservation.temporaryMp4Path);
        const bool optionalAudioAbsent = !QFileInfo::exists(reservation.audioSpoolPath);
        const bool lockExistsWhileReserved = QFileInfo::exists(lockPath(directory.path()));
#ifdef Q_OS_WIN
        const DWORD lockAttributes = GetFileAttributesW(
            QDir::toNativeSeparators(lockPath(directory.path())).toStdWString().c_str());
        const bool lockIsHidden = lockAttributes != INVALID_FILE_ATTRIBUTES &&
                                  (lockAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
#endif
        RecordingFileTransaction::discard(reservation);

        QVERIFY(deleted.isEmpty());
        QVERIFY(allRequiredTempsRemain);
        QVERIFY(optionalAudioAbsent);
        QVERIFY(lockExistsWhileReserved);
#ifdef Q_OS_WIN
        QVERIFY(lockIsHidden);
#endif
        QVERIFY(!QFileInfo::exists(lockPath(directory.path())));
    }

    void cleanupSkipsOtherProcessLiveLockThenReclaimsCrashStaleLock() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString helperId = QStringLiteral("dddddddddddddddddddddddddddddddd");
        const QString helperUuid = QStringLiteral("dddddddd-dddd-dddd-dddd-dddddddddddd");
        QProcess helper;
        helper.setProcessChannelMode(QProcess::MergedChannels);
        helper.start(QCoreApplication::applicationFilePath(),
                     {QStringLiteral("--recording-lock-helper"), directory.path(), helperUuid});
        const bool started = helper.waitForStarted(5000);
        const bool ready = started && helper.waitForReadyRead(5000);
        const QByteArray helperOutput = helper.readAll();
        if (!ready || !helperOutput.contains("READY")) {
            helper.kill();
            helper.waitForFinished(5000);
        }
        QVERIFY2(started && ready && helperOutput.contains("READY"), helperOutput.constData());

        const QStringList activeDeleted =
            RecordingFileTransaction::cleanupStaleTemporaryFiles(directory.path());
        const RecordingFileReservationResult competingReservation =
            RecordingFileTransaction::reserve(directory.path(), kLocalNow, QUuid(helperUuid));
        const QStringList expectedTemps = temporaryPaths(directory.path(), helperId);
        bool allTempsRemain = true;
        for (const QString &path : expectedTemps) allTempsRemain &= QFileInfo::exists(path);
        helper.kill();
        QVERIFY(helper.waitForFinished(5000));

        QVERIFY(activeDeleted.isEmpty());
        QVERIFY(!competingReservation.reservation.has_value());
        QVERIFY2(competingReservation.error.contains(QStringLiteral("lock"), Qt::CaseInsensitive),
                 qPrintable(competingReservation.error));
        QVERIFY(allTempsRemain);
        QStringList expectedDeleted = expectedTemps;
        expectedDeleted.sort();
        QCOMPARE(RecordingFileTransaction::cleanupStaleTemporaryFiles(directory.path()),
                 expectedDeleted);
        QVERIFY(!QFileInfo::exists(lockPath(directory.path(), helperId)));
    }

    void cleanupPreservesLiveLockWithoutPartsThenReclaimsOrphanLockOnly() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString helperId = QStringLiteral("ffffffffffffffffffffffffffffffff");
        const QString helperUuid = QStringLiteral("ffffffff-ffff-ffff-ffff-ffffffffffff");
        QProcess helper;
        helper.setProcessChannelMode(QProcess::MergedChannels);
        helper.start(QCoreApplication::applicationFilePath(),
                     {QStringLiteral("--recording-lock-helper"), directory.path(), helperUuid});
        const bool started = helper.waitForStarted(5000);
        const bool ready = started && helper.waitForReadyRead(5000);
        const QByteArray helperOutput = helper.readAll();
        if (!ready || !helperOutput.contains("READY")) {
            helper.kill();
            helper.waitForFinished(5000);
        }
        QVERIFY2(started && ready && helperOutput.contains("READY"), helperOutput.constData());

        for (const QString &path : temporaryPaths(directory.path(), helperId)) {
            QVERIFY2(QFile::remove(path), qPrintable(path));
        }
        const QString optionalAudio = directory.filePath(
            QStringLiteral(".airplay-recording-%1.audio.mka.part").arg(helperId));
        if (QFileInfo::exists(optionalAudio)) QVERIFY(QFile::remove(optionalAudio));
        const QString exactLock = lockPath(directory.path(), helperId);
        const QString nearLock = directory.filePath(QStringLiteral(
            ".airplay-recording-gggggggggggggggggggggggggggggggg.lock"));
        const QString suffixedLock = exactLock + QStringLiteral(".bak");
        writeBytes(nearLock, "near lock");
        writeBytes(suffixedLock, "suffixed lock");

        const QStringList liveDeleted =
            RecordingFileTransaction::cleanupStaleTemporaryFiles(directory.path());
        const bool liveLockRemained = QFileInfo::exists(exactLock);
        helper.kill();
        QVERIFY(helper.waitForFinished(5000));

        const QStringList orphanDeleted =
            RecordingFileTransaction::cleanupStaleTemporaryFiles(directory.path());

        QVERIFY(liveDeleted.isEmpty());
        QVERIFY(liveLockRemained);
        QVERIFY(orphanDeleted.isEmpty());
        QVERIFY(!QFileInfo::exists(exactLock));
        QCOMPARE(readBytes(nearLock), QByteArray("near lock"));
        QCOMPARE(readBytes(suffixedLock), QByteArray("suffixed lock"));
    }
};

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() == 4 && arguments.at(1) == QStringLiteral("--recording-lock-helper")) {
        const RecordingFileReservationResult result = RecordingFileTransaction::reserve(
            arguments.at(2), QDateTime::currentDateTime(), QUuid(arguments.at(3)));
        QTextStream output(stdout);
        if (!result.reservation.has_value()) {
            output << "ERROR: " << result.error << Qt::endl;
            return 2;
        }
        output << "READY" << Qt::endl;
        return application.exec();
    }
    RecordingFileTransactionTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "RecordingFileTransactionTest.moc"
