#include "platform/GStreamerCacheStorage.h"
#include "platform/FileSystemPath.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
constexpr qint64 RecordLimit = 1024 * 1024;
const QByteArray LockMarker("AIRPLAY-GSTREAMER-REGISTRY-LOCK/1\n");
QString registry(const QString &root) { return root + "/gstreamer-1.0/registry.x86_64.bin"; }
QString recordPath(const QString &root) { return root + "/gstreamer-1.0/registry.x86_64.validation.json"; }
QString identity(const QString &path) { return FileSystemPath::absolute(path).toCaseFolded(); }
QString nativeError(const QString &operation, const QString &path) {
    return QString("%1: %2 (Win32 %3)").arg(operation, path).arg(GetLastError());
}
LPCWSTR wide(const QString &value) { return reinterpret_cast<LPCWSTR>(value.utf16()); }
bool fail(QString *error, const QString &reason) { if (error) *error=reason; return false; }

bool safePath(const QString &path, bool allowMissing, QString *error) {
    QString cursor=FileSystemPath::absolute(path);
    if (cursor.isEmpty()) return fail(error,"Unsupported path: "+path);
    bool first=true;
    while (!cursor.isEmpty()) {
        const auto io=FileSystemPath::forIo(cursor);
        const DWORD attributes=GetFileAttributesW(wide(io));
        if (attributes==INVALID_FILE_ATTRIBUTES) {
            const auto code=GetLastError();
            if (!allowMissing || (code!=ERROR_FILE_NOT_FOUND && code!=ERROR_PATH_NOT_FOUND))
                return fail(error,nativeError("Path inaccessible",cursor));
        } else {
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                return fail(error,"Reparse object: "+cursor);
            if (!first && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
                return fail(error,"Non-directory ancestor: "+cursor);
        }
        const auto parent=QFileInfo(cursor).absolutePath();
        if (parent==cursor) break;
        cursor=parent; first=false;
    }
    return true;
}
bool ordinaryFile(const QString &path, QString *error) {
    if (!safePath(path,false,error)) return false;
    const auto io=FileSystemPath::forIo(path);
    const DWORD attributes=GetFileAttributesW(wide(io));
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return fail(error,"Expected ordinary file: "+path);
    return true;
}
QString canonicalPackageDirectory(const QString &path,QString *error) {
    const auto logical=FileSystemPath::absolute(path);
    if (logical.isEmpty() || !safePath(logical,false,error))
        { fail(error,"Package directory unavailable: "+logical); return {}; }
    const auto io=FileSystemPath::forIo(logical);
    HANDLE handle=CreateFileW(wide(io),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (handle==INVALID_HANDLE_VALUE) { fail(error,nativeError("Open package identity",logical)); return {}; }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ordinary=GetFileInformationByHandle(handle,&info)
        && (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT);
    const DWORD length=ordinary?GetFinalPathNameByHandleW(handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS):0;
    std::vector<wchar_t> bytes(length+1);
    const DWORD written=length?GetFinalPathNameByHandleW(handle,bytes.data(),DWORD(bytes.size()),
        FILE_NAME_NORMALIZED|VOLUME_NAME_DOS):0;
    const auto reason=nativeError("Resolve package identity",logical);
    CloseHandle(handle);
    if (!written || written>=bytes.size()) { fail(error,reason); return {}; }
    const auto actual=FileSystemPath::absolute(QString::fromWCharArray(bytes.data(),int(written)));
    if (!safePath(actual,false,error)) return {};
    return actual;
}
QByteArray readBounded(const QString &path, qint64 limit, QString *error) {
    if (!ordinaryFile(path,error)) return {};
    QFile file(FileSystemPath::forIo(path));
    if (!file.open(QIODevice::ReadOnly)) { fail(error,file.errorString()+": "+path); return {}; }
    if (file.size()>limit) { fail(error,"File exceeds limit: "+path); return {}; }
    const auto result=file.read(limit+1);
    if (file.error()!=QFileDevice::NoError || result.size()>limit)
        { fail(error,"Incomplete file read: "+path); return {}; }
    return result;
}
bool hashFile(const QString &path, QByteArray *result, QString *error, CacheReadStatus *status = nullptr) {
    if(status)*status=CacheReadStatus::Rejected;
    if (!ordinaryFile(path,error)) return false;
    const auto io=FileSystemPath::forIo(path);
    HANDLE handle=CreateFileW(wide(io),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if (handle==INVALID_HANDLE_VALUE) {if(status)*status=CacheReadStatus::Unavailable;return fail(error,nativeError("Read input",path));}
    BY_HANDLE_FILE_INFORMATION before{},after{};
    bool ok=GetFileInformationByHandle(handle,&before) && !(before.dwFileAttributes &
        (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    char bytes[64*1024]; DWORD length=0; quint64 total=0;
    while (ok) {
        if (!ReadFile(handle,bytes,sizeof(bytes),&length,nullptr)) { if(status)*status=CacheReadStatus::Unavailable;ok=false; break; }
        if (!length) break;
        hash.addData(QByteArrayView(bytes,length)); total+=length;
    }
    ok=ok && GetFileInformationByHandle(handle,&after)
        && before.dwVolumeSerialNumber==after.dwVolumeSerialNumber
        && before.nFileIndexHigh==after.nFileIndexHigh && before.nFileIndexLow==after.nFileIndexLow
        && before.nFileSizeHigh==after.nFileSizeHigh && before.nFileSizeLow==after.nFileSizeLow
        && before.ftLastWriteTime.dwLowDateTime==after.ftLastWriteTime.dwLowDateTime
        && before.ftLastWriteTime.dwHighDateTime==after.ftLastWriteTime.dwHighDateTime
        && total==((quint64(before.nFileSizeHigh)<<32)|before.nFileSizeLow);
    CloseHandle(handle);
    if (!ok) return fail(error,"Input changed or incomplete read: "+path);
    if(status)*status=CacheReadStatus::Available;
    *result=hash.result(); return true;
}
bool relativePath(const QString &path) {
    if (path.isEmpty() || path.contains(':') || path.contains('\\') || path.startsWith('/')
        || QDir::isAbsolutePath(path) || QDir::cleanPath(path)!=path) return false;
    const auto parts=path.split('/');
    return std::none_of(parts.cbegin(),parts.cend(),[](const QString &part) {
        return part.isEmpty() || part=="." || part==".." || part.endsWith('.') || part.endsWith(' ');
    });
}
bool enumerateDlls(const QString &root, const QString &relative, bool recursive,
    QStringList *paths, QString *error) {
    const auto directory=QDir(root).filePath(relative);
    if (!safePath(directory,false,error) || !QFileInfo(directory).isDir())
        return fail(error,"Input directory unavailable: "+directory);
    QDir dir(FileSystemPath::forIo(directory));
    const auto entries=dir.entryInfoList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System,QDir::Name);
    for (const auto &entry:entries) {
        const auto name=relative.isEmpty()?entry.fileName():relative+"/"+entry.fileName();
        if (entry.isDir()) {
            if (recursive && !enumerateDlls(root,name,true,paths,error)) return false;
        } else if (entry.fileName().endsWith(".dll",Qt::CaseInsensitive)) {
            if (!ordinaryFile(QDir(root).filePath(name),error)) return false;
            paths->append(name.toCaseFolded());
        }
    }
    return true;
}
bool inputSet(const QString &root, QStringList *paths, QString *error) {
    if (!safePath(root,false,error) || !QFileInfo(root).isDir()) return false;
    if (!enumerateDlls(root,{},false,paths,error)
        || !enumerateDlls(root,"gstreamer-plugins",true,paths,error)
        || !enumerateDlls(root,"libexec/gstreamer-1.0",true,paths,error)) return false;
    paths->append("airplay_receiver.exe");
    paths->append("libexec/gstreamer-1.0/gst-plugin-scanner.exe");
    const auto manifestPath=root+"/config/portable-runtime-manifest.txt";
    const auto manifest=readBounded(manifestPath,RecordLimit,error);
    if (manifest.isEmpty()) return fail(error,"Manifest unavailable or empty: "+manifestPath);
    paths->append("config/portable-runtime-manifest.txt");
    for (const auto &line:QString::fromUtf8(manifest).split('\n')) {
        const auto value=line.trimmed();
        if (value.isEmpty() || value.startsWith('#')) continue;
        if (!relativePath(value)) return fail(error,"Unsafe manifest entry: "+value);
        if (value.endsWith(".dll",Qt::CaseInsensitive) || value.endsWith(".exe",Qt::CaseInsensitive))
            paths->append(value.toCaseFolded());
    }
    paths->removeDuplicates();
    std::sort(paths->begin(),paths->end());
    return true;
}
void field(QCryptographicHash &hash, const QByteArray &value) {
    const quint64 size=value.size(); char length[8];
    for (int i=0;i<8;++i) length[i]=char((size>>(i*8))&0xff);
    hash.addData(QByteArrayView(length,8)); hash.addData(value);
}
bool validPlugins(const QJsonArray &plugins) {
    if (plugins.isEmpty()) return false;
    QStringList names;
    for (const auto &value:plugins) {
        if (!value.isObject()) return false;
        const auto object=value.toObject();
        const auto name=object.value("name"),source=object.value("source");
        if (!name.isString() || name.toString().isEmpty() || !source.isString()
            || !relativePath(source.toString()) || !source.toString().startsWith("gstreamer-plugins/",Qt::CaseInsensitive)
            || !source.toString().endsWith(".dll",Qt::CaseInsensitive)) return false;
        const auto folded=name.toString().toCaseFolded();
        if (names.contains(folded)) return false;
        names.append(folded);
    }
    return true;
}
QByteArray decodeHash(const QJsonValue &value) {
    if (!value.isString()
        || !QRegularExpression("^[0-9a-f]{64}$").match(value.toString()).hasMatch()) return {};
    return QByteArray::fromHex(value.toString().toLatin1());
}
CacheValidationRecord parseRecord(const QByteArray &bytes, QString *error) {
    QJsonParseError parseError;
    const auto doc=QJsonDocument::fromJson(bytes,&parseError);
    if (parseError.error!=QJsonParseError::NoError || !doc.isObject())
        { fail(error,"Invalid validation JSON"); return {}; }
    const auto obj=doc.object();
    const auto version=obj.value("schemaVersion");
    CacheValidationRecord record;
    if (!version.isDouble() || version.toDouble()!=CacheSchemaVersion
        || !obj.value("validated").isBool() || !obj.value("validated").toBool()
        || !obj.value("plugins").isArray() || !obj.value("blacklist").isArray()
        || !obj.value("blacklist").toArray().isEmpty())
        { fail(error,"Incomplete validation record"); return {}; }
    record.schemaVersion=CacheSchemaVersion;
    record.inputSha256=decodeHash(obj.value("inputSha256"));
    record.registrySha256=decodeHash(obj.value("registrySha256"));
    record.plugins=obj.value("plugins").toArray();
    record.validated=true;
    if (record.inputSha256.size()!=32 || record.registrySha256.size()!=32 || !validPlugins(record.plugins))
        { fail(error,"Invalid validation tuple"); return {}; }
    return record;
}
QByteArray encodeRecord(const CacheValidationRecord &record) {
    return QJsonDocument(QJsonObject{{"schemaVersion",record.schemaVersion},
        {"inputSha256",QString::fromLatin1(record.inputSha256.toHex())},
        {"registrySha256",QString::fromLatin1(record.registrySha256.toHex())},
        {"validated",record.validated},{"plugins",record.plugins},{"blacklist",QJsonArray{}}}).toJson(QJsonDocument::Compact);
}
bool writeNew(const QString &path, const QByteArray &bytes, QString *error) {
    if (!safePath(path,true,error)) return false;
    QFile file(FileSystemPath::forIo(path));
    if (!file.open(QIODevice::WriteOnly|QIODevice::NewOnly))
        return fail(error,file.errorString()+": "+path);
    if (file.write(bytes)!=bytes.size() || !file.flush())
        return fail(error,file.errorString()+": "+path);
    return true;
}
bool supportedVolume(const QString &directory, QString *error) {
    const auto io=FileSystemPath::forIo(directory);
    wchar_t volume[MAX_PATH]{},filesystem[MAX_PATH]{};
    if (!GetVolumePathNameW(wide(io),volume,MAX_PATH)
        || GetDriveTypeW(volume)!=DRIVE_FIXED
        || !GetVolumeInformationW(volume,nullptr,0,nullptr,nullptr,nullptr,filesystem,MAX_PATH))
        return fail(error,"Unconfirmed local publication volume: "+directory);
    // Be conservative: this implementation's ReplaceFile contract is confirmed only for local NTFS.
    if (QString::fromWCharArray(filesystem).compare("NTFS",Qt::CaseInsensitive)!=0)
        return fail(error,"Unsupported publication filesystem: "+QString::fromWCharArray(filesystem));
    return true;
}
QByteArray fileId(HANDLE handle);
bool atomicPublish(const QString &source, const QString &target, const QByteArray &expectedId,
    GStreamerCacheNative::Publisher &publisher, QString *error) {
    if (identity(QFileInfo(source).absolutePath())!=identity(QFileInfo(target).absolutePath())
        || !ordinaryFile(source,error) || !safePath(target,true,error)
        || !supportedVolume(QFileInfo(target).absolutePath(),error)) return false;
    const auto src=FileSystemPath::forIo(source),dst=FileSystemPath::forIo(target);
    const DWORD attrs=GetFileAttributesW(wide(dst));
    if (attrs==INVALID_FILE_ATTRIBUTES) {
        const auto code=GetLastError();
        if (code!=ERROR_FILE_NOT_FOUND && code!=ERROR_PATH_NOT_FOUND)
            return fail(error,nativeError("Inspect publication target",target));
    } else if (attrs & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))
        return fail(error,"Foreign publication target: "+target);
    // ReplaceFile opens its replacement without sharing and conflicts with the
    // retained content seal. A native NTFS handle rename keeps that seal intact.
    HANDLE rename=CreateFileW(wide(src),DELETE|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (rename==INVALID_HANDLE_VALUE) return fail(error,nativeError("Open atomic rename",source));
    // The path may have been replaced after precommit checks. Only this actual
    // operation handle proves which object will be renamed; keep it through commit.
    if (expectedId.isEmpty() || fileId(rename)!=expectedId) {
        CloseHandle(rename); SetLastError(ERROR_FILE_INVALID);
        return fail(error,"Publication native identity changed: "+source);
    }
    const bool ok=publisher.renameFile(rename,target,error);
    const auto code=GetLastError(); CloseHandle(rename); SetLastError(code);
    return ok;
}
bool renameHandle(HANDLE rename,const QString &target,QString *error) {
    const auto dst=FileSystemPath::forIo(target);
    const auto attrs=GetFileAttributesW(wide(dst));
    const auto name=QDir::toNativeSeparators(FileSystemPath::absolute(target));
    const DWORD bytes=DWORD(name.size()*sizeof(wchar_t));
    struct NativeRename { DWORD flags; HANDLE root; DWORD length; wchar_t name[1]; };
    std::vector<unsigned char> buffer(offsetof(NativeRename,name)+bytes+sizeof(wchar_t));
    auto *info=reinterpret_cast<NativeRename *>(buffer.data());
    // POSIX rename semantics retain readers of the replaced inode while atomically
    // changing the name. Unsupported kernels/filesystems are refused, without fallback.
    constexpr DWORD ReplaceExisting = 0x1u, PosixSemantics = 0x2u;
    static_assert(offsetof(NativeRename,name)==offsetof(FILE_RENAME_INFO,FileName));
    info->flags=PosixSemantics | (attrs!=INVALID_FILE_ATTRIBUTES?ReplaceExisting:0u);
    info->root=nullptr; info->length=bytes;
    memcpy(info->name,name.utf16(),bytes);
    const bool ok=SetFileInformationByHandle(rename,FileRenameInfoEx,info,DWORD(buffer.size()));
    if (!ok) return fail(error,nativeError("Atomic publication",target));
    return true;
}
QByteArray fileId(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle,&info)
        || (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) return {};
    return QByteArray::number(info.dwVolumeSerialNumber)+"/"+QByteArray::number(info.nFileIndexHigh)
        +"/"+QByteArray::number(info.nFileIndexLow);
}
QByteArray fileId(const QString &path) {
    const auto io=FileSystemPath::forIo(path);
    HANDLE handle=CreateFileW(wide(io),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (handle==INVALID_HANDLE_VALUE) return {};
    const auto id=fileId(handle); CloseHandle(handle); return id;
}
QJsonObject boundedObject(const QByteArray &bytes,QString *error) {
    if (bytes.isEmpty() || bytes.size()>RecordLimit) { fail(error,"Worker message size invalid"); return {}; }
    QJsonParseError parsed;
    const auto doc=QJsonDocument::fromJson(bytes,&parsed);
    if (parsed.error!=QJsonParseError::NoError || !doc.isObject()) {
        fail(error,"Worker message JSON invalid"); return {};
    }
    return doc.object();
}
bool sameBaseline(const CacheBaseline &a, const CacheBaseline &b) {
    return a.exists==b.exists && a.sha256==b.sha256;
}
CacheCommitResult failure(CacheFailureReason reason, const QString &detail) {
    return {CacheState::RecoveryFailed,RecordState::NotApplicable,detail,reason};
}
}

struct RuntimeCacheLease::Implementation {
    QString directory,registry,markerPath,package;
    QByteArray marker;
    bool closed=false;
    CacheCleanupResult result;
    bool owned(QString *error) const {
        return safePath(directory,false,error)
            && readBounded(markerPath,256,error)==marker;
    }
    CacheCleanupResult cleanup(bool keepRuntime,int remainingBudgetMs=CleanupBudgetMs) {
        QElapsedTimer timer; timer.start();
        const int budget=std::clamp(remainingBudgetMs,0,CleanupBudgetMs);
        CacheCleanupResult result{true,{},{}};
        QString error;
        if (timer.elapsed()>=budget) return {false,{directory,registry},"Cleanup deadline expired"};
        if (!owned(&error)) return {false,{directory},error.isEmpty()?"Runtime ownership changed":error};
        // Incremental enumeration is checked before every kernel enumeration step.
        const auto removeChildren=[&](const auto &self,const QString &dir)->bool {
            if (timer.elapsed()>=budget) { result.residualPaths.append(dir); return false; }
            QDirIterator entries(FileSystemPath::forIo(dir),
                QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System,QDirIterator::NoIteratorFlags);
            bool complete=true;
            while (true) {
                if (timer.elapsed()>=budget) { result.residualPaths.append(dir); return false; }
                if (!entries.hasNext()) break;
                entries.next();
                const auto entry=entries.fileInfo();
                const auto path=FileSystemPath::absolute(entry.absoluteFilePath());
                if (identity(path)==identity(markerPath)
                    || (keepRuntime && identity(path)==identity(registry))) continue;
                if (timer.elapsed()>=budget || !safePath(path,false,&error)) {
                    complete=false; result.residualPaths.append(path); continue;
                }
                bool removed=false;
                if (entry.isDir()) {
                    removed=self(self,path);
                    if (removed && timer.elapsed()<budget) removed=QDir().rmdir(FileSystemPath::forIo(path));
                    else removed=false;
                } else removed=QFile::remove(FileSystemPath::forIo(path));
                if (!removed) { complete=false; result.residualPaths.append(path); }
            }
            return complete;
        };
        result.complete=removeChildren(removeChildren,directory);
        if (!keepRuntime && result.complete && timer.elapsed()>=std::clamp(remainingBudgetMs,0,CleanupBudgetMs)) {
            result.complete=false; result.residualPaths.append(directory);
        }
        if (!keepRuntime && result.complete) {
            result.complete=QFile::remove(FileSystemPath::forIo(markerPath))
                && QDir().rmdir(FileSystemPath::forIo(directory));
            if (!result.complete) result.residualPaths.append(directory);
        }
        result.residualPaths.removeDuplicates();
        if (!result.complete) result.reason=error.isEmpty()?"Owned resource cleanup incomplete":error;
        return result;
    }
};
struct RegistryWriteLease::Implementation {
    QString package,lockPath;
    HANDLE handle=INVALID_HANDLE_VALUE;
    struct Pending {
        QString registry,record;
        std::weak_ptr<RuntimeCacheLease> runtime;
        CacheFingerprint input;
        CacheBaseline baseline;
        QByteArray registryId,recordId,targetId,registryHash,recordHash,nonce,sourceOwnership;
        HANDLE registrySeal=INVALID_HANDLE_VALUE,recordSeal=INVALID_HANDLE_VALUE;
        HANDLE runtimeSeal=INVALID_HANDLE_VALUE,markerSeal=INVALID_HANDLE_VALUE,targetSeal=INVALID_HANDLE_VALUE;
        bool sealAttempted=false,sealed=false,adopted=false;
        CacheFailureReason preparationFailure=CacheFailureReason::None;
        QString preparationError;
        ~Pending() {
            for (const auto seal:{registrySeal,recordSeal,runtimeSeal,markerSeal,targetSeal})
                if (seal!=INVALID_HANDLE_VALUE) CloseHandle(seal);
        }
    };
    std::unique_ptr<Pending> pending;
    bool closed=false;
    CacheCleanupResult closeResult;
    QStringList priorResiduals;
    bool registered(const PreparedCacheCommit &prepared) const {
        return pending && prepared.runtime && pending->runtime.lock()==prepared.runtime
            && identity(pending->registry)==identity(prepared.pendingRegistry)
            && identity(pending->record)==identity(prepared.pendingRecord)
            && pending->input.sha256==prepared.input.sha256 && prepared.input.valid
            && sameBaseline(pending->baseline,prepared.baseline);
    }
    CacheCleanupResult discardPending(int remainingBudgetMs, GStreamerCacheNative::Remover &remover) {
        CacheCleanupResult result{true,{},{}};
        if (!pending) return result;
        const auto entries={std::make_pair(pending->registry,pending->registryId),
            std::make_pair(pending->record,pending->recordId)};
        // Retire seals before removing the exact registered objects. Never retry after explicit close.
        pending.reset();
        QElapsedTimer timer; timer.start();
        for (const auto &entry:entries) {
            const auto io=FileSystemPath::forIo(entry.first);
            const auto attrs=GetFileAttributesW(wide(io)),code=GetLastError();
            if (attrs==INVALID_FILE_ATTRIBUTES && (code==ERROR_FILE_NOT_FOUND || code==ERROR_PATH_NOT_FOUND)) continue;
            QString error;
            if (timer.elapsed()>=std::clamp(remainingBudgetMs,0,CleanupBudgetMs)
                || !ordinaryFile(entry.first,&error) || fileId(entry.first)!=entry.second
                || !remover.removeFile(entry.first,entry.second,&error)) {
                result.complete=false; result.residualPaths.append(entry.first);
            }
        }
        if (!result.complete) {
            result.reason="Owned pending cleanup incomplete";
            priorResiduals.append(result.residualPaths);
            priorResiduals.removeDuplicates();
        }
        return result;
    }
    CacheCleanupResult discardPending(int remainingBudgetMs=CleanupBudgetMs) {
        GStreamerCacheNative::Remover remover; return discardPending(remainingBudgetMs,remover);
    }
};
RuntimeCacheLease::RuntimeCacheLease(std::unique_ptr<Implementation> i):implementation_(std::move(i)) {}
RuntimeCacheLease::~RuntimeCacheLease() { if (!implementation_->closed) close(); }
QString RuntimeCacheLease::registryPath() const { return implementation_->registry; }
CacheCleanupResult RuntimeCacheLease::close() { return close(CleanupBudgetMs); }
CacheCleanupResult RuntimeCacheLease::close(int remainingBudgetMs) {
    if (!implementation_->closed) {
        implementation_->result=implementation_->cleanup(false,remainingBudgetMs);
        implementation_->closed=true;
    }
    return implementation_->result;
}
RegistryWriteLease::RegistryWriteLease(std::unique_ptr<Implementation> i):implementation_(std::move(i)) {}
RegistryWriteLease::~RegistryWriteLease() { if (!implementation_->closed) close(); }
CacheCleanupResult RegistryWriteLease::close() { return close(CleanupBudgetMs); }
CacheCleanupResult RegistryWriteLease::close(int remainingBudgetMs) {
    GStreamerCacheNative::Remover remover; return close(remainingBudgetMs,remover);
}
CacheCleanupResult RegistryWriteLease::close(int remainingBudgetMs, GStreamerCacheNative::Remover &remover) {
    auto &impl=*implementation_;
    if (!impl.closed) {
        impl.closeResult=impl.discardPending(remainingBudgetMs,remover);
        impl.closeResult.residualPaths.append(impl.priorResiduals);
        impl.closeResult.residualPaths.removeDuplicates();
        impl.closeResult.complete=impl.closeResult.residualPaths.isEmpty();
        if (!impl.closeResult.complete && impl.closeResult.reason.isEmpty())
            impl.closeResult.reason="Owned pending cleanup incomplete";
        if (impl.handle!=INVALID_HANDLE_VALUE) { CloseHandle(impl.handle); impl.handle=INVALID_HANDLE_VALUE; }
        impl.closed=true;
    }
    return impl.closeResult;
}
bool validationRecordMatches(const CacheValidationRecord &record,const CacheFingerprint &input,
    const CacheBaseline &baseline) {
    return record.schemaVersion==CacheSchemaVersion && record.validated && input.valid
        && input.sha256.size()==32 && baseline.exists && baseline.sha256.size()==32
        && record.inputSha256==input.sha256 && record.registrySha256==baseline.sha256
        && validPlugins(record.plugins);
}
CacheFingerprint CacheStorage::fingerprint(const QString &packageDirectory) {
    QString error; QStringList paths;
    const auto root=canonicalPackageDirectory(packageDirectory,&error);
    if (root.isEmpty()) return {false,{},error};
    if (!inputSet(root,&paths,&error)) return {false,{},error};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    field(hash,"AIRPLAY-GSTREAMER-STARTUP/1;PREPARE/2;VALIDATE/1");
    field(hash,identity(root).toUtf8());
    // Versioned fixed required plugin policy, plus the required runtime manifest itself.
    field(hash,"app;libav;playback;autodetect;videoparsersbad");
    for (const auto &path:paths) {
        QByteArray content;
        if (!hashFile(root+"/"+path,&content,&error)) return {false,{},error};
        field(hash,path.toUtf8()); field(hash,"present"); field(hash,content);
    }
    QStringList after;
    if (!inputSet(root,&after,&error) || paths!=after)
        return {false,{},error.isEmpty()?"Input file collection changed":error};
    return {true,hash.result(),{}};
}
std::shared_ptr<RuntimeCacheLease> CacheStorage::createRuntime(const QString &packageDirectory,
    const QString &temporaryParent,QString *error) {
    if (error) error->clear();
    const auto root=canonicalPackageDirectory(packageDirectory,error);
    const auto parent=FileSystemPath::absolute(temporaryParent);
    if (root.isEmpty() || parent.isEmpty() || !safePath(parent,true,error)) return {};
    const auto gst=parent+"/gst";
    if (!QDir().mkpath(FileSystemPath::forIo(gst)) || !safePath(gst,false,error)) {
        fail(error,"Temporary parent unavailable: "+gst); return {};
    }
    auto impl=std::make_unique<RuntimeCacheLease::Implementation>();
    impl->directory=gst+"/startup-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkdir(FileSystemPath::forIo(impl->directory))) {
        fail(error,"Exclusive runtime directory unavailable: "+impl->directory); return {};
    }
    impl->marker=QUuid::createUuid().toRfc4122().toHex()+QUuid::createUuid().toRfc4122().toHex();
    impl->markerPath=impl->directory+"/.owner"; impl->registry=impl->directory+"/runtime.bin";
    impl->package=identity(root);
    if (!writeNew(impl->markerPath,impl->marker,error)) {
        QDir().rmdir(FileSystemPath::forIo(impl->directory)); return {};
    }
    return std::shared_ptr<RuntimeCacheLease>(new RuntimeCacheLease(std::move(impl)));
}
CacheLockAttempt CacheStorage::tryRegistryWriteLock(const QString &packageDirectory) {
    QString error;
    const auto root=canonicalPackageDirectory(packageDirectory,&error);
    if (root.isEmpty()) return {CacheLockState::Unavailable,{},error};
    const auto dir=root+"/gstreamer-1.0",path=dir+"/.registry-startup.lock";
    if (!safePath(dir,true,&error)) return {CacheLockState::ForeignObject,{},error};
    if (!QDir().mkpath(FileSystemPath::forIo(dir)))
        return {CacheLockState::Unavailable,{},"Registry directory unavailable: "+dir};
    if (!safePath(path,true,&error)) return {CacheLockState::ForeignObject,{},error};
    const auto io=FileSystemPath::forIo(path);
    const auto existingAttributes=GetFileAttributesW(wide(io));
    if (existingAttributes!=INVALID_FILE_ATTRIBUTES
        && (existingAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
        return {CacheLockState::ForeignObject,{},"Foreign registry lock object: "+path};
    HANDLE handle=CreateFileW(wide(io),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    bool created=handle!=INVALID_HANDLE_VALUE;
    if (!created && GetLastError()==ERROR_FILE_EXISTS)
        handle=CreateFileW(wide(io),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (handle==INVALID_HANDLE_VALUE) {
        const auto code=GetLastError();
        return {code==ERROR_SHARING_VIOLATION||code==ERROR_LOCK_VIOLATION?CacheLockState::Busy:CacheLockState::Unavailable,
            {},nativeError("Registry lock",path)};
    }
    BY_HANDLE_FILE_INFORMATION info{};
    bool known=GetFileInformationByHandle(handle,&info)
        && !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
    DWORD count=0;
    if (created) known=known && WriteFile(handle,LockMarker.constData(),DWORD(LockMarker.size()),&count,nullptr)
        && count==DWORD(LockMarker.size()) && FlushFileBuffers(handle);
    else {
        char bytes[64]{};
        known=known && info.nFileSizeHigh==0 && info.nFileSizeLow==DWORD(LockMarker.size())
            && ReadFile(handle,bytes,sizeof(bytes),&count,nullptr)
            && QByteArray(bytes,count)==LockMarker;
    }
    if (!known) { CloseHandle(handle); return {CacheLockState::ForeignObject,{},"Unknown registry lock object: "+path}; }
    auto impl=std::make_unique<RegistryWriteLease::Implementation>();
    impl->package=root; impl->lockPath=path; impl->handle=handle;
    return {CacheLockState::Acquired,std::shared_ptr<RegistryWriteLease>(new RegistryWriteLease(std::move(impl))),{}};
}
CacheBaseline CacheStorage::readBaseline(const QString &packageDirectory,QString *error,CacheReadStatus *status) {
    if(status)*status=CacheReadStatus::Rejected;
    if (error) error->clear();
    const auto path=registry(FileSystemPath::absolute(packageDirectory));
    if (!safePath(path,true,error)) return {};
    const auto io=FileSystemPath::forIo(path);
    const auto attributes=GetFileAttributesW(wide(io));
    if (attributes==INVALID_FILE_ATTRIBUTES) {
        if (GetLastError()!=ERROR_FILE_NOT_FOUND && GetLastError()!=ERROR_PATH_NOT_FOUND)
            fail(error,nativeError("Baseline unavailable",path));
        else if(status)*status=CacheReadStatus::Available;
        return {};
    }
    QByteArray hash;
    if (!hashFile(path,&hash,error,status)) return {true,{}};
    return {true,hash};
}
CacheValidationRecord CacheStorage::readValidationRecord(const QString &packageDirectory,QString *error,CacheRecordReadStatus *status) {
    if (error) error->clear();
    if(status)*status=CacheRecordReadStatus::Unavailable;
    QString readError;
    const auto bytes=readBounded(recordPath(FileSystemPath::absolute(packageDirectory)),RecordLimit,&readError);
    if(!readError.isEmpty()){if(error)*error=readError;return {};}
    QString parseError;const auto record=parseRecord(bytes,&parseError);
    if(status)*status=parseError.isEmpty()?CacheRecordReadStatus::Parsed:CacheRecordReadStatus::Malformed;
    if(error)*error=parseError;
    return record;
}
PreparedCacheCommit CacheStorage::reservePreparedCommit(RegistryWriteLease &lease,
    const std::shared_ptr<RuntimeCacheLease> &runtime,const CacheFingerprint &input,
    const CacheBaseline &baseline,QString *error) {
    if (error) error->clear();
    auto &impl=*lease.implementation_;
    if (!runtime || runtime->implementation_->closed
        || runtime->implementation_->package!=identity(impl.package)
        || !runtime->implementation_->owned(error) || !input.valid || input.sha256.size()!=32
        || (baseline.exists && baseline.sha256.size()!=32) || impl.handle==INVALID_HANDLE_VALUE) {
        fail(error,"Invalid preparation ownership or tuple"); return {};
    }
    const auto discarded=impl.discardPending();
    if (!discarded.complete) { fail(error,discarded.reason); return {}; }
    const auto prefix=impl.package+"/gstreamer-1.0/.startup-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto pending=std::make_unique<RegistryWriteLease::Implementation::Pending>();
    pending->registry=prefix+".bin"; pending->record=prefix+".json";
    pending->runtime=runtime; pending->input=input; pending->baseline=baseline;
    if (!writeNew(pending->registry,{},error)) return {};
    if (!writeNew(pending->record,{},error)) { QFile::remove(FileSystemPath::forIo(pending->registry)); return {}; }
    pending->registryId=fileId(pending->registry); pending->recordId=fileId(pending->record);
    PreparedCacheCommit result{pending->registry,pending->record,baseline,input,runtime};
    impl.pending=std::move(pending);
    return result;
}
bool CacheStorage::sealPreparedCommit(RegistryWriteLease &lease,const PreparedCacheCommit &prepared,
    const QByteArray &registrySha256,const QByteArray &recordSha256,QString *error) {
    if (error) error->clear();
    auto &impl=*lease.implementation_;
    if (!impl.registered(prepared) || impl.pending->sealAttempted || registrySha256.size()!=32
        || recordSha256.size()!=32 || prepared.runtime->implementation_->closed)
        return fail(error,"Invalid seal ownership or tuple");
    auto &pending=*impl.pending;
    pending.sealAttempted=true;
    const auto open=[&](const QString &path,const QByteArray &expected,HANDLE *seal) {
        if (!ordinaryFile(path,error)) return false;
        const auto io=FileSystemPath::forIo(path);
        *seal=CreateFileW(wide(io),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,
            OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if (*seal==INVALID_HANDLE_VALUE) return fail(error,nativeError("Seal prepared object",path));
        if (!expected.isEmpty() && fileId(*seal)!=expected) return fail(error,"Reserved file identity changed: "+path);
        return true;
    };
    if (!open(pending.registry,pending.registryId,&pending.registrySeal)
        || !open(pending.record,pending.recordId,&pending.recordSeal)
        || !open(prepared.runtime->registryPath(),{},&pending.runtimeSeal)
        || !open(prepared.runtime->implementation_->markerPath,{},&pending.markerSeal))
        return false;
    if (prepared.baseline.exists) {
        if (!open(registry(impl.package),{},&pending.targetSeal)) return false;
        pending.targetId=fileId(pending.targetSeal);
    }
    pending.registryHash=registrySha256; pending.recordHash=recordSha256;
    pending.nonce=QUuid::createUuid().toRfc4122().toHex();
    pending.sealed=true;
    return true;
}
QByteArray CacheStorage::workerPreparationRequest(RegistryWriteLease &lease,const PreparedCacheCommit &prepared,QString *error) {
    if (error) error->clear();
    auto &impl=*lease.implementation_;
    if (!impl.registered(prepared) || !impl.pending->sealed) { fail(error,"Unsealed delegation"); return {}; }
    const auto &pending=*impl.pending;
    const auto &runtime=*prepared.runtime->implementation_;
    return QJsonDocument(QJsonObject{{"schemaVersion",CacheSchemaVersion},{"phase","sealed-verification"},
        {"nonce",QString::fromLatin1(pending.nonce)},{"package",impl.package},
        {"sourceOwnershipRequest",QString::fromUtf8(pending.sourceOwnership)},
        {"pendingRegistry",pending.registry},{"pendingRecord",pending.record},
        {"registryId",QString::fromLatin1(pending.registryId)},{"recordId",QString::fromLatin1(pending.recordId)},
        {"runtimeRegistry",runtime.registry},{"runtimeMarker",QString::fromLatin1(runtime.marker)},
        {"inputSha256",QString::fromLatin1(pending.input.sha256.toHex())},
        {"baselineExists",pending.baseline.exists},{"baselineSha256",QString::fromLatin1(pending.baseline.sha256.toHex())},
        {"registrySha256",QString::fromLatin1(pending.registryHash.toHex())},
        {"recordSha256",QString::fromLatin1(pending.recordHash.toHex())}}).toJson(QJsonDocument::Compact);
}
QByteArray CacheStorage::verifyPreparedWorkerRequest(const QByteArray &request,QString *error) {
    if (error) error->clear();
    const auto obj=boundedObject(request,error);
    if (obj.value("schemaVersion").toDouble()!=CacheSchemaVersion || obj.value("phase")!="sealed-verification"
        || !QRegularExpression("^[0-9a-f]{32}$").match(obj.value("nonce").toString()).hasMatch()) {
        fail(error,"Invalid sealed verification protocol"); return {};
    }
    const auto package=FileSystemPath::absolute(obj.value("package").toString());
    const auto pendingRegistry=FileSystemPath::absolute(obj.value("pendingRegistry").toString());
    const auto pendingRecord=FileSystemPath::absolute(obj.value("pendingRecord").toString());
    const auto runtimeRegistry=FileSystemPath::absolute(obj.value("runtimeRegistry").toString());
    if (package.isEmpty() || identity(QFileInfo(pendingRegistry).absolutePath())!=identity(package+"/gstreamer-1.0")
        || identity(QFileInfo(pendingRecord).absolutePath())!=identity(package+"/gstreamer-1.0")
        || QFileInfo(runtimeRegistry).fileName()!="runtime.bin"
        || !QRegularExpression("^startup-[0-9a-f-]{36}$").match(QFileInfo(QFileInfo(runtimeRegistry).absolutePath()).fileName()).hasMatch()
        || !QRegularExpression("^[0-9a-f]{64}$").match(obj.value("runtimeMarker").toString()).hasMatch()
        || fileId(pendingRegistry)!=obj.value("registryId").toString().toLatin1()
        || fileId(pendingRecord)!=obj.value("recordId").toString().toLatin1()) {
        fail(error,"Invalid delegated ownership"); return {};
    }
    RuntimeCacheLease::Implementation runtime;
    runtime.directory=QFileInfo(runtimeRegistry).absolutePath(); runtime.registry=runtimeRegistry;
    runtime.markerPath=runtime.directory+"/.owner"; runtime.marker=obj.value("runtimeMarker").toString().toLatin1();
    if (!runtime.owned(error)) return {};
    QByteArray registryHash,recordHash,runtimeHash;
    if (!hashFile(pendingRegistry,&registryHash,error) || !hashFile(pendingRecord,&recordHash,error)
        || !hashFile(runtimeRegistry,&runtimeHash,error)) return {};
    if (registryHash!=decodeHash(obj.value("registrySha256")) || runtimeHash!=registryHash
        || recordHash!=decodeHash(obj.value("recordSha256"))) {
        fail(error,"Prepared sealed content differs from worker output"); return {};
    }
    const auto input=fingerprint(package);
    const CacheBaseline expectedBaseline{obj.value("baselineExists").toBool(),decodeHash(obj.value("baselineSha256"))};
    if (!obj.value("baselineExists").isBool() || !input.valid || input.sha256!=decodeHash(obj.value("inputSha256"))
        || (expectedBaseline.exists && expectedBaseline.sha256.size()!=32)) {
        fail(error,input.reason.isEmpty()?"Package inputs changed":input.reason); return {};
    }
    const auto actualBaseline=readBaseline(package,error);
    if ((error && !error->isEmpty()) || !sameBaseline(actualBaseline,expectedBaseline)) {
        fail(error,"Shared registry baseline changed"); return {};
    }
    const auto record=parseRecord(readBounded(pendingRecord,RecordLimit,error),error);
    if (!validationRecordMatches(record,input,{true,registryHash})) { fail(error,"Prepared validation tuple mismatch"); return {}; }
    // Both owned roots share one precommit cleanup budget; the assessed runtime is retained.
    QElapsedTimer cleanupClock;cleanupClock.start();
    const auto sourceOwnership=obj.value("sourceOwnershipRequest").toString().toUtf8();
    if (!sourceOwnership.isEmpty()) {
        const auto source=boundedObject(sourceOwnership,error);
        const auto sourceRegistry=source.value("runtimeRegistry").toString();
        if (identity(source.value("package").toString())!=identity(package)
            || !verifyWorkerOwnershipRequest(sourceOwnership,QFileInfo(sourceRegistry).absolutePath(),error)
            || source.value("runtimeId").toString().isEmpty()
            || fileId(sourceRegistry)!=source.value("runtimeId").toString().toLatin1()) {
            fail(error,"Prepared source ownership changed");return {};
        }
        const auto sourceCleanup=cleanupWorkerOwnershipRequest(sourceOwnership,true,
            int(std::max<qint64>(0,CleanupBudgetMs-cleanupClock.elapsed())));
        if (!sourceCleanup.complete) {fail(error,"Precommit cleanup incomplete: "+sourceCleanup.reason);return {};}
    }
    const auto cleanup=runtime.cleanup(true,int(std::max<qint64>(0,CleanupBudgetMs-cleanupClock.elapsed())));
    if (!cleanup.complete) { fail(error,"Precommit cleanup incomplete: "+cleanup.reason); return {}; }
    return QJsonDocument(QJsonObject{{"schemaVersion",CacheSchemaVersion},{"phase","sealed-proof"},
        {"nonce",obj.value("nonce")},{"validated",true},{"cleanupComplete",true},
        {"sourceOwnershipRequest",QString::fromUtf8(sourceOwnership)},
        {"inputSha256",QString::fromLatin1(input.sha256.toHex())},{"baselineExists",actualBaseline.exists},
        {"baselineSha256",QString::fromLatin1(actualBaseline.sha256.toHex())},
        {"registrySha256",QString::fromLatin1(registryHash.toHex())},
        {"recordSha256",QString::fromLatin1(recordHash.toHex())},
        {"runtimeSha256",QString::fromLatin1(runtimeHash.toHex())}}).toJson(QJsonDocument::Compact);
}
bool CacheStorage::adoptPreparedCommit(RegistryWriteLease &lease,const PreparedCacheCommit &prepared,
    const QByteArray &workerProof,QString *error) {
    if (error) error->clear();
    auto &impl=*lease.implementation_;
    if (!impl.registered(prepared) || !impl.pending->sealed)
        return fail(error,"Unregistered or unsealed prepared objects");
    auto &pending=*impl.pending;
    const auto proof=boundedObject(workerProof,error);
    if (proof.value("schemaVersion").toDouble()!=CacheSchemaVersion || proof.value("phase")!="sealed-proof"
        || proof.value("nonce").toString().toLatin1()!=pending.nonce
        || proof.value("sourceOwnershipRequest").toString().toUtf8()!=pending.sourceOwnership
        || !proof.value("validated").isBool() || !proof.value("validated").toBool()
        || !proof.value("cleanupComplete").isBool() || !proof.value("cleanupComplete").toBool()
        || decodeHash(proof.value("inputSha256"))!=pending.input.sha256
        || !proof.value("baselineExists").isBool() || proof.value("baselineExists").toBool()!=pending.baseline.exists
        || proof.value("baselineSha256").toString().toLatin1()!=pending.baseline.sha256.toHex()
        || decodeHash(proof.value("registrySha256"))!=pending.registryHash
        || decodeHash(proof.value("recordSha256"))!=pending.recordHash
        || decodeHash(proof.value("runtimeSha256")).size()!=32
        || fileId(pending.registry)!=pending.registryId || fileId(pending.record)!=pending.recordId
        || fileId(prepared.runtime->registryPath())!=fileId(pending.runtimeSeal)
        || fileId(prepared.runtime->implementation_->markerPath)!=fileId(pending.markerSeal)) {
        return fail(error,"Sealed worker proof mismatch");
    }
    pending.adopted=true; return true;
}
PreparedCacheCommit CacheStorage::prepareCommit(RegistryWriteLease &lease,
    const std::shared_ptr<RuntimeCacheLease> &runtime,const QString &candidatePath,
    const CacheValidationRecord &record,const CacheFingerprint &input,const CacheBaseline &baseline,QString *error) {
    if (!runtime || identity(QFileInfo(candidatePath).absolutePath())!=identity(runtime->implementation_->directory)
        || !runtime->implementation_->owned(error)) { fail(error,"Candidate outside owned runtime"); return {}; }
    auto prepared=reservePreparedCommit(lease,runtime,input,baseline,error);
    if (prepared.pendingRegistry.isEmpty()) return {};
    QByteArray registryHash,recordHash;
    if (fillPreparedWorkerRequest(workerFillRequest(lease,prepared,error),candidatePath,record,error).isEmpty()
        || !hashFile(prepared.pendingRegistry,&registryHash,error) || !hashFile(prepared.pendingRecord,&recordHash,error)
        || !sealPreparedCommit(lease,prepared,registryHash,recordHash,error)) {
        lease.implementation_->discardPending(); return {};
    }
    const auto proof=verifyPreparedWorkerRequest(workerPreparationRequest(lease,prepared,error),error);
    if (proof.isEmpty()) {
        auto &pending=*lease.implementation_->pending;
        pending.preparationError=error?*error:"Prepared worker verification failed";
        pending.preparationFailure=pending.preparationError.startsWith("Precommit cleanup")?
            CacheFailureReason::PrecommitCleanupFailed:CacheFailureReason::ValidationFailed;
        return prepared;
    }
    if (!adoptPreparedCommit(lease,prepared,proof,error)) {
        lease.implementation_->discardPending(); return {};
    }
    return prepared;
}
bool GStreamerCacheNative::Publisher::publishFile(const QString &source,const QString &target,
    const QByteArray &expectedNativeId,QString *error) {
    return atomicPublish(source,target,expectedNativeId,*this,error);
}
bool GStreamerCacheNative::Publisher::renameFile(void *handle,const QString &target,QString *error) {
    return renameHandle(handle,target,error);
}
bool GStreamerCacheNative::Remover::removeFile(const QString &path,const QByteArray &expectedNativeId,QString *error) {
    const auto io=FileSystemPath::forIo(path);
    HANDLE handle=CreateFileW(wide(io),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (handle==INVALID_HANDLE_VALUE) return fail(error,nativeError("Open cleanup",path));
    if (expectedNativeId.isEmpty() || fileId(handle)!=expectedNativeId) {
        CloseHandle(handle); SetLastError(ERROR_FILE_INVALID);
        return fail(error,"Cleanup native identity changed: "+path);
    }
    // Never reopen the registered name for deletion: a foreign object can have
    // acquired that name while this verified owned object remains open.
    const bool ok=deleteFile(handle,path,error);
    const auto code=GetLastError(); CloseHandle(handle); SetLastError(code);
    if (!ok) return false;
    const auto attrs=GetFileAttributesW(wide(io)),attributeError=GetLastError();
    if (attrs==INVALID_FILE_ATTRIBUTES && (attributeError==ERROR_FILE_NOT_FOUND || attributeError==ERROR_PATH_NOT_FOUND))
        return true;
    return fail(error,"Cleanup left a replacement at registered path: "+path);
}
bool GStreamerCacheNative::Remover::deleteFile(void *handle,const QString &path,QString *error) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(handle,FileDispositionInfo,&disposition,sizeof(disposition)))
        return fail(error,nativeError("Delete owned native object",path));
    return true;
}
CacheCommitResult CacheStorage::publishPreparedCache(RegistryWriteLease &lease,const PreparedCacheCommit &prepared,
    int remainingCleanupBudgetMs) {
    GStreamerCacheNative::Publisher native;
    return publishPreparedCache(lease,prepared,native,remainingCleanupBudgetMs);
}
CacheCommitResult CacheStorage::publishPreparedCache(RegistryWriteLease &lease,const PreparedCacheCommit &prepared,
    GStreamerCacheNative::Publisher &native,int remainingCleanupBudgetMs) {
    QElapsedTimer cleanupClock; cleanupClock.start();
    auto &impl=*lease.implementation_;
    if (!impl.registered(prepared))
        return failure(CacheFailureReason::OwnershipUnknown,"Unregistered prepared objects");
    auto &pending=*impl.pending;
    if (pending.preparationFailure!=CacheFailureReason::None)
        return failure(pending.preparationFailure,pending.preparationError);
    if (!pending.adopted || !pending.sealed || prepared.runtime->implementation_->closed
        || fileId(pending.registry)!=pending.registryId || fileId(pending.record)!=pending.recordId
        || fileId(prepared.runtime->registryPath())!=fileId(pending.runtimeSeal)
        || fileId(prepared.runtime->implementation_->markerPath)!=fileId(pending.markerSeal))
        return failure(CacheFailureReason::OwnershipUnknown,"Unadopted or changed sealed objects");
    // Retained READ seals deny content writes. Names must still identify those native files.
    const auto targetId=fileId(registry(impl.package));
    if ((prepared.baseline.exists && targetId!=pending.targetId)
        || (!prepared.baseline.exists && (QFileInfo::exists(FileSystemPath::forIo(registry(impl.package))) || !targetId.isEmpty())))
        return failure(CacheFailureReason::InputChanged,"Shared registry baseline changed before native commit");
    QString error;
    if (!supportedVolume(impl.package+"/gstreamer-1.0",&error))
        return failure(CacheFailureReason::UnsupportedPublish,error);
    SetLastError(ERROR_SUCCESS);
    if (!native.publishFile(prepared.pendingRegistry,registry(impl.package),pending.registryId,&error)) {
        const auto code=GetLastError();
        const auto nativeFailure=code==ERROR_FILE_INVALID ? CacheFailureReason::OwnershipUnknown :
            code==ERROR_INVALID_PARAMETER || code==ERROR_NOT_SUPPORTED
            || code==ERROR_INVALID_FUNCTION || code==ERROR_CALL_NOT_IMPLEMENTED ?
            CacheFailureReason::UnsupportedPublish : CacheFailureReason::DirectoryNotWritable;
        const auto after=fileId(registry(impl.package));
        const auto io=FileSystemPath::forIo(registry(impl.package));
        const auto attrs=GetFileAttributesW(wide(io)),attributeError=GetLastError();
        const bool confirmedMissing=attrs==INVALID_FILE_ATTRIBUTES
            && (attributeError==ERROR_FILE_NOT_FOUND || attributeError==ERROR_PATH_NOT_FOUND);
        const bool unchanged=prepared.baseline.exists?after==pending.targetId:confirmedMissing;
        if (!unchanged)
            return {CacheState::Unknown,RecordState::NotApplicable,error+"; target state unconfirmed",
                CacheFailureReason::OwnershipUnknown};
        return failure(nativeFailure,error);
    }
    CacheCommitResult result{CacheState::Updated,RecordState::Saved,{},CacheFailureReason::None};
    if (!native.publishFile(prepared.pendingRecord,recordPath(impl.package),pending.recordId,&error))
        result={CacheState::Updated,RecordState::NotSaved,error,CacheFailureReason::RecordSaveFailed};
    result.cleanup=lease.close(int(std::max<qint64>(0,std::clamp(remainingCleanupBudgetMs,0,CleanupBudgetMs)-cleanupClock.elapsed())));
    return result;
}



QByteArray CacheStorage::workerOwnershipRequest(const std::shared_ptr<RuntimeCacheLease> &runtime,QString *error) {
    if (error) error->clear();
    if (!runtime || runtime->implementation_->closed || !runtime->implementation_->owned(error)) {
        fail(error,"Invalid runtime delegation"); return {};
    }
    const auto &impl=*runtime->implementation_;
    return QJsonDocument(QJsonObject{{"schemaVersion",CacheSchemaVersion},{"phase","runtime-authority"},
        {"nonce",QUuid::createUuid().toString(QUuid::Id128)}, {"package",impl.package},
        {"runtimeRegistry",impl.registry},{"runtimeMarker",QString::fromLatin1(impl.marker)},
        {"markerId",QString::fromLatin1(fileId(impl.markerPath))},
        {"runtimeId",QString::fromLatin1(fileId(impl.registry))}}).toJson(QJsonDocument::Compact);
}
bool CacheStorage::verifyWorkerOwnershipRequest(const QByteArray &request,const QString &ownedRoot,QString *error) {
    if (error) error->clear();
    const auto obj=boundedObject(request,error);
    const auto runtime=FileSystemPath::absolute(obj.value("runtimeRegistry").toString());
    const auto root=FileSystemPath::absolute(ownedRoot);
    const auto marker=root+"/.owner";
    if (obj.value("schemaVersion").toDouble()!=CacheSchemaVersion || obj.value("phase")!="runtime-authority"
        || !QRegularExpression("^[0-9a-f]{32}$").match(obj.value("nonce").toString()).hasMatch()
        || !QRegularExpression("^[0-9a-f]{64}$").match(obj.value("runtimeMarker").toString()).hasMatch()
        || !QRegularExpression("^startup-[0-9a-f-]{36}$").match(QFileInfo(root).fileName()).hasMatch()
        || identity(runtime)!=identity(root+"/runtime.bin") || !safePath(root,false,error)
        || obj.value("markerId").toString().isEmpty() || fileId(marker)!=obj.value("markerId").toString().toLatin1()
        || readBounded(marker,256,error)!=obj.value("runtimeMarker").toString().toLatin1())
        return fail(error,"Runtime worker ownership rejected");
    return true;
}
QByteArray CacheStorage::workerFillRequest(RegistryWriteLease &lease,const PreparedCacheCommit &prepared,QString *error) {
    return workerFillRequest(lease,prepared,{},error);
}
QByteArray CacheStorage::workerFillRequest(RegistryWriteLease &lease,const PreparedCacheCommit &prepared,
    const std::shared_ptr<RuntimeCacheLease> &sourceRuntime,QString *error) {
    if (error) error->clear();
    auto &impl=*lease.implementation_;
    if (!impl.registered(prepared) || impl.pending->sealAttempted) { fail(error,"Invalid fill delegation"); return {}; }
    const auto &pending=*impl.pending;
    const auto ownership=workerOwnershipRequest(prepared.runtime,error);
    if (ownership.isEmpty()) return {};
    QByteArray sourceOwnership;
    if (sourceRuntime) {
        if (identity(sourceRuntime->implementation_->package)!=identity(impl.package)) {fail(error,"Foreign fill source package");return {};}
        sourceOwnership=workerOwnershipRequest(sourceRuntime,error);
        if (sourceOwnership.isEmpty() || fileId(sourceRuntime->registryPath()).isEmpty()) {fail(error,"Invalid fill source owner");return {};}
    }
    impl.pending->sourceOwnership=sourceOwnership;
    auto obj=QJsonDocument::fromJson(ownership).object();
    obj["sourceOwnershipRequest"]=QString::fromUtf8(sourceOwnership);
    obj["phase"]="prepared-fill";
    obj["pendingRegistry"]=pending.registry; obj["pendingRecord"]=pending.record;
    obj["registryId"]=QString::fromLatin1(pending.registryId); obj["recordId"]=QString::fromLatin1(pending.recordId);
    obj["inputSha256"]=QString::fromLatin1(pending.input.sha256.toHex());
    obj["baselineExists"]=pending.baseline.exists; obj["baselineSha256"]=QString::fromLatin1(pending.baseline.sha256.toHex());
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}
bool CacheStorage::copyWorkerRegistry(const QByteArray &request,const QString &input,const QString &output,QString *error,
    bool allowSharedReadFallback,CacheReadStatus *sourceStatus) {
    if(sourceStatus)*sourceStatus=CacheReadStatus::Rejected;
    const auto obj=boundedObject(request,error);
    const auto root=QFileInfo(obj.value("runtimeRegistry").toString()).absolutePath();
    const auto destination=FileSystemPath::absolute(output);
    if (!verifyWorkerOwnershipRequest(request,root,error)
        || identity(QFileInfo(destination).absolutePath())!=identity(root)
        || !QRegularExpression("^[A-Za-z0-9-]+\\.bin$").match(QFileInfo(destination).fileName()).hasMatch()
        || (identity(destination)==identity(root+"/runtime.bin") && !obj.value("runtimeId").toString().isEmpty())
        || !safePath(destination,true,error))
        return fail(error,"Worker destination rejected");
    if (input.isEmpty()) return writeNew(destination,{},error);
    const auto source=FileSystemPath::absolute(input);
    if ((identity(source)!=identity(registry(obj.value("package").toString()))
            && identity(QFileInfo(source).absolutePath())!=identity(root))
        || !source.endsWith(".bin",Qt::CaseInsensitive) || !safePath(source,true,error))
        return fail(error,"Worker source rejected");
    if (!QFileInfo::exists(FileSystemPath::forIo(source))) return writeNew(destination,{},error);
    if (!ordinaryFile(source,error)) return false;
    const bool fallbackAllowed=allowSharedReadFallback && identity(source)==identity(registry(obj.value("package").toString()));
    QFile src(FileSystemPath::forIo(source));
    if(!src.open(QIODevice::ReadOnly)) {
        if(sourceStatus)*sourceStatus=CacheReadStatus::Unavailable;
        if(!fallbackAllowed)return fail(error,"Worker source read unavailable");
        return writeNew(destination,{},error);
    }
    const auto io=FileSystemPath::forIo(destination);
    HANDLE dst=CreateFileW(wide(io),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(dst==INVALID_HANDLE_VALUE)return fail(error,"Worker exclusive destination unavailable");
    bool ok=true,readFailed=false;
    while(!src.atEnd()) {
        const auto bytes=src.read(64*1024);
        if(bytes.isEmpty() || src.error()!=QFileDevice::NoError){readFailed=true;break;}
        DWORD written=0;
        if(!WriteFile(dst,bytes.constData(),DWORD(bytes.size()),&written,nullptr) || written!=DWORD(bytes.size())){ok=false;break;}
    }
    readFailed=readFailed || src.error()!=QFileDevice::NoError;
    if(readFailed) {
        if(sourceStatus)*sourceStatus=CacheReadStatus::Unavailable;
        LARGE_INTEGER zero{};
        ok=ok && fallbackAllowed && SetFilePointerEx(dst,zero,nullptr,FILE_BEGIN) && SetEndOfFile(dst);
    }
    ok=ok && FlushFileBuffers(dst);CloseHandle(dst);
    if(!ok)return fail(error,"Worker copy incomplete");
    if(sourceStatus && !readFailed)*sourceStatus=CacheReadStatus::Available;
    return true;
}
namespace {
// The identity is checked on the same operation handle before truncation; no name reopen for writing.
bool fillAuthorizedFile(const QString &path,const QByteArray &id,const QString &source,const QByteArray &bytes,QString *error) {
    if (!safePath(path,id.isEmpty(),error)) return false;
    const auto io=FileSystemPath::forIo(path);
    HANDLE handle=CreateFileW(wide(io),GENERIC_WRITE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,
        id.isEmpty()?CREATE_NEW:OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (handle==INVALID_HANDLE_VALUE) return fail(error,nativeError("Open delegated fill",path));
    BY_HANDLE_FILE_INFORMATION info{};
    bool ok=GetFileInformationByHandle(handle,&info) && !(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))
        && (id.isEmpty() || fileId(handle)==id);
    if (!ok) { CloseHandle(handle); return fail(error,"Delegated operation handle identity changed: "+path); }
    LARGE_INTEGER zero{}; ok=SetFilePointerEx(handle,zero,nullptr,FILE_BEGIN) && SetEndOfFile(handle);
    QFile src(FileSystemPath::forIo(source));
    if (!source.isEmpty()) ok=ok && src.open(QIODevice::ReadOnly);
    const auto write=[&](const QByteArray &data) { DWORD written=0; return WriteFile(handle,data.constData(),DWORD(data.size()),&written,nullptr) && written==DWORD(data.size()); };
    if (source.isEmpty()) ok=ok && write(bytes);
    else while (ok && !src.atEnd()) { const auto data=src.read(64*1024); ok=!data.isEmpty() && write(data); }
    ok=ok && (source.isEmpty() || src.error()==QFileDevice::NoError) && FlushFileBuffers(handle);
    const auto detail=nativeError("Fill delegated object",path); CloseHandle(handle);
    return ok || fail(error,detail);
}
}
QByteArray CacheStorage::fillPreparedWorkerRequest(const QByteArray &request,const QString &candidate,
    const CacheValidationRecord &record,QString *error) {
    if (error) error->clear();
    auto obj=boundedObject(request,error);
    if (obj.value("phase")!="prepared-fill") { fail(error,"Invalid fill phase"); return {}; }
    obj["phase"]="runtime-authority";
    const auto ownership=QJsonDocument(obj).toJson(QJsonDocument::Compact);
    const auto runtime=FileSystemPath::absolute(obj.value("runtimeRegistry").toString());
    const auto root=QFileInfo(runtime).absolutePath();
    const auto package=obj.value("package").toString();
    const auto pendingRegistry=FileSystemPath::absolute(obj.value("pendingRegistry").toString());
    const auto pendingRecord=FileSystemPath::absolute(obj.value("pendingRecord").toString());
    QString sourceRoot=root;
    const auto sourceOwnership=obj.value("sourceOwnershipRequest").toString().toUtf8();
    if (!sourceOwnership.isEmpty()) {
        const auto source=boundedObject(sourceOwnership,error);
        const auto sourceRegistry=source.value("runtimeRegistry").toString();
        sourceRoot=QFileInfo(sourceRegistry).absolutePath();
        if (identity(source.value("package").toString())!=identity(package)
            || !verifyWorkerOwnershipRequest(sourceOwnership,sourceRoot,error)
            || source.value("runtimeId").toString().isEmpty()
            || fileId(sourceRegistry)!=source.value("runtimeId").toString().toLatin1()) {
            fail(error,"Invalid fill source authority");return {};
        }
    }
    if (!verifyWorkerOwnershipRequest(ownership,root,error)
        || identity(QFileInfo(candidate).absolutePath())!=identity(sourceRoot) || !ordinaryFile(candidate,error)
        || identity(QFileInfo(pendingRegistry).absolutePath())!=identity(package+"/gstreamer-1.0")
        || identity(QFileInfo(pendingRecord).absolutePath())!=identity(package+"/gstreamer-1.0")
        || !QRegularExpression("^\\.startup-[0-9a-f-]{36}\\.bin$").match(QFileInfo(pendingRegistry).fileName()).hasMatch()
        || pendingRecord!=pendingRegistry.chopped(4)+".json"
        || obj.value("registryId").toString().isEmpty() || obj.value("recordId").toString().isEmpty()) {
        fail(error,"Invalid fill ownership"); return {};
    }
    const auto input=fingerprint(package);
    const CacheBaseline baseline{obj.value("baselineExists").toBool(),decodeHash(obj.value("baselineSha256"))};
    const auto actual=readBaseline(package,error);
    QByteArray candidateHash;
    if ((error && !error->isEmpty()) || !input.valid || input.sha256!=decodeHash(obj.value("inputSha256"))
        || !sameBaseline(actual,baseline) || !hashFile(candidate,&candidateHash,error)
        || !validationRecordMatches(record,input,{true,candidateHash})) {
        fail(error,"Verified candidate/input/baseline tuple changed"); return {};
    }
    if (!fillAuthorizedFile(pendingRegistry,obj.value("registryId").toString().toLatin1(),candidate,{},error)
        || !fillAuthorizedFile(pendingRecord,obj.value("recordId").toString().toLatin1(),{},encodeRecord(record),error)
        || (identity(candidate)!=identity(runtime)
            && !fillAuthorizedFile(runtime,obj.value("runtimeId").toString().toLatin1(),candidate,{},error))) return {};
    QByteArray registryHash,recordHash,runtimeHash;
    if (!hashFile(pendingRegistry,&registryHash,error) || !hashFile(pendingRecord,&recordHash,error)
        || !hashFile(runtime,&runtimeHash,error) || registryHash!=candidateHash || runtimeHash!=candidateHash) {
        fail(error,"Filled candidate bytes changed"); return {};
    }
    return QJsonDocument(QJsonObject{{"schemaVersion",CacheSchemaVersion},{"phase","prepared-filled"},
        {"nonce",obj.value("nonce")},{"pendingRegistry",pendingRegistry},{"pendingRecord",pendingRecord},
        {"registrySha256",QString::fromLatin1(registryHash.toHex())},
        {"recordSha256",QString::fromLatin1(recordHash.toHex())}}).toJson(QJsonDocument::Compact);
}
CacheCleanupResult CacheStorage::cleanupWorkerOwnershipRequest(const QByteArray &request,bool keepRuntime,int remainingBudgetMs) {
    QElapsedTimer elapsed;elapsed.start();
    QString error; const auto obj=boundedObject(request,&error);
    const auto registry=FileSystemPath::absolute(obj.value("runtimeRegistry").toString());
    const auto root=QFileInfo(registry).absolutePath();
    if (!verifyWorkerOwnershipRequest(request,root,&error)) return {false,{root},error};
    RuntimeCacheLease::Implementation runtime;
    runtime.directory=root; runtime.registry=registry; runtime.markerPath=root+"/.owner";
    runtime.marker=obj.value("runtimeMarker").toString().toLatin1();
    return runtime.cleanup(keepRuntime,int(std::max<qint64>(0,std::clamp(remainingBudgetMs,0,CleanupBudgetMs)-elapsed.elapsed())));
}
bool CacheStorage::acknowledgeCleanupProof(const std::shared_ptr<RuntimeCacheLease> &runtime,
    const QByteArray &authority,const QString &nonce,const QByteArray &proofBytes,
    const CacheCleanupResult &outcome,QString *error) {
    if (!runtime) return fail(error,"Missing runtime owner");
    auto &impl=*runtime->implementation_;
    if (impl.closed) return fail(error,"Runtime cleanup already acknowledged");
    const auto authorityObject=boundedObject(authority,error),proof=boundedObject(proofBytes,error);
    QStringList residuals;
    bool typed=proof.value("residualPaths").isArray();
    for (const auto &value:proof.value("residualPaths").toArray()) {
        typed=typed && value.isString() && !value.toString().isEmpty(); residuals.append(value.toString());
    }
    const bool accepted=authorityObject.value("phase")=="runtime-authority"
        && authorityObject.value("runtimeMarker").toString().toLatin1()==impl.marker
        && identity(authorityObject.value("runtimeRegistry").toString())==identity(impl.registry)
        && authorityObject.value("nonce").isString() && !authorityObject.value("nonce").toString().isEmpty()
        && proof.value("schemaVersion").toDouble()==CacheSchemaVersion && proof.value("phase")=="cleanup-proof"
        && QRegularExpression("^[0-9a-f]{32}$").match(nonce).hasMatch()
        && proof.value("nonce").toString()==nonce && proof.value("ownershipNonce")==authorityObject.value("nonce")
        && identity(proof.value("ownedRoot").toString())==identity(impl.directory)
        && proof.value("complete").isBool() && proof.value("complete").toBool()==outcome.complete
        && typed && residuals==outcome.residualPaths && (!outcome.complete || residuals.isEmpty());
    impl.closed=true;
    impl.result=accepted?outcome:CacheCleanupResult{false,{impl.directory},"Cleanup proof invalid or unavailable"};
    if (!accepted) return fail(error,impl.result.reason);
    return true;
}
CacheProtocolObject CacheStorage::createProtocolRequest(const std::shared_ptr<RuntimeCacheLease> &runtime,
    const QString &nonce,const QByteArray &bytes,QString *error) {
    if (!runtime || runtime->implementation_->closed || bytes.isEmpty() || bytes.size()>RecordLimit
        || !QRegularExpression("^[0-9a-f]{32}$").match(nonce).hasMatch()
        || !runtime->implementation_->owned(error)) { fail(error,"Invalid owned protocol request"); return {}; }
    const auto path=QFileInfo(runtime->implementation_->directory).absolutePath()+"/worker-"+nonce+".request.json";
    if (!safePath(path,true,error)) return {};
    const auto io=FileSystemPath::forIo(path);
    HANDLE handle=CreateFileW(wide(io),GENERIC_WRITE|GENERIC_READ,FILE_SHARE_READ,nullptr,CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (handle==INVALID_HANDLE_VALUE) { fail(error,nativeError("Create protocol request",path)); return {}; }
    const auto id=fileId(handle);DWORD written=0;
    const bool ok=!id.isEmpty() && WriteFile(handle,bytes.constData(),DWORD(bytes.size()),&written,nullptr)
        && written==DWORD(bytes.size()) && FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok) fail(error,"Protocol request write incomplete");
    // Retain the ID even on write failure so the owner can clean this exact object.
    return {path,id};
}
CacheProtocolObject CacheStorage::readProtocolResult(const std::shared_ptr<RuntimeCacheLease> &runtime,
    const QString &nonce,QByteArray *bytes,QString *error) {
    if (!runtime || !bytes || !QRegularExpression("^[0-9a-f]{32}$").match(nonce).hasMatch()) return {};
    const auto path=QFileInfo(runtime->implementation_->directory).absolutePath()+"/worker-"+nonce+".json";
    if (!safePath(path,false,error)) return {};
    const auto io=FileSystemPath::forIo(path);
    HANDLE handle=CreateFileW(wide(io),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (handle==INVALID_HANDLE_VALUE) { fail(error,nativeError("Read protocol result",path)); return {}; }
    const auto id=fileId(handle);LARGE_INTEGER length{};DWORD received=0;
    bool ok=!id.isEmpty() && GetFileSizeEx(handle,&length) && length.QuadPart>0 && length.QuadPart<=RecordLimit;
    if (ok) { bytes->resize(qsizetype(length.QuadPart));ok=ReadFile(handle,bytes->data(),DWORD(bytes->size()),&received,nullptr) && received==DWORD(bytes->size()); }
    CloseHandle(handle);
    if (!ok) { bytes->clear();fail(error,"Protocol result read incomplete");return {}; }
    return {path,id};
}
CacheCleanupResult CacheStorage::cleanupProtocolObjects(const QList<CacheProtocolObject> &objects,
    int remainingBudgetMs,GStreamerCacheNative::Remover &remover) {
    QElapsedTimer timer;timer.start();CacheCleanupResult result{true,{},{}};
    const int budget=std::clamp(remainingBudgetMs,0,CleanupBudgetMs);
    for (const auto &object:objects) {
        QString error;
        if (timer.elapsed()>=budget || object.nativeId.isEmpty()
            || !remover.removeFile(object.path,object.nativeId,&error)) {
            result.complete=false;result.residualPaths.append(object.path);continue;
        }
        // Actual-handle deletion may leave a foreign replacement at the registered name.
        if (QFileInfo::exists(FileSystemPath::forIo(object.path))) {
            result.complete=false;result.residualPaths.append(object.path);
        }
    }
    if (!result.complete) result.reason="Owned protocol cleanup incomplete";
    return result;
}