#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QStringList>
#include <memory>

inline constexpr int PrepareBudgetMs = 60000;
inline constexpr int CleanupBudgetMs = 5000;
inline constexpr int CacheSchemaVersion = 1;
enum class CacheState { Reused, Updated, RecoveryFailed, RecoverySkipped, Unknown };
enum class RecordState { Matched, Saved, NotSaved, Invalid, NotApplicable };
enum class ReadinessState { Ready, NotReady, Unknown };
enum class CacheFailureReason { None, Busy, DirectoryNotWritable, TempUnavailable, Timeout,
    ScanFailed, ValidationFailed, InputChanged, PrecommitCleanupFailed, RecordSaveFailed,
    OwnershipUnknown, UnsupportedPublish };
struct CacheFingerprint { bool valid = false; QByteArray sha256; QString reason; };
enum class CacheReadStatus { Available, Unavailable, Rejected };
enum class CacheRecordReadStatus { Unavailable, Malformed, Parsed };
struct CacheBaseline { bool exists = false; QByteArray sha256; };
struct CacheValidationRecord {
    int schemaVersion = 0;
    QByteArray inputSha256;
    QByteArray registrySha256;
    QJsonArray plugins;
    bool validated = false;
};
struct CacheCleanupResult { bool complete = false; QStringList residualPaths; QString reason; };
class RuntimeCacheLease;
class RegistryWriteLease;
enum class CacheLockState { Acquired, Busy, Unavailable, ForeignObject };
struct CacheLockAttempt {
    CacheLockState state = CacheLockState::Unavailable;
    std::shared_ptr<RegistryWriteLease> lease;
    QString reason;
};
struct PreparedCacheCommit {
    QString pendingRegistry;
    QString pendingRecord;
    CacheBaseline baseline;
    CacheFingerprint input;
    std::shared_ptr<RuntimeCacheLease> runtime;
};
struct CacheCommitResult {
    CacheState cacheState = CacheState::Unknown;
    RecordState recordState = RecordState::NotApplicable;
    QString reason;
    CacheFailureReason failureReason = CacheFailureReason::None;
    CacheCleanupResult cleanup;
};
bool validationRecordMatches(const CacheValidationRecord &, const CacheFingerprint &, const CacheBaseline &);