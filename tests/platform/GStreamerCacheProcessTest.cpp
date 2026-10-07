#include <QtTest/QtTest>
#include "platform/GStreamerCacheProcess.h"
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <atomic>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace {
QString fixture(){return QCoreApplication::applicationDirPath()+"/GStreamerCacheProcessFixture.exe";}
QByteArray read(const QString &p){QFile f(p);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray{};}
struct ProcessHandle {
    HANDLE h=nullptr;
    explicit ProcessHandle(DWORD pid):h(OpenProcess(SYNCHRONIZE|PROCESS_TERMINATE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_DUP_HANDLE,FALSE,pid)){}
    ~ProcessHandle(){if(h){if(WaitForSingleObject(h,0)==WAIT_TIMEOUT){TerminateProcess(h,90);WaitForSingleObject(h,5000);}CloseHandle(h);}}
    bool ended()const{return h && WaitForSingleObject(h,0)==WAIT_OBJECT_0;}
    bool running()const{return h && WaitForSingleObject(h,0)==WAIT_TIMEOUT;}
};
QString runtime(){const auto p=qEnvironmentVariable("AIRPLAY_CACHE_PROCESS_TEST_RUNTIME");QDir().mkpath(p);return p;}
QString currentResultPath;
CacheProcessRequest request(const QString &mode,const QTemporaryDir &dir){
    currentResultPath=dir.path()+"/result";
    return {fixture(),{mode,dir.path()+"/result","test-nonce",dir.path()+"/pid"},QProcessEnvironment::systemEnvironment(),dir.path()+"/result","test-nonce"};
}
CacheProcessResult result(QSignalSpy &spy){
    const auto r=qvariant_cast<CacheProcessResult>(spy.at(0).at(0));
    const QString evidence=qEnvironmentVariable("AIRPLAY_CACHE_PROCESS_TEST_REPORT");
    const QString tag=QString::fromLatin1(QTest::currentTestFunction())+"-"+QString::fromLatin1(QTest::currentDataTag());
    const QString prefix=evidence+"/task2-"+tag;
    auto put=[](const QString &p,const QByteArray &bytes){QFile f(p);return f.open(QIODevice::WriteOnly|QIODevice::NewOnly)&&f.write(bytes)==bytes.size();};
    if(!evidence.isEmpty() && !QFile::exists(prefix+".status.json")) {
        QJsonObject summary{{"normalExit",r.normalExit},{"exitCode",r.exitCode},{"timedOut",r.timedOut},
            {"cancelled",r.cancelled},{"outputComplete",r.outputComplete},{"reason",r.reason},
            {"stdoutSize",double(r.stdoutBytes.size())},{"stderrSize",double(r.stderrBytes.size())}};
        const bool stored=put(prefix+".stdout.bin",r.stdoutBytes)&&put(prefix+".stderr.bin",r.stderrBytes)
            &&put(prefix+".result-file.bin",read(currentResultPath))
            &&put(prefix+".status.json",QJsonDocument(summary).toJson());
        if(!stored) QTest::qFail("Cannot persist raw native process evidence",__FILE__,__LINE__);
    }
    return r;
}
}
class GStreamerCacheProcessTest : public QObject {
    Q_OBJECT
private slots:
    void cleanupLaunchCapSurvivesTimeoutAndDestruction_data();
    void cleanupLaunchCapSurvivesTimeoutAndDestruction();
    void normalJsonWaitsForCompleteOutput();
    void nativeStopDoesNotBlockQObjectBoundary_data();
    void nativeStopDoesNotBlockQObjectBoundary();
    void sharedCleanupDeadline_data();
    void sharedCleanupDeadline();
    void repeatedCancelAndDestructionDoNotExtendCleanup();
    void unrelatedInheritableHandleIsExcluded();
    void deadlineAlreadyElapsedNeverExecutes();
    void busyRunnerRejectsAnotherStart();
    void destructionCancelsWithoutLateCallbacks();
    void oversizedResultTerminatesRunningWriter();
    void descendantAndInheritedPipeCannotOutliveOwnedJob();
    void parentCrashKillsWorkersButLeavesOtherProcess();
    void noJobAssignmentNoExecution();
    void noJobCreationNoExecution();
    void outputFloodCannotProduceValidSuccess_data();
    void outputFloodCannotProduceValidSuccess();
    void cancellationRejectsResultAndEndsWorker();
    void crashIsNotNormalExit();
    void oversizedResultCannotProduceValidSuccess();
};
void GStreamerCacheProcessTest::normalJsonWaitsForCompleteOutput(){
    QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());
    QElapsedTimer clock;clock.start();CacheProcessRunner runner;QSignalSpy spy(&runner,&CacheProcessRunner::completed);
    QVERIFY(runner.start(request("normal",dir),10000,[&]{return clock.elapsed();}));QTRY_COMPARE_WITH_TIMEOUT(spy.count(),1,15000);
    const auto r=result(spy);QVERIFY(r.normalExit);QCOMPARE(r.exitCode,0);QVERIFY(r.outputComplete);
    QCOMPARE(QJsonDocument::fromJson(r.stdoutBytes).object()["nonce"].toString(),QString("test-nonce"));
    QCOMPARE(read(dir.path()+"/result"),r.stdoutBytes);QVERIFY(!r.cancelled);QVERIFY(!r.timedOut);
}
// Removing tree containment or completing on the direct child's exit breaks this test.
void GStreamerCacheProcessTest::descendantAndInheritedPipeCannotOutliveOwnedJob(){
    QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());
    std::atomic<qint64> clock{0};CacheProcessRunner runner;QSignalSpy spy(&runner,&CacheProcessRunner::completed);
    QVERIFY(runner.start(request("tree",dir),1000,[&]{return clock.load();}));
    QTRY_VERIFY_WITH_TIMEOUT(read(dir.path()+"/pid").split('\n').size()==2,10000);
    ProcessHandle child(read(dir.path()+"/pid").split('\n')[1].toULong());QVERIFY(child.h);
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(dir.path()+"/pid.child"),10000);
    clock=1000;QTRY_COMPARE_WITH_TIMEOUT(spy.count(),1,10000);
    const auto hungResult=result(spy);const bool ownedTreeEnded=child.ended();
    qInfo()<<"owned descendant handle ended"<<ownedTreeEnded;
    QVERIFY(!hungResult.normalExit);QVERIFY(hungResult.timedOut);QVERIFY(ownedTreeEnded);
}
// Removing kill-on-close allows the worker to survive a parent crash.
void GStreamerCacheProcessTest::parentCrashKillsWorkersButLeavesOtherProcess(){
    QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());
    QProcess unrelated;unrelated.start(fixture(),{"hang",dir.path()+"/other-result","n",dir.path()+"/other-pid"});
    QVERIFY(unrelated.waitForStarted(10000));ProcessHandle other(DWORD(unrelated.processId()));QVERIFY(other.h);
    QProcess owner;owner.start(fixture(),{"owner",dir.path()+"/result","n",dir.path()+"/pid"});QVERIFY(owner.waitForStarted(10000));
    QTRY_VERIFY_WITH_TIMEOUT(read(dir.path()+"/pid").split('\n').size()==2,10000);
    ProcessHandle child(read(dir.path()+"/pid").split('\n')[1].toULong());QVERIFY(child.h);
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(dir.path()+"/pid.child"),10000);
    ProcessHandle ownerHandle(DWORD(owner.processId()));QVERIFY(ownerHandle.h);QVERIFY(TerminateProcess(ownerHandle.h,91));
    QVERIFY(owner.waitForFinished(10000));
    const bool ownedTreeEnded=WaitForSingleObject(child.h,5000)==WAIT_OBJECT_0;
    const bool unrelatedProcessStillRunning=other.running();
    qInfo()<<"crashed owner's descendant handle ended"<<ownedTreeEnded<<"unrelated handle running"<<unrelatedProcessStillRunning;
    unrelated.kill();QVERIFY(unrelated.waitForFinished(5000));
    QVERIFY(ownedTreeEnded);QVERIFY(unrelatedProcessStillRunning);
}
void GStreamerCacheProcessTest::noJobAssignmentNoExecution(){
    QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());
    CacheProcessJobOperations ops;
    ops.assignProcess=[](void*,void*,QString *error){*error="controlled assignment failure";return false;};
    CacheProcessRunner runner(ops);QSignalSpy spy(&runner,&CacheProcessRunner::completed);QElapsedTimer clock;clock.start();
    QVERIFY(runner.start(request("normal",dir),10000,[&]{return clock.elapsed();}));QTRY_COMPARE_WITH_TIMEOUT(spy.count(),1,15000);
    const bool sideEffectFileExistsAfterFailedJobAssignment=QFile::exists(dir.path()+"/result");
    QVERIFY(!sideEffectFileExistsAfterFailedJobAssignment);QVERIFY(!result(spy).normalExit);QVERIFY(!result(spy).reason.isEmpty());
}
void GStreamerCacheProcessTest::noJobCreationNoExecution(){
    QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());CacheProcessJobOperations ops;
    ops.createJob=[](QString *error)->void*{*error="controlled creation failure";return nullptr;};
    CacheProcessRunner runner(ops);QSignalSpy spy(&runner,&CacheProcessRunner::completed);QElapsedTimer clock;clock.start();
    QVERIFY(runner.start(request("normal",dir),10000,[&]{return clock.elapsed();}));QTRY_COMPARE_WITH_TIMEOUT(spy.count(),1,15000);
    QVERIFY(!QFile::exists(dir.path()+"/result"));QVERIFY(!result(spy).normalExit);QVERIFY(!result(spy).reason.isEmpty());
}
void GStreamerCacheProcessTest::outputFloodCannotProduceValidSuccess_data(){QTest::addColumn<QString>("mode");QTest::newRow("stdout")<<QString("flood");QTest::newRow("stderr")<<QString("stderr-flood");}
void GStreamerCacheProcessTest::outputFloodCannotProduceValidSuccess(){
    QFETCH(QString,mode);QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());QElapsedTimer clock;clock.start();
    CacheProcessRunner runner;QSignalSpy spy(&runner,&CacheProcessRunner::completed);
    QVERIFY(runner.start(request(mode,dir),10000,[&]{return clock.elapsed();}));QTRY_COMPARE_WITH_TIMEOUT(spy.count(),1,15000);
    const auto overflowResult=result(spy);QVERIFY(!overflowResult.outputComplete);QVERIFY(!overflowResult.normalExit);
    QVERIFY(overflowResult.stdoutBytes.size()<=8388608);QVERIFY(overflowResult.stderrBytes.size()<=8388608);
}
void GStreamerCacheProcessTest::cancellationRejectsResultAndEndsWorker(){
    QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());QElapsedTimer clock;clock.start();CacheProcessRunner runner;QSignalSpy spy(&runner,&CacheProcessRunner::completed);
    QVERIFY(runner.start(request("hang",dir),10000,[&]{return clock.elapsed();}));QTRY_VERIFY_WITH_TIMEOUT(!read(dir.path()+"/pid").isEmpty(),10000);
    ProcessHandle child(read(dir.path()+"/pid").toULong());QVERIFY(child.h);runner.cancel();QTRY_COMPARE_WITH_TIMEOUT(spy.count(),1,10000);
    QVERIFY(result(spy).cancelled);QVERIFY(!result(spy).normalExit);QVERIFY(child.ended());QVERIFY(!result(spy).timedOut);
}
void GStreamerCacheProcessTest::crashIsNotNormalExit(){
    QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());QElapsedTimer clock;clock.start();CacheProcessRunner runner;QSignalSpy spy(&runner,&CacheProcessRunner::completed);
    QVERIFY(runner.start(request("crash",dir),10000,[&]{return clock.elapsed();}));QTRY_COMPARE_WITH_TIMEOUT(spy.count(),1,15000);
    QVERIFY(!result(spy).normalExit);QVERIFY(result(spy).exitCode!=0);
}
void GStreamerCacheProcessTest::oversizedResultCannotProduceValidSuccess(){
    QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());QElapsedTimer clock;clock.start();CacheProcessRunner runner;QSignalSpy spy(&runner,&CacheProcessRunner::completed);
    QVERIFY(runner.start(request("large-result",dir),10000,[&]{return clock.elapsed();}));QTRY_COMPARE_WITH_TIMEOUT(spy.count(),1,15000);
    QVERIFY(!result(spy).normalExit);QVERIFY(!result(spy).outputComplete);QVERIFY(!result(spy).reason.isEmpty());
}
void GStreamerCacheProcessTest::oversizedResultTerminatesRunningWriter(){
    QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());CacheProcessRunner runner;QSignalSpy spy(&runner,&CacheProcessRunner::completed);
    QVERIFY(runner.start(request("large-result-hang",dir),10000,[]{return qint64(0);}));
    QTRY_VERIFY_WITH_TIMEOUT(!read(dir.path()+"/pid").isEmpty(),10000);ProcessHandle child(read(dir.path()+"/pid").toULong());QVERIFY(child.h);
    const bool finished=spy.wait(5000);runner.cancel();
    if(!finished) QTRY_COMPARE_WITH_TIMEOUT(spy.count(),1,10000);
    QVERIFY(finished);QVERIFY(!result(spy).outputComplete);QVERIFY(!result(spy).normalExit);QVERIFY(!result(spy).timedOut);QVERIFY(child.ended());
}
void GStreamerCacheProcessTest::unrelatedInheritableHandleIsExcluded(){
    QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),nullptr,TRUE};
    ProcessHandle sentinel(0);sentinel.h=CreateEventW(&attributes,TRUE,FALSE,nullptr);QVERIFY(sentinel.h);
    auto r=request("inheritance",dir);r.arguments<<QString::number(quintptr(sentinel.h));
    QElapsedTimer clock;clock.start();CacheProcessRunner runner;QSignalSpy spy(&runner,&CacheProcessRunner::completed);
    QVERIFY(runner.start(r,10000,[&]{return clock.elapsed();}));QTRY_COMPARE_WITH_TIMEOUT(spy.count(),1,15000);
    QVERIFY(result(spy).normalExit);QCOMPARE(result(spy).exitCode,0);
    QCOMPARE(WaitForSingleObject(sentinel.h,0),DWORD(WAIT_TIMEOUT));
    CloseHandle(sentinel.h);sentinel.h=nullptr;
}
void GStreamerCacheProcessTest::deadlineAlreadyElapsedNeverExecutes(){
    QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());CacheProcessRunner runner;QSignalSpy spy(&runner,&CacheProcessRunner::completed);
    QVERIFY(runner.start(request("normal",dir),50,[]{return qint64(50);}));QTRY_COMPARE_WITH_TIMEOUT(spy.count(),1,10000);
    QVERIFY(result(spy).timedOut);QVERIFY(!result(spy).normalExit);QVERIFY(!QFile::exists(dir.path()+"/result"));
}
void GStreamerCacheProcessTest::busyRunnerRejectsAnotherStart(){
    QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());QElapsedTimer clock;clock.start();CacheProcessRunner runner;QSignalSpy spy(&runner,&CacheProcessRunner::completed);
    QVERIFY(runner.start(request("hang",dir),10000,[&]{return clock.elapsed();}));
    QVERIFY(!runner.start(request("normal",dir),10000,[&]{return clock.elapsed();}));
    QTRY_VERIFY_WITH_TIMEOUT(!read(dir.path()+"/pid").isEmpty(),10000);ProcessHandle child(read(dir.path()+"/pid").toULong());QVERIFY(child.h);
    runner.cancel();QTRY_COMPARE_WITH_TIMEOUT(spy.count(),1,10000);QVERIFY(result(spy).cancelled);QVERIFY(child.ended());
    QVERIFY(!QFile::exists(dir.path()+"/result"));
}
void GStreamerCacheProcessTest::destructionCancelsWithoutLateCallbacks(){
    QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());QElapsedTimer clock;clock.start();
    auto runner=std::make_unique<CacheProcessRunner>();int callbacks=0;
    connect(runner.get(),&CacheProcessRunner::completed,this,[&]{++callbacks;});
    QVERIFY(runner->start(request("hang",dir),10000,[&]{return clock.elapsed();}));
    QTRY_VERIFY_WITH_TIMEOUT(!read(dir.path()+"/pid").isEmpty(),10000);ProcessHandle child(read(dir.path()+"/pid").toULong());QVERIFY(child.h);
    runner.reset();QTRY_VERIFY_WITH_TIMEOUT(child.ended(),10000);QCOMPARE(callbacks,0);
}
void GStreamerCacheProcessTest::sharedCleanupDeadline_data(){
    QTest::addColumn<int>("remaining");QTest::newRow("short-budget")<<150;QTest::newRow("expired-budget")<<-1;
}
void GStreamerCacheProcessTest::sharedCleanupDeadline(){
    QFETCH(int,remaining);QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());
    QProcess holder;holder.start(fixture(),{"hang",dir.path()+"/holder-result","n",dir.path()+"/holder-pid"});QVERIFY(holder.waitForStarted(10000));
    ProcessHandle holderHandle(DWORD(holder.processId()));QVERIFY(holderHandle.h);
    auto r=request("pipe-held-outside-job",dir);r.arguments<<QString::number(holder.processId());
    QElapsedTimer clock;clock.start();CacheProcessRunner runner;QSignalSpy spy(&runner,&CacheProcessRunner::completed);
    QVERIFY(runner.start(r,10000,[&]{return clock.elapsed();}));QTRY_VERIFY_WITH_TIMEOUT(read(dir.path()+"/pid").split('\n').size()==2,10000);
    ProcessHandle child(read(dir.path()+"/pid").split('\n')[0].toULong());QVERIFY(child.h);
    runner.cancel(clock.elapsed()+5000); // A later earlier cap must shorten the existing budget.
    runner.cancel(clock.elapsed()+remaining);
    const bool completedWithinSharedBudget=spy.wait(1000);
    const bool unrelatedStillRunning=holderHandle.running();
    holder.kill();QVERIFY(holder.waitForFinished(5000));
    if(!completedWithinSharedBudget)QTRY_COMPARE_WITH_TIMEOUT(spy.count(),1,10000);
    QVERIFY(completedWithinSharedBudget);QVERIFY(unrelatedStillRunning);QVERIFY(child.ended());
    QVERIFY(result(spy).cancelled);QVERIFY(!result(spy).normalExit);QVERIFY(!result(spy).outputComplete);
    QVERIFY(result(spy).reason.contains("cleanup incomplete"));
}
void GStreamerCacheProcessTest::repeatedCancelAndDestructionDoNotExtendCleanup(){
    QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());
    QProcess holder;holder.start(fixture(),{"hang",dir.path()+"/holder-result","n",dir.path()+"/holder-pid"});QVERIFY(holder.waitForStarted(10000));
    ProcessHandle holderHandle(DWORD(holder.processId()));QVERIFY(holderHandle.h);
    auto r=request("pipe-held-outside-job",dir);r.arguments<<QString::number(holder.processId());
    QElapsedTimer clock;clock.start();
    const auto closedAt=std::make_shared<std::atomic<quint64>>(0);
    const auto closedThread=std::make_shared<std::atomic<DWORD>>(0);
    CacheProcessJobOperations ops;
    ops.closeJob=[closedAt,closedThread](void *job){closedThread->store(GetCurrentThreadId());closedAt->store(GetTickCount64());CloseHandle(job);};
    auto runner=std::make_unique<CacheProcessRunner>(ops);int callbacks=0;
    connect(runner.get(),&CacheProcessRunner::completed,this,[&]{++callbacks;});
    const auto clockAlive=std::make_shared<std::atomic<bool>>(true);
    const auto lateClockCalls=std::make_shared<std::atomic<int>>(0);
    QVERIFY(runner->start(r,10000,[&clock,clockAlive,lateClockCalls]{if(!clockAlive->load())++*lateClockCalls;return clock.elapsed();}));QTRY_VERIFY_WITH_TIMEOUT(read(dir.path()+"/pid").split('\n').size()==2,10000);
    const auto marker=read(dir.path()+"/pid").split('\n');ProcessHandle child(marker[0].toULong());QVERIFY(child.h);
    HANDLE probe=nullptr;
    QVERIFY(DuplicateHandle(holderHandle.h,reinterpret_cast<HANDLE>(quintptr(marker[1].toULongLong())),GetCurrentProcess(),&probe,0,FALSE,DUPLICATE_SAME_ACCESS));
    const quint64 cancellationTick=GetTickCount64();const qint64 firstDeadline=clock.elapsed()+150;
    runner->cancel(firstDeadline);QTest::qWait(50);runner->cancel();const qint64 laterDeadline=clock.elapsed()+5000;
    runner->cancel(laterDeadline);runner.reset();clockAlive->store(false);
    bool pipeReadEndClosed=false;QElapsedTimer wait;wait.start();
    while(wait.elapsed()<1000){DWORD written=0;if(!WriteFile(probe,"p",1,&written,nullptr)){const DWORD error=GetLastError();qInfo()<<"native probe WriteFile error"<<error<<"after ms"<<GetTickCount64()-cancellationTick;pipeReadEndClosed=error==ERROR_BROKEN_PIPE || error==ERROR_NO_DATA;break;}QTest::qWait(10);}
    qInfo()<<"cleanup first absolute deadline"<<firstDeadline<<"later requested absolute deadline"<<laterDeadline
        <<"job close after ms"<<(closedAt->load()?qint64(closedAt->load()-cancellationTick):qint64(-1))
        <<"closeJob native thread"<<closedThread->load()<<"QObject thread"<<GetCurrentThreadId();
    CloseHandle(probe);
    const bool unrelatedStillRunning=holderHandle.running();holder.kill();QVERIFY(holder.waitForFinished(5000));
    QVERIFY(pipeReadEndClosed);QVERIFY(unrelatedStillRunning);QTRY_VERIFY_WITH_TIMEOUT(child.ended(),10000);QCOMPARE(callbacks,0);
    QVERIFY(closedAt->load()!=0);QVERIFY(closedAt->load()-cancellationTick<1000);
    QVERIFY(closedThread->load()!=GetCurrentThreadId());QCOMPARE(lateClockCalls->load(),0);
}
void GStreamerCacheProcessTest::nativeStopDoesNotBlockQObjectBoundary_data(){
    QTest::addColumn<bool>("destroy");
    QTest::newRow("cancel-deadline")<<false;
    QTest::newRow("destruction")<<true;
}
void GStreamerCacheProcessTest::nativeStopDoesNotBlockQObjectBoundary(){
    QFETCH(bool,destroy);QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());
    struct SharedCloseGate {
        HANDLE entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        HANDLE release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        std::atomic<bool> finished{false};
        std::atomic<DWORD> thread{0};
        std::atomic<DWORD> terminationError{0};
        ~SharedCloseGate(){if(entered)CloseHandle(entered);if(release)CloseHandle(release);}
    };
    const auto gate=std::make_shared<SharedCloseGate>();QVERIFY(gate->entered);QVERIFY(gate->release);
    const auto clockAlive=std::make_shared<std::atomic<bool>>(true);
    const auto lateClockCalls=std::make_shared<std::atomic<int>>(0);
    const auto callbacks=std::make_shared<std::atomic<int>>(0);
    CacheProcessJobOperations ops;
    ops.createJob=[](QString *error)->void*{
        HANDLE full=CreateJobObjectW(nullptr,nullptr);if(!full){*error="Cannot create real test Job";return nullptr;}
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        HANDLE restricted=nullptr;
        const bool configured=SetInformationJobObject(full,JobObjectExtendedLimitInformation,&limits,sizeof(limits))
            &&DuplicateHandle(GetCurrentProcess(),full,GetCurrentProcess(),&restricted,JOB_OBJECT_ASSIGN_PROCESS|JOB_OBJECT_QUERY,FALSE,0);
        CloseHandle(full);if(!configured){*error="Cannot restrict real test Job";return nullptr;}return restricted;
    };
    ops.closeJob=[gate](void *job){
        gate->terminationError.store(GetLastError());gate->thread.store(GetCurrentThreadId());SetEvent(gate->entered);
        // A retained native event controls only this adapter; the watchdog prevents a RED deadlock.
        WaitForSingleObject(gate->release,1000);CloseHandle(job);gate->finished.store(true);
    };
    auto runner=std::make_unique<CacheProcessRunner>(ops);QSignalSpy spy(runner.get(),&CacheProcessRunner::completed);
    connect(runner.get(),&CacheProcessRunner::completed,this,[callbacks]{++*callbacks;});
    QVERIFY(runner->start(request("hang",dir),10000,[clockAlive,lateClockCalls]{if(!clockAlive->load())++*lateClockCalls;return qint64(0);}));
    QTRY_VERIFY_WITH_TIMEOUT(!read(dir.path()+"/pid").isEmpty(),10000);
    ProcessHandle child(read(dir.path()+"/pid").toULong());QVERIFY(child.h);
    runner->cancel(100); // Shared cleanup already has a short cap before native stop begins.
    QTRY_COMPARE_WITH_TIMEOUT(WaitForSingleObject(gate->entered,0),DWORD(WAIT_OBJECT_0),1000);
    QVERIFY(!gate->finished.load());
    std::thread watchdog([gate]{WaitForSingleObject(gate->release,350);SetEvent(gate->release);});
    const quint64 began=GetTickCount64();
    if(destroy){runner.reset();clockAlive->store(false);}else runner->cancel(50);
    const quint64 callerElapsed=GetTickCount64()-began;
    const bool nativeStillBlocked=!gate->finished.load();
    SetEvent(gate->release);watchdog.join();
    QTRY_VERIFY_WITH_TIMEOUT(gate->finished.load(),1000);
    QTRY_VERIFY_WITH_TIMEOUT(child.ended(),1000);
    if(!destroy)QTRY_COMPARE_WITH_TIMEOUT(spy.count(),1,1000);
    qInfo()<<"QObject boundary destroy"<<destroy<<"caller elapsed ms"<<callerElapsed
        <<"native still blocked on return"<<nativeStillBlocked<<"termination failure Win32"<<gate->terminationError.load()
        <<"close adapter thread"<<gate->thread.load()<<"QObject thread"<<GetCurrentThreadId();
    QVERIFY(callerElapsed<100);QVERIFY(nativeStillBlocked);
    QCOMPARE(gate->terminationError.load(),DWORD(ERROR_ACCESS_DENIED));
    QVERIFY(gate->thread.load()!=GetCurrentThreadId());QCOMPARE(lateClockCalls->load(),0);
    if(destroy)QCOMPARE(callbacks->load(),0);
    else {QCOMPARE(callbacks->load(),1);QVERIFY(result(spy).cancelled);QVERIFY(!result(spy).normalExit);QVERIFY(!result(spy).outputComplete);}
}
void GStreamerCacheProcessTest::cleanupLaunchCapSurvivesTimeoutAndDestruction_data(){
    QTest::addColumn<bool>("destroy");QTest::newRow("native-timeout")<<false;QTest::newRow("owner-destruction")<<true;
}
void GStreamerCacheProcessTest::cleanupLaunchCapSurvivesTimeoutAndDestruction(){
    QFETCH(bool,destroy);QTemporaryDir dir(runtime()+"/pXXXXXX");QVERIFY(dir.isValid());
    QProcess holder;holder.start(fixture(),{"hang",dir.path()+"/holder-result","n",dir.path()+"/holder-pid"});QVERIFY(holder.waitForStarted(10000));
    ProcessHandle holderHandle(DWORD(holder.processId()));QVERIFY(holderHandle.h);
    auto r=request("pipe-held-outside-job",dir);r.arguments<<QString::number(holder.processId());
    const auto clock=std::make_shared<std::atomic<qint64>>(0);
    const auto closed=std::make_shared<std::atomic<quint64>>(0);
    CacheProcessJobOperations ops;ops.closeJob=[closed](void *job){CloseHandle(job);closed->store(GetTickCount64());};
    auto runner=std::make_unique<CacheProcessRunner>(ops);QSignalSpy spy(runner.get(),&CacheProcessRunner::completed);int notifications=0;
    connect(runner.get(),&CacheProcessRunner::completed,this,[&]{++notifications;});
    const quint64 began=GetTickCount64();
    QVERIFY(runner->start(r,1000,[clock]{return clock->load();},150));
    QTRY_VERIFY_WITH_TIMEOUT(read(dir.path()+"/pid").split('\n').size()==2,10000);
    ProcessHandle child(read(dir.path()+"/pid").split('\n')[0].toULong());QVERIFY(child.h);
    if(destroy)runner.reset();else clock->store(1000);
    QElapsedTimer wait;wait.start();while(!closed->load() && wait.elapsed()<1000)QTest::qWait(10);
    const bool capped=closed->load()!=0 && closed->load()-began<1000;
    const bool otherRunning=holderHandle.running();holder.kill();QVERIFY(holder.waitForFinished(5000));
    if(!destroy)QTRY_COMPARE_WITH_TIMEOUT(spy.size(),1,10000);
    qInfo()<<"Cleanup launch capped"<<capped<<"destroy"<<destroy<<"close elapsed"<<(closed->load()?qint64(closed->load()-began):-1);
    QVERIFY(capped);QVERIFY(otherRunning);QTRY_VERIFY_WITH_TIMEOUT(child.ended(),10000);
    if(destroy)QCOMPARE(notifications,0);else{QVERIFY(result(spy).timedOut);QVERIFY(!result(spy).outputComplete);}
}
QTEST_GUILESS_MAIN(GStreamerCacheProcessTest)
#include "GStreamerCacheProcessTest.moc"
