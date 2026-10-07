#pragma once
#include "platform/GStreamerCacheTypes.h"
namespace GStreamerCacheNative { class Remover; }
class RuntimeCacheLease {
public:
    ~RuntimeCacheLease();
    QString registryPath() const;
    // Normal runtime close is one bounded synchronous cleanup after final Gst use.
    // Startup/destruction on the UI thread uses worker cleanup and proof adoption.
    CacheCleanupResult close();
    CacheCleanupResult close(int remainingBudgetMs);
private:
    struct Implementation;
    explicit RuntimeCacheLease(std::unique_ptr<Implementation>);
    std::unique_ptr<Implementation> implementation_;
    friend class CacheStorage;
};
class RegistryWriteLease {
public:
    ~RegistryWriteLease();
    CacheCleanupResult close();
    CacheCleanupResult close(int remainingBudgetMs);
    CacheCleanupResult close(int remainingBudgetMs, GStreamerCacheNative::Remover &);
private:
    struct Implementation;
    explicit RegistryWriteLease(std::unique_ptr<Implementation>);
    std::unique_ptr<Implementation> implementation_;
    friend class CacheStorage;
};
namespace GStreamerCacheNative {
class Publisher {
public:
    virtual ~Publisher() = default;
    virtual bool publishFile(const QString &pending, const QString &target,
        const QByteArray &expectedNativeId, QString *error);
    virtual bool renameFile(void *nativeHandle, const QString &target, QString *error);
};
class Remover {
public:
    virtual ~Remover() = default;
    virtual bool removeFile(const QString &path, const QByteArray &expectedNativeId, QString *error);
    virtual bool deleteFile(void *nativeHandle, const QString &registeredPath, QString *error);
};
}
struct CacheProtocolObject { QString path; QByteArray nativeId; };
class CacheStorage {
public:
    // Accept a normal current worker proof cheaply. Invalid proof freezes incomplete
    // residuals, preventing a recursive retry or a fresh destructor budget.
    static bool acknowledgeCleanupProof(const std::shared_ptr<RuntimeCacheLease> &, const QByteArray &authority,
        const QString &nonce, const QByteArray &proof, const CacheCleanupResult &, QString *error);
    static CacheProtocolObject createProtocolRequest(const std::shared_ptr<RuntimeCacheLease> &, const QString &nonce,
        const QByteArray &bytes, QString *error);
    static CacheProtocolObject readProtocolResult(const std::shared_ptr<RuntimeCacheLease> &, const QString &nonce,
        QByteArray *bytes, QString *error);
    static CacheCleanupResult cleanupProtocolObjects(const QList<CacheProtocolObject> &, int remainingBudgetMs,
        GStreamerCacheNative::Remover &);
    static QByteArray workerOwnershipRequest(const std::shared_ptr<RuntimeCacheLease> &, QString *error);
    static QByteArray workerFillRequest(RegistryWriteLease &, const PreparedCacheCommit &, QString *error);
    // A separate source owner preserves the assessed runtime while a second owner is filled.
    static QByteArray workerFillRequest(RegistryWriteLease &, const PreparedCacheCommit &,
        const std::shared_ptr<RuntimeCacheLease> &sourceRuntime, QString *error);
    static bool verifyWorkerOwnershipRequest(const QByteArray &, const QString &ownedRoot, QString *error);
    static bool copyWorkerRegistry(const QByteArray &, const QString &input, const QString &output, QString *error,
        bool allowSharedReadFallback = false, CacheReadStatus *sourceStatus = nullptr);
    static QByteArray fillPreparedWorkerRequest(const QByteArray &, const QString &candidate,
        const CacheValidationRecord &, QString *error);
    static CacheCleanupResult cleanupWorkerOwnershipRequest(const QByteArray &, bool keepRuntime,
        int remainingBudgetMs = CleanupBudgetMs);
    static CacheFingerprint fingerprint(const QString &packageDirectory);
    static std::shared_ptr<RuntimeCacheLease> createRuntime(const QString &packageDirectory,
        const QString &temporaryParent, QString *error);
    static CacheLockAttempt tryRegistryWriteLock(const QString &packageDirectory);
    static CacheBaseline readBaseline(const QString &packageDirectory, QString *error = nullptr,
        CacheReadStatus *status = nullptr);
    static CacheValidationRecord readValidationRecord(const QString &packageDirectory, QString *error = nullptr,
        CacheRecordReadStatus *status = nullptr);
    // Reserve exact pending objects in the lock owner before delegation to a worker.
    // The worker receives only these paths; adoption accepts no arbitrary paths.
    static PreparedCacheCommit reservePreparedCommit(RegistryWriteLease &,
        const std::shared_ptr<RuntimeCacheLease> &, const CacheFingerprint &, const CacheBaseline &, QString *error);
    static bool sealPreparedCommit(RegistryWriteLease &, const PreparedCacheCommit &,
        const QByteArray &registrySha256, const QByteArray &recordSha256, QString *error);
    // Opaque request/proof carries the private runtime marker only to the owned worker.
    static QByteArray workerPreparationRequest(RegistryWriteLease &, const PreparedCacheCommit &, QString *error);
    static QByteArray verifyPreparedWorkerRequest(const QByteArray &request, QString *error);
    static bool adoptPreparedCommit(RegistryWriteLease &, const PreparedCacheCommit &,
        const QByteArray &workerProof, QString *error);
    // Synchronous file-work convenience; keep this inside a controlled worker, never GUI startup.
    static PreparedCacheCommit prepareCommit(RegistryWriteLease &, const std::shared_ptr<RuntimeCacheLease> &,
        const QString &candidatePath, const CacheValidationRecord &, const CacheFingerprint &,
        const CacheBaseline &, QString *error);
    static CacheCommitResult publishPreparedCache(RegistryWriteLease &, const PreparedCacheCommit &,
        int remainingCleanupBudgetMs = CleanupBudgetMs);
    // Internal native boundary used by controlled failure adapters, with no product test switch.
    static CacheCommitResult publishPreparedCache(RegistryWriteLease &, const PreparedCacheCommit &,
        GStreamerCacheNative::Publisher &, int remainingCleanupBudgetMs = CleanupBudgetMs);
};