#include <QtTest>
#include <QApplication>
#include <QEventLoop>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>
#include "app/GStreamerStartupPresentation.h"
class GStreamerStartupPresentationTest : public QObject {
    Q_OBJECT
private slots:
    void statusWindowRemainsResponsiveAndCloseCancels_data() {
        QTest::addColumn<bool>("applicationQuit");
        QTest::newRow("status-close") << false;
        QTest::newRow("application-quit") << true;
    }
    void statusWindowRemainsResponsiveAndCloseCancels() {
        QFETCH(bool,applicationQuit);
        QTemporaryDir dir(qEnvironmentVariable("AIRPLAY_CACHE_STARTUP_TEST_RUNTIME")+"/p-XXXXXX");
        QVERIFY(dir.isValid());
        QObject timerContext;
        bool heartbeat=false, cleanupDelivered=false, receiverStarted=false;
        GStreamerCacheOperations ops;
        ops.nowMs=[] { return qint64(0); };
        ops.launch=[](const auto &,qint64,auto) {};
        ops.stopAndCleanup=[&](qint64,auto done) {
            QTimer::singleShot(20, qApp, [&,done] {cleanupDelivered=true;done({true,{},{}});});
        };
        GStreamerStartupCache cache(ops);
        GStreamerStartupPresentation presentation;
        const bool original=qApp->quitOnLastWindowClosed();
        QTimer::singleShot(10,&timerContext,[&] {
            for(auto *widget:QApplication::topLevelWidgets())
                if(widget->objectName()=="gstreamerStartupStatus") {heartbeat=true;if(applicationQuit)QCoreApplication::postEvent(qApp,new QEvent(QEvent::Quit));else widget->close();}
        });
        const auto result=presentation.prepare(cache,{dir.path(),"unused",dir.path()+"/tmp"});
        if(!result.cancelled && result.readinessState==ReadinessState::Ready && result.runtime) receiverStarted=true;
        QVERIFY(heartbeat);
        QVERIFY(result.cancelled);
        QVERIFY(cleanupDelivered);
        QVERIFY(!receiverStarted);
        QCOMPARE(qApp->quitOnLastWindowClosed(),original);
    }
    void statusCompletionDoesNotQuitMainWindow() {
        for(bool original:{true,false}) {
            qApp->setQuitOnLastWindowClosed(original);
            GStreamerStartupCache cache;
            GStreamerStartupPresentation presentation;
            GStreamerCacheRequest request;request.mode=GStreamerCacheMode::Unmanaged;
            const auto result=presentation.prepare(cache,request);
            QCOMPARE(result.cacheState,CacheState::RecoverySkipped);
            QCOMPARE(qApp->quitOnLastWindowClosed(),original);
            QWidget normal; normal.show();
            QObject timerContext;
            bool heartbeat=false;
            QTimer::singleShot(15,&timerContext,[&]{heartbeat=true;qApp->quit();});
            qApp->exec();
            QVERIFY(heartbeat);
            qApp->setQuitOnLastWindowClosed(false);
            normal.close();
        }
        qApp->setQuitOnLastWindowClosed(true);
    }
    void cacheFailureNoticeIsModelessOnce() {
        QWidget owner;owner.show();
        GStreamerStartupPresentation presentation;
        GStreamerCacheResult result;result.readinessState=ReadinessState::Ready;
        result.cacheState=CacheState::RecoveryFailed;result.failureReason=CacheFailureReason::Busy;
        presentation.showCacheNoticeOnce(&owner,result);
        presentation.showCacheNoticeOnce(&owner,result);
        QTRY_COMPARE(owner.findChildren<QMessageBox *>().size(),1);
        auto *warning=owner.findChild<QMessageBox *>();
        QVERIFY(!warning->isModal());
        QVERIFY(warning->text().contains("continue starting"));
        QVERIFY(warning->text().contains("another application"));
        warning->close();
    }
    void recordFailureNoticeSaysUpdated() {
        QWidget owner;owner.show();
        GStreamerStartupPresentation presentation;
        GStreamerCacheResult result;result.readinessState=ReadinessState::Ready;
        result.cacheState=CacheState::Updated;result.recordState=RecordState::NotSaved;
        result.failureReason=CacheFailureReason::RecordSaveFailed;
        result.reason="C:/private/raw-error";
        presentation.showCacheNoticeOnce(&owner,result);
        QTRY_VERIFY(owner.findChild<QMessageBox *>());
        auto *warning=owner.findChild<QMessageBox *>();
        QVERIFY(warning->text().contains("was updated"));
        QVERIFY(!warning->text().contains("private"));
        warning->close();
    }
    void failedCoreDoesNotShowCacheNotice() {
        QWidget owner;
        GStreamerStartupPresentation presentation;
        for(auto readiness:{ReadinessState::Unknown,ReadinessState::NotReady}) {
            GStreamerCacheResult r;r.cacheState=CacheState::RecoveryFailed;r.readinessState=readiness;
            presentation.showCacheNoticeOnce(&owner,r);
        }
        QTest::qWait(20);
        QVERIFY(owner.findChildren<QMessageBox *>().isEmpty());
    }
};
QTEST_MAIN(GStreamerStartupPresentationTest)
#include "GStreamerStartupPresentationTest.moc"
