#include "platform/GStreamerStartupCache.h"
#include "platform/FileSystemPath.h"
#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#include <algorithm>
namespace {
QString stageName(CacheWorkerStage stage) {
    switch(stage) {
    case CacheWorkerStage::Assess:return "Assess";case CacheWorkerStage::Scan:return "Scan";
    case CacheWorkerStage::Verify:return "Verify";case CacheWorkerStage::PrepareCommit:return "PrepareCommit";
    case CacheWorkerStage::Cleanup:return "Cleanup";
    } return {};
}
QByteArray hashValue(const QJsonValue &v) {
    const auto s=v.toString();return QRegularExpression("^[0-9a-f]{64}$").match(s).hasMatch()?QByteArray::fromHex(s.toLatin1()):QByteArray{};
}
bool sameBaseline(const CacheBaseline &a,const CacheBaseline &b) { return a.exists==b.exists && a.sha256==b.sha256; }
bool samePath(const QString &a,const QString &b) { return FileSystemPath::absolute(a).compare(FileSystemPath::absolute(b),Qt::CaseInsensitive)==0; }
void combine(CacheCleanupResult &a,const CacheCleanupResult &b) {
    a.complete=a.complete && b.complete;a.residualPaths.append(b.residualPaths);a.residualPaths.removeDuplicates();
    if(!b.reason.isEmpty()) {if(!a.reason.isEmpty())a.reason+="; ";a.reason+=b.reason;}
}
QByteArray encodeRequest(const CacheWorkerRequest &r) {
    return QJsonDocument(QJsonObject{{"schemaVersion",r.schemaVersion},{"stage",stageName(r.stage)},{"nonce",r.nonce},
        {"packageDirectory",r.packageDirectory},{"ownedRoot",r.ownedRoot},{"inputRegistry",r.inputRegistry},
        {"outputRegistry",r.outputRegistry},{"resultPath",r.resultPath},{"ownershipRequest",QString::fromUtf8(r.ownershipRequest)},
        {"preparationRequest",QString::fromUtf8(r.preparationRequest)},
        {"remainingCleanupBudgetMs",r.remainingCleanupBudgetMs},
        {"baseline",QJsonObject{{"exists",r.baseline.exists},{"sha256",QString(r.baseline.sha256.toHex())}}},
        {"expectedInput",QJsonObject{{"valid",r.expectedInput.valid},{"sha256",QString(r.expectedInput.sha256.toHex())}}},
        {"validationRecord",QJsonObject{{"schemaVersion",r.validationRecord.schemaVersion},{"validated",r.validationRecord.validated},
            {"inputSha256",QString(r.validationRecord.inputSha256.toHex())},{"registrySha256",QString(r.validationRecord.registrySha256.toHex())},
            {"plugins",r.validationRecord.plugins}}}}).toJson(QJsonDocument::Compact);
}
bool decodeResult(const QByteArray &bytes,const CacheWorkerRequest &request,CacheWorkerResult *r,QString *error) {
    QJsonParseError parse;const auto doc=QJsonDocument::fromJson(bytes,&parse);
    if(bytes.isEmpty() || bytes.size()>1024*1024 || parse.error!=QJsonParseError::NoError || !doc.isObject()) {
        *error="Worker result JSON missing or invalid";return false;
    }
    const auto o=doc.object();
    if(o.value("schemaVersion").toDouble()!=CacheSchemaVersion || o.value("nonce").toString()!=request.nonce
        || o.value("stage").toString()!=stageName(request.stage) || !o.value("complete").isBool() || !o.value("complete").toBool()
        || !o.value("readiness").isObject() || !o.value("fingerprint").isObject() || !o.value("baseline").isObject()
        || !o.value("baselineReadStatus").isDouble() || o.value("baselineReadStatus").toInt(-1)<0 || o.value("baselineReadStatus").toInt(-1)>2
        || o.value("baselineReadStatus").toDouble()!=o.value("baselineReadStatus").toInt(-1)
        || (o.value("baselineTrusted").toBool() && o.value("baselineReadStatus").toInt()!=int(CacheReadStatus::Available))
        || !o.value("baselineTrusted").isBool() || !o.value("plugins").isArray() || !o.value("reason").isString() || !o.value("cleanup").isObject()) {
        *error="Worker result protocol, nonce, stage or completeness mismatch";return false;
    }
    auto ready=o.value("readiness").toObject(),fp=o.value("fingerprint").toObject(),base=o.value("baseline").toObject(),cleanup=o.value("cleanup").toObject();
    if(!ready.value("ready").isBool() || !ready.value("missingPlugins").isArray() || !ready.value("initializationError").isString()
        || !fp.value("valid").isBool() || !base.value("exists").isBool() || !cleanup.value("complete").isBool()
        || !cleanup.value("residualPaths").isArray() || !cleanup.value("reason").isString()
        || !o.value("blacklistFree").isBool() || !o.value("snapshotUnchanged").isBool() || !o.value("scannerFallback").isBool()) {
        *error="Worker result field types invalid";return false;
    }
    r->nonce=request.nonce;r->stage=request.stage;r->complete=true;
    r->readiness.ready=ready.value("ready").toBool();r->readiness.initializationError=ready.value("initializationError").toString();
    for(const auto &p:ready.value("missingPlugins").toArray()) {if(!p.isString())return false;r->readiness.missingPlugins.append(p.toString());}
    r->fingerprint={fp.value("valid").toBool(),hashValue(fp.value("sha256")),fp.value("reason").toString()};
    r->baseline={base.value("exists").toBool(),hashValue(base.value("sha256"))};
    if((r->fingerprint.valid && r->fingerprint.sha256.size()!=32) || (r->baseline.exists && (o.value("baselineTrusted").toBool() || o.value("baselineReadStatus").toInt()==0) && r->baseline.sha256.size()!=32)
        || (!r->baseline.exists && !base.value("sha256").toString().isEmpty())
        || !base.value("sha256").isString() || (!base.value("sha256").toString().isEmpty() && r->baseline.sha256.size()!=32)) {
        *error="Worker result hash invalid";return false;
    }
    r->registrySha256=hashValue(o.value("registrySha256"));r->recordSha256=hashValue(o.value("recordSha256"));
    r->workerProof=o.value("workerProof").toString().toUtf8();r->plugins=o.value("plugins").toArray();
    r->blacklistFree=o.value("blacklistFree").toBool();r->snapshotUnchanged=o.value("snapshotUnchanged").toBool();
    r->baselineReadStatus=CacheReadStatus(o.value("baselineReadStatus").toInt());r->baselineTrusted=o.value("baselineTrusted").toBool();r->scannerFallback=o.value("scannerFallback").toBool();r->pendingRegistry=o.value("pendingRegistry").toString();
    r->pendingRecord=o.value("pendingRecord").toString();r->reason=o.value("reason").toString();
    r->cleanup.complete=cleanup.value("complete").toBool();r->cleanup.reason=cleanup.value("reason").toString();
    for(const auto &p:cleanup.value("residualPaths").toArray()) {if(!p.isString())return false;r->cleanup.residualPaths.append(p.toString());}
    return true;
}
struct NativeSession:std::enable_shared_from_this<NativeSession> {
    GStreamerCacheRequest request;
    QList<std::shared_ptr<RuntimeCacheLease>> runtimes;
    QList<std::shared_ptr<RuntimeCacheLease>> closedRuntimes;
    std::shared_ptr<RuntimeCacheLease> retainedRuntime;
    std::function<qint64()> now;
    QPointer<CacheProcessRunner> runner;
    QList<CacheProtocolObject> protocols;
    QStringList unacceptedResults;
    qint64 publicationCleanupDeadline=0;
    std::function<void()> stopped;
    int remaining(qint64 deadline) const {return int(std::clamp<qint64>(deadline-now(),0,CleanupBudgetMs));}
    void launch(const CacheWorkerRequest &r,qint64 deadline,std::function<void(CacheProcessResult,CacheWorkerResult)> completion) {
        QString error;std::shared_ptr<RuntimeCacheLease> runtime;
        for(const auto &owned:runtimes)if(samePath(QFileInfo(owned->registryPath()).absolutePath(),r.ownedRoot)){runtime=owned;break;}
        auto registered=CacheStorage::createProtocolRequest(runtime,r.nonce,encodeRequest(r),&error);
        if(!registered.nativeId.isEmpty())protocols.append(registered);
        if(!error.isEmpty() || registered.nativeId.isEmpty()) {CacheProcessResult failed;failed.reason=error;completion(failed,{});return;}
        CacheProcessRequest process;process.executable=request.executable;process.arguments={"--gstreamer-cache-worker",registered.path};
        process.nonce=r.nonce;process.ownedResultPath=r.resultPath;
        process.environment=QProcessEnvironment::systemEnvironment();
        for(const auto &key:process.environment.keys())if(key.startsWith("GST_",Qt::CaseInsensitive))process.environment.remove(key);
        const auto windows=process.environment.value("SystemRoot","C:/Windows");
        process.environment.insert("PATH",QDir::toNativeSeparators(request.packageDirectory)+";"+QDir::toNativeSeparators(windows+"/System32")+";"+QDir::toNativeSeparators(windows));
        process.environment.insert("GST_REGISTRY",r.outputRegistry.isEmpty()?runtime->registryPath():r.outputRegistry);
        process.environment.insert("GST_PLUGIN_PATH",request.packageDirectory+"/gstreamer-plugins");
        process.environment.insert("GST_PLUGIN_PATH_1_0",request.packageDirectory+"/gstreamer-plugins");
        process.environment.insert("GST_PLUGIN_SYSTEM_PATH","");process.environment.insert("GST_PLUGIN_SYSTEM_PATH_1_0","");
        process.environment.insert("GST_PLUGIN_SCANNER",request.packageDirectory+"/libexec/gstreamer-1.0/gst-plugin-scanner.exe");
        process.environment.insert("GST_PLUGIN_SCANNER_1_0",process.environment.value("GST_PLUGIN_SCANNER"));
        auto *active=new CacheProcessRunner;runner=active;auto self=shared_from_this();
        QObject::connect(active,&CacheProcessRunner::completed,active,[self,active,r,runtime,completion](CacheProcessResult p){
            self->runner=nullptr;active->deleteLater();CacheWorkerResult result;
            if(!p.stderrBytes.isEmpty())qWarning().noquote()<<QString::fromUtf8(p.stderrBytes);
            if(p.normalExit && p.exitCode==0 && p.outputComplete && !p.cancelled && !p.timedOut) {
                QByteArray bytes;QString error;
                const auto object=CacheStorage::readProtocolResult(runtime,r.nonce,&bytes,&error);
                if(!object.nativeId.isEmpty() && p.stdoutBytes.trimmed()==bytes.trimmed() && decodeResult(bytes,r,&result,&error))
                    self->protocols.append(object);
                else {result={};p.reason=error.isEmpty()?"Worker file/frame mismatch":error;self->unacceptedResults.append(r.resultPath);}
            } else self->unacceptedResults.append(r.resultPath);
            completion(p,result);
            if(self->stopped) {auto done=std::move(self->stopped);self->stopped={};done();}
        });
        const bool started=r.stage==CacheWorkerStage::Cleanup?active->start(process,deadline,now,deadline):active->start(process,deadline,now);
        if(!started) {runner=nullptr;active->deleteLater();CacheProcessResult p;p.reason="Worker launch rejected";completion(p,{});}
    }
    void finishCleanup(qint64 deadline,std::function<void(CacheCleanupResult)> completion) {
        GStreamerCacheNative::Remover remover;
        auto result=CacheStorage::cleanupProtocolObjects(protocols,remaining(deadline),remover);protocols.clear();
        for(const auto &path:unacceptedResults)if(QFileInfo::exists(path)){result.complete=false;result.residualPaths.append(path);}
        unacceptedResults.clear();completion(result);
    }
    void cleanupNext(qint64 deadline,CacheCleanupResult outcome,std::function<void(CacheCleanupResult)> completion) {
        std::shared_ptr<RuntimeCacheLease> runtime;
        for(const auto &owned:runtimes)if(owned!=retainedRuntime && !closedRuntimes.contains(owned)){runtime=owned;break;}
        if(!runtime) {finishCleanup(deadline,[outcome,completion](CacheCleanupResult protocol)mutable{combine(outcome,protocol);completion(outcome);});return;}
        auto self=shared_from_this();QString error;CacheWorkerRequest r;r.stage=CacheWorkerStage::Cleanup;r.nonce=QUuid::createUuid().toString(QUuid::Id128);
        r.packageDirectory=request.packageDirectory;r.ownedRoot=QFileInfo(runtime->registryPath()).absolutePath();
        r.resultPath=QFileInfo(r.ownedRoot).absolutePath()+"/worker-"+r.nonce+".json";
        r.ownershipRequest=CacheStorage::workerOwnershipRequest(runtime,&error);r.remainingCleanupBudgetMs=remaining(deadline);
        auto accept=[self,r,runtime,deadline,outcome,completion](CacheProcessResult p,CacheWorkerResult worker)mutable{
            QString error;const bool valid=p.normalExit && p.exitCode==0 && p.outputComplete && worker.complete;
            CacheStorage::acknowledgeCleanupProof(runtime,r.ownershipRequest,r.nonce,valid?worker.workerProof:QByteArray{},worker.cleanup,&error);
            combine(outcome,runtime->close(0));self->closedRuntimes.append(runtime);
            self->cleanupNext(deadline,outcome,completion);
        };
        if(r.remainingCleanupBudgetMs==0 || r.ownershipRequest.isEmpty())accept({},{});
        else launch(r,deadline,accept);
    }
    void cleanup(qint64 deadline,std::function<void(CacheCleanupResult)> completion) {
        auto self=shared_from_this();
        auto tail=[self,deadline,completion]{self->cleanupNext(deadline,{true,{},{}},completion);};
        if(runner) {stopped=tail;runner->cancel(deadline);}else tail();
    }
};
}
struct GStreamerStartupCache::Session:std::enable_shared_from_this<Session> {
    QPointer<GStreamerStartupCache> owner;
    GStreamerCacheOperations operations;
    std::shared_ptr<NativeSession> native;
    GStreamerCacheRequest request;
    GStreamerCacheResult result;
    std::shared_ptr<RuntimeCacheLease> runtime,stagedRuntime;
    QList<std::shared_ptr<RuntimeCacheLease>> runtimes;
    std::shared_ptr<RegistryWriteLease> lock;
    PreparedCacheCommit prepared;
    CacheFingerprint input;
    CacheBaseline baseline;
    CacheValidationRecord record;
    GStreamerPluginReadiness verifiedReadiness;
    qint64 deadline=0,cleanupDeadline=-1;
    quint64 generation=0;
    bool started=false,finishing=false,finished=false,cancelled=false,publishing=false;
    QPointer<QTimer> timeout;
    ~Session(){if(timeout)timeout->deleteLater();}
    int remaining()const{return int(std::clamp<qint64>(cleanupDeadline-operations.nowMs(),0,CleanupBudgetMs));}
    void finish() {
        if(finishing || finished)return;finishing=true;++generation;if(timeout)timeout->stop();
        if(cleanupDeadline<0)cleanupDeadline=operations.nowMs()+CleanupBudgetMs;
        const bool keep=!cancelled && result.readinessState==ReadinessState::Ready && runtime;
        if(native)native->retainedRuntime=keep?runtime:nullptr;
        auto self=shared_from_this();
        operations.stopAndCleanup(cleanupDeadline,[self,keep](CacheCleanupResult cleanup){
            if(self->finished)return;
            if(keep && self->cancelled) {
                if(self->native)self->native->retainedRuntime.reset();
                self->operations.stopAndCleanup(self->cleanupDeadline,[self,cleanup](CacheCleanupResult tail)mutable{
                    combine(cleanup,tail);self->complete(false,cleanup);
                });
            }else self->complete(keep,cleanup);
        });
    }
    void complete(bool keep,CacheCleanupResult cleanup) {
        if(finished)return;
        if(lock)combine(cleanup,lock->close(remaining()));
        if(!native)for(const auto &owned:runtimes)if(!keep || owned!=runtime) {
            QString error;CacheStorage::acknowledgeCleanupProof(owned,{}, {}, {},{},&error);
            combine(cleanup,owned->close(0));
        }
        result.cleanup=cleanup;result.cancelled=cancelled;
        if(keep)result.runtime=runtime;
        else {result.runtime.reset();if(result.readinessState==ReadinessState::Ready)result.readinessState=ReadinessState::Unknown;}
        finished=true;finishing=false;
        if(owner)emit owner->finished(result);
    }
    void fail(CacheFailureReason reason,const QString &text,CacheState state=CacheState::RecoveryFailed) {
        result.cacheState=state;result.failureReason=reason;result.reason=text;finish();
    }
    void cancel() {
        if(finished)return;cancelled=true;
        if(cleanupDeadline<0)cleanupDeadline=std::min(operations.nowMs(),deadline)+CleanupBudgetMs;
        if(publishing)return;
        if(!finishing)fail(CacheFailureReason::None,"Cache preparation cancelled");
    }
    bool expired() {
        if(cancelled || operations.nowMs()>=deadline) {
            if(!cancelled){cleanupDeadline=cleanupDeadline<0?deadline+CleanupBudgetMs:std::min(cleanupDeadline,deadline+CleanupBudgetMs);result.failureReason=CacheFailureReason::Timeout;result.reason="Cache preparation deadline expired";result.cacheState=CacheState::RecoveryFailed;}
            finish();return true;
        }return false;
    }
    bool candidate(const CacheWorkerResult &r)const {
        return r.reason.isEmpty() && r.readiness.ready && r.readiness.missingPlugins.isEmpty() && r.readiness.initializationError.isEmpty()
            && r.fingerprint.valid && r.registrySha256.size()==32 && r.blacklistFree && !r.scannerFallback && !r.plugins.isEmpty();
    }
    void launch(CacheWorkerStage stage,const QString &source={},const QString &output={},const QByteArray &preparation={}) {
        if(expired() || finishing || finished)return;
        CacheWorkerRequest r;r.stage=stage;r.nonce=QUuid::createUuid().toString(QUuid::Id128);
        const auto workerRuntime=stage==CacheWorkerStage::PrepareCommit?stagedRuntime:runtime;
        r.packageDirectory=request.packageDirectory;r.ownedRoot=QFileInfo(workerRuntime->registryPath()).absolutePath();
        r.inputRegistry=source;r.outputRegistry=output;r.baseline=baseline;r.expectedInput=input;r.validationRecord=record;
        r.resultPath=QFileInfo(r.ownedRoot).absolutePath()+"/worker-"+r.nonce+".json";r.preparationRequest=preparation;
        QString error;r.ownershipRequest=CacheStorage::workerOwnershipRequest(workerRuntime,&error);
        if(r.ownershipRequest.isEmpty()){fail(CacheFailureReason::OwnershipUnknown,error);return;}
        const auto token=++generation;auto self=shared_from_this();if(owner)emit owner->stageChanged(stage);
        if(finishing || finished || expired())return;
        operations.launch(r,deadline,[self,r,token](CacheProcessResult p,CacheWorkerResult v){
            if(token!=self->generation || self->finishing || self->finished)return;
            ++self->generation;
            if(self->expired())return;
            if(!p.normalExit || p.exitCode!=0 || !p.outputComplete || p.cancelled || p.timedOut
                || !v.complete || v.nonce!=r.nonce || v.stage!=r.stage) {
                self->fail(p.timedOut?CacheFailureReason::Timeout:CacheFailureReason::ValidationFailed,
                    p.reason.isEmpty()?"Worker did not produce a normal current complete result":p.reason);return;
            }
            self->accept(r,v);
        });
    }
    void accept(const CacheWorkerRequest &r,const CacheWorkerResult &v) {
        const auto root=QFileInfo(runtime->registryPath()).absolutePath();
        if(r.stage==CacheWorkerStage::Assess) {
            result.readiness=v.readiness;
            result.readinessState=v.readiness.ready?ReadinessState::Ready:ReadinessState::NotReady;
            if(v.registrySha256.size()!=32 || !QFileInfo::exists(runtime->registryPath()))result.readinessState=ReadinessState::Unknown;
            input=v.fingerprint;baseline=v.baseline;
            CacheRecordReadStatus recordStatus;
            record=CacheStorage::readValidationRecord(request.packageDirectory,nullptr,&recordStatus);
            if(recordStatus==CacheRecordReadStatus::Malformed)result.recordState=RecordState::Invalid;
            else if(recordStatus==CacheRecordReadStatus::Parsed && input.valid && v.baselineTrusted)
                result.recordState=validationRecordMatches(record,input,baseline)?RecordState::Matched:RecordState::Invalid;
            if(!v.baselineTrusted){fail(v.baselineReadStatus==CacheReadStatus::Unavailable?CacheFailureReason::ValidationFailed:CacheFailureReason::InputChanged,
                v.reason.isEmpty()?"Shared registry baseline unavailable":v.reason,
                v.baselineReadStatus==CacheReadStatus::Unavailable?CacheState::RecoverySkipped:CacheState::RecoveryFailed);return;}
            if(candidate(v) && v.snapshotUnchanged && validationRecordMatches(record,input,baseline) && v.registrySha256==baseline.sha256) {
                result.cacheState=CacheState::Reused;result.recordState=RecordState::Matched;finish();return;
            }
            if(!input.valid){fail(CacheFailureReason::ValidationFailed,input.reason);return;}
            auto attempt=CacheStorage::tryRegistryWriteLock(request.packageDirectory);lock=attempt.lease;
            if(!lock){fail(attempt.state==CacheLockState::Busy?CacheFailureReason::Busy:CacheFailureReason::DirectoryNotWritable,attempt.reason,CacheState::RecoverySkipped);return;}
            launch(CacheWorkerStage::Scan,{},root+"/scan.bin");return;
        }
        if(!v.fingerprint.valid || v.fingerprint.sha256!=input.sha256 || !sameBaseline(v.baseline,baseline)) {
            fail(CacheFailureReason::InputChanged,"Package inputs or shared target changed");return;
        }
        if(r.stage==CacheWorkerStage::Scan) {
            if(!candidate(v)){fail(CacheFailureReason::ScanFailed,v.reason);return;}
            record={CacheSchemaVersion,input.sha256,v.registrySha256,v.plugins,true};
            launch(CacheWorkerStage::Verify,r.outputRegistry,root+"/verify.bin");return;
        }
        if(r.stage==CacheWorkerStage::Verify) {
            if(!candidate(v) || !v.snapshotUnchanged || v.registrySha256!=record.registrySha256){fail(CacheFailureReason::ValidationFailed,v.reason);return;}
            record.plugins=v.plugins;verifiedReadiness=v.readiness;QString error;
            stagedRuntime=CacheStorage::createRuntime(request.packageDirectory,request.temporaryParent,&error);
            if(!stagedRuntime){fail(CacheFailureReason::TempUnavailable,error);return;}
            runtimes.append(stagedRuntime);if(native)native->runtimes.append(stagedRuntime);
            prepared=CacheStorage::reservePreparedCommit(*lock,stagedRuntime,input,baseline,&error);
            auto fill=CacheStorage::workerFillRequest(*lock,prepared,runtime,&error);
            if(fill.isEmpty()){fail(CacheFailureReason::OwnershipUnknown,error);return;}
            launch(CacheWorkerStage::PrepareCommit,r.outputRegistry,{},fill);return;
        }
        if(r.stage==CacheWorkerStage::PrepareCommit) {
            QString error;
            if(!v.reason.isEmpty() || !samePath(v.pendingRegistry,prepared.pendingRegistry) || !samePath(v.pendingRecord,prepared.pendingRecord)
                || v.registrySha256!=record.registrySha256 || v.recordSha256.size()!=32) {fail(CacheFailureReason::ValidationFailed,v.reason);return;}
            if(QJsonDocument::fromJson(r.preparationRequest).object().value("phase")!="sealed-verification") {
                if(!CacheStorage::sealPreparedCommit(*lock,prepared,v.registrySha256,v.recordSha256,&error)){fail(CacheFailureReason::OwnershipUnknown,error);return;}
                auto request=CacheStorage::workerPreparationRequest(*lock,prepared,&error);
                if(request.isEmpty()){fail(CacheFailureReason::ValidationFailed,error);return;}
                launch(CacheWorkerStage::PrepareCommit,{}, {},request);return;
            }
            if(!v.cleanup.complete || !CacheStorage::adoptPreparedCommit(*lock,prepared,v.workerProof,&error)) {fail(CacheFailureReason::PrecommitCleanupFailed,error);return;}
            if(expired())return;
            runtime=stagedRuntime;
            result.readiness=verifiedReadiness;result.readinessState=ReadinessState::Ready;
            if(cleanupDeadline<0)cleanupDeadline=operations.nowMs()+CleanupBudgetMs;
            if(native)native->publicationCleanupDeadline=cleanupDeadline;
            publishing=true;auto committed=operations.publish(*lock,prepared);publishing=false;
            result.cacheState=committed.cacheState;if(committed.cacheState==CacheState::Updated)result.recordState=committed.recordState;result.failureReason=committed.failureReason;result.reason=committed.reason;
            finish();
        }
    }
    bool start(const GStreamerCacheRequest &r) {
        if(started)return false;started=true;request=r;deadline=operations.nowMs()+PrepareBudgetMs;
        if(r.mode==GStreamerCacheMode::Unmanaged){result.cacheState=CacheState::RecoverySkipped;finish();return true;}
        QString error;runtime=CacheStorage::createRuntime(r.packageDirectory,r.temporaryParent,&error);
        if(runtime)runtimes.append(runtime);
        if(native){native->request=r;native->runtimes=runtimes;}
        if(!runtime){fail(CacheFailureReason::TempUnavailable,error);return true;}
        auto self=shared_from_this();timeout=new QTimer;timeout->setSingleShot(true);
        QObject::connect(timeout,&QTimer::timeout,timeout,[weak=std::weak_ptr<Session>(self)]{if(auto s=weak.lock()){s->expired();}});
        timeout->start(PrepareBudgetMs);
        launch(CacheWorkerStage::Assess,r.packageDirectory+"/gstreamer-1.0/registry.x86_64.bin",runtime->registryPath());return true;
    }
};
GStreamerStartupCache::GStreamerStartupCache(QObject *parent):QObject(parent),session_(std::make_shared<Session>()) {
    auto clock=std::make_shared<QElapsedTimer>();clock->start();session_->operations.nowMs=[clock]{return clock->elapsed();};
    auto native=std::make_shared<NativeSession>();session_->native=native;native->now=session_->operations.nowMs;
    session_->operations.launch=[native](const auto &r,qint64 d,auto cb){native->launch(r,d,std::move(cb));};
    session_->operations.stopAndCleanup=[native](qint64 d,auto cb){native->cleanup(d,std::move(cb));};
    session_->operations.publish=[native](auto &l,const auto &p){return CacheStorage::publishPreparedCache(l,p,native->remaining(native->publicationCleanupDeadline));};
    session_->owner=this;
}
GStreamerStartupCache::GStreamerStartupCache(GStreamerCacheOperations operations,QObject *parent):QObject(parent),session_(std::make_shared<Session>()) {
    session_->operations=std::move(operations);session_->owner=this;
}
GStreamerStartupCache::~GStreamerStartupCache(){session_->owner=nullptr;if(session_->started)session_->cancel();}
bool GStreamerStartupCache::start(const GStreamerCacheRequest &r){return session_->start(r);}
void GStreamerStartupCache::cancel(){if(session_->started)session_->cancel();}
