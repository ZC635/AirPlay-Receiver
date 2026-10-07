#pragma once
#include "platform/GStreamerCacheWorker.h"
#include "platform/GStreamerCacheProcess.h"
#include "platform/GStreamerCacheStorage.h"
#include <QObject>
#include <functional>
#include <memory>
enum class GStreamerCacheMode { PortablePackage, Unmanaged };
struct GStreamerCacheRequest {
    QString packageDirectory, executable, temporaryParent;
    GStreamerCacheMode mode = GStreamerCacheMode::PortablePackage;
};
struct GStreamerCacheResult {
    CacheState cacheState = CacheState::Unknown;
    RecordState recordState = RecordState::NotApplicable;
    ReadinessState readinessState = ReadinessState::Unknown;
    GStreamerPluginReadiness readiness;
    bool cancelled = false;
    CacheFailureReason failureReason = CacheFailureReason::None;
    QString reason;
    std::shared_ptr<RuntimeCacheLease> runtime;
    CacheCleanupResult cleanup;
};
struct GStreamerCacheOperations {
    std::function<qint64()> nowMs;
    std::function<void(const CacheWorkerRequest &, qint64,
        std::function<void(CacheProcessResult, CacheWorkerResult)>)> launch;
    std::function<void(qint64, std::function<void(CacheCleanupResult)>)> stopAndCleanup;
    std::function<CacheCommitResult(RegistryWriteLease &, const PreparedCacheCommit &)> publish;
};
Q_DECLARE_METATYPE(GStreamerCacheResult)
Q_DECLARE_METATYPE(CacheWorkerStage)
class GStreamerStartupCache : public QObject {
    Q_OBJECT
public:
    explicit GStreamerStartupCache(QObject *parent = nullptr);
    explicit GStreamerStartupCache(GStreamerCacheOperations, QObject *parent = nullptr);
    ~GStreamerStartupCache() override;
    bool start(const GStreamerCacheRequest &);
    void cancel();
signals:
    void stageChanged(CacheWorkerStage);
    void finished(const GStreamerCacheResult &);
private:
    struct Session;
    std::shared_ptr<Session> session_;
};
