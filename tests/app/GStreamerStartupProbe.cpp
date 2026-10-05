#include <QApplication>
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
bool mainWindowSeen = false;
bool receiverStartCalled = false;
bool pendingEventsProcessed = false;
bool duplicateQueued = false;
QString mutation() { return qEnvironmentVariable("AIRPLAY_STARTUP_TEST_MUTATION"); }

// This wrapper observes real modal widgets for the whole QApplication lifetime,
// including queued work during destruction after production main has returned.
class ObservedApplication : public QApplication {
public:
    ObservedApplication(int &argc, char **argv) : QApplication(argc, argv) {
        QObject::connect(&timer, &QTimer::timeout, this, [] {
            for (QWidget *widget : QApplication::topLevelWidgets()) {
                if (qobject_cast<MainWindow *>(widget)) {
                    mainWindowSeen = true;
                    QCoreApplication::quit();
                }
                auto *box = qobject_cast<QMessageBox *>(widget);
                if (!box || !box->isVisible()) continue;
                const QString title = box->windowTitle();
                const QString text = box->text();
                dialogs.append(QJsonObject{{"title", title}, {"text", text},
                                            {"icon", int(box->icon())}});
                box->accept();
                if (mutation() == "duplicate-dialog" && !duplicateQueued) {
                    duplicateQueued = true;
                    QTimer::singleShot(0, qApp, [title, text] {
                        QMessageBox::critical(nullptr, title, text);
                    });
                }
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
    static GStreamerPluginReadiness checkGStreamerPluginReadiness() {
        auto result = DependencyDiagnostics::checkGStreamerPluginReadiness();
        if (mutation() == "detection-bypass") {
            result.ready = true;
            result.missingPlugins.clear();
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
        if (mutation() != "detection-bypass") UxPlayReceiver::start();
    }
};
}

#define QApplication ObservedApplication
#define DependencyDiagnostics ObservedDependencies
#define UxPlayReceiver ObservedReceiver
#define main productionStartupMain
#include "app/main.cpp"
#undef main
#undef UxPlayReceiver
#undef DependencyDiagnostics
#undef QApplication

int main(int argc, char **argv) {
    QJsonObject result;
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
    result.insert("dialogs", dialogs);
    result.insert("mainWindowSeen", mainWindowSeen);
    result.insert("receiverStartCalled", receiverStartCalled);
    result.insert("pendingEventsProcessed", pendingEventsProcessed);
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
    return startupExit;
}
