#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <atomic>
#include <cstring>
#include <gst/gst.h>
#include "backend/UxPlayReceiver.h"
#include "backend/UxPlayDiscovery.h"
#include "platform/MdnsPublishing.h"
#include "lib/raop.h"

namespace {
std::atomic_int duplicateWarnings{0};
void observeLog(const gchar *domain, GLogLevelFlags level, const gchar *message, gpointer) {
    if (message && std::strstr(message, "g_set_application_name() called multiple times"))
        ++duplicateWarnings;
    // Observe without hiding the native diagnostic.
    g_log_default_handler(domain, level, message, nullptr);
}
QString applicationName() {
    const char *name = g_get_application_name();
    return name ? QString::fromUtf8(name) : QString();
}
class TestPublisher final : public MdnsPublishing {
public:
    QString publishedName;
    int publications = 0;
    bool publish(const QString &name, const QByteArray &, quint16,
                 const char *, int, const char *, int) override {
        publishedName = name;
        ++publications;
        return true;
    }
    void stop() override {}
};
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString mode = argc > 1 ? QString::fromUtf8(argv[1]) : QString();
    if (mode != "same" && mode != "rename" && mode != "reset"
        && mode != "preseed" && mode != "program") return 64;
    // Each CTest invocation owns a fresh GLib identity and plugin cache. Scan in
    // this disposable process so a timeout cannot leave a scanner child behind.
    QTemporaryDir registry;
    if (!registry.isValid()) return 65;
    qputenv("GST_REGISTRY_1_0", registry.filePath("registry.bin").toUtf8());
    qputenv("GST_REGISTRY_FORK", "no");
    const guint handler = g_log_set_handler("GLib",
        GLogLevelFlags(G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION), observeLog, nullptr);
    QString expectedIdentity = QStringLiteral("AirPlay Receiver");
    if (mode == "preseed") {
        g_set_application_name("Embedding Application");
        expectedIdentity = QStringLiteral("Embedding Application");
    } else if (mode == "program") {
        g_set_prgname("embedding-program");
        expectedIdentity = QStringLiteral("embedding-program");
    }
    // Gst may supply argv[0] as a program name during first initialization.
    // Initialize it first, then construct the absent-name scenario explicitly.
    gst_init(nullptr, nullptr);
    if (mode == "same" || mode == "rename" || mode == "reset")
        g_set_prgname(nullptr);
    TestPublisher publisher;
    int initializations = 0, recordingPipelines = 0, failures = 0;
    bool initialIdentityChecked = false;
    auto require = [&](bool condition, const QString &message) {
        if (!condition) {
            ++failures;
            QTextStream(stderr) << "CONTRACT FAILURE: " << message << Qt::endl;
        }
    };
    UxPlayReceiverConfig config;
    config.serverName = QStringLiteral("Receiver A");
    config.videoSink = QStringLiteral("fakesink");
    config.audioSink = QStringLiteral("fakesink");
    config.mdnsPublisher = &publisher;
    config.rendererCallObserver = [&](const QString &call) {
        if (call != QStringLiteral("video_renderer_init")) return;
        if (!initialIdentityChecked) {
            initialIdentityChecked = true;
            // Check the real precondition after gst_init, before the renderer.
            require(mode == "preseed" || mode == "program"
                    ? applicationName() == expectedIdentity
                    : g_get_application_name() == nullptr,
                    QStringLiteral("initial GLib identity precondition"));
        }
        ++initializations;
    };
    // Keep real receiver/renderers/discovery logic, replace external publication
    // and recording I/O; this test must never create a recording pipeline.
    config.recordingControllerHooks.probeCapabilities = [] { return GstRecordingCapabilityResult{}; };
    config.recordingControllerHooks.ensureOutputDirectory = [](const QString &) { return QString(); };
    config.recordingControllerHooks.cleanupStaleTemporaryFiles = [](const QString &) { return QStringList{}; };
    config.recordingControllerHooks.createPipeline = [&] { ++recordingPipelines; return RecordingPipelineSession{}; };
    {
        UxPlayReceiver receiver(config);
        QString expectedReceiver = QStringLiteral("Receiver A");
        const auto check = [&](const char *phase, ReceiverState state, int expectedInitializations) {
            require(receiver.state() == state, QString::fromLatin1(phase) + " receiver state");
            require(receiver.receiverName() == expectedReceiver, QString::fromLatin1(phase) + " receiver name");
            require(publisher.publishedName == expectedReceiver, QString::fromLatin1(phase) + " publication name");
            require(applicationName() == expectedIdentity, QString::fromLatin1(phase) + " GLib identity");
            require(initializations == expectedInitializations, QString::fromLatin1(phase) + " renderer count");
            require(duplicateWarnings.load() == 0, QString::fromLatin1(phase) + " duplicate application-name warning");
            require(recordingPipelines == 0, QString::fromLatin1(phase) + " recording scope");
            const QJsonObject result{{"phase", QString::fromLatin1(phase)}, {"mode", mode},
                {"glibName", applicationName()}, {"receiverName", receiver.receiverName()},
                {"publishedName", publisher.publishedName}, {"publications", publisher.publications},
                {"state", int(receiver.state())}, {"videoInitializations", initializations},
                {"duplicateWarnings", duplicateWarnings.load()}, {"recordingPipelines", recordingPipelines}};
            QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
        };
        receiver.start();
        check("first-start", ReceiverState::Discoverable, 1);
        if (receiver.state() != ReceiverState::Discoverable) return 66;
        if (mode != "same") {
            expectedReceiver = QStringLiteral("Receiver B");
            require(receiver.applyReceiverName(expectedReceiver), QStringLiteral("rename accepted"));
            QElapsedTimer wait;
            wait.start();
            const int deadline = UxPlayDiscoveryConfig{}.restartDelayMs + 1000;
            while (publisher.publishedName != expectedReceiver && wait.elapsed() < deadline) {
                QCoreApplication::processEvents();
                QThread::msleep(2);
            }
            require(publisher.publications >= 2, QStringLiteral("rename republished"));
            check("after-rename", ReceiverState::Discoverable, 1);
        }
        if (mode == "reset") {
            receiver.setStateFromUxPlayCallback(ReceiverState::Connected);
            QCoreApplication::processEvents();
            require(receiver.state() == ReceiverState::Connected, QStringLiteral("simulated connection accepted"));
            receiver.handleVideoResetFromUxPlayCallback(RESET_TYPE_NOHOLD);
            check("after-video-reset", ReceiverState::Connected, 2);
        } else {
            for (int cycle = 2; cycle <= 3; ++cycle) {
                receiver.stop();
                require(receiver.state() == ReceiverState::Idle, QStringLiteral("restart stopped cleanly"));
                receiver.start();
                check(cycle == 2 ? "second-start" : "third-start", ReceiverState::Discoverable, cycle);
            }
        }
        receiver.stop();
        check("stopped", ReceiverState::Idle, mode == "reset" ? 2 : 3);
    }
    require(initialIdentityChecked, QStringLiteral("real renderer initialization observed"));
    require(duplicateWarnings.load() == 0, QStringLiteral("no late duplicate application-name warning"));
    g_log_remove_handler("GLib", handler);
    return failures == 0 ? 0 : 1;
}
