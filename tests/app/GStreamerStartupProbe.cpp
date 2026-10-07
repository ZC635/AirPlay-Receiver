#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include "app/GStreamerStartupPresentation.h"
#include "platform/GStreamerCacheWorker.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QThread>
#include <QTimer>
#include <QTextStream>
#include <gst/gst.h>
#include "app/MainWindow.h"
#include "backend/UxPlayReceiver.h"
#include "platform/DependencyDiagnostics.h"

namespace {
QJsonArray dialogs;
QJsonObject preparation{{"executed",false},{"ready",false},{"leaseReturned",false}}, actualCore{{"executed",false},{"ready",false}};
bool parentConfigured=false;
QString privateRegistry;
QByteArray readBytes(const QString &path) { QFile f(path);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray{}; }
QString hash(const QByteArray &bytes) {return QString(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
QJsonArray origins() {
    QJsonArray result;
    for(const char *name:{"app","libav","playback","autodetect","videoparsersbad"}) {
        GstPlugin *plugin=gst_plugin_load_by_name(name);
        if(!plugin) {result.append(QJsonObject{{"name",name},{"loaded",false}});continue;}
        const char *filename=gst_plugin_get_filename(plugin);
        result.append(QJsonObject{{"name",name},{"loaded",bool(gst_plugin_is_loaded(plugin))},{"filename",filename?filename:""}});
        gst_object_unref(plugin);
    }
    return result;
}
std::optional<int> observedWorker(int argc,char **argv) {
    if(argc!=3 || QByteArray(argv[1])!="--gstreamer-cache-worker") return std::nullopt;
    const auto bytes=readBytes(QString::fromLocal8Bit(argv[2]));
    const auto request=QJsonDocument::fromJson(bytes).object();
    const auto exit=dispatchGStreamerCacheWorker(argc,argv);
    const auto report=qEnvironmentVariable("AIRPLAY_STARTUP_TEST_REPORT");
    if(!report.isEmpty() && exit) {
        const QString prefix=report+"/"+QFileInfo(request.value("packageDirectory").toString()).fileName()+"-"+request.value("nonce").toString();
        QFile input(prefix+"-request.json"); if(input.open(QIODevice::WriteOnly|QIODevice::NewOnly)) input.write(bytes);
        QFile output(prefix+"-result.json"); if(output.open(QIODevice::WriteOnly|QIODevice::NewOnly)) output.write(readBytes(request.value("resultPath").toString()));
        QFile status(prefix+"-exit.txt");if(status.open(QIODevice::WriteOnly|QIODevice::NewOnly))status.write(QByteArray::number(*exit));
    }
    return exit;
}
bool mainWindowSeen = false;
bool receiverStartCalled = false;
bool pendingEventsProcessed = false;
bool duplicateQueued = false;
bool exitQueued = false;
bool cancellationRequested = false;
QString mutation() { return qEnvironmentVariable("AIRPLAY_STARTUP_TEST_MUTATION"); }

void observeDialogs() {
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        auto *box = qobject_cast<QMessageBox *>(widget);
        if (!box || !box->isVisible()) continue;
        const QString title = box->windowTitle();
        const QString text = box->text();
        dialogs.append(QJsonObject{{"title", title}, {"text", text},
                                    {"icon", int(box->icon())}, {"modal",box->isModal()}, {"receiverStarted",receiverStartCalled}});
        box->accept();
        if (mutation() == "duplicate-dialog" && !duplicateQueued) {
            duplicateQueued = true;
            QTimer::singleShot(0, qApp, [title, text] {
                QMessageBox::critical(nullptr, title, text);
            });
        }
    }
}

// This wrapper observes real modal widgets for the whole QApplication lifetime,
// including queued work during destruction after production main has returned.
class ObservedApplication : public QApplication {
public:
    ObservedApplication(int &argc, char **argv) : QApplication(argc, argv) {
        QObject::connect(&timer, &QTimer::timeout, this, [] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                if (qobject_cast<MainWindow *>(widget)) mainWindowSeen = true;
                if(mutation()=="cancel-startup" && !cancellationRequested && widget->objectName()=="gstreamerStartupStatus") {
                    cancellationRequested=true;widget->close();
                }
            }
            observeDialogs();
            if(mainWindowSeen && !exitQueued) {
                exitQueued=true;
                // Product queues its notice before app.exec. Observe it after that
                // queued delivery, before requesting exit and destroying the owner.
                QMetaObject::invokeMethod(qApp,[] {observeDialogs();QCoreApplication::quit();},Qt::QueuedConnection);
            }
        });
        timer.start(1);
    }
    ~ObservedApplication() override {
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 50) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        pendingEventsProcessed = true;
    }
private:
    QTimer timer;
};

// Mutations exist only in this test translation unit. Always probe the real
// registry before the bypass control changes the startup decision.
class ObservedDependencies : public DependencyDiagnostics {
public:
    using DependencyDiagnostics::configurePackageLocalGStreamerEnvironment;
    static bool configurePackageLocalGStreamerEnvironment(const QString &root,const QString &registry) {
        parentConfigured=DependencyDiagnostics::configurePackageLocalGStreamerEnvironment(root,registry);
        privateRegistry=registry;
        return parentConfigured;
    }
    static GStreamerPluginReadiness checkPackageGStreamerPluginReadiness(const QString &root) {
        auto result=DependencyDiagnostics::checkPackageGStreamerPluginReadiness(root);
        actualCore=QJsonObject{{"executed",true},{"ready",result.ready},{"missing",QJsonArray::fromStringList(result.missingPlugins)},
            {"origins",origins()},{"privateRegistry",privateRegistry}};
        if(mutation()=="detection-bypass") {result.ready=true;result.missingPlugins.clear();}
        return result;
    }
    static GStreamerPluginReadiness checkGStreamerPluginReadiness() {
        auto result = DependencyDiagnostics::checkGStreamerPluginReadiness();
        if (mutation() == "detection-bypass") {
            result.ready = true;
            result.missingPlugins.clear();
        }
        return result;
    }
};
class ObservedCachePresentation : public GStreamerStartupPresentation {
public:
    GStreamerCacheResult prepare(GStreamerStartupCache &cache,const GStreamerCacheRequest &request) {
        auto adjusted=request;
        adjusted.temporaryParent=qEnvironmentVariable("AIRPLAY_STARTUP_TEST_TEMP");
        const QString plugin=request.packageDirectory+"/gstreamer-plugins/libgstapp.dll";
        const QByteArray nonplugin=readBytes(plugin);
        const bool bypass=mutation()=="detection-bypass";
        if(bypass) {
            const QByteArray good=readBytes(qEnvironmentVariable("AIRPLAY_STARTUP_TEST_GOOD_APP"));
            QFile target(plugin);
            if(good.isEmpty() || !target.open(QIODevice::WriteOnly|QIODevice::Truncate) || target.write(good)!=good.size()) qFatal("Phase fixture genuine app restoration failed");
        }
        auto result=GStreamerStartupPresentation::prepare(cache,adjusted);
        preparation=QJsonObject{{"executed",true},{"readinessState",int(result.readinessState)},
            {"ready",result.readiness.ready},{"missing",QJsonArray::fromStringList(result.readiness.missingPlugins)},
            {"cacheState",int(result.cacheState)},{"recordState",int(result.recordState)},
            {"cancelled",result.cancelled},{"leaseReturned",bool(result.runtime)},
            {"registry",result.runtime?result.runtime->registryPath():QString{}},
            {"cleanupComplete",result.cleanup.complete},{"residuals",QJsonArray::fromStringList(result.cleanup.residualPaths)}};
        if(bypass) {
            QFile target(plugin);
            if(!target.open(QIODevice::WriteOnly|QIODevice::Truncate) || target.write(nonplugin)!=nonplugin.size()) qFatal("Phase fixture nonplugin restoration failed");
            target.close();
            preparation.insert("nonpluginHashBefore",hash(nonplugin));
            preparation.insert("nonpluginHashAfter",hash(readBytes(plugin)));
            preparation.insert("preparationBypassNoOp",result.readinessState==ReadinessState::Ready);
            if(result.runtime) {result.readinessState=ReadinessState::Ready;result.readiness.ready=true;}
        }
        return result;
    }
};
class ObservedReceiver : public UxPlayReceiver {
public:
    using UxPlayReceiver::UxPlayReceiver;
    void start() override {
        receiverStartCalled = true;
        // A broken startup must fail, and must not publish on the host network.
        // Real acceptance cases abort before reaching this containment boundary.
        if (mutation() != "detection-bypass" && qEnvironmentVariable("AIRPLAY_STARTUP_TEST_MODE") != "cache") UxPlayReceiver::start();
    }
};
}

#define GStreamerStartupPresentation ObservedCachePresentation
#define QApplication ObservedApplication
#define DependencyDiagnostics ObservedDependencies
#define UxPlayReceiver ObservedReceiver
#define main productionStartupMain
#include "app/main.cpp"
#undef main
#undef UxPlayReceiver
#undef DependencyDiagnostics
#undef QApplication
#undef GStreamerStartupPresentation

int main(int argc, char **argv) {
    if(const auto worker=observedWorker(argc,argv)) return *worker;
    // These observations describe production ordinary startup only; core mode
    // separately reports its genuine configured/ready/origins values.
    QJsonObject result{{"ordinaryStartupParentConfigured",false},{"preparation",preparation},{"ordinaryStartupActualCore",actualCore}};
    if (qEnvironmentVariable("AIRPLAY_STARTUP_TEST_MODE") == "core") {
        QCoreApplication app(argc, argv);
        const QString root = QCoreApplication::applicationDirPath();
        result.insert("manifestComplete", DependencyDiagnostics::standaloneRuntimeSnapshot(root).complete);
        result.insert("configured", DependencyDiagnostics::configurePackageLocalGStreamerEnvironment(root));
        const auto readiness = DependencyDiagnostics::checkGStreamerPluginReadiness();
        result.insert("ready", readiness.ready);
        result.insert("missing", QJsonArray::fromStringList(readiness.missingPlugins));
        QJsonArray origins;
        bool loaded = true;
        for (const char *name : {"app", "libav", "playback", "autodetect", "videoparsersbad"}) {
            GstPlugin *plugin = gst_plugin_load_by_name(name);
            if (!plugin) { loaded = false; continue; }
            const char *filename = gst_plugin_get_filename(plugin);
            origins.append(QJsonObject{{"name", name}, {"filename", filename ? filename : ""},
                                        {"loaded", bool(gst_plugin_is_loaded(plugin))}});
            loaded = loaded && gst_plugin_is_loaded(plugin);
            gst_object_unref(plugin);
        }
        result.insert("origins", origins);
        result.insert("allLoaded", loaded);
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
        return readiness.ready && loaded ? 0 : 1;
    }
    const int startupExit = productionStartupMain(argc, argv);
    result.insert("startupExit", startupExit);
    result.insert("cancellationRequested",cancellationRequested);
    result.insert("preparation",preparation);
    result.insert("ordinaryStartupActualCore",actualCore);
    result.insert("ordinaryStartupParentConfigured",parentConfigured);
    result.insert("privateRegistry",privateRegistry);
    result.insert("runtimeRemoved",privateRegistry.isEmpty() || !QFileInfo::exists(privateRegistry));
    result.insert("dialogs", dialogs);
    result.insert("mainWindowSeen", mainWindowSeen);
    result.insert("receiverStartCalled", receiverStartCalled);
    result.insert("pendingEventsProcessed", pendingEventsProcessed);
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
    return startupExit;
}
