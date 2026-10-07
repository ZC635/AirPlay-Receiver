#include <QtTest/QtTest>
#include "platform/GStreamerStartupCache.h"
#include <QCryptographicHash>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <QTextStream>
#include <QThread>
#include <QCoreApplication>
#include <QScopeGuard>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace {
void put(const QString &p,const QByteArray &b) {
    QDir().mkpath(QFileInfo(p).absolutePath()); QFile f(p);
    QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(b),b.size());
}
QByteArray sha(const QByteArray &b) { return QCryptographicHash::hash(b,QCryptographicHash::Sha256); }
QString runtimeParent() { return qEnvironmentVariable("AIRPLAY_CACHE_STARTUP_TEST_RUNTIME"); }
struct Fixture {
    QTemporaryDir dir{runtimeParent()+"/c-XXXXXX"};
    qint64 now=0,cleanupDeadline=-1;
    QList<qint64> deadlines;
    QList<CacheWorkerStage> stages;
    int scanCount=0,publishCount=0;
    bool healthy=false,scanFails=false,changed=false,hold=false,recordFails=false,targetChanged=false;
    std::function<void(CacheProcessResult,CacheWorkerResult)> pending;
    CacheWorkerRequest held;
    std::function<void()> duringPublish;
    QString prepareFault; bool assessReady=true,publishRefused=false;
    Fixture() {
        put(dir.path()+"/airplay_receiver.exe","exe"); put(dir.path()+"/core.dll","dll");
        put(dir.path()+"/gstreamer-plugins/libgstapp.dll","plugin");
        put(dir.path()+"/libexec/gstreamer-1.0/gst-plugin-scanner.exe","scanner");
        put(dir.path()+"/config/portable-runtime-manifest.txt","airplay_receiver.exe\ncore.dll\n");
        put(dir.path()+"/gstreamer-1.0/registry.x86_64.bin","old");
    }
    GStreamerCacheRequest request() { return {dir.path(),"unused",dir.path()+"/tmp"}; }
    void matchedRecord() {
        auto input=CacheStorage::fingerprint(dir.path());
        put(dir.path()+"/gstreamer-1.0/registry.x86_64.validation.json",QJsonDocument(QJsonObject{
            {"schemaVersion",1},{"validated",true},{"inputSha256",QString(input.sha256.toHex())},
            {"registrySha256",QString(sha("old").toHex())},{"blacklist",QJsonArray{}},
            {"plugins",QJsonArray{QJsonObject{{"name","app"},{"source","gstreamer-plugins/libgstapp.dll"}}}}}).toJson());
    }
    CacheWorkerResult answer(const CacheWorkerRequest &r) {
        CacheWorkerResult v; v.complete=true; v.nonce=r.nonce; v.stage=r.stage;
        v.readiness.ready=true; v.fingerprint=CacheStorage::fingerprint(dir.path());
        v.baseline=CacheStorage::readBaseline(dir.path());v.baselineTrusted=true;v.baselineReadStatus=CacheReadStatus::Available; v.blacklistFree=true; v.snapshotUnchanged=true;
        v.plugins=QJsonArray{QJsonObject{{"name","app"},{"source","gstreamer-plugins/libgstapp.dll"},{"loaded",true},{"local",true}}};
        if(r.stage==CacheWorkerStage::Assess) { put(r.outputRegistry,"old"); v.registrySha256=sha("old");v.readiness.ready=assessReady; }
        if(r.stage==CacheWorkerStage::Scan || r.stage==CacheWorkerStage::Verify) {
            if(scanFails) v.reason="optional scanner unavailable";
            else { put(r.outputRegistry,"new"); v.registrySha256=sha("new"); }
            if(changed) v.fingerprint.sha256=sha("changed");
            if(targetChanged) v.baseline.sha256=sha("changed-target");
        }
        if(r.stage==CacheWorkerStage::PrepareCommit) {
            auto obj=QJsonDocument::fromJson(r.preparationRequest).object();
            QByteArray proof;
            if(obj.value("phase")=="sealed-verification") {
                proof=CacheStorage::verifyPreparedWorkerRequest(r.preparationRequest,&v.reason);
                v.workerProof=proof; v.cleanup.complete=!proof.isEmpty();
            } else {
                proof=CacheStorage::fillPreparedWorkerRequest(r.preparationRequest,r.inputRegistry,r.validationRecord,&v.reason);
                if(!proof.isEmpty() && !prepareFault.isEmpty()) {
                    const auto path=obj.value("runtimeRegistry").toString();
                    if(prepareFault=="seal")QFile::remove(path);else put(path,"n");
                    if(prepareFault=="hash")v.reason="Injected post-fill hash failure";
                }
            }
            auto p=QJsonDocument::fromJson(proof).object();
            v.registrySha256=QByteArray::fromHex(p.value("registrySha256").toString().toLatin1());
            v.recordSha256=QByteArray::fromHex(p.value("recordSha256").toString().toLatin1());
            v.pendingRegistry=obj.value("pendingRegistry").toString(); v.pendingRecord=obj.value("pendingRecord").toString();
        }
        return v;
    }
    static CacheProcessResult process() { CacheProcessResult p; p.normalExit=true;p.exitCode=0;p.outputComplete=true;return p; }
    GStreamerCacheOperations operations() {
        return {[this]{return now;},[this](const CacheWorkerRequest &r,qint64 d,auto cb){
            stages.append(r.stage);deadlines.append(d); if(r.stage==CacheWorkerStage::Scan) ++scanCount;
            if(hold){held=r;pending=cb;}else {
                auto value=answer(r);auto p=process();
                if(r.stage==CacheWorkerStage::PrepareCommit && prepareFault=="partial") {p.normalExit=false;p.exitCode=7;p.reason="Injected crash after partial runtime fill";}
                cb(p,value);
            }
        },[this](qint64 d,auto cb){cleanupDeadline=d;cb({true,{},{}});},
        [this](RegistryWriteLease &l,const PreparedCacheCommit &p){
            ++publishCount;
            if(publishRefused)return CacheCommitResult{CacheState::RecoveryFailed,RecordState::NotApplicable,"Injected shared publication refusal",CacheFailureReason::DirectoryNotWritable};
            auto v=CacheStorage::publishPreparedCache(l,p);
            if(recordFails && v.cacheState==CacheState::Updated){v.recordState=RecordState::NotSaved;v.failureReason=CacheFailureReason::RecordSaveFailed;}
            if(duringPublish) duringPublish(); return v;
        }};
    }
};
}
QString nativePackage,nativeEvidence;
void captureNativeProtocols(const QString &temporaryParent) {
    QDir().mkpath(nativeEvidence+"/protocols");
    const auto files=QDir(temporaryParent+"/gst").entryInfoList({"*.json"},QDir::Files);
    for(const auto &file:files) {
        const auto target=nativeEvidence+"/protocols/"+file.fileName();
        if(!QFileInfo::exists(target))QFile::copy(file.absoluteFilePath(),target);
    }
}
class GStreamerStartupCacheTest:public QObject {
    Q_OBJECT
private slots:
    void initTestCase(){if(!runtimeParent().isEmpty())QDir().mkpath(runtimeParent());}
    void fingerprintRecheckFacts_data(){
        QTest::addColumn<bool>("beforeValid");QTest::addColumn<bool>("afterValid");
        QTest::addColumn<bool>("different");QTest::addColumn<bool>("coreReady");
        for(bool ready:{false,true})for(int kind=0;kind<5;++kind) {
            const bool before=kind!=3 && kind!=4,after=kind!=2 && kind!=4;
            QTest::newRow(qPrintable(QString("%1-%2").arg(ready?"ready":"not-ready").arg(kind)))<<before<<after<<(kind==1)<<ready;
        }
    }
    void fingerprintRecheckFacts(){
        QFETCH(bool,beforeValid);QFETCH(bool,afterValid);QFETCH(bool,different);QFETCH(bool,coreReady);
        CacheWorkerResult result;result.readiness.ready=coreReady;result.snapshotUnchanged=true;
        result.fingerprint={beforeValid,sha("before"),beforeValid?QString{}:QStringLiteral("initial unavailable")};
        const CacheFingerprint after{afterValid,sha(different?"different":"before"),afterValid?QString{}:QStringLiteral("final unavailable")};
        const bool changed=beforeValid && afterValid && different;
        QCOMPARE(reconcileCacheWorkerFingerprint(result,after),changed);
        QCOMPARE(result.readiness.ready,coreReady && !changed);
        QCOMPARE(result.fingerprint.valid,beforeValid && afterValid && !different);
        QCOMPARE(result.snapshotUnchanged,result.fingerprint.valid);
        if(!result.fingerprint.valid) {
            QCOMPARE(result.fingerprint.reason,changed?QStringLiteral("Package inputs changed during worker"):(!beforeValid?QStringLiteral("initial unavailable"):QStringLiteral("final unavailable")));
            Fixture f;auto operations=f.operations();
            operations.launch=[&](const CacheWorkerRequest &r,qint64,auto cb){
                f.stages.append(r.stage);auto answer=f.answer(r);answer.fingerprint=result.fingerprint;answer.readiness.ready=result.readiness.ready;cb(Fixture::process(),answer);
            };
            GStreamerStartupCache c(operations);QSignalSpy done(&c,&GStreamerStartupCache::finished);QVERIFY(c.start(f.request()));
            QCOMPARE(done.size(),1);const auto value=qvariant_cast<GStreamerCacheResult>(done[0][0]);
            QCOMPARE(value.cacheState,CacheState::RecoveryFailed);QCOMPARE(value.readinessState,result.readiness.ready?ReadinessState::Ready:ReadinessState::NotReady);
            QCOMPARE(bool(value.runtime),result.readiness.ready);QCOMPARE(f.stages.size(),1);QCOMPARE(f.publishCount,0);
            QVERIFY(!QFileInfo::exists(f.dir.path()+"/gstreamer-1.0/.registry-startup.lock"));
        }
    }
    void nativeDefaultRecoveryAndReuse(){
        QVERIFY(!nativePackage.isEmpty());
        GStreamerCacheRequest r{nativePackage,nativePackage+"/airplay_receiver.exe",nativePackage+"/native-tmp"};
        const auto old=qgetenv("GST_PLUGIN_PATH");qputenv("GST_PLUGIN_PATH","C:/foreign-forbidden");
        const auto oldDebug=qgetenv("GST_DEBUG_FILE");qputenv("GST_DEBUG_FILE",(nativePackage+"/must-not-create.log").toUtf8());
        GStreamerStartupCache first;QSignalSpy done(&first,&GStreamerStartupCache::finished);QSignalSpy stages(&first,&GStreamerStartupCache::stageChanged);
        connect(&first,&GStreamerStartupCache::stageChanged,this,[&](CacheWorkerStage stage){qInfo()<<"Native recovery stage"<<int(stage);captureNativeProtocols(r.temporaryParent);});
        QVERIFY(first.start(r));QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,60000);
        qputenv("GST_PLUGIN_PATH",old);qputenv("GST_DEBUG_FILE",oldDebug);
        auto value=qvariant_cast<GStreamerCacheResult>(done[0][0]);qInfo()<<"Native recovery"<<int(value.cacheState)<<int(value.readinessState)<<value.reason<<value.cleanup.reason;
        QCOMPARE(value.cacheState,CacheState::Updated);QCOMPARE(value.readinessState,ReadinessState::Ready);QVERIFY(value.runtime);QVERIFY(value.cleanup.complete);
        QCOMPARE(stages.size(),5);QVERIFY(!QFileInfo::exists(nativePackage+"/must-not-create.log"));
        QVERIFY(value.runtime->close().complete);
        GStreamerStartupCache second;QSignalSpy reused(&second,&GStreamerStartupCache::finished);QSignalSpy reusedStages(&second,&GStreamerStartupCache::stageChanged);
        connect(&second,&GStreamerStartupCache::stageChanged,this,[&](CacheWorkerStage stage){qInfo()<<"Native reuse stage"<<int(stage);captureNativeProtocols(r.temporaryParent);});
        QVERIFY(second.start(r));QTRY_COMPARE_WITH_TIMEOUT(reused.size(),1,60000);
        auto healthy=qvariant_cast<GStreamerCacheResult>(reused[0][0]);qInfo()<<"Native reuse"<<int(healthy.cacheState)<<healthy.reason;
        QCOMPARE(healthy.cacheState,CacheState::Reused);QCOMPARE(reusedStages.size(),1);QVERIFY(healthy.runtime);QVERIFY(healthy.cleanup.complete);QVERIFY(healthy.runtime->close().complete);
    }
    void nativeCancelAfterSealing(){
        QVERIFY(!nativePackage.isEmpty());const auto shared=nativePackage+"/gstreamer-1.0/registry.x86_64.bin";
        QFile file(shared);QVERIFY(file.open(QIODevice::ReadOnly));const auto before=sha(file.readAll());file.close();
        QVERIFY(QFile::remove(nativePackage+"/gstreamer-1.0/registry.x86_64.validation.json"));
        GStreamerStartupCache c;QSignalSpy done(&c,&GStreamerStartupCache::finished);int prepareCount=0;
        const auto temporary=nativePackage+"/sealed-cancel-tmp";
        connect(&c,&GStreamerStartupCache::stageChanged,this,[&](CacheWorkerStage stage){
            captureNativeProtocols(temporary);
            if(stage==CacheWorkerStage::PrepareCommit && ++prepareCount==2)c.cancel();
        });
        QVERIFY(c.start({nativePackage,nativePackage+"/airplay_receiver.exe",temporary}));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,60000);const auto value=qvariant_cast<GStreamerCacheResult>(done[0][0]);
        qInfo()<<"Native sealed cancellation"<<value.cancelled<<value.cleanup.complete<<value.cleanup.reason<<value.cleanup.residualPaths;
        QVERIFY(value.cancelled);QVERIFY(!value.runtime);QCOMPARE(prepareCount,2);QVERIFY(value.cleanup.complete);
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(sha(file.readAll()),before);
        QVERIFY(QDir(temporary+"/gst").entryList({"startup-*"},QDir::Dirs|QDir::NoDotAndDotDot).isEmpty());
    }
    void nativePartialPreparationPreservesFallback(){
        const auto shared=nativePackage+"/gstreamer-1.0/registry.x86_64.bin";
        QFile file(shared);QVERIFY(file.open(QIODevice::ReadOnly));const auto before=sha(file.readAll());file.close();
        QFile::remove(nativePackage+"/gstreamer-1.0/registry.x86_64.validation.json");
        const auto temporary=nativePackage+"/partial-tmp";QString assessedPath;QByteArray assessedBytes;
        qputenv("AIRPLAY_CACHE_TEST_FRAME","partial-prepare");
        qputenv("AIRPLAY_CACHE_TEST_FAULT_EVIDENCE",(nativeEvidence+"/partial-fill.json").toUtf8());
        GStreamerStartupCache c;QSignalSpy done(&c,&GStreamerStartupCache::finished);QSignalSpy stages(&c,&GStreamerStartupCache::stageChanged);
        connect(&c,&GStreamerStartupCache::stageChanged,this,[&](CacheWorkerStage stage){
            captureNativeProtocols(temporary);
            if(stage==CacheWorkerStage::Scan){
                const auto roots=QDir(temporary+"/gst").entryList({"startup-*"},QDir::Dirs|QDir::NoDotAndDotDot);
                if(roots.size()==1){assessedPath=temporary+"/gst/"+roots[0]+"/runtime.bin";QFile f(assessedPath);if(f.open(QIODevice::ReadOnly))assessedBytes=f.readAll();}
            }
        });
        QVERIFY(c.start({nativePackage,nativePackage+"/coordinator-test.exe",temporary}));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,60000);qunsetenv("AIRPLAY_CACHE_TEST_FRAME");qunsetenv("AIRPLAY_CACHE_TEST_FAULT_EVIDENCE");
        const auto value=qvariant_cast<GStreamerCacheResult>(done[0][0]);
        qInfo()<<"Native partial fill"<<value.reason<<value.cleanup.reason<<value.cleanup.residualPaths;
        QCOMPARE(stages.size(),4);QCOMPARE(value.cacheState,CacheState::RecoveryFailed);QCOMPARE(value.readinessState,ReadinessState::Ready);
        QVERIFY(value.runtime);QCOMPARE(QDir::fromNativeSeparators(value.runtime->registryPath()),QDir::fromNativeSeparators(assessedPath));QVERIFY(!assessedBytes.isEmpty());
        QFile fallback(assessedPath);QVERIFY(fallback.open(QIODevice::ReadOnly));QCOMPARE(fallback.readAll(),assessedBytes);fallback.close();
        QFile evidence(nativeEvidence+"/partial-fill.json");QVERIFY(evidence.open(QIODevice::ReadOnly));
        const auto fault=QJsonDocument::fromJson(evidence.readAll()).object();QCOMPARE(fault.value("bytes").toString(),QString("partial"));
        QVERIFY(!fault.value("receipt").toString().isEmpty());
        const auto staged=fault.value("partialRuntime").toString();QVERIFY(!staged.isEmpty());
        QVERIFY(QDir::fromNativeSeparators(staged)!=QDir::fromNativeSeparators(assessedPath));QVERIFY(!QFileInfo::exists(QFileInfo(staged).absolutePath()));
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(sha(file.readAll()),before);file.close();
        QVERIFY(value.cleanup.complete);QCOMPARE(QDir(temporary+"/gst").entryList({"startup-*"},QDir::Dirs|QDir::NoDotAndDotDot).size(),1);
        QVERIFY(value.runtime->close().complete);
    }
    void nativeTransportAcceptance_data(){
        QTest::addColumn<QByteArray>("mode");
        for(const auto *mode:{"wrong-nonce","wrong-stage","incomplete","old-json","empty-output","crash","refusal","invalid-baseline-status","invalid-baseline-combination","missing-baseline-trust","invalid-baseline-hash","invalid-baseline-absent-hash","invalid-baseline-available-empty"})QTest::newRow(mode)<<QByteArray(mode);
    }
    void nativeTransportAcceptance(){
        QFETCH(QByteArray,mode);QVERIFY(!nativePackage.isEmpty());qputenv("AIRPLAY_CACHE_TEST_FRAME",mode);
        GStreamerStartupCache c;QSignalSpy done(&c,&GStreamerStartupCache::finished);QSignalSpy stages(&c,&GStreamerStartupCache::stageChanged);
        QVERIFY(c.start({nativePackage,nativePackage+"/coordinator-test.exe",nativePackage+"/transport-tmp"}));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);qunsetenv("AIRPLAY_CACHE_TEST_FRAME");
        auto value=qvariant_cast<GStreamerCacheResult>(done[0][0]);qInfo()<<"Native transport"<<mode<<value.reason<<value.cleanup.reason<<value.cleanup.residualPaths;
        QCOMPARE(value.cacheState,CacheState::RecoveryFailed);QVERIFY(!value.runtime);QCOMPARE(value.readinessState,ReadinessState::Unknown);
        QCOMPARE(stages.size(),1);QVERIFY(!value.reason.isEmpty());
        if(mode.startsWith("invalid-baseline") || mode=="missing-baseline-trust")QVERIFY(value.reason.startsWith("Worker result"));
    }
    void nativeDestructionRetainsCleanupOwner(){
        QVERIFY(!nativePackage.isEmpty());qputenv("AIRPLAY_CACHE_TEST_FRAME","wait");
        const auto marker=nativePackage+"/worker-live";QFile::remove(marker);int done=0;
        auto *c=new GStreamerStartupCache;connect(c,&GStreamerStartupCache::finished,this,[&]{++done;});
        QVERIFY(c->start({nativePackage,nativePackage+"/coordinator-test.exe",nativePackage+"/destroy-tmp"}));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker),10000);
        QVERIFY(!QDir(nativePackage+"/destroy-tmp/gst").entryList({"startup-*"},QDir::Dirs|QDir::NoDotAndDotDot).isEmpty());
        delete c;qunsetenv("AIRPLAY_CACHE_TEST_FRAME");
        QTRY_VERIFY_WITH_TIMEOUT(QDir(nativePackage+"/destroy-tmp/gst").entryList({"startup-*"},QDir::Dirs|QDir::NoDotAndDotDot).isEmpty(),6000);
        QTest::qWait(100);QCOMPARE(done,0);
    }
    void healthyReuseNeedsNoWriteLock(){
        Fixture f;f.matchedRecord();auto lock=CacheStorage::tryRegistryWriteLock(f.dir.path());QVERIFY(lock.lease);
        GStreamerStartupCache c(f.operations());QSignalSpy done(&c,&GStreamerStartupCache::finished);
        QVERIFY(c.start(f.request()));QCOMPARE(done.size(),1);auto v=qvariant_cast<GStreamerCacheResult>(done[0][0]);
        QCOMPARE(v.cacheState,CacheState::Reused);QCOMPARE(v.recordState,RecordState::Matched);QCOMPARE(f.scanCount,0);QCOMPARE(f.stages.size(),1);QVERIFY(v.runtime);
        QVERIFY(!c.start(f.request()));QCOMPARE(done.size(),1);
    }
    void changedTupleAttemptsRecoveryOnce(){
        Fixture f;GStreamerStartupCache c(f.operations());QSignalSpy done(&c,&GStreamerStartupCache::finished);c.start(f.request());
        QCOMPARE(done.size(),1);QCOMPARE(qvariant_cast<GStreamerCacheResult>(done[0][0]).cacheState,CacheState::Updated);
        QCOMPARE(qvariant_cast<GStreamerCacheResult>(done[0][0]).recordState,RecordState::Saved);
        QCOMPARE(f.scanCount,1);QCOMPARE(f.publishCount,1);QCOMPARE(f.deadlines.mid(0,3),QList<qint64>({60000,60000,60000}));
    }
    void optionalFailureWithReadyCoreCanContinue(){
        Fixture f;f.scanFails=true;GStreamerStartupCache c(f.operations());QSignalSpy done(&c,&GStreamerStartupCache::finished);c.start(f.request());
        QCOMPARE(done.size(),1);auto v=qvariant_cast<GStreamerCacheResult>(done[0][0]);
        QCOMPARE(v.readinessState,ReadinessState::Ready);QVERIFY(v.runtime);QCOMPARE(f.scanCount,1);QCOMPARE(f.publishCount,0);
    }
    void failedPreparationPreservesAssessedRuntime_data(){
        QTest::addColumn<QString>("fault");QTest::addColumn<bool>("ready");
        QTest::newRow("partial-crash-ready")<<QString("partial")<<true;
        QTest::newRow("hash-failure-ready")<<QString("hash")<<true;
        QTest::newRow("seal-failure-ready")<<QString("seal")<<true;
        QTest::newRow("partial-crash-not-ready")<<QString("partial")<<false;
        QTest::newRow("sealed-proof-failure-ready")<<QString("proof")<<true;
        QTest::newRow("sealed-proof-failure-not-ready")<<QString("proof")<<false;
    }
    void failedPreparationPreservesAssessedRuntime(){
        QFETCH(QString,fault);QFETCH(bool,ready);Fixture f;f.prepareFault=fault;f.assessReady=ready;
        GStreamerStartupCache c(f.operations());QSignalSpy done(&c,&GStreamerStartupCache::finished);c.start(f.request());
        QCOMPARE(done.size(),1);const auto value=qvariant_cast<GStreamerCacheResult>(done[0][0]);
        QCOMPARE(f.scanCount,1);QCOMPARE(f.publishCount,0);QCOMPARE(value.cacheState,CacheState::RecoveryFailed);
        QFile shared(f.dir.path()+"/gstreamer-1.0/registry.x86_64.bin");QVERIFY(shared.open(QIODevice::ReadOnly));QCOMPARE(shared.readAll(),QByteArray("old"));
        if(ready){
            QCOMPARE(value.readinessState,ReadinessState::Ready);QVERIFY(value.runtime);
            QFile fallback(value.runtime->registryPath());QVERIFY(fallback.open(QIODevice::ReadOnly));QCOMPARE(fallback.readAll(),QByteArray("old"));
        }else{QVERIFY(value.readinessState!=ReadinessState::Ready);QVERIFY(!value.runtime);}
    }
    void verifiedStagedRuntimeSurvivesPublicationRefusal(){
        Fixture f;f.assessReady=false;f.publishRefused=true;
        GStreamerStartupCache c(f.operations());QSignalSpy done(&c,&GStreamerStartupCache::finished);QVERIFY(c.start(f.request()));
        QCOMPARE(done.size(),1);QCOMPARE(f.publishCount,1);QCOMPARE(f.stages.size(),5);QCOMPARE(f.scanCount,1);
        const auto value=qvariant_cast<GStreamerCacheResult>(done[0][0]);QCOMPARE(value.cacheState,CacheState::RecoveryFailed);
        QCOMPARE(value.readinessState,ReadinessState::Ready);QVERIFY(value.runtime);
        QFile runtime(value.runtime->registryPath());QVERIFY(runtime.open(QIODevice::ReadOnly));QCOMPARE(runtime.readAll(),QByteArray("new"));
        QFile shared(f.dir.path()+"/gstreamer-1.0/registry.x86_64.bin");QVERIFY(shared.open(QIODevice::ReadOnly));QCOMPARE(shared.readAll(),QByteArray("old"));
    }
    void tempRootFailureHasNoDefaultFallback(){
        Fixture f;auto r=f.request();put(f.dir.path()+"/blocked","x");r.temporaryParent=f.dir.path()+"/blocked";
        GStreamerStartupCache c(f.operations());QSignalSpy done(&c,&GStreamerStartupCache::finished);c.start(r);
        QCOMPARE(done.size(),1);auto v=qvariant_cast<GStreamerCacheResult>(done[0][0]);
        QCOMPARE(v.failureReason,CacheFailureReason::TempUnavailable);QCOMPARE(v.readinessState,ReadinessState::Unknown);QVERIFY(!v.runtime);QVERIFY(f.stages.isEmpty());
    }
    void sourceOrSharedTargetChangeRejectsCandidate(){
        Fixture f;f.changed=true;GStreamerStartupCache c(f.operations());QSignalSpy done(&c,&GStreamerStartupCache::finished);c.start(f.request());
        QCOMPARE(done.size(),1);auto v=qvariant_cast<GStreamerCacheResult>(done[0][0]);
        QCOMPARE(v.cacheState,CacheState::RecoveryFailed);QCOMPARE(v.failureReason,CacheFailureReason::InputChanged);QCOMPARE(f.publishCount,0);
    }
    void sharedTargetChangeRejectsCandidate(){
        Fixture f;f.targetChanged=true;GStreamerStartupCache c(f.operations());QSignalSpy done(&c,&GStreamerStartupCache::finished);c.start(f.request());
        QCOMPARE(done.size(),1);auto value=qvariant_cast<GStreamerCacheResult>(done[0][0]);
        QCOMPARE(value.failureReason,CacheFailureReason::InputChanged);QCOMPARE(f.publishCount,0);QCOMPARE(f.scanCount,1);
    }
    void busyRecoveryKeepsActualReadyRuntime(){
        Fixture f;auto lock=CacheStorage::tryRegistryWriteLock(f.dir.path());QVERIFY(lock.lease);
        GStreamerStartupCache c(f.operations());QSignalSpy done(&c,&GStreamerStartupCache::finished);c.start(f.request());
        QCOMPARE(done.size(),1);auto value=qvariant_cast<GStreamerCacheResult>(done[0][0]);
        QCOMPARE(value.cacheState,CacheState::RecoverySkipped);QCOMPARE(value.readinessState,ReadinessState::Ready);QVERIFY(value.runtime);QCOMPARE(f.scanCount,0);
    }
    void assessedRecordFactsSurviveBusy_data(){
        QTest::addColumn<QString>("fact");QTest::addColumn<int>("expected");
        QTest::newRow("missing")<<QString("missing")<<4;
        QTest::newRow("malformed")<<QString("malformed")<<3;
        QTest::newRow("unreadable")<<QString("unreadable")<<4;
        QTest::newRow("tuple-mismatch")<<QString("mismatch")<<3;
        QTest::newRow("matching-sdk-change")<<QString("sdk-change")<<0;
    }
    void assessedRecordFactsSurviveBusy(){
        QFETCH(QString,fact);QFETCH(int,expected);Fixture f;
        if(fact=="malformed")put(f.dir.path()+"/gstreamer-1.0/registry.x86_64.validation.json","invalid-json");
        if(fact=="mismatch" || fact=="sdk-change" || fact=="unreadable")f.matchedRecord();
        HANDLE heldRecord=INVALID_HANDLE_VALUE;
        const auto release=qScopeGuard([&]{if(heldRecord!=INVALID_HANDLE_VALUE)CloseHandle(heldRecord);});
        if(fact=="unreadable") {
            const auto path=QDir::toNativeSeparators(f.dir.path()+"/gstreamer-1.0/registry.x86_64.validation.json");
            heldRecord=CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            QVERIFY(heldRecord!=INVALID_HANDLE_VALUE);
        }
        if(fact=="mismatch")put(f.dir.path()+"/core.dll","changed-input");
        auto lock=CacheStorage::tryRegistryWriteLock(f.dir.path());QVERIFY(lock.lease);
        auto operations=f.operations();
        operations.launch=[&](const CacheWorkerRequest &r,qint64,auto cb){
            f.stages.append(r.stage);auto answer=f.answer(r);
            if(fact=="sdk-change")answer.snapshotUnchanged=false;
            cb(Fixture::process(),answer);
        };
        GStreamerStartupCache c(operations);QSignalSpy done(&c,&GStreamerStartupCache::finished);QVERIFY(c.start(f.request()));
        QCOMPARE(done.size(),1);const auto value=qvariant_cast<GStreamerCacheResult>(done[0][0]);
        QCOMPARE(value.cacheState,CacheState::RecoverySkipped);QCOMPARE(int(value.recordState),expected);
        QCOMPARE(value.readinessState,ReadinessState::Ready);QVERIFY(value.runtime);QCOMPARE(f.stages.size(),1);QCOMPARE(f.publishCount,0);
    }
    void untrustedBaselineNeverAttemptsPublication_data(){
        QTest::addColumn<bool>("ready");QTest::addColumn<int>("status");QTest::newRow("ready-unavailable")<<true<<1;QTest::newRow("not-ready-unavailable")<<false<<1;QTest::newRow("known-change-ready")<<true<<2;QTest::newRow("untrusted-available")<<true<<0;
    }
    void untrustedBaselineNeverAttemptsPublication(){
        QFETCH(bool,ready);QFETCH(int,status);Fixture f;f.assessReady=ready;auto operations=f.operations();
        operations.launch=[&](const CacheWorkerRequest &r,qint64,auto cb){
            f.stages.append(r.stage);auto answer=f.answer(r);answer.baselineTrusted=false;answer.baselineReadStatus=CacheReadStatus(status);answer.reason="Shared source read denied";cb(Fixture::process(),answer);
        };
        GStreamerStartupCache c(operations);QSignalSpy done(&c,&GStreamerStartupCache::finished);QVERIFY(c.start(f.request()));
        QCOMPARE(done.size(),1);const auto value=qvariant_cast<GStreamerCacheResult>(done[0][0]);
        QCOMPARE(value.cacheState,status==1?CacheState::RecoverySkipped:CacheState::RecoveryFailed);QCOMPARE(value.recordState,RecordState::NotApplicable);
        QCOMPARE(value.readinessState,ready?ReadinessState::Ready:ReadinessState::NotReady);QCOMPARE(bool(value.runtime),ready);
        QCOMPARE(f.stages.size(),1);QCOMPARE(f.publishCount,0);QVERIFY(!QFileInfo::exists(f.dir.path()+"/gstreamer-1.0/.registry-startup.lock"));
    }
    void unmanagedMakesNoWorkerOrRuntime(){
        Fixture f;auto request=f.request();request.mode=GStreamerCacheMode::Unmanaged;
        GStreamerStartupCache c(f.operations());QSignalSpy done(&c,&GStreamerStartupCache::finished);c.start(request);
        QCOMPARE(done.size(),1);auto value=qvariant_cast<GStreamerCacheResult>(done[0][0]);QCOMPARE(value.cacheState,CacheState::RecoverySkipped);QVERIFY(!value.runtime);QVERIFY(f.stages.isEmpty());
    }
    void deadlineIsSharedAndLateResultCannotPublish(){
        Fixture f;f.hold=true;GStreamerStartupCache c(f.operations());QSignalSpy done(&c,&GStreamerStartupCache::finished);c.start(f.request());
        f.now=60000;QVERIFY(f.pending);auto cb=f.pending;cb(Fixture::process(),f.answer(f.held));
        QCOMPARE(done.size(),1);QCOMPARE(f.publishCount,0);QCOMPARE(f.cleanupDeadline,qint64(65000));
        cb(Fixture::process(),f.answer(f.held));c.cancel();c.cancel();QCOMPARE(done.size(),1);
    }
    void lateTimeoutObservationDoesNotExtendCleanup(){
        Fixture f;f.hold=true;GStreamerStartupCache c(f.operations());QSignalSpy done(&c,&GStreamerStartupCache::finished);c.start(f.request());
        QVERIFY(f.pending);f.now=70000;auto cb=f.pending;cb(Fixture::process(),f.answer(f.held));
        QCOMPARE(done.size(),1);QCOMPARE(f.cleanupDeadline,qint64(65000));QCOMPARE(f.publishCount,0);
        QCOMPARE(qvariant_cast<GStreamerCacheResult>(done[0][0]).failureReason,CacheFailureReason::Timeout);
    }
    void cancelRacingCommitReportsActualOutcome(){
        Fixture f;GStreamerStartupCache c(f.operations());f.duringPublish=[&]{c.cancel();};QSignalSpy done(&c,&GStreamerStartupCache::finished);c.start(f.request());
        QCOMPARE(done.size(),1);auto v=qvariant_cast<GStreamerCacheResult>(done[0][0]);QCOMPARE(v.cacheState,CacheState::Updated);QVERIFY(v.cancelled);QCOMPARE(f.publishCount,1);
    }
    void recordFailureAfterCommitIsNotRollback(){
        Fixture f;f.recordFails=true;GStreamerStartupCache c(f.operations());QSignalSpy done(&c,&GStreamerStartupCache::finished);c.start(f.request());
        QCOMPARE(done.size(),1);auto v=qvariant_cast<GStreamerCacheResult>(done[0][0]);QCOMPARE(v.cacheState,CacheState::Updated);QCOMPARE(v.recordState,RecordState::NotSaved);
    }
    void cancelDuringCleanupCannotReturnRuntime(){
        Fixture f;f.matchedRecord();auto operations=f.operations();
        std::function<void(CacheCleanupResult)> completion;
        operations.stopAndCleanup=[&](qint64 d,auto cb){f.cleanupDeadline=d;completion=cb;};
        GStreamerStartupCache c(operations);QSignalSpy done(&c,&GStreamerStartupCache::finished);c.start(f.request());
        QVERIFY(completion);c.cancel();auto cb=completion;cb({true,{},{}});
        if(done.isEmpty()){auto next=completion;next({true,{},{}});}
        QCOMPARE(done.size(),1);auto value=qvariant_cast<GStreamerCacheResult>(done[0][0]);
        QVERIFY(value.cancelled);QVERIFY(!value.runtime);QCOMPARE(f.cleanupDeadline,qint64(5000));
    }
    void deadlineRacingSuccessfulPublicationIsUpdated(){
        Fixture f;GStreamerStartupCache c(f.operations());f.duringPublish=[&]{f.now=60000;};QSignalSpy done(&c,&GStreamerStartupCache::finished);c.start(f.request());
        QCOMPARE(done.size(),1);auto value=qvariant_cast<GStreamerCacheResult>(done[0][0]);
        QCOMPARE(value.cacheState,CacheState::Updated);QCOMPARE(f.cleanupDeadline,qint64(5000));QCOMPARE(f.publishCount,1);
    }
    void destructionSuppressesLateNotification(){
        Fixture f;f.hold=true;int notified=0;auto c=new GStreamerStartupCache(f.operations());connect(c,&GStreamerStartupCache::finished,this,[&]{++notified;});c->start(f.request());
        QVERIFY(f.pending);auto cb=f.pending;auto request=f.held;delete c;cb(Fixture::process(),f.answer(request));QCOMPARE(notified,0);QCOMPARE(f.publishCount,0);
    }
};
int main(int argc,char **argv) {
    if(argc==3 && QByteArray(argv[1])=="--gstreamer-cache-worker") {
        QFile file(QString::fromLocal8Bit(argv[2]));if(!file.open(QIODevice::ReadOnly))return 2;
        const auto request=QJsonDocument::fromJson(file.readAll()).object();file.close();
        const auto mode=qgetenv("AIRPLAY_CACHE_TEST_FRAME");
        if(request.value("stage")=="Cleanup" || mode.isEmpty() || (mode=="partial-prepare" && request.value("stage")!="PrepareCommit"))return dispatchGStreamerCacheWorker(argc,argv).value_or(2);
        QCoreApplication app(argc,argv);
        if(mode=="partial-prepare") {
            const auto record=request.value("validationRecord").toObject();
            CacheValidationRecord validation{record.value("schemaVersion").toInt(),QByteArray::fromHex(record.value("inputSha256").toString().toLatin1()),QByteArray::fromHex(record.value("registrySha256").toString().toLatin1()),record.value("plugins").toArray(),record.value("validated").toBool()};
            QString error;const auto preparation=request.value("preparationRequest").toString().toUtf8();
            const auto receipt=CacheStorage::fillPreparedWorkerRequest(preparation,request.value("inputRegistry").toString(),validation,&error);
            if(receipt.isEmpty())return 8;
            const auto path=QJsonDocument::fromJson(preparation).object().value("runtimeRegistry").toString();
            put(path,"partial");
            put(qEnvironmentVariable("AIRPLAY_CACHE_TEST_FAULT_EVIDENCE"),QJsonDocument(QJsonObject{{"request",request},{"receipt",QString::fromUtf8(receipt)},{"partialRuntime",path},{"bytes","partial"}}).toJson());
            volatile int *invalid=nullptr;*invalid=1;return 7;
        }
        if(mode=="crash") {volatile int *invalid=nullptr;*invalid=1;return 3;}
        if(mode=="wait") {put(request.value("packageDirectory").toString()+"/worker-live","live");QThread::msleep(1500);}
        QJsonObject result{{"schemaVersion",1},{"complete",mode!="incomplete"},{"nonce",mode=="wrong-nonce" || mode=="old-json"?QString(32,'f'):request.value("nonce")},
            {"stage",mode=="wrong-stage"?QJsonValue("Scan"):request.value("stage")},
            {"readiness",QJsonObject{{"ready",false},{"missingPlugins",QJsonArray{}},{"initializationError","Fixture refusal"}}},
            {"fingerprint",QJsonObject{{"valid",false},{"sha256",""},{"reason","Fixture refusal"}}},
            {"baseline",QJsonObject{{"exists",false},{"sha256",""}}},{"plugins",QJsonArray{}},
            {"baselineReadStatus",2},{"baselineTrusted",false},{"blacklistFree",false},{"snapshotUnchanged",false},{"scannerFallback",false},
            {"cleanup",QJsonObject{{"complete",false},{"residualPaths",QJsonArray{}},{"reason",""}}},
            {"reason","Fixture refusal"}};
        if(mode=="invalid-baseline-status")result["baselineReadStatus"]=1.5;
        if(mode=="invalid-baseline-combination")result["baselineTrusted"]=true;
        if(mode=="missing-baseline-trust")result.remove("baselineTrusted");
        if(mode=="invalid-baseline-hash")result["baseline"]=QJsonObject{{"exists",true},{"sha256","malformed"}};
        if(mode=="invalid-baseline-absent-hash")result["baseline"]=QJsonObject{{"exists",false},{"sha256",QString(64,'a')}};
        if(mode=="invalid-baseline-available-empty"){result["baselineReadStatus"]=0;result["baseline"]=QJsonObject{{"exists",true},{"sha256",""}};}
        auto bytes=QJsonDocument(result).toJson(QJsonDocument::Compact);
        QFile output(request.value("resultPath").toString());if(!output.open(QIODevice::WriteOnly|QIODevice::NewOnly))return 2;
        if(output.write(bytes)!=bytes.size() || !output.flush())return 2;output.close();
        if(mode!="empty-output")QTextStream(stdout)<<QString::fromUtf8(bytes)<<Qt::endl;
        return 0;
    }
    QCoreApplication app(argc,argv);GStreamerStartupCacheTest test;
    if(argc==4 && QByteArray(argv[1])=="--native-coordinator-suite") {
        nativePackage=QString::fromLocal8Bit(argv[2]);nativeEvidence=QFileInfo(QString::fromLocal8Bit(argv[3])).absolutePath();
        return QTest::qExec(&test,QStringList{QString::fromLocal8Bit(argv[0]),"nativeDefaultRecoveryAndReuse","nativeCancelAfterSealing","nativePartialPreparationPreservesFallback","nativeTransportAcceptance","nativeDestructionRetainsCleanupOwner","-o",QString::fromLocal8Bit(argv[3])+",txt"});
    }
    const auto reportDirectory=qEnvironmentVariable("AIRPLAY_CACHE_STARTUP_TEST_REPORT_DIRECTORY");
    if(reportDirectory.isEmpty() || !QDir().mkpath(reportDirectory) || !QFileInfo(reportDirectory).isDir()) {
        QTextStream(stderr)<<"Coordinator report directory unavailable: "<<reportDirectory<<Qt::endl;return 2;
    }
    return QTest::qExec(&test,QStringList{QString::fromLocal8Bit(argv[0]),"fingerprintRecheckFacts","healthyReuseNeedsNoWriteLock","changedTupleAttemptsRecoveryOnce",
        "optionalFailureWithReadyCoreCanContinue","failedPreparationPreservesAssessedRuntime","verifiedStagedRuntimeSurvivesPublicationRefusal","tempRootFailureHasNoDefaultFallback","sourceOrSharedTargetChangeRejectsCandidate",
        "deadlineIsSharedAndLateResultCannotPublish","cancelRacingCommitReportsActualOutcome","recordFailureAfterCommitIsNotRollback","destructionSuppressesLateNotification",
        "lateTimeoutObservationDoesNotExtendCleanup","cancelDuringCleanupCannotReturnRuntime","deadlineRacingSuccessfulPublicationIsUpdated","sharedTargetChangeRejectsCandidate","busyRecoveryKeepsActualReadyRuntime","assessedRecordFactsSurviveBusy","untrustedBaselineNeverAttemptsPublication","unmanagedMakesNoWorkerOrRuntime",
        "-o",qEnvironmentVariable("AIRPLAY_CACHE_STARTUP_TEST_REPORT_DIRECTORY")+"/task4-coordinator-results.txt,txt"});
}
#include "GStreamerStartupCacheTest.moc"
