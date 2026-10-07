#include <QtTest/QtTest>
#include "platform/GStreamerCacheStorage.h"
#include "platform/FileSystemPath.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QRegularExpression>
#include <vector>
#include <QScopeGuard>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
void put(const QString &path, const QByteArray &bytes) {
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(FileSystemPath::forIo(path));
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(file.errorString()));
    QCOMPARE(file.write(bytes), bytes.size());
}
QByteArray get(const QString &path) {
    QFile file(FileSystemPath::forIo(path));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
QByteArray hash(const QByteArray &bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
QString runtimeParent() {
    const QString parent=qEnvironmentVariable("AIRPLAY_CACHE_STORAGE_TEST_RUNTIME");
    QDir().mkpath(parent);
    return parent;
}
void package(const QString &root) {
    put(root+"/airplay_receiver.exe", "app");
    put(root+"/core.dll", "core");
    put(root+"/gstreamer-plugins/libgstapp.dll", "plugin");
    put(root+"/libexec/gstreamer-1.0/gst-plugin-scanner.exe", "scanner");
    put(root+"/platforms/qwindows.dll", "platform");
    put(root+"/config/portable-runtime-manifest.txt",
        "airplay_receiver.exe\ncore.dll\nplatforms/qwindows.dll\nlibexec/gstreamer-1.0/gst-plugin-scanner.exe\n");
    QDir().mkpath(root+"/gstreamer-1.0");
}
CacheValidationRecord record(const CacheFingerprint &input, const QByteArray &registry) {
    CacheValidationRecord value;
    value.schemaVersion=CacheSchemaVersion;
    value.inputSha256=input.sha256;
    value.registrySha256=hash(registry);
    value.validated=true;
    value.plugins=QJsonArray{QJsonObject{{"name","app"},{"source","gstreamer-plugins/libgstapp.dll"}}};
    return value;
}
bool replaceName(const QString &path,const QString &aside) {
    const auto source=FileSystemPath::forIo(path),destination=FileSystemPath::forIo(aside);
    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()),reinterpret_cast<LPCWSTR>(destination.utf16()),0))
        return false;
    QFile foreign(source);
    return foreign.open(QIODevice::WriteOnly|QIODevice::NewOnly)
        && foreign.write("foreign-unverified")==18 && foreign.flush();
}
struct ReplacingPublisher : GStreamerCacheNative::Publisher {
    QString path,aside;
    bool beforeOpen=false,replaced=false;
    bool publishFile(const QString &source,const QString &target,const QByteArray &id,QString *error) override {
        if (source==path && beforeOpen) replaced=replaceName(path,aside);
        return Publisher::publishFile(source,target,id,error);
    }
    bool renameFile(void *handle,const QString &target,QString *error) override {
        if (!beforeOpen && !replaced) replaced=replaceName(path,aside);
        return Publisher::renameFile(handle,target,error);
    }
};
struct ReplacingRemover : GStreamerCacheNative::Remover {
    QString path,aside;
    bool beforeOpen=false,replaced=false;
    bool removeFile(const QString &source,const QByteArray &id,QString *error) override {
        if (source==path && beforeOpen) replaced=replaceName(path,aside);
        return Remover::removeFile(source,id,error);
    }
    bool deleteFile(void *handle,const QString &source,QString *error) override {
        if (source==path && !beforeOpen) replaced=replaceName(path,aside);
        return Remover::deleteFile(handle,source,error);
    }
};
HANDLE block(const QString &path) {
    const auto native=FileSystemPath::forIo(path);
    return CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}
}

class GStreamerCacheStorageTest : public QObject {
    Q_OBJECT
private slots:
    void sharedReadDenialCreatesOnlyOwnedBlankFallback(){
        QTemporaryDir temp(runtimeParent()+"/deny-XXXXXX");package(temp.path());QString error;
        const auto shared=temp.path()+"/gstreamer-1.0/registry.x86_64.bin";put(shared,"protected-shared");
        auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error);QVERIFY(runtime);
        const auto authority=CacheStorage::workerOwnershipRequest(runtime,&error);
        const auto io=FileSystemPath::forIo(shared);
        HANDLE holder=CreateFileW(reinterpret_cast<LPCWSTR>(io.utf16()),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        QVERIFY(holder!=INVALID_HANDLE_VALUE);const auto release=qScopeGuard([&]{if(holder!=INVALID_HANDLE_VALUE)CloseHandle(holder);});
        CacheReadStatus status;CacheStorage::readBaseline(temp.path(),&error,&status);QCOMPARE(status,CacheReadStatus::Unavailable);
        QVERIFY(CacheStorage::copyWorkerRegistry(authority,shared,runtime->registryPath(),&error,true,&status));
        QCOMPARE(status,CacheReadStatus::Unavailable);QVERIFY(QFileInfo::exists(runtime->registryPath()));QCOMPARE(get(runtime->registryPath()),QByteArray{});
        put(runtime->registryPath(),"already-owned-output");
        QVERIFY(!CacheStorage::copyWorkerRegistry(authority,shared,runtime->registryPath(),&error,true,&status));
        QCOMPARE(get(runtime->registryPath()),QByteArray("already-owned-output"));
        CloseHandle(holder);holder=INVALID_HANDLE_VALUE;QCOMPARE(get(shared),QByteArray("protected-shared"));
        QVERIFY(runtime->close().complete);
    }

    void assessCreatesOnlyAbsentAuthorizedRuntime() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX");package(temp.path());
        const auto shared=temp.path()+"/gstreamer-1.0/registry.x86_64.bin";put(shared,"old");
        QString error;auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error);QVERIFY(runtime);
        const auto authority=CacheStorage::workerOwnershipRequest(runtime,&error);
        QVERIFY(CacheStorage::copyWorkerRegistry(authority,shared,runtime->registryPath(),&error));
        QCOMPARE(get(runtime->registryPath()),QByteArray("old"));put(shared,"new");
        QVERIFY(!CacheStorage::copyWorkerRegistry(authority,shared,runtime->registryPath(),&error));
        QCOMPARE(get(runtime->registryPath()),QByteArray("old"));
        QVERIFY(replaceName(runtime->registryPath(),runtime->registryPath()+".aside"));
        QVERIFY(!CacheStorage::copyWorkerRegistry(authority,shared,runtime->registryPath(),&error));
        QCOMPARE(get(runtime->registryPath()),QByteArray("foreign-unverified"));QVERIFY(runtime->close().complete);
    }
    void separateSourceAuthorityPreservesFallbackAndBindsProof() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX");package(temp.path());
        const auto shared=temp.path()+"/gstreamer-1.0/registry.x86_64.bin";put(shared,"old-shared");
        QString error;auto a=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error);
        auto b=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error);QVERIFY(a);QVERIFY(b);
        put(a->registryPath(),"assessed");const auto sourceRoot=QFileInfo(a->registryPath()).absolutePath();
        const auto candidate=sourceRoot+"/verify.bin";put(candidate,"verified");put(sourceRoot+"/scan.bin","scan");
        const auto neighbor=QFileInfo(sourceRoot).absolutePath()+"/neighbor.bin";put(neighbor,"neighbor");
        auto lock=CacheStorage::tryRegistryWriteLock(temp.path());QVERIFY(lock.lease);
        const auto input=CacheStorage::fingerprint(temp.path());const auto baseline=CacheStorage::readBaseline(temp.path());
        const auto prepared=CacheStorage::reservePreparedCommit(*lock.lease,b,input,baseline,&error);
        const auto fill=CacheStorage::workerFillRequest(*lock.lease,prepared,a,&error);QVERIFY2(!fill.isEmpty(),qPrintable(error));
        QVERIFY(CacheStorage::fillPreparedWorkerRequest(fill,neighbor,record(input,"neighbor"),&error).isEmpty());
        auto forged=QJsonDocument::fromJson(fill).object();auto source=QJsonDocument::fromJson(forged.value("sourceOwnershipRequest").toString().toUtf8()).object();
        source["markerId"]="foreign";forged["sourceOwnershipRequest"]=QString::fromUtf8(QJsonDocument(source).toJson());
        QVERIFY(CacheStorage::fillPreparedWorkerRequest(QJsonDocument(forged).toJson(),candidate,record(input,"verified"),&error).isEmpty());
        const auto receipt=CacheStorage::fillPreparedWorkerRequest(fill,candidate,record(input,"verified"),&error);QVERIFY2(!receipt.isEmpty(),qPrintable(error));
        QCOMPARE(get(a->registryPath()),QByteArray("assessed"));QCOMPARE(get(b->registryPath()),QByteArray("verified"));
        const auto fields=QJsonDocument::fromJson(receipt).object();
        QVERIFY(CacheStorage::sealPreparedCommit(*lock.lease,prepared,hash("verified"),QByteArray::fromHex(fields.value("recordSha256").toString().toLatin1()),&error));
        const auto proof=CacheStorage::verifyPreparedWorkerRequest(CacheStorage::workerPreparationRequest(*lock.lease,prepared,&error),&error);QVERIFY2(!proof.isEmpty(),qPrintable(error));
        QVERIFY(!QFileInfo::exists(candidate));QVERIFY(!QFileInfo::exists(sourceRoot+"/scan.bin"));
        QCOMPARE(get(a->registryPath()),QByteArray("assessed"));QCOMPARE(get(neighbor),QByteArray("neighbor"));
        auto changed=QJsonDocument::fromJson(proof).object();changed["sourceOwnershipRequest"]="";
        QVERIFY(!CacheStorage::adoptPreparedCommit(*lock.lease,prepared,QJsonDocument(changed).toJson(),&error));
        QCOMPARE(get(shared),QByteArray("old-shared"));QVERIFY(CacheStorage::adoptPreparedCommit(*lock.lease,prepared,proof,&error));
        QCOMPARE(CacheStorage::publishPreparedCache(*lock.lease,prepared).cacheState,CacheState::Updated);
        QCOMPARE(get(shared),QByteArray("verified"));QVERIFY(a->close().complete);QVERIFY(b->close().complete);
    }
    void publicationElapsedTimeConsumesCleanupBudget() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX");package(temp.path());
        QString error;auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error);QVERIFY(runtime);
        put(runtime->registryPath(),"verified-new");auto lock=CacheStorage::tryRegistryWriteLock(temp.path());QVERIFY(lock.lease);
        const auto input=CacheStorage::fingerprint(temp.path());
        auto prepared=CacheStorage::prepareCommit(*lock.lease,runtime,runtime->registryPath(),record(input,"verified-new"),input,{},&error);
        QVERIFY(!prepared.pendingRegistry.isEmpty());
        struct DelayedPublisher:GStreamerCacheNative::Publisher {
            int count=0;
            bool publishFile(const QString &source,const QString &target,const QByteArray &id,QString *error)override {
                if(++count==1){QTest::qWait(30);return Publisher::publishFile(source,target,id,error);}
                *error="Intentional record refusal";return false;
            }
        } publisher;
        const auto result=CacheStorage::publishPreparedCache(*lock.lease,prepared,publisher,10);
        QCOMPARE(result.cacheState,CacheState::Updated);QCOMPARE(result.recordState,RecordState::NotSaved);
        QVERIFY(!result.cleanup.complete);QVERIFY(result.cleanup.residualPaths.contains(prepared.pendingRecord));
        QVERIFY(QFileInfo::exists(prepared.pendingRecord));QVERIFY(runtime->close().complete);
    }
    void cleanupProofClosesWithoutRecursionAndFreezesFailure() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        const auto authority=CacheStorage::workerOwnershipRequest(runtime,&error);
        const auto a=QJsonDocument::fromJson(authority).object();
        const auto root=QFileInfo(runtime->registryPath()).absolutePath();
        const QString nonce(32,'a');
        auto cleanup=CacheStorage::cleanupWorkerOwnershipRequest(authority,false,5000);QVERIFY(cleanup.complete);
        auto proof=QJsonDocument(QJsonObject{{"schemaVersion",1},{"phase","cleanup-proof"},{"nonce",nonce},
            {"ownershipNonce",a.value("nonce")},{"ownedRoot",root},{"complete",true},{"residualPaths",QJsonArray{}}}).toJson();
        QVERIFY(CacheStorage::acknowledgeCleanupProof(runtime,authority,nonce,proof,cleanup,&error));
        QVERIFY(runtime->close(0).complete); // removed root is deliberately not re-enumerated
        auto second=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error);QVERIFY(second);
        const auto secondPath=second->registryPath();put(secondPath,"preserved");
        QVERIFY(!CacheStorage::acknowledgeCleanupProof(second,authority,nonce,proof,cleanup,&error));
        auto closed=second->close(5000);QVERIFY(!closed.complete);QVERIFY(!closed.residualPaths.isEmpty());
        second.reset();QCOMPARE(get(secondPath),QByteArray("preserved"));
    }
    void protocolObjectsUseActualNativeOperationIdentity() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        QString error;auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error);QVERIFY(runtime);
        const QString nonce(32,'a');
        const auto request=CacheStorage::createProtocolRequest(runtime,nonce,"request",&error);QVERIFY(!request.nativeId.isEmpty());
        QVERIFY(CacheStorage::createProtocolRequest(runtime,nonce,"replace",&error).nativeId.isEmpty());
        const auto resultPath=QFileInfo(QFileInfo(runtime->registryPath()).absolutePath()).absolutePath()+"/worker-"+nonce+".json";
        put(resultPath,"result");QByteArray bytes;
        const auto result=CacheStorage::readProtocolResult(runtime,nonce,&bytes,&error);QCOMPARE(bytes,QByteArray("result"));QVERIFY(!result.nativeId.isEmpty());
        put(resultPath+".neighbor","neighbor");
        ReplacingRemover remover;remover.path=request.path;remover.aside=request.path+".aside";
        const auto cleanup=CacheStorage::cleanupProtocolObjects({request,result},5000,remover);
        QVERIFY(remover.replaced);QVERIFY(!cleanup.complete);QVERIFY(cleanup.residualPaths.contains(request.path));
        QCOMPARE(get(request.path),QByteArray("foreign-unverified"));QVERIFY(!QFileInfo::exists(resultPath));
        QCOMPARE(get(resultPath+".neighbor"),QByteArray("neighbor"));QVERIFY(runtime->close().complete);
    }
    void sealedRuntimeMustMatchVerifiedCandidate() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        auto lock=CacheStorage::tryRegistryWriteLock(temp.path()); QVERIFY(lock.lease);
        const auto input=CacheStorage::fingerprint(temp.path());
        const auto prepared=CacheStorage::reservePreparedCommit(*lock.lease,runtime,input,{},&error);
        put(runtime->registryPath(),"different-runtime"); put(prepared.pendingRegistry,"verified-new");
        auto value=record(input,"verified-new");
        put(prepared.pendingRecord,QJsonDocument(QJsonObject{{"schemaVersion",1},{"inputSha256",QString(input.sha256.toHex())},
            {"registrySha256",QString(hash("verified-new").toHex())},{"validated",true},{"blacklist",QJsonArray{}},{"plugins",value.plugins}}).toJson());
        QVERIFY(CacheStorage::sealPreparedCommit(*lock.lease,prepared,hash("verified-new"),hash(get(prepared.pendingRecord)),&error));
        QVERIFY(CacheStorage::verifyPreparedWorkerRequest(CacheStorage::workerPreparationRequest(*lock.lease,prepared,&error),&error).isEmpty());
        QVERIFY(lock.lease->close().complete); QVERIFY(runtime->close().complete);
    }
    void workerDelegationOwnsExactObjects() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        const auto root=QFileInfo(runtime->registryPath()).absolutePath();
        const auto ownership=CacheStorage::workerOwnershipRequest(runtime,&error);
        QVERIFY2(!ownership.isEmpty(),qPrintable(error));
        QVERIFY(CacheStorage::verifyWorkerOwnershipRequest(ownership,root,&error));
        QVERIFY(!CacheStorage::verifyWorkerOwnershipRequest(ownership,temp.path(),&error));
        auto altered=QJsonDocument::fromJson(ownership).object(); altered["runtimeMarker"]=QString(64,'0');
        QVERIFY(!CacheStorage::verifyWorkerOwnershipRequest(QJsonDocument(altered).toJson(),root,&error));
        const auto candidate=root+"/candidate.bin";
        QVERIFY(CacheStorage::copyWorkerRegistry(ownership,{},candidate,&error));
        put(candidate,"verified-new");
        QVERIFY(!CacheStorage::copyWorkerRegistry(ownership,{},candidate,&error));
        QCOMPARE(get(candidate),QByteArray("verified-new"));
        QVERIFY(!CacheStorage::copyWorkerRegistry(ownership,{},temp.path()+"/outside.bin",&error));
        QVERIFY(!CacheStorage::copyWorkerRegistry(ownership,temp.path()+"/core.dll",root+"/foreign.bin",&error));
        auto lock=CacheStorage::tryRegistryWriteLock(temp.path()); QVERIFY(lock.lease);
        const auto input=CacheStorage::fingerprint(temp.path());
        const auto prepared=CacheStorage::reservePreparedCommit(*lock.lease,runtime,input,CacheStorage::readBaseline(temp.path()),&error);
        QVERIFY(!prepared.pendingRegistry.isEmpty());
        const auto fill=CacheStorage::workerFillRequest(*lock.lease,prepared,&error); QVERIFY(!fill.isEmpty());
        const auto shared=temp.path()+"/gstreamer-1.0/registry.x86_64.bin";
        put(shared,"protected-shared");
        auto malicious=QJsonDocument::fromJson(fill).object();
        malicious["pendingRegistry"]=shared; malicious["pendingRecord"]=temp.path()+"/gstreamer-1.0/registry.x86_64.validation.json";
        QVERIFY(CacheStorage::fillPreparedWorkerRequest(QJsonDocument(malicious).toJson(),candidate,record(input,"verified-new"),&error).isEmpty());
        QCOMPARE(get(shared),QByteArray("protected-shared"));
        QVERIFY(QFile::remove(shared));
        malicious=QJsonDocument::fromJson(fill).object(); malicious["pendingRegistry"]=temp.path()+"/user.bin";
        put(temp.path()+"/user.bin","protected-user");
        QVERIFY(CacheStorage::fillPreparedWorkerRequest(QJsonDocument(malicious).toJson(),candidate,record(input,"verified-new"),&error).isEmpty());
        QCOMPARE(get(temp.path()+"/user.bin"),QByteArray("protected-user"));
        QVERIFY(CacheStorage::fillPreparedWorkerRequest(fill,candidate,record(input,"different-candidate"),&error).isEmpty());
        QCOMPARE(get(prepared.pendingRegistry),QByteArray{});
        const auto receipt=CacheStorage::fillPreparedWorkerRequest(fill,candidate,record(input,"verified-new"),&error);
        QVERIFY2(!receipt.isEmpty(),qPrintable(error));
        QCOMPARE(get(prepared.pendingRegistry),QByteArray("verified-new"));
        QCOMPARE(get(runtime->registryPath()),QByteArray("verified-new"));
        QVERIFY(CacheStorage::sealPreparedCommit(*lock.lease,prepared,hash("verified-new"),hash(get(prepared.pendingRecord)),&error));
        const auto proof=CacheStorage::verifyPreparedWorkerRequest(CacheStorage::workerPreparationRequest(*lock.lease,prepared,&error),&error);
        QVERIFY2(!proof.isEmpty(),qPrintable(error));
        QVERIFY(!QFileInfo::exists(candidate));
        QVERIFY(CacheStorage::adoptPreparedCommit(*lock.lease,prepared,proof,&error));
        QVERIFY(lock.lease->close().complete); QVERIFY(runtime->close().complete);
    }
    void workerFillPreservesForeignReplacement() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        auto lock=CacheStorage::tryRegistryWriteLock(temp.path()); QVERIFY(lock.lease);
        const auto input=CacheStorage::fingerprint(temp.path());
        const auto prepared=CacheStorage::reservePreparedCommit(*lock.lease,runtime,input,CacheStorage::readBaseline(temp.path()),&error);
        const auto request=CacheStorage::workerFillRequest(*lock.lease,prepared,&error); QVERIFY(!request.isEmpty());
        const auto candidate=QFileInfo(runtime->registryPath()).absolutePath()+"/candidate.bin"; put(candidate,"verified-new");
        QVERIFY(replaceName(prepared.pendingRegistry,prepared.pendingRegistry+".aside"));
        QVERIFY(CacheStorage::fillPreparedWorkerRequest(request,candidate,record(input,"verified-new"),&error).isEmpty());
        QCOMPARE(get(prepared.pendingRegistry),QByteArray("foreign-unverified"));
        QVERIFY(!lock.lease->close().complete); QVERIFY(runtime->close().complete);
    }
    void workerCleanupPreservesNeighborsAndBudget() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        const auto root=QFileInfo(runtime->registryPath()).absolutePath();
        const auto request=CacheStorage::workerOwnershipRequest(runtime,&error); QVERIFY(!request.isEmpty());
        put(root+"/candidate.bin","candidate"); put(temp.path()+"/neighbor.bin","neighbor");
        const auto zero=CacheStorage::cleanupWorkerOwnershipRequest(request,true,0); QVERIFY(!zero.complete);
        QCOMPARE(get(root+"/candidate.bin"),QByteArray("candidate"));
        const auto cleanup=CacheStorage::cleanupWorkerOwnershipRequest(request,true); QVERIFY(cleanup.complete);
        QVERIFY(!QFileInfo::exists(root+"/candidate.bin")); QCOMPARE(get(temp.path()+"/neighbor.bin"),QByteArray("neighbor"));
        QVERIFY(runtime->close().complete);
    }
    void nativePublicationReplacement_data() {
        QTest::addColumn<bool>("beforeOpen");
        QTest::newRow("before-operation-open") << true;
        QTest::newRow("after-operation-verification") << false;
    }
    void nativePublicationReplacement() {
        QFETCH(bool,beforeOpen);
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        const auto shared=temp.path()+"/gstreamer-1.0/registry.x86_64.bin"; put(shared,"old-shared");
        auto lock=CacheStorage::tryRegistryWriteLock(temp.path()); QVERIFY(lock.lease);
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        put(runtime->registryPath(),"verified-new");
        const auto input=CacheStorage::fingerprint(temp.path());
        const auto prepared=CacheStorage::prepareCommit(*lock.lease,runtime,runtime->registryPath(),record(input,"verified-new"),
            input,CacheStorage::readBaseline(temp.path()),&error); QVERIFY2(!prepared.pendingRegistry.isEmpty(),qPrintable(error));
        ReplacingPublisher native; native.path=prepared.pendingRegistry; native.aside=native.path+".aside"; native.beforeOpen=beforeOpen;
        const auto result=CacheStorage::publishPreparedCache(*lock.lease,prepared,native);
        QVERIFY(native.replaced);
        qInfo() << "Publication interleaving beforeOpen=" << beforeOpen << "target=" << get(shared)
                << "foreign=" << get(native.path) << "aside=" << get(native.aside);
        QCOMPARE(get(native.path),QByteArray("foreign-unverified"));
        QVERIFY(get(shared)!=QByteArray("foreign-unverified"));
        if (beforeOpen) {
            QCOMPARE(result.cacheState,CacheState::RecoveryFailed);
            QCOMPARE(result.failureReason,CacheFailureReason::OwnershipUnknown);
            QCOMPARE(get(shared),QByteArray("old-shared"));
            QCOMPARE(get(native.aside),QByteArray("verified-new"));
            const auto cleanup=lock.lease->close();
            QVERIFY(!cleanup.complete); QVERIFY(cleanup.residualPaths.contains(native.path));
        } else {
            QCOMPARE(result.cacheState,CacheState::Updated);
            QCOMPARE(result.recordState,RecordState::Saved);
            QCOMPARE(get(shared),QByteArray("verified-new"));
            QVERIFY(!QFileInfo::exists(native.aside));
            QVERIFY(!result.cleanup.complete); QVERIFY(result.cleanup.residualPaths.contains(native.path));
        }
        QVERIFY(runtime->close().complete);
    }
    void nativeCleanupReplacement_data() { nativePublicationReplacement_data(); }
    void nativeCleanupReplacement() {
        QFETCH(bool,beforeOpen);
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        const auto shared=temp.path()+"/gstreamer-1.0/registry.x86_64.bin"; put(shared,"old-shared");
        auto lock=CacheStorage::tryRegistryWriteLock(temp.path()); QVERIFY(lock.lease);
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        const auto prepared=CacheStorage::reservePreparedCommit(*lock.lease,runtime,CacheStorage::fingerprint(temp.path()),
            CacheStorage::readBaseline(temp.path()),&error); QVERIFY(!prepared.pendingRegistry.isEmpty());
        put(prepared.pendingRegistry,"verified-new");
        ReplacingRemover native; native.path=prepared.pendingRegistry; native.aside=native.path+".aside"; native.beforeOpen=beforeOpen;
        const auto result=lock.lease->close(5000,native);
        QVERIFY(native.replaced);
        qInfo() << "Cleanup interleaving beforeOpen=" << beforeOpen << "foreign=" << get(native.path)
                << "aside=" << get(native.aside);
        QCOMPARE(get(native.path),QByteArray("foreign-unverified"));
        QCOMPARE(get(shared),QByteArray("old-shared"));
        QVERIFY(!result.complete); QVERIFY(result.residualPaths.contains(native.path));
        QCOMPARE(lock.lease->close(5000).residualPaths,result.residualPaths);
        if (beforeOpen) QCOMPARE(get(native.aside),QByteArray("verified-new"));
        else QVERIFY(!QFileInfo::exists(native.aside));
        QVERIFY(!QFileInfo::exists(prepared.pendingRecord));
        QVERIFY(runtime->close().complete);
    }
    // A real DOS short-name alias must derive the same physical package fingerprint and ownership.
    void nativePackageAliasesHaveOneIdentity() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        const auto logical=FileSystemPath::absolute(temp.path());
        const auto native=QDir::toNativeSeparators(logical);
        const DWORD length=GetShortPathNameW(reinterpret_cast<LPCWSTR>(native.utf16()),nullptr,0);
        QVERIFY(length>0);
        std::vector<wchar_t> buffer(length+1);
        const DWORD written=GetShortPathNameW(reinterpret_cast<LPCWSTR>(native.utf16()),buffer.data(),DWORD(buffer.size()));
        QVERIFY(written>0 && written<buffer.size());
        const auto alias=QString::fromWCharArray(buffer.data(),int(written));
        const bool mapped=FileSystemPath::absolute(alias).compare(logical,Qt::CaseInsensitive)!=0;
        qInfo() << "Native DOS alias fixture mapped:" << mapped << logical << alias;
        const auto original=CacheStorage::fingerprint(logical),alternate=CacheStorage::fingerprint(alias);
        QVERIFY(original.valid); QVERIFY(alternate.valid);
        QCOMPARE(alternate.sha256,original.sha256);
        auto lock=CacheStorage::tryRegistryWriteLock(logical); QVERIFY(lock.lease);
        QCOMPARE(CacheStorage::tryRegistryWriteLock(alias).state,CacheLockState::Busy);
        QString error; auto runtime=CacheStorage::createRuntime(alias,logical+"/tmp",&error); QVERIFY(runtime);
        auto prepared=CacheStorage::reservePreparedCommit(*lock.lease,runtime,original,{},&error);
        QVERIFY2(!prepared.pendingRegistry.isEmpty(),qPrintable(error));
        QVERIFY(runtime->close().complete);
    }
    // Exhausted shared cleanup time must report exact residuals without a fresh destructor budget.
    void exhaustedSharedCleanupBudgetIsIdempotent() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        auto lock=CacheStorage::tryRegistryWriteLock(temp.path()); QVERIFY(lock.lease);
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        put(runtime->registryPath(),"retained-runtime");
        const auto prepared=CacheStorage::reservePreparedCommit(*lock.lease,runtime,CacheStorage::fingerprint(temp.path()),{},&error);
        QVERIFY(!prepared.pendingRegistry.isEmpty());
        const auto lockClose=lock.lease->close(0); QVERIFY(!lockClose.complete);
        QVERIFY(lockClose.residualPaths.contains(prepared.pendingRegistry));
        QVERIFY(lockClose.residualPaths.contains(prepared.pendingRecord));
        QCOMPARE(lock.lease->close(5000).residualPaths,lockClose.residualPaths);
        lock.lease.reset();
        QVERIFY(QFileInfo::exists(prepared.pendingRegistry)); QVERIFY(QFileInfo::exists(prepared.pendingRecord));
        QCOMPARE(CacheStorage::tryRegistryWriteLock(temp.path()).state,CacheLockState::Acquired);
    }
    void exhaustedRuntimeCleanupBudgetIsIdempotent() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        put(runtime->registryPath(),"retained-runtime");
        const auto runtimePath=runtime->registryPath();
        const auto runtimeClose=runtime->close(0); QVERIFY(!runtimeClose.complete);
        QVERIFY(runtimeClose.residualPaths.contains(runtimePath));
        QCOMPARE(runtime->close(5000).residualPaths,runtimeClose.residualPaths);
        runtime.reset(); QCOMPARE(get(runtimePath),QByteArray("retained-runtime"));
    }
    // Unknown lock types and nonexistent package roots must not be taken over.
    void foreignDirectoryLockStaysProtected() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        const auto foreign=temp.path()+"/gstreamer-1.0/.registry-startup.lock";
        QVERIFY(QDir().mkdir(foreign)); put(foreign+"/keep","user-data");
        QCOMPARE(CacheStorage::tryRegistryWriteLock(temp.path()).state,CacheLockState::ForeignObject);
        QCOMPARE(get(foreign+"/keep"),QByteArray("user-data"));
    }
    void missingPackageIsNotCreatedByLockAttempt() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX");
        const auto missing=temp.path()+"/absent";
        QCOMPARE(CacheStorage::tryRegistryWriteLock(missing).state,CacheLockState::Unavailable);
        QVERIFY(!QFileInfo::exists(missing));
    }
    // Native refusal must leave distinct old bytes and report a permission failure.
    void nativePermissionFailurePreservesOldCache() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        const auto shared=temp.path()+"/gstreamer-1.0/registry.x86_64.bin"; put(shared,"old");
        const auto io=FileSystemPath::forIo(shared);
        struct ResetAttributes {
            QString path;
            ~ResetAttributes() { SetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()),FILE_ATTRIBUTE_NORMAL); }
        } reset{io};
        QVERIFY(SetFileAttributesW(reinterpret_cast<LPCWSTR>(io.utf16()),FILE_ATTRIBUTE_READONLY));
        auto lock=CacheStorage::tryRegistryWriteLock(temp.path()); QVERIFY(lock.lease);
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        put(runtime->registryPath(),"new");
        const auto input=CacheStorage::fingerprint(temp.path());
        const auto prepared=CacheStorage::prepareCommit(*lock.lease,runtime,runtime->registryPath(),record(input,"new"),
            input,CacheStorage::readBaseline(temp.path()),&error); QVERIFY(!prepared.pendingRegistry.isEmpty());
        const auto result=CacheStorage::publishPreparedCache(*lock.lease,prepared);
        QCOMPARE(result.cacheState,CacheState::RecoveryFailed);
        QCOMPARE(result.failureReason,CacheFailureReason::DirectoryNotWritable);
        QCOMPARE(get(shared),QByteArray("old")); QVERIFY(runtime->close().complete);
    }
    // A controlled native record failure must retain the actual successful native cache commit.
    void injectedRecordFailureRetainsNativeCommit() {
        struct FailRecord : GStreamerCacheNative::Publisher {
            bool publishFile(const QString &pending,const QString &target,const QByteArray &expectedId,QString *error) override {
                if (target.endsWith(".validation.json")) { *error="Injected record publish failure"; return false; }
                return Publisher::publishFile(pending,target,expectedId,error);
            }
        } native;
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        const auto shared=temp.path()+"/gstreamer-1.0/registry.x86_64.bin"; put(shared,"old");
        auto lock=CacheStorage::tryRegistryWriteLock(temp.path()); QVERIFY(lock.lease);
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        put(runtime->registryPath(),"new");
        const auto input=CacheStorage::fingerprint(temp.path());
        const auto prepared=CacheStorage::prepareCommit(*lock.lease,runtime,runtime->registryPath(),record(input,"new"),
            input,CacheStorage::readBaseline(temp.path()),&error); QVERIFY(!prepared.pendingRegistry.isEmpty());
        const auto result=CacheStorage::publishPreparedCache(*lock.lease,prepared,native);
        QVERIFY2(result.cacheState==CacheState::Updated,qPrintable(result.reason));
        QCOMPARE(result.cacheState,CacheState::Updated); QCOMPARE(result.recordState,RecordState::NotSaved);
        QCOMPARE(get(shared),QByteArray("new")); QVERIFY(runtime->close().complete);
    }
    // Re-adoption must not silently authorize mutated contents after their validation.
    void adoptedPendingMutationCannotPublish() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        const auto shared=temp.path()+"/gstreamer-1.0/registry.x86_64.bin"; put(shared,"old");
        auto lock=CacheStorage::tryRegistryWriteLock(temp.path()); QVERIFY(lock.lease);
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        put(runtime->registryPath(),"new");
        const auto input=CacheStorage::fingerprint(temp.path());
        const auto prepared=CacheStorage::prepareCommit(*lock.lease,runtime,runtime->registryPath(),record(input,"new"),
            input,CacheStorage::readBaseline(temp.path()),&error); QVERIFY(!prepared.pendingRegistry.isEmpty());
        QFile tamper(prepared.pendingRegistry); QVERIFY(!tamper.open(QIODevice::WriteOnly|QIODevice::Truncate));
        QFile recordTamper(prepared.pendingRecord); QVERIFY(!recordTamper.open(QIODevice::WriteOnly|QIODevice::Truncate));
        auto foreignTuple=prepared; foreignTuple.input.sha256=hash("foreign-input");
        const auto result=CacheStorage::publishPreparedCache(*lock.lease,foreignTuple);
        QCOMPARE(result.cacheState,CacheState::RecoveryFailed);
        QCOMPARE(get(shared),QByteArray("old"));
        QVERIFY(runtime->close().complete);
    }
    // A pending filename is not ownership proof after the reserved object is replaced.
    void replacedPendingObjectIsPreserved() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        auto lock=CacheStorage::tryRegistryWriteLock(temp.path()); QVERIFY(lock.lease);
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        const auto input=CacheStorage::fingerprint(temp.path());
        const auto prepared=CacheStorage::reservePreparedCommit(*lock.lease,runtime,input,{},&error);
        QVERIFY(!prepared.pendingRegistry.isEmpty());
        QVERIFY(QFile::remove(prepared.pendingRegistry));
        put(prepared.pendingRegistry,"foreign");
        QVERIFY(!CacheStorage::adoptPreparedCommit(*lock.lease,prepared,QByteArray{},&error));
        lock.lease.reset();
        QCOMPARE(get(prepared.pendingRegistry),QByteArray("foreign"));
        QVERIFY(runtime->close().complete);
    }
    // Lost runtime ownership must leave user data intact, including the expected marker name.
    void alteredRuntimeMarkerPreventsCleanup() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        put(runtime->registryPath(),"retained");
        const auto marker=QFileInfo(runtime->registryPath()).absolutePath()+"/.owner"; put(marker,"foreign-marker");
        const auto result=runtime->close(); QVERIFY(!result.complete); QVERIFY(!result.residualPaths.isEmpty());
        QCOMPARE(get(runtime->registryPath()),QByteArray("retained")); QCOMPARE(get(marker),QByteArray("foreign-marker"));
        QVERIFY(!runtime->close().complete);
    }
    // Initial publication and exact-path delegation should preserve the complete tuple.
    void reservedWorkerOutputCanPublishFirstRegistry() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        auto lock=CacheStorage::tryRegistryWriteLock(temp.path()); QVERIFY(lock.lease);
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        put(runtime->registryPath(),"first-registry");
        const auto input=CacheStorage::fingerprint(temp.path());
        const auto prepared=CacheStorage::reservePreparedCommit(*lock.lease,runtime,input,{},&error);
        QVERIFY(!prepared.pendingRegistry.isEmpty());
        put(prepared.pendingRegistry,"first-registry");
        put(prepared.pendingRecord,QJsonDocument(QJsonObject{{"schemaVersion",1},
            {"inputSha256",QString(input.sha256.toHex())},{"registrySha256",QString(hash("first-registry").toHex())},
            {"validated",true},{"blacklist",QJsonArray{}},{"plugins",record(input,"first-registry").plugins}}).toJson());
        QVERIFY2(CacheStorage::sealPreparedCommit(*lock.lease,prepared,hash("first-registry"),hash(get(prepared.pendingRecord)),&error),qPrintable(error));
        const auto request=CacheStorage::workerPreparationRequest(*lock.lease,prepared,&error); QVERIFY(!request.isEmpty());
        QProcess worker;
        worker.start(QCoreApplication::applicationFilePath(),{"--verify-prepared",QString::fromLatin1(request.toBase64())});
        QVERIFY(worker.waitForFinished(5000)); QCOMPARE(worker.exitStatus(),QProcess::NormalExit); QCOMPARE(worker.exitCode(),0);
        const auto proof=worker.readAllStandardOutput(); QVERIFY2(!proof.isEmpty(),worker.readAllStandardError().constData());
        QVERIFY(!CacheStorage::adoptPreparedCommit(*lock.lease,prepared,"{broken",&error));
        auto wrong=QJsonDocument::fromJson(proof).object(); wrong["nonce"]="00000000000000000000000000000000";
        QVERIFY(!CacheStorage::adoptPreparedCommit(*lock.lease,prepared,QJsonDocument(wrong).toJson(),&error));
        wrong=QJsonDocument::fromJson(proof).object(); wrong["cleanupComplete"]=false;
        QVERIFY(!CacheStorage::adoptPreparedCommit(*lock.lease,prepared,QJsonDocument(wrong).toJson(),&error));
        QVERIFY2(CacheStorage::adoptPreparedCommit(*lock.lease,prepared,proof,&error),qPrintable(error));
        const auto result=CacheStorage::publishPreparedCache(*lock.lease,prepared);
        QVERIFY2(result.cacheState==CacheState::Updated,qPrintable(result.reason));
        QCOMPARE(result.cacheState,CacheState::Updated); QCOMPARE(result.recordState,RecordState::Saved);
        QCOMPARE(get(temp.path()+"/gstreamer-1.0/registry.x86_64.bin"),QByteArray("first-registry"));
        QVERIFY(validationRecordMatches(CacheStorage::readValidationRecord(temp.path()),input,CacheStorage::readBaseline(temp.path())));
        QVERIFY(QFileInfo::exists(runtime->registryPath())); QVERIFY(runtime->close().complete);
    }
    // Omitting file contents from the fingerprint would conceal equal-size mutations.
    void contentMutationWithUnchangedSizeAndMtimeChangesFingerprint() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX");
        QVERIFY(temp.isValid()); package(temp.path());
        auto before=CacheStorage::fingerprint(temp.path()); QVERIFY2(before.valid,qPrintable(before.reason));
        QFile dll(temp.path()+"/core.dll"); QVERIFY(dll.open(QIODevice::ReadOnly));
        const auto time=dll.fileTime(QFileDevice::FileModificationTime); dll.close();
        put(dll.fileName(),"CORE"); QVERIFY(dll.open(QIODevice::ReadWrite));
        QVERIFY(dll.setFileTime(time,QFileDevice::FileModificationTime)); dll.close();
        const auto after=CacheStorage::fingerprint(temp.path()); QVERIFY(after.valid);
        QVERIFY(before.sha256!=after.sha256);
        put(temp.path()+"/libjson-glib.dll","json");
        const auto added=CacheStorage::fingerprint(temp.path()); QVERIFY(added.valid);
        QVERIFY(after.sha256!=added.sha256);
        QVERIFY(QFile::remove(temp.path()+"/libjson-glib.dll"));
        QCOMPARE(CacheStorage::fingerprint(temp.path()).sha256, after.sha256);
        const auto moved=temp.path()+"-moved"; QVERIFY(QDir().rename(temp.path(),moved));
        temp.setAutoRemove(false);
        QVERIFY(CacheStorage::fingerprint(moved).sha256!=after.sha256);
        QVERIFY(QDir(moved).removeRecursively());
    }
    // A missing scanner, unreadable input, or unsafe manifest must never produce success evidence.
    void incompleteOrUnsafeInputsAreUnknown() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        const auto scanner=temp.path()+"/libexec/gstreamer-1.0/gst-plugin-scanner.exe";
        QVERIFY(QFile::remove(scanner)); QVERIFY(!CacheStorage::fingerprint(temp.path()).valid);
        put(scanner,"scanner");
        const auto file=temp.path()+"/core.dll";
        const auto native=FileSystemPath::forIo(file);
        HANDLE exclusive=CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
        QVERIFY(exclusive!=INVALID_HANDLE_VALUE);
        QVERIFY(!CacheStorage::fingerprint(temp.path()).valid); CloseHandle(exclusive);
        put(temp.path()+"/config/portable-runtime-manifest.txt","../outside.dll\n");
        QVERIFY(!CacheStorage::fingerprint(temp.path()).valid);
        put(temp.path()+"/config/portable-runtime-manifest.txt","C:/Windows/system32/kernel32.dll\n");
        QVERIFY(!CacheStorage::fingerprint(temp.path()).valid);
    }
    // Trusting any incomplete validation tuple could reuse an unvalidated registry.
    void completeValidationTupleRequired() {
        CacheFingerprint input{true,hash("input"),{}};
        CacheBaseline baseline{true,hash("registry")};
        auto value=record(input,"registry");
        QVERIFY(validationRecordMatches(value,input,baseline));
        value.registrySha256=hash("wrong");
        QVERIFY(!validationRecordMatches(value,input,baseline));
        value=record(input,"registry"); value.validated=false;
        QVERIFY(!validationRecordMatches(value,input,baseline));
        value=record(input,"registry"); ++value.schemaVersion;
        QVERIFY(!validationRecordMatches(value,input,baseline));
        value=record(input,"registry"); value.inputSha256=hash("wrong");
        QVERIFY(!validationRecordMatches(value,input,baseline));
        value=record(input,"registry"); value.plugins=QJsonArray{};
        QVERIFY(!validationRecordMatches(value,input,baseline));
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        put(temp.path()+"/gstreamer-1.0/registry.x86_64.validation.json","{broken");
        QVERIFY(!validationRecordMatches(CacheStorage::readValidationRecord(temp.path()),input,baseline));
        put(temp.path()+"/gstreamer-1.0/registry.x86_64.validation.json",
            QJsonDocument(QJsonObject{{"schemaVersion",1},{"inputSha256",QString(input.sha256.toHex())},
                {"registrySha256",QString(baseline.sha256.toHex())},{"validated",true},
                {"plugins",QJsonArray{QJsonObject{{"name","app"},{"source","C:/foreign/app.dll"}}}}}).toJson());
        QVERIFY(!validationRecordMatches(CacheStorage::readValidationRecord(temp.path()),input,baseline));
    }
    // Removing or stealing a persistent lock would permit concurrent publishers.
    void lockMarkersAndExclusiveHandlesProtectForeignFiles() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        const auto path=temp.path()+"/gstreamer-1.0/.registry-startup.lock";
        put(path,"AIRPLAY-GSTREAMER-REGISTRY-LOCK/1"); // Missing LF is foreign.
        QCOMPARE(CacheStorage::tryRegistryWriteLock(temp.path()).state,CacheLockState::ForeignObject);
        QCOMPARE(get(path),QByteArray("AIRPLAY-GSTREAMER-REGISTRY-LOCK/1"));
        QVERIFY(QFile::remove(path));
        auto first=CacheStorage::tryRegistryWriteLock(temp.path());
        QCOMPARE(first.state,CacheLockState::Acquired); QVERIFY(first.lease);
        QCOMPARE(CacheStorage::tryRegistryWriteLock(temp.path()).state,CacheLockState::Busy);
        QProcess child; child.start(QCoreApplication::applicationFilePath(),{"--try-lock",temp.path()});
        QVERIFY(child.waitForFinished(5000)); QCOMPARE(child.exitStatus(),QProcess::NormalExit);
        QCOMPARE(child.exitCode(),int(CacheLockState::Busy));
        first.lease.reset();
        QCOMPARE(get(path),QByteArray("AIRPLAY-GSTREAMER-REGISTRY-LOCK/1\n"));
        QCOMPARE(CacheStorage::tryRegistryWriteLock(temp.path()).state,CacheLockState::Acquired);
    }
    // Alternate path spelling must preserve identity; owned cleanup must preserve neighbors.
    void pathIdentityAndReparseObjectsStayProtected() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        const auto input=CacheStorage::fingerprint(temp.path()); QVERIFY(input.valid);
        QCOMPARE(CacheStorage::fingerprint(temp.path().toUpper()+"/.").sha256,input.sha256);
        const auto extended=FileSystemPath::forIo(temp.path());
        QCOMPARE(CacheStorage::fingerprint(extended).sha256,input.sha256);
        QString error;
        auto one=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error);
        auto two=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error);
        QVERIFY2(one,qPrintable(error)); QVERIFY(two);
        QVERIFY(one->registryPath()!=two->registryPath()); QVERIFY(!QFileInfo::exists(one->registryPath()));
        put(temp.path()+"/tmp/runtime.bin","foreign"); put(two->registryPath(),"other");
        const QString owned=QFileInfo(one->registryPath()).absolutePath();
        put(owned+"/runtime.bin","owned");
        QVERIFY(one->close().complete); QVERIFY(one->close().complete);
        QCOMPARE(get(temp.path()+"/tmp/runtime.bin"),QByteArray("foreign"));
        QCOMPARE(get(two->registryPath()),QByteArray("other"));
        const auto pluginJunction=temp.path()+"/gstreamer-plugins/junction";
        QProcess inputLink;
        inputLink.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(pluginJunction),
            QDir::toNativeSeparators(temp.path()+"/tmp")});
        QVERIFY(inputLink.waitForFinished(5000)); QCOMPARE(inputLink.exitCode(),0);
        QVERIFY(!CacheStorage::fingerprint(temp.path()).valid);
        QVERIFY(QDir().rmdir(pluginJunction));
        const auto linkDirectory=temp.path()+"/junction";
        QProcess mklink;
        mklink.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(linkDirectory),QDir::toNativeSeparators(temp.path()+"/tmp")});
        QVERIFY(mklink.waitForFinished(5000)); QCOMPARE(mklink.exitCode(),0);
        QVERIFY(!CacheStorage::createRuntime(temp.path(),linkDirectory,&error));
        QVERIFY(QDir().rmdir(linkDirectory)); QVERIFY(two->close().complete);
    }
    // A successful cache commit must remain Updated when its auxiliary record cannot publish.
    void postCommitRecordFailureRemainsUpdated() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        const auto shared=temp.path()+"/gstreamer-1.0/registry.x86_64.bin";
        const auto records=temp.path()+"/gstreamer-1.0/registry.x86_64.validation.json";
        put(shared,"old-cache"); put(records,"old-record");
        auto lock=CacheStorage::tryRegistryWriteLock(temp.path()); QVERIFY(lock.lease);
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        put(runtime->registryPath(),"new-cache");
        auto input=CacheStorage::fingerprint(temp.path());
        auto prepared=CacheStorage::prepareCommit(*lock.lease,runtime,runtime->registryPath(),
            record(input,"new-cache"),input,CacheStorage::readBaseline(temp.path()),&error);
        QVERIFY2(!prepared.pendingRegistry.isEmpty(),qPrintable(error));
        HANDLE held=block(records); QVERIFY(held!=INVALID_HANDLE_VALUE);
        auto result=CacheStorage::publishPreparedCache(*lock.lease,prepared);
        QVERIFY2(result.cacheState==CacheState::Updated,qPrintable(result.reason));
        QCOMPARE(result.cacheState,CacheState::Updated); QCOMPARE(result.recordState,RecordState::NotSaved);
        QCOMPARE(get(shared),QByteArray("new-cache")); QCOMPARE(get(records),QByteArray("old-record"));
        CloseHandle(held); QVERIFY(runtime->close().complete);
    }
    // Candidate cleanup must precede the single commit point.
    void candidateCleanupFailurePreservesOldCache() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        const auto shared=temp.path()+"/gstreamer-1.0/registry.x86_64.bin"; put(shared,"old-cache");
        auto lock=CacheStorage::tryRegistryWriteLock(temp.path()); QVERIFY(lock.lease);
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        const auto candidate=QFileInfo(runtime->registryPath()).absolutePath()+"/candidate.bin";
        put(candidate,"new-cache"); put(runtime->registryPath(),"runtime");
        const auto input=CacheStorage::fingerprint(temp.path());
        const auto before=hash(get(shared));
        HANDLE held=block(candidate); QVERIFY(held!=INVALID_HANDLE_VALUE);
        auto prepared=CacheStorage::prepareCommit(*lock.lease,runtime,candidate,record(input,"new-cache"),
            input,CacheStorage::readBaseline(temp.path()),&error);
        QVERIFY2(!prepared.pendingRegistry.isEmpty(),qPrintable(error));
        auto result=CacheStorage::publishPreparedCache(*lock.lease,prepared);
        QCOMPARE(result.cacheState,CacheState::RecoveryFailed); QCOMPARE(hash(get(shared)),before);
        CloseHandle(held); QVERIFY(runtime->close().complete);
    }
    // Concurrent inputs or target mutation must invalidate pending commit instead of overwriting it.
    void changedInputsAndTargetPreventPublication() {
        QTemporaryDir temp(runtimeParent()+"/s-XXXXXX"); package(temp.path());
        const auto shared=temp.path()+"/gstreamer-1.0/registry.x86_64.bin"; put(shared,"old");
        auto lock=CacheStorage::tryRegistryWriteLock(temp.path()); QVERIFY(lock.lease);
        QString error; auto runtime=CacheStorage::createRuntime(temp.path(),temp.path()+"/tmp",&error); QVERIFY(runtime);
        put(runtime->registryPath(),"new");
        const auto input=CacheStorage::fingerprint(temp.path());
        auto prepared=CacheStorage::reservePreparedCommit(*lock.lease,runtime,input,CacheStorage::readBaseline(temp.path()),&error);
        QVERIFY(!prepared.pendingRegistry.isEmpty()); put(prepared.pendingRegistry,"new");
        const QByteArray json=QJsonDocument(QJsonObject{{"schemaVersion",1},
            {"inputSha256",QString(input.sha256.toHex())},{"registrySha256",QString(hash("new").toHex())},
            {"validated",true},{"blacklist",QJsonArray{}},{"plugins",record(input,"new").plugins}}).toJson();
        put(prepared.pendingRecord,json); put(shared,"foreign-change");
        QVERIFY(CacheStorage::sealPreparedCommit(*lock.lease,prepared,hash("new"),hash(json),&error));
        QVERIFY(CacheStorage::verifyPreparedWorkerRequest(CacheStorage::workerPreparationRequest(*lock.lease,prepared,&error),&error).isEmpty());
        QCOMPARE(CacheStorage::publishPreparedCache(*lock.lease,prepared).cacheState,CacheState::RecoveryFailed);
        QCOMPARE(get(shared),QByteArray("foreign-change"));
        prepared=CacheStorage::reservePreparedCommit(*lock.lease,runtime,input,CacheStorage::readBaseline(temp.path()),&error);
        QVERIFY(!prepared.pendingRegistry.isEmpty()); put(prepared.pendingRegistry,"new"); put(prepared.pendingRecord,json);
        QVERIFY(CacheStorage::sealPreparedCommit(*lock.lease,prepared,hash("new"),hash(json),&error));
        put(temp.path()+"/core.dll","changed");
        QVERIFY(CacheStorage::verifyPreparedWorkerRequest(CacheStorage::workerPreparationRequest(*lock.lease,prepared,&error),&error).isEmpty());
        QCOMPARE(CacheStorage::publishPreparedCache(*lock.lease,prepared).cacheState,CacheState::RecoveryFailed);
        QCOMPARE(get(shared),QByteArray("foreign-change"));
        QVERIFY(runtime->close().complete);    }
};
// Test-only native producer, contained by the caller's owned Job. Default Qt tests stay unchanged.
int holdRegistryLock(const QString &packagePath,const QString &controlPath,const QString &nonce) {
    if (!QRegularExpression("^[a-f0-9]{32}$").match(nonce).hasMatch()) return 2;
    const auto package=QFileInfo(packagePath).canonicalFilePath();
    const auto control=QFileInfo(controlPath).canonicalFilePath();
    if (package.isEmpty() || control.isEmpty() || !control.startsWith(package+"/",Qt::CaseInsensitive)) return 2;
    for (QString cursor=control; !cursor.isEmpty(); cursor=QFileInfo(cursor).absolutePath()) {
        const auto native=FileSystemPath::forIo(cursor);
        const auto attributes=GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
        if (attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return 2;
        if (QFileInfo(cursor).absolutePath()==cursor) break;
    }
    if (get(control+"/.owner")!=nonce.toUtf8() ||
        QDir(control).entryList(QDir::Files|QDir::Dirs|QDir::Hidden|QDir::NoDotAndDotDot)!=QStringList{".owner"}) return 2;
    auto lock=CacheStorage::tryRegistryWriteLock(package);
    if (!lock.lease) return 3;
    QElapsedTimer lifetime; lifetime.start();
    const auto receipt=[&](const QString &phase,bool complete) {
        const auto path=control+"/"+phase+"-"+nonce+".json";
        const auto temporary=path+".pending";
        QFile file(FileSystemPath::forIo(temporary));
        const auto bytes=QJsonDocument(QJsonObject{{"nonce",nonce},{"phase",phase},{"complete",complete},
            {"producer","CacheStorage::RegistryWriteLease"},{"package",package}}).toJson(QJsonDocument::Compact);
        if (!file.open(QIODevice::WriteOnly|QIODevice::NewOnly) || file.write(bytes)!=bytes.size() || !file.flush()) return false;
        file.close();
        return QFile::rename(FileSystemPath::forIo(temporary),FileSystemPath::forIo(path));
    };
    if (!receipt("held",true)) { lock.lease->close(); return 4; }
    bool release=false;
    while (lifetime.elapsed()<80000) {
        const auto bytes=get(control+"/release-"+nonce+".json");
        if (!bytes.isEmpty()) {
            QJsonParseError parseError; const auto document=QJsonDocument::fromJson(bytes,&parseError);
            const auto fields=document.object();
            release=parseError.error==QJsonParseError::NoError && fields.size()==2 &&
                fields.value("nonce").toString()==nonce && fields.value("phase").toString()=="release";
            if (!release) { lock.lease->close(); return 5; }
            break;
        }
        QThread::msleep(10);
    }
    const auto cleanup=lock.lease->close();
    if (!release || !cleanup.complete || !receipt("released",cleanup.complete)) return 6;
    return 0;
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    if(app.arguments().size()==5 && app.arguments().at(1)=="--hold-lock")
        return holdRegistryLock(app.arguments().at(2),app.arguments().at(3),app.arguments().at(4));
    if(app.arguments().size()==3 && app.arguments().at(1)=="--try-lock")
        return int(CacheStorage::tryRegistryWriteLock(app.arguments().at(2)).state);
    if(app.arguments().size()==3 && app.arguments().at(1)=="--verify-prepared") {
        QString error;
        const auto proof=CacheStorage::verifyPreparedWorkerRequest(QByteArray::fromBase64(app.arguments().at(2).toLatin1()),&error);
        QFile output;
        if (!output.open(stdout,QIODevice::WriteOnly) || output.write(proof)!=proof.size() || !output.flush()) return 2;
        return proof.isEmpty()?1:0;
    }
    GStreamerCacheStorageTest test; return QTest::qExec(&test,argc,argv);
}
#include "GStreamerCacheStorageTest.moc"