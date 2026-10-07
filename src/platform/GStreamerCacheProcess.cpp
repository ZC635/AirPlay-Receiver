#include "platform/GStreamerCacheProcess.h"
#include "platform/GStreamerCacheTypes.h"
#include "platform/FileSystemPath.h"
#include <QTimer>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <vector>
#include <algorithm>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
constexpr qsizetype StreamLimit = 8 * 1024 * 1024;
constexpr LONGLONG ResultLimit = 1024 * 1024;
QString nativeError(const QString &operation) {
    return operation + QStringLiteral(" (Win32 %1)").arg(GetLastError());
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { reset(); }
    void reset(HANDLE next = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = next;
    }
    Handle() = default;
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
CacheProcessJobOperations nativeJobOperations() {
    return {
        [](QString *error) -> void* {
            HANDLE job = CreateJobObjectW(nullptr, nullptr); // Never inheritable.
            if (!job) { *error = nativeError(QStringLiteral("CreateJobObject")); return nullptr; }
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
                *error = nativeError(QStringLiteral("SetInformationJobObject")); CloseHandle(job); return nullptr;
            }
            return job;
        },
        [](void *job, void *process, QString *error) {
            if (AssignProcessToJobObject(job, process)) return true;
            *error = nativeError(QStringLiteral("AssignProcessToJobObject")); return false;
        },
        [](void *job) { CloseHandle(job); }
    };
}
// Windows command-line quoting, including trailing backslashes before a quote.
QString quoteArgument(const QString &argument) {
    QString quoted = QStringLiteral("\"");
    int slashes = 0;
    for (const QChar c : argument) {
        if (c == QLatin1Char('\\')) { ++slashes; continue; }
        if (c == QLatin1Char('"')) {
            quoted += QString(slashes * 2 + 1, QLatin1Char('\\')); quoted += c;
        } else {
            quoted += QString(slashes, QLatin1Char('\\')); quoted += c;
        }
        slashes = 0;
    }
    return quoted + QString(slashes * 2, QLatin1Char('\\')) + QLatin1Char('"');
}
struct RunState {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};
    std::atomic<qint64> cleanupDeadline{0};
    CacheProcessResult result;
    std::mutex clockMutex;
    std::function<qint64()> clock;
};
qint64 steadyMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void shortenCleanup(const std::shared_ptr<RunState> &state, qint64 requestedDeadline) {
    qint64 existing = state->cleanupDeadline.load();
    while (existing == 0 || requestedDeadline < existing) {
        if (state->cleanupDeadline.compare_exchange_weak(existing, requestedDeadline)) return;
    }
}
void requestCancellation(const std::shared_ptr<RunState> &state, qint64 requestedDeadline) {
    shortenCleanup(state, requestedDeadline);
    state->cancelled.store(true);
}
struct Job {
    void *value = nullptr;
    CacheProcessJobOperations &operations;
    ~Job() { if (value) operations.closeJob(value); }
};
bool createOutputPipe(Handle &read, Handle &write, QString *error) {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    if (!CreatePipe(&read.value, &write.value, &attributes, 0)) {
        *error = nativeError(QStringLiteral("CreatePipe")); return false;
    }
    if (!SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0)) {
        *error = nativeError(QStringLiteral("SetHandleInformation")); return false;
    }
    return true;
}
// Single reader: ReadFile is issued only for bytes that PeekNamedPipe reports available.
// Each turn drains a bounded amount so floods cannot starve deadline/cancellation checks.
bool drain(Handle &pipe, QByteArray &bytes, bool &eof, bool &overflow, QString &reason) {
    for (int turn = 0; turn < 4 && !eof; ++turn) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe.value, nullptr, 0, nullptr, &available, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) { eof = true; return true; }
            reason = nativeError(QStringLiteral("PeekNamedPipe")); eof = true; return false;
        }
        if (!available) return true;
        char buffer[65536]; DWORD count = 0;
        if (!ReadFile(pipe.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) { eof = true; return true; }
            reason = nativeError(QStringLiteral("ReadFile")); eof = true; return false;
        }
        const qsizetype room = StreamLimit - bytes.size();
        bytes.append(buffer, std::min<qsizetype>(room, count));
        if (count > room) overflow = true;
    }
    return true;
}
void run(const std::shared_ptr<RunState> &state, const CacheProcessRequest &request,
         qint64 deadline, CacheProcessJobOperations operations) {
    CacheProcessResult result;
    // All native handles and pipe operations belong to this worker thread.
    auto work = [&] {
        Handle stdoutRead, stdoutWrite, stderrRead, stderrWrite, input, process, thread;
        Job job{nullptr, operations};
        bool failure = false, stopping = false, stdoutEof = false, stderrEof = false;
        bool overflow = false, resultOversized = false, pipesHealthy = true, treeEnded = false, directEnded = false;
        bool assigned = false;
        const QString resultPath = FileSystemPath::forIo(request.ownedResultPath);
        auto stop = [&](const QString &reason) {
            failure = true;
            if (result.reason.isEmpty()) result.reason = reason;
            if (stopping) return;
            stopping = true;
            shortenCleanup(state, steadyMilliseconds() + CleanupBudgetMs);
            if (job.value && !TerminateJobObject(job.value, ERROR_CANCELLED)) {
                result.reason += QStringLiteral("; ") + nativeError(QStringLiteral("TerminateJobObject"));
                // Kill-on-close still applies; loss of tree proof keeps completion false.
                operations.closeJob(job.value); job.value = nullptr;
            }
            if (process.value) TerminateProcess(process.value, ERROR_CANCELLED);
        };
        auto interrupted = [&] {
            bool cancelled = false;
            qint64 sampledNow = 0;
            {
                // Synchronize only cancellation and the fast caller-clock sample.
                // Native termination and copied adapters must never span this lock.
                std::lock_guard<std::mutex> guard(state->clockMutex);
                cancelled = state->cancelled.load();
                if (!cancelled) sampledNow = state->clock();
            }
            if (cancelled) {
                result.cancelled = true; stop(QStringLiteral("Cancelled")); return true;
            }
            if (sampledNow >= deadline) {
                const qint64 late = sampledNow - deadline;
                shortenCleanup(state, steadyMilliseconds() + std::max<qint64>(0, CleanupBudgetMs - late));
                result.timedOut = true; stop(QStringLiteral("Preparation deadline expired")); return true;
            }
            return false;
        };
        if (interrupted()) return;
        if (!createOutputPipe(stdoutRead, stdoutWrite, &result.reason)
            || !createOutputPipe(stderrRead, stderrWrite, &result.reason)) return;
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (input.value == INVALID_HANDLE_VALUE) { result.reason = nativeError(QStringLiteral("Open stdin")); return; }
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<unsigned char> storage(size);
        auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) {
            result.reason = nativeError(QStringLiteral("Initialize handle list")); return;
        }
        struct AttributeList { LPPROC_THREAD_ATTRIBUTE_LIST value; ~AttributeList(){DeleteProcThreadAttributeList(value);} } listOwner{list};
        HANDLE inherited[] = {input.value, stdoutWrite.value, stderrWrite.value};
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
            result.reason = nativeError(QStringLiteral("Set explicit handle list")); return;
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.value; startup.StartupInfo.hStdOutput = stdoutWrite.value;
        startup.StartupInfo.hStdError = stderrWrite.value; startup.lpAttributeList = list;
        QString command = quoteArgument(request.executable);
        for (const auto &argument : request.arguments) command += QLatin1Char(' ') + quoteArgument(argument);
        QStringList entries = request.environment.toStringList();
        std::sort(entries.begin(), entries.end(), [](const QString &a,const QString &b){return a.compare(b,Qt::CaseInsensitive)<0;});
        QString environment;
        for (const auto &entry : entries) { environment += entry; environment += QChar(0); }
        environment += QChar(0); if (entries.isEmpty()) environment += QChar(0);
        PROCESS_INFORMATION information{};
        if (!CreateProcessW(reinterpret_cast<LPCWSTR>(request.executable.utf16()), reinterpret_cast<LPWSTR>(command.data()),
                            nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                            environment.data(), nullptr, &startup.StartupInfo, &information)) {
            result.reason = nativeError(QStringLiteral("CreateProcess suspended")); return;
        }
        process.value = information.hProcess; thread.value = information.hThread;
        stdoutWrite.reset(); stderrWrite.reset(); input.reset();
        // A failed Job gate never resumes the new process.
        job.value = operations.createJob(&result.reason);
        if (!job.value || !operations.assignProcess(job.value, process.value, &result.reason)) {
            stop(result.reason.isEmpty() ? QStringLiteral("Job ownership unavailable") : result.reason);
        } else {
            assigned = true;
            if (!interrupted() && ResumeThread(thread.value) == DWORD(-1))
                stop(nativeError(QStringLiteral("ResumeThread")));
        }
        thread.reset();
        while (true) {
            if (!stopping) interrupted();
            if (!stopping && !resultPath.isEmpty()) {
                Handle file;
                file.value = CreateFileW(reinterpret_cast<LPCWSTR>(resultPath.utf16()), FILE_READ_ATTRIBUTES,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                LARGE_INTEGER size{};
                if (file.value != INVALID_HANDLE_VALUE && GetFileSizeEx(file.value, &size) && size.QuadPart > ResultLimit) {
                    resultOversized = true; stop(QStringLiteral("Result file exceeded 1 MiB limit"));
                }
            }
            bool iterationHealthy = drain(stdoutRead,result.stdoutBytes,stdoutEof,overflow,result.reason);
            iterationHealthy = drain(stderrRead,result.stderrBytes,stderrEof,overflow,result.reason) && iterationHealthy;
            pipesHealthy = pipesHealthy && iterationHealthy;
            if (overflow) stop(QStringLiteral("Output exceeded 8 MiB stream limit"));
            if (!pipesHealthy) stop(result.reason);
            const DWORD status = WaitForSingleObject(process.value, 0);
            directEnded = status == WAIT_OBJECT_0;
            if (status == WAIT_FAILED) stop(nativeError(QStringLiteral("Poll process")));
            if (!assigned) {
                // Never resumed: this direct process cannot have created descendants.
                treeEnded = directEnded;
            } else if (job.value) {
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
                if (QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
                    treeEnded = accounting.ActiveProcesses == 0;
                } else stop(nativeError(QStringLiteral("Query owned process tree")));
            }
            if (directEnded && treeEnded && stdoutEof && stderrEof) break;
            if (stopping && steadyMilliseconds() >= state->cleanupDeadline.load()) {
                result.reason += QStringLiteral("; process tree or output cleanup incomplete"); break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!stopping) interrupted();
        DWORD exitCode = 0;
        const bool gotExitCode = directEnded && GetExitCodeProcess(process.value, &exitCode);
        if (gotExitCode) result.exitCode = static_cast<int>(exitCode);
        result.outputComplete = treeEnded && directEnded && stdoutEof && stderrEof && pipesHealthy && !overflow && !resultOversized;
        result.normalExit = gotExitCode && exitCode < 0x80000000UL && result.outputComplete && !failure;
        if (!gotExitCode && result.reason.isEmpty()) result.reason = QStringLiteral("Process exit was not confirmed");
        // Do not consume JSON here. The upper layer validates nonce, contents and exit code.
        if (result.normalExit && !request.ownedResultPath.isEmpty()) {
            Handle file;
            file.value = CreateFileW(reinterpret_cast<LPCWSTR>(resultPath.utf16()), FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            LARGE_INTEGER resultSize{};
            if (file.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.value, &resultSize) || resultSize.QuadPart > ResultLimit) {
                result.normalExit = false; result.outputComplete = false;
                result.reason = QStringLiteral("Result file unavailable or exceeded 1 MiB limit");
            }
        }
    };
    work(); // Scoped native objects close before publishing completion.
    state->result = std::move(result);
    state->done.store(true);
}
}
struct CacheProcessRunner::Implementation {
    CacheProcessJobOperations operations;
    std::shared_ptr<RunState> state;
    std::thread worker;
    QTimer notifications;
};
CacheProcessRunner::CacheProcessRunner(QObject *parent) : CacheProcessRunner({},parent) {}
CacheProcessRunner::CacheProcessRunner(CacheProcessJobOperations operations,QObject *parent)
    : QObject(parent), implementation_(new Implementation) {
    qRegisterMetaType<CacheProcessResult>();
    const auto defaults = nativeJobOperations();
    if (!operations.createJob) operations.createJob = defaults.createJob;
    if (!operations.assignProcess) operations.assignProcess = defaults.assignProcess;
    if (!operations.closeJob) operations.closeJob = defaults.closeJob;
    implementation_->operations = std::move(operations);
    connect(&implementation_->notifications,&QTimer::timeout,this,[this] {
        auto &i = *implementation_;
        if (!i.state || !i.state->done.load()) return;
        i.notifications.stop(); i.worker.join();
        auto result = std::move(i.state->result);
        if (i.state->cancelled.load()) { result.cancelled = true; result.normalExit = false; if (result.reason.isEmpty()) result.reason = QStringLiteral("Cancelled"); }
        i.state.reset();
        emit completed(result);
    });
}
CacheProcessRunner::~CacheProcessRunner() {
    auto &i = *implementation_;
    if (i.state) {
        requestCancellation(i.state, steadyMilliseconds() + CleanupBudgetMs);
        // Synchronize the last clock sample before the caller can destroy its clock.
        std::lock_guard<std::mutex> guard(i.state->clockMutex);
        i.state->clock = {};
    }
    // No pipe waits or callbacks on the QObject thread. The detached native owner
    // retains its state/adapters and closes the Job within its one cleanup budget.
    if (i.worker.joinable()) i.worker.detach();
}
bool CacheProcessRunner::start(const CacheProcessRequest &request,qint64 deadline,std::function<qint64()> now) {
    auto &i = *implementation_;
    if (i.state || !now || request.executable.isEmpty()) return false;
    i.state = std::make_shared<RunState>();
    i.state->clock = std::move(now);
    i.worker = std::thread(run,i.state,request,deadline,i.operations);
    i.notifications.start(5);
    return true;
}
void CacheProcessRunner::cancel() {
    if (implementation_->state) requestCancellation(implementation_->state, steadyMilliseconds() + CleanupBudgetMs);
}

void CacheProcessRunner::cancel(qint64 absoluteCleanupDeadlineMs) {
    const auto state = implementation_->state;
    if (!state) return;
    std::lock_guard<std::mutex> guard(state->clockMutex);
    const qint64 sampledNow = state->clock();
    const qint64 remaining = absoluteCleanupDeadlineMs <= sampledNow ? 0
        : std::min<qint64>(CleanupBudgetMs, absoluteCleanupDeadlineMs - sampledNow);
    requestCancellation(state, steadyMilliseconds() + remaining);
}
bool CacheProcessRunner::start(const CacheProcessRequest &request,qint64 deadline,std::function<qint64()> now,qint64 cleanupDeadline) {
    auto &i=*implementation_;
    if(i.state || !now || request.executable.isEmpty())return false;
    const qint64 sampled=now();
    const qint64 remaining=cleanupDeadline<=sampled?0:std::min<qint64>(CleanupBudgetMs,cleanupDeadline-sampled);
    i.state=std::make_shared<RunState>();i.state->clock=std::move(now);
    shortenCleanup(i.state,steadyMilliseconds()+remaining);
    i.worker=std::thread(run,i.state,request,deadline,i.operations);
    i.notifications.start(5);return true;
}