#include "platform/GStreamerCacheWorker.h"
#include "platform/GStreamerCacheStorage.h"
#include "platform/FileSystemPath.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextStream>
#include <gst/gst.h>
#include <atomic>

namespace {
constexpr qint64 MessageLimit = 1024 * 1024;
QString stageName(CacheWorkerStage stage) {
    switch (stage) {
    case CacheWorkerStage::Assess: return "Assess";
    case CacheWorkerStage::Scan: return "Scan";
    case CacheWorkerStage::Verify: return "Verify";
    case CacheWorkerStage::PrepareCommit: return "PrepareCommit";
    case CacheWorkerStage::Cleanup: return "Cleanup";
    }
    return {};
}
QByteArray decodeHash(const QJsonValue &value) {
    const auto text=value.toString();
    return QRegularExpression("^[0-9a-f]{64}$").match(text).hasMatch()?QByteArray::fromHex(text.toLatin1()):QByteArray{};
}
QByteArray hashFile(const QString &path, QString *reason) {
    QFile file(FileSystemPath::forIo(path));
    if (!file.open(QIODevice::ReadOnly)) { *reason="Registry unavailable: "+path; return {}; }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || file.error()!=QFileDevice::NoError) { *reason="Registry read incomplete"; return {}; }
    return hash.result();
}
bool samePath(const QString &left,const QString &right) {
    return FileSystemPath::absolute(left).compare(FileSystemPath::absolute(right),Qt::CaseInsensitive)==0;
}
std::atomic_bool fallback{false};
void inspectMessage(const QString &text) {
    const auto folded=text.toLower();
    if (folded.contains("external plugin loader failed") || folded.contains("failed to spawn gst-plugin-scanner")
        || folded.contains("couldn't create helper process") || folded.contains("failed to start plugin scanner"))
        fallback.store(true);
}
void gstLog(GstDebugCategory *,GstDebugLevel,const gchar *,const gchar *,gint,GObject *,GstDebugMessage *message,gpointer) {
    inspectMessage(QString::fromUtf8(gst_debug_message_get(message)));
}
void glibLog(const gchar *domain,GLogLevelFlags level,const gchar *message,gpointer) {
    inspectMessage(QString::fromUtf8(message));
    g_log_default_handler(domain,level,message,nullptr);
}
QJsonObject fingerprintJson(const CacheFingerprint &value) {
    return {{"valid",value.valid},{"sha256",QString::fromLatin1(value.sha256.toHex())},{"reason",value.reason}};
}
QJsonObject resultJson(const CacheWorkerResult &value) {
    return {{"schemaVersion",CacheSchemaVersion},{"complete",value.complete},{"nonce",value.nonce},{"stage",stageName(value.stage)},
        {"fingerprint",fingerprintJson(value.fingerprint)},{"registrySha256",QString::fromLatin1(value.registrySha256.toHex())},
        {"recordSha256",QString::fromLatin1(value.recordSha256.toHex())},{"workerProof",QString::fromUtf8(value.workerProof)},
        {"readiness",QJsonObject{{"ready",value.readiness.ready},{"missingPlugins",QJsonArray::fromStringList(value.readiness.missingPlugins)},
            {"initializationError",value.readiness.initializationError}}},{"plugins",value.plugins},
        {"blacklistFree",value.blacklistFree},{"snapshotUnchanged",value.snapshotUnchanged},{"scannerFallback",value.scannerFallback},
        {"pendingRegistry",value.pendingRegistry},{"pendingRecord",value.pendingRecord},
        {"baselineTrusted",value.baselineTrusted},{"baselineReadStatus",int(value.baselineReadStatus)},{"baseline",QJsonObject{{"exists",value.baseline.exists},{"sha256",QString::fromLatin1(value.baseline.sha256.toHex())}}},
        {"cleanup",QJsonObject{{"complete",value.cleanup.complete},{"residualPaths",QJsonArray::fromStringList(value.cleanup.residualPaths)},
            {"reason",value.cleanup.reason}}},{"reason",value.reason}};
}
bool parseRequest(const QJsonObject &obj,CacheWorkerRequest *request,QString *reason) {
    request->nonce=obj.value("nonce").toString();
    const auto stage=obj.value("stage").toString();
    bool found=false;
    for (auto candidate:{CacheWorkerStage::Assess,CacheWorkerStage::Scan,CacheWorkerStage::Verify,CacheWorkerStage::PrepareCommit,CacheWorkerStage::Cleanup})
        if (stage==stageName(candidate)) { request->stage=candidate; found=true; }
    request->packageDirectory=obj.value("packageDirectory").toString(); request->ownedRoot=obj.value("ownedRoot").toString();
    request->inputRegistry=obj.value("inputRegistry").toString(); request->outputRegistry=obj.value("outputRegistry").toString();
    request->resultPath=obj.value("resultPath").toString();
    request->ownershipRequest=obj.value("ownershipRequest").toString().toUtf8();
    request->preparationRequest=obj.value("preparationRequest").toString().toUtf8();
    const auto baseline=obj.value("baseline").toObject();
    request->baseline={baseline.value("exists").toBool(),decodeHash(baseline.value("sha256"))};
    const auto expected=obj.value("expectedInput").toObject();
    request->expectedInput={expected.value("valid").toBool(),decodeHash(expected.value("sha256")),{}};
    const auto record=obj.value("validationRecord").toObject();
    request->validationRecord={record.value("schemaVersion").toInt(),decodeHash(record.value("inputSha256")),
        decodeHash(record.value("registrySha256")),record.value("plugins").toArray(),record.value("validated").toBool()};
    if (obj.contains("remainingCleanupBudgetMs")) request->remainingCleanupBudgetMs=obj.value("remainingCleanupBudgetMs").toInt(-1);
    if (obj.value("schemaVersion").toDouble()!=CacheSchemaVersion || !found
        || !QRegularExpression("^[0-9a-f]{32}$").match(request->nonce).hasMatch()
        || request->packageDirectory.isEmpty() || request->ownedRoot.isEmpty()
        || request->remainingCleanupBudgetMs<0 || request->remainingCleanupBudgetMs>CleanupBudgetMs) {
        *reason="Invalid worker request protocol"; return false;
    }
    return true;
}
bool inventory(const QString &package,CacheWorkerResult *result) {
    auto registry=gst_registry_get();
    GList *all=gst_registry_get_plugin_list(registry);
    result->blacklistFree=true;
    QStringList discovered;
    bool ok=true;
    const auto pluginDirectory=QDir(package).filePath("gstreamer-plugins");
    for (GList *cursor=all;cursor;cursor=cursor->next) {
        auto plugin=GST_PLUGIN(cursor->data);
        if (GST_OBJECT_FLAG_IS_SET(plugin,GST_PLUGIN_FLAG_BLACKLISTED)) { result->blacklistFree=false; ok=false; continue; }
        const char *filename=gst_plugin_get_filename(plugin);
        if (!filename) continue; // statically registered Gst core has no deployed DLL
        const auto source=FileSystemPath::absolute(QString::fromUtf8(filename));
        const auto relative=QDir(package).relativeFilePath(source);
        const auto name=QString::fromUtf8(gst_plugin_get_name(plugin));
        auto loaded=gst_plugin_load_by_name(name.toUtf8().constData());
        const bool local=samePath(QFileInfo(source).absolutePath(),pluginDirectory)
            && !QFileInfo(source).canonicalFilePath().isEmpty()
            && samePath(QFileInfo(source).canonicalFilePath(),source);
        const bool loadMatches=loaded && gst_plugin_get_filename(loaded)
            && samePath(QString::fromUtf8(gst_plugin_get_filename(loaded)),source);
        if (!local || !loadMatches || discovered.contains(source,Qt::CaseInsensitive)) ok=false;
        discovered.append(source);
        result->plugins.append(QJsonObject{{"name",name},{"source",relative},{"loaded",loadMatches},{"local",local}});
        if (loaded) gst_object_unref(loaded);
    }
    gst_plugin_list_free(all);
    const auto files=QDir(pluginDirectory).entryInfoList({"*.dll"},QDir::Files|QDir::Hidden|QDir::System);
    if (files.isEmpty()) ok=false;
    for (const auto &file:files) if (!discovered.contains(FileSystemPath::absolute(file.absoluteFilePath()),Qt::CaseInsensitive)) ok=false;
    if (!ok) result->reason="Plugin discovery, name loading, source or blacklist validation failed";
    return ok;
}
CacheWorkerResult runWorker(const CacheWorkerRequest &request) {
    CacheWorkerResult result; result.complete=true; result.nonce=request.nonce; result.stage=request.stage; result.baseline=request.baseline;
    if (!CacheStorage::verifyWorkerOwnershipRequest(request.ownershipRequest,request.ownedRoot,&result.reason)) return result;
    const auto authority=QJsonDocument::fromJson(request.ownershipRequest).object();
    if (!samePath(authority.value("package").toString(),request.packageDirectory)) { result.reason="Worker package ownership mismatch"; return result; }
    if (request.stage==CacheWorkerStage::Cleanup) {
        result.cleanup=CacheStorage::cleanupWorkerOwnershipRequest(request.ownershipRequest,false,request.remainingCleanupBudgetMs);
        result.reason=result.cleanup.reason;
        result.workerProof=QJsonDocument(QJsonObject{{"schemaVersion",CacheSchemaVersion},{"phase","cleanup-proof"},
            {"nonce",request.nonce},{"ownershipNonce",authority.value("nonce")},{"ownedRoot",request.ownedRoot},
            {"complete",result.cleanup.complete},{"residualPaths",QJsonArray::fromStringList(result.cleanup.residualPaths)}}).toJson(QJsonDocument::Compact);
        return result;
    }
    if (request.stage==CacheWorkerStage::PrepareCommit) {
        const auto obj=QJsonDocument::fromJson(request.preparationRequest).object();
        if (!samePath(obj.value("package").toString(),request.packageDirectory)
            || !samePath(QFileInfo(obj.value("runtimeRegistry").toString()).absolutePath(),request.ownedRoot)) {
            result.reason="Preparation authority mismatch"; return result;
        }
        if (obj.value("phase")=="sealed-verification") {
            result.workerProof=CacheStorage::verifyPreparedWorkerRequest(request.preparationRequest,&result.reason);
            if (!result.workerProof.isEmpty()) {
                const auto proof=QJsonDocument::fromJson(result.workerProof).object();
                result.registrySha256=decodeHash(proof.value("registrySha256")); result.recordSha256=decodeHash(proof.value("recordSha256"));
                result.fingerprint={true,decodeHash(proof.value("inputSha256")),{}};
                result.pendingRegistry=obj.value("pendingRegistry").toString(); result.pendingRecord=obj.value("pendingRecord").toString();
                result.snapshotUnchanged=true; result.cleanup.complete=true;
            }
        } else {
            const auto receipt=CacheStorage::fillPreparedWorkerRequest(request.preparationRequest,request.inputRegistry,request.validationRecord,&result.reason);
            if (!receipt.isEmpty()) {
                const auto filled=QJsonDocument::fromJson(receipt).object();
                result.pendingRegistry=filled.value("pendingRegistry").toString(); result.pendingRecord=filled.value("pendingRecord").toString();
                result.registrySha256=decodeHash(filled.value("registrySha256")); result.recordSha256=decodeHash(filled.value("recordSha256"));
                result.fingerprint={true,decodeHash(obj.value("inputSha256")),{}};
            }
        }
        return result;
    }
    QString baselineError;
    CacheReadStatus baselineStatus;
    const auto actualBaseline=CacheStorage::readBaseline(request.packageDirectory,&baselineError,&baselineStatus);
    const bool assess=request.stage==CacheWorkerStage::Assess;
    if (baselineStatus!=CacheReadStatus::Available && (!assess || baselineStatus!=CacheReadStatus::Unavailable)) { result.reason=baselineError; return result; }
    result.baseline=actualBaseline;
    result.baselineTrusted=baselineStatus==CacheReadStatus::Available;result.baselineReadStatus=baselineStatus;
    const QString unavailableReason=baselineError;
    if (request.stage!=CacheWorkerStage::Assess && (actualBaseline.exists!=request.baseline.exists || actualBaseline.sha256!=request.baseline.sha256)) {
        result.reason="Shared target changed before worker"; return result;
    }
    result.fingerprint=CacheStorage::fingerprint(request.packageDirectory);
    if (!result.fingerprint.valid && request.stage!=CacheWorkerStage::Assess) { result.reason=result.fingerprint.reason; return result; }
    if (request.stage==CacheWorkerStage::Verify && (!request.expectedInput.valid || request.expectedInput.sha256!=result.fingerprint.sha256)) {
        result.reason="Candidate package fingerprint differs"; return result;
    }
    const bool scan=request.stage==CacheWorkerStage::Scan;
    if (!scan && request.inputRegistry.isEmpty()) { result.reason="Input registry required"; return result; }
    if (request.stage==CacheWorkerStage::Verify && !QFileInfo::exists(FileSystemPath::forIo(request.inputRegistry))) { result.reason="Verification candidate missing"; return result; }
    CacheReadStatus copyStatus=CacheReadStatus::Available;
    const QString source=scan || (assess && !result.baselineTrusted)?QString{}:request.inputRegistry;
    if (!CacheStorage::copyWorkerRegistry(request.ownershipRequest,source,request.outputRegistry,&result.reason,assess,&copyStatus)) return result;
    if(assess && copyStatus==CacheReadStatus::Unavailable){result.baselineTrusted=false;result.baselineReadStatus=CacheReadStatus::Unavailable;}
    QString hashError;
    const auto before=hashFile(request.outputRegistry,&hashError);
    if (before.isEmpty()) { result.reason=hashError; return result; }
    if(assess && result.baselineTrusted && actualBaseline.exists
        && samePath(request.inputRegistry,request.packageDirectory+"/gstreamer-1.0/registry.x86_64.bin") && before!=actualBaseline.sha256) {
        result.baselineTrusted=false;result.baselineReadStatus=CacheReadStatus::Rejected;result.reason="Shared snapshot changed while copied";return result;
    }
    if (!DependencyDiagnostics::configurePackageLocalGStreamerEnvironment(request.packageDirectory,request.outputRegistry)) {
        result.reason="Private package environment unavailable"; return result;
    }
    qputenv("GST_REGISTRY_UPDATE",scan || request.stage==CacheWorkerStage::Assess?"yes":"no");
    qputenv("GST_REGISTRY_FORK","yes");
    fallback.store(false);
    gst_debug_set_default_threshold(GST_LEVEL_WARNING);
    gst_debug_add_log_function(gstLog,nullptr,nullptr);
    const auto handler=g_log_set_handler("GStreamer",GLogLevelFlags(G_LOG_LEVEL_WARNING|G_LOG_FLAG_FATAL|G_LOG_FLAG_RECURSION),glibLog,nullptr);
    result.readiness=DependencyDiagnostics::checkPackageGStreamerPluginReadiness(request.packageDirectory);
    if (!result.readiness.initializationError.isEmpty()) result.reason=result.readiness.initializationError;
    else inventory(request.packageDirectory,&result);
    result.scannerFallback=fallback.load();
    gst_debug_remove_log_function(gstLog); g_log_remove_handler("GStreamer",handler);
    result.registrySha256=hashFile(request.outputRegistry,&hashError);
    result.snapshotUnchanged=!scan && !result.registrySha256.isEmpty() && result.registrySha256==before;
    if (result.registrySha256.isEmpty()) result.reason=hashError;
    if (result.scannerFallback) result.reason="Normal scanner unavailable; fallback rejected";
    if (request.stage==CacheWorkerStage::Verify && !result.snapshotUnchanged) result.reason="Verification registry snapshot changed";
    const auto after=CacheStorage::fingerprint(request.packageDirectory);
    const bool inputsChanged=reconcileCacheWorkerFingerprint(result,after);
    CacheReadStatus finalStatus;
    const auto finalBaseline=CacheStorage::readBaseline(request.packageDirectory,&baselineError,&finalStatus);
    if(finalStatus!=CacheReadStatus::Available) {
        result.baselineTrusted=false;result.baselineReadStatus=finalStatus;result.snapshotUnchanged=false;
        result.reason=baselineError;
        if(finalStatus==CacheReadStatus::Rejected)result.readiness.ready=false;
    } else if(baselineStatus==CacheReadStatus::Available
        && (finalBaseline.exists!=actualBaseline.exists || finalBaseline.sha256!=actualBaseline.sha256)) {
        result.baselineTrusted=false;result.baselineReadStatus=CacheReadStatus::Rejected;result.reason="Shared target changed during worker";result.snapshotUnchanged=false;
    }
    if(inputsChanged && !result.baselineTrusted){result.baselineReadStatus=CacheReadStatus::Rejected;result.readiness.ready=false;result.reason="Package inputs changed during worker";}
    if(assess && !result.baselineTrusted) {
        result.snapshotUnchanged=false;
        if(result.reason.isEmpty())result.reason=unavailableReason.isEmpty()?"Shared registry snapshot unavailable":unavailableReason;
    }
    return result;
}
}
bool reconcileCacheWorkerFingerprint(CacheWorkerResult &result,const CacheFingerprint &after) {
    const bool changed=result.fingerprint.valid && after.valid && after.sha256!=result.fingerprint.sha256;
    if(changed || !result.fingerprint.valid || !after.valid) {
        const QString reason=changed?QStringLiteral("Package inputs changed during worker"):
            (!result.fingerprint.valid?result.fingerprint.reason:after.reason);
        result.fingerprint.valid=false;result.fingerprint.reason=reason;
        result.snapshotUnchanged=false;result.reason=reason;
        if(changed)result.readiness.ready=false;
    }
    return changed;
}
std::optional<int> dispatchGStreamerCacheWorker(int argc,char *argv[]) {
    bool requested=false;
    for (int index=1;index<argc;++index) if (QByteArray(argv[index])=="--gstreamer-cache-worker") requested=true;
    if (!requested) return std::nullopt;
    QCoreApplication app(argc,argv);
    if (argc!=3 || QByteArray(argv[1])!="--gstreamer-cache-worker") return 2;
    QFile file(FileSystemPath::forIo(QString::fromLocal8Bit(argv[2])));
    if (!file.open(QIODevice::ReadOnly) || file.size()>MessageLimit) return 2;
    QJsonParseError parseError; const auto doc=QJsonDocument::fromJson(file.read(MessageLimit+1),&parseError);
    if (parseError.error!=QJsonParseError::NoError || !doc.isObject()) return 2;
    CacheWorkerRequest request; QString reason;
    const bool valid=parseRequest(doc.object(),&request,&reason);
    const auto parent=QFileInfo(request.ownedRoot).absolutePath();
    const bool localResult=QRegularExpression("^[0-9a-f]{32}$").match(request.nonce).hasMatch()
        && samePath(QFileInfo(request.resultPath).absolutePath(),parent)
        && QFileInfo(request.resultPath).fileName()=="worker-"+request.nonce+".json"
        && samePath(QFileInfo(QString::fromLocal8Bit(argv[2])).absolutePath(),parent)
        && QFileInfo(QString::fromLocal8Bit(argv[2])).fileName()=="worker-"+request.nonce+".request.json";
    QString ownershipError;
    if (!localResult || !CacheStorage::verifyWorkerOwnershipRequest(request.ownershipRequest,request.ownedRoot,&ownershipError)) {
        QTextStream(stderr)<<"Invalid owned worker protocol paths or authority: "<<ownershipError<<Qt::endl; return 2;
    }
    CacheWorkerResult result;
    if (valid) result=runWorker(request);
    else { result.complete=true; result.nonce=request.nonce; result.stage=request.stage; result.reason=reason; }
    const auto bytes=QJsonDocument(resultJson(result)).toJson(QJsonDocument::Compact);
    // Results are retained outside ownedRoot so Cleanup can remove its complete scope.
    // Every request/result is a freshly created direct child of the owned runtime parent.
    if (bytes.size()>MessageLimit) { QTextStream(stderr)<<"Invalid owned worker result path"<<Qt::endl; return 2; }
    QFile output(FileSystemPath::forIo(request.resultPath));
    if (!output.open(QIODevice::WriteOnly|QIODevice::NewOnly) || output.write(bytes)!=bytes.size() || !output.flush()) return 2;
    QTextStream(stdout)<<QString::fromUtf8(bytes)<<Qt::endl;
    return 0;
}
