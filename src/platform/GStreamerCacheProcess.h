#pragma once
#include <QObject>
#include <QProcessEnvironment>
#include <QStringList>
#include <QByteArray>
#include <functional>
#include <memory>
struct CacheProcessRequest {
    QString executable;
    QStringList arguments;
    QProcessEnvironment environment;
    QString ownedResultPath;
    QString nonce;
};
struct CacheProcessResult {
    bool normalExit = false;
    int exitCode = -1;
    bool timedOut = false;
    bool cancelled = false;
    bool outputComplete = false;
    QByteArray stdoutBytes;
    QByteArray stderrBytes;
    QString reason;
};
Q_DECLARE_METATYPE(CacheProcessResult)
// Native adapters receive borrowed opaque handles, never ownership of a process.
// The native worker copies adapters and invokes createJob/assignProcess/closeJob there.
// Destruction may leave only bounded native cleanup, including copied closeJob calls.
// Adapters must own/retain captured resources for that tail; never capture the runner,
// its QObject parent, or caller stack references that can expire at destruction.
struct CacheProcessJobOperations {
    std::function<void*(QString*)> createJob;
    std::function<bool(void*, void*, QString*)> assignProcess;
    std::function<void(void*)> closeJob;
};
class CacheProcessRunner : public QObject {
    Q_OBJECT
public:
    explicit CacheProcessRunner(QObject *parent = nullptr);
    explicit CacheProcessRunner(CacheProcessJobOperations operations, QObject *parent = nullptr);
    ~CacheProcessRunner() override;
    // One active request. The clock is sampled on the native worker; use a fast,
    // thread-safe callable (also sampled by cancel(deadline) on the calling thread).
    // Public methods must be called on this QObject's thread. Destruction synchronizes
    // and clears the last clock sample: no tail call to the caller clock or QObject.
    // Completion is emitted only after native handles close; keep the runner alive to
    // observe outputComplete=false/reason on incomplete cleanup before releasing leases.
    bool start(const CacheProcessRequest &, qint64 absoluteDeadlineMs, std::function<qint64()> monotonicNow);
    // Cleanup-stage launch: sample the same owned/thread-safe clock once here and
    // install a native cap before starting. Timeout/destruction cannot extend it.
    bool start(const CacheProcessRequest &, qint64 absoluteDeadlineMs, std::function<qint64()> monotonicNow,
        qint64 absoluteCleanupDeadlineMs);
    // One cleanup deadline; subsequent cancellation/destruction can only shorten it.
    void cancel();
    // Internal coordinator boundary: same clock epoch as start(); remaining native
    // cleanup is clamped to 0..5000ms. An expired cap terminates/closes immediately.
    void cancel(qint64 absoluteCleanupDeadlineMs);
signals:
    void completed(const CacheProcessResult &);
private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};
