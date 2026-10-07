#pragma once
#include "platform/GStreamerCacheTypes.h"
#include "platform/DependencyDiagnostics.h"
#include <optional>
enum class CacheWorkerStage { Assess, Scan, Verify, PrepareCommit, Cleanup };
struct CacheWorkerRequest {
    int schemaVersion = CacheSchemaVersion;
    CacheWorkerStage stage = CacheWorkerStage::Assess;
    QString nonce, packageDirectory, ownedRoot, inputRegistry, outputRegistry, resultPath;
    CacheBaseline baseline;
    CacheFingerprint expectedInput;
    QByteArray ownershipRequest, preparationRequest;
    CacheValidationRecord validationRecord;
    int remainingCleanupBudgetMs = CleanupBudgetMs;
};
struct CacheWorkerResult {
    bool complete = false;
    QString nonce;
    CacheWorkerStage stage = CacheWorkerStage::Assess;
    CacheFingerprint fingerprint;
    QByteArray registrySha256, recordSha256, workerProof;
    GStreamerPluginReadiness readiness;
    QJsonArray plugins;
    bool blacklistFree = false, snapshotUnchanged = false, scannerFallback = false;
    QString pendingRegistry, pendingRecord;
    CacheBaseline baseline;
    bool baselineTrusted = false;
    CacheReadStatus baselineReadStatus = CacheReadStatus::Rejected;
    QString reason;
    CacheCleanupResult cleanup;
};
// Retain measured core readiness when fingerprint evidence is unavailable.
// Returns true only for a positively observed package-input change.
bool reconcileCacheWorkerFingerprint(CacheWorkerResult &result, const CacheFingerprint &after);
// Normal mode returns nullopt. Internal worker mode uses only QCoreApplication.
std::optional<int> dispatchGStreamerCacheWorker(int argc, char *argv[]);
