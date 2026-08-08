#include "backend/UxPlayDiscovery.h"

#include "backend/DiscoveryRestartController.h"
#include "diagnostics/DiagnosticEvent.h"

#include <QByteArray>

#include <utility>

#if AIRPLAY_WITH_UXPLAY
#include "lib/dnssd.h"
#include "lib/raop.h"
#include "platform/MdnsPublisher.h"
#endif

namespace {
QByteArray defaultHardwareAddress() {
    return QByteArray::fromHex("020000000001");
}
}

UxPlayDiscovery::UxPlayDiscovery(UxPlayDiscoveryConfig config, QObject *parent)
    : QObject(parent)
    , m_config(std::move(config))
{
    if (!m_config.diagnosticSink) {
        m_config.diagnosticSink = &nullDiagnosticLogSink();
    }
#if AIRPLAY_WITH_UXPLAY
    m_discoveryRestartController = new DiscoveryRestartController(m_config.restartDelayMs, this);
    m_mdnsPublisher = m_config.mdnsPublisher;
#endif
}

UxPlayDiscovery::~UxPlayDiscovery()
{
    stop();
}

bool UxPlayDiscovery::start(void *raop, unsigned short requestedPort)
{
#if AIRPLAY_WITH_UXPLAY
    if (m_raop || m_dnssd || m_raopHttpdStarted) {
        stop();
    } else if (m_discoveryRestartController) {
        m_discoveryRestartController->cancel();
    }
    m_lastError.clear();
    if (raop == nullptr) {
        recordFailure(QStringLiteral("raop_required"),
                      {{QStringLiteral("classification"), QStringLiteral("missing_raop")}});
        setLastError(QStringLiteral("Cannot initialize DNS-SD before RAOP initialization"));
        return false;
    }

    m_raop = raop;
    m_raopPort = requestedPort;
    raop_set_port(static_cast<raop_t *>(m_raop), m_raopPort);
    applyVideoQualityToRaop();

    if (!createBroadcast()) {
        stop();
        return false;
    }

    unsigned short port = m_raopPort;
    const int httpdResult = failsForStage(QStringLiteral("http_server_start"))
        ? -1 : raop_start_httpd(static_cast<raop_t *>(m_raop), &port);
    if (httpdResult < 0) {
        recordFailure(QStringLiteral("http_server_start"),
                      {{QStringLiteral("code"), QString::number(httpdResult)}});
        setLastError(QStringLiteral("Failed to start RAOP HTTP server"));
        stop();
        return false;
    }
    m_raopHttpdStarted = true;
    m_raopPort = port;
    raop_set_port(static_cast<raop_t *>(m_raop), m_raopPort);
    recordEvent(QStringLiteral("http_server_started"),
                {{QStringLiteral("port"), QString::number(m_raopPort)}});

    if (!registerBroadcast(m_raopPort)) {
        stop();
        return false;
    }

    return true;
#else
    Q_UNUSED(raop);
    Q_UNUSED(requestedPort);
    setLastError(QStringLiteral("UxPlay support is not enabled in this build"));
    return false;
#endif
}

void UxPlayDiscovery::stop()
{
#if AIRPLAY_WITH_UXPLAY
    if (m_discoveryRestartController) {
        m_discoveryRestartController->cancel();
    }
    stopHttpdIfStarted();
    stopBroadcast();
    m_raopPort = 0;
    m_raop = nullptr;
#endif
}

bool UxPlayDiscovery::restart(QString recoveryName)
{
#if AIRPLAY_WITH_UXPLAY
    m_lastError.clear();
    recordEvent(QStringLiteral("restart_requested"));
    const bool shouldStartHttpd = m_raopHttpdStarted || m_dnssd != nullptr;

    DiscoveryRestartOperations ops;
    ops.canRestart = [this] {
        return m_raop != nullptr && m_raopPort != 0;
    };
    ops.stopHttpdIfStarted = [this] {
        stopHttpdIfStarted();
    };
    ops.unregisterBroadcast = [this] {
        unregisterBroadcast();
    };
    ops.destroyBroadcast = [this] {
        destroyBroadcast();
    };
    ops.createBroadcast = [this] {
        return m_raop != nullptr && createBroadcast();
    };
    ops.startHttpdIfNeeded = [this] {
        if (!m_raop) {
            return false;
        }
        unsigned short port = m_raopPort;
        const int httpdResult = failsForStage(QStringLiteral("http_server_start"))
            ? -1 : raop_start_httpd(static_cast<raop_t *>(m_raop), &port);
        if (httpdResult < 0) {
            recordFailure(QStringLiteral("http_server_start"),
                          {{QStringLiteral("code"), QString::number(httpdResult)}});
            return false;
        }
        raop_set_port(static_cast<raop_t *>(m_raop), port);
        m_raopPort = port;
        m_raopHttpdStarted = true;
        recordEvent(QStringLiteral("http_server_started"),
                    {{QStringLiteral("port"), QString::number(m_raopPort)}});
        return true;
    };
    ops.registerBroadcast = [this] {
        const bool registered = m_raop != nullptr && registerBroadcast(m_raopPort);
        if (registered) {
            recordEvent(QStringLiteral("restart_completed"));
        }
        return registered;
    };
    ops.restoreReceiverName = [this](QString name) {
        m_config.receiverName = std::move(name);
    };
    ops.fail = [this](QString error) {
        recordEvent(QStringLiteral("recovery_failed"),
                    {{QStringLiteral("classification"), QStringLiteral("restart_operation_failed")}},
                    DiagnosticSeverity::Error);
        setLastError(std::move(error));
        emit failed(m_lastError);
    };
    ops.canContinue = [this] {
        return m_raop != nullptr;
    };

    const bool scheduled = m_discoveryRestartController->schedule(std::move(recoveryName), shouldStartHttpd, std::move(ops));
    if (scheduled) {
        recordEvent(QStringLiteral("restart_scheduled"));
    }
    if (!scheduled) {
        stopBroadcast();
    }
    return scheduled;
#else
    Q_UNUSED(recoveryName);
    setLastError(QStringLiteral("UxPlay support is not enabled in this build"));
    return false;
#endif
}

void UxPlayDiscovery::setReceiverName(QString receiverName)
{
    m_config.receiverName = std::move(receiverName);
}

void UxPlayDiscovery::setVideoQuality(VideoQualitySettings quality)
{
    m_config.videoQuality = quality;
    applyVideoQualityToRaop();
}

QString UxPlayDiscovery::lastError() const
{
    return m_lastError;
}

bool UxPlayDiscovery::failsForStage(QStringView stage) const
{
    return m_config.failureForStage && !m_config.failureForStage(stage).isEmpty();
}

void UxPlayDiscovery::recordEvent(QString name, QMap<QString, QString> fields,
                                  DiagnosticSeverity severity, bool flushImmediately) const
{
    m_config.diagnosticSink->record(
        makeDiagnosticEvent(severity, QStringLiteral("discovery"), std::move(name),
                            std::move(fields), flushImmediately));
}

void UxPlayDiscovery::recordFailure(QStringView stage, QMap<QString, QString> fields) const
{
    fields.insert(QStringLiteral("stage"), stage.toString());
    fields.insert(QStringLiteral("result"), QStringLiteral("failed"));
    recordEvent(QStringLiteral("failure"), std::move(fields), DiagnosticSeverity::Error);
}

void UxPlayDiscovery::setLastError(QString error)
{
    m_lastError = std::move(error);
}

void UxPlayDiscovery::applyVideoQualityToRaop()
{
#if AIRPLAY_WITH_UXPLAY
    if (!m_raop) {
        return;
    }
    auto *raop = static_cast<raop_t *>(m_raop);
    raop_set_plist(raop, "width", videoQualityWidth(m_config.videoQuality.resolution));
    raop_set_plist(raop, "height", videoQualityHeight(m_config.videoQuality.resolution));
    raop_set_plist(raop, "refreshRate", videoQualityRefreshRate(m_config.videoQuality.frameRate));
    raop_set_plist(raop, "maxFPS", videoQualityMaxFPS(m_config.videoQuality.frameRate));
#endif
}

bool UxPlayDiscovery::createBroadcast()
{
#if AIRPLAY_WITH_UXPLAY
    if (!m_raop) {
        recordFailure(QStringLiteral("raop_required"),
                      {{QStringLiteral("classification"), QStringLiteral("missing_raop")}});
        setLastError(QStringLiteral("Cannot initialize DNS-SD before RAOP initialization"));
        return false;
    }
    if (m_dnssd) {
        return true;
    }

    int dnssdError = 0;
    const QByteArray serverName = m_config.receiverName.toUtf8();
    const QByteArray hwAddress = defaultHardwareAddress();
    const bool injectedFailure = failsForStage(QStringLiteral("dns_sd_init"));
    auto *dnssd = injectedFailure
        ? nullptr
        : dnssd_init(serverName.constData(), serverName.size(), hwAddress.constData(), hwAddress.size(),
                     &dnssdError, 0);
    if (dnssdError || !dnssd) {
        if (injectedFailure) {
            recordFailure(QStringLiteral("dns_sd_init"),
                          {{QStringLiteral("classification"), QStringLiteral("injected_failure")}});
            setLastError(QStringLiteral("Failed to initialize DNS-SD"));
        } else {
            recordFailure(QStringLiteral("dns_sd_init"),
                          {{QStringLiteral("code"), QString::number(dnssdError)}});
            setLastError(QStringLiteral("Failed to initialize DNS-SD: %1").arg(dnssdError));
        }
        return false;
    }

    m_dnssd = dnssd;
    raop_set_dnssd(static_cast<raop_t *>(m_raop), dnssd);
    dnssd_set_airplay_features(dnssd, 42, videoQualityH265Support() ? 1 : 0);
    recordEvent(QStringLiteral("dns_sd_initialized"));
    return true;
#else
    return false;
#endif
}

bool UxPlayDiscovery::registerBroadcast(unsigned short port)
{
#if AIRPLAY_WITH_UXPLAY
    if (!m_dnssd) {
        recordFailure(QStringLiteral("dns_sd_required"),
                      {{QStringLiteral("classification"), QStringLiteral("missing_dns_sd")}});
        setLastError(QStringLiteral("Cannot register DNS-SD services before DNS-SD initialization"));
        return false;
    }

    auto *dnssd = static_cast<dnssd_t *>(m_dnssd);
    int registerError = failsForStage(QStringLiteral("raop_register"))
        ? -1 : dnssd_register_raop(dnssd, port);
    if (registerError != 0) {
        recordFailure(QStringLiteral("raop_register"),
                      {{QStringLiteral("code"), QString::number(registerError)}});
        setLastError(QStringLiteral("Failed to register DNS-SD services: %1").arg(registerError));
        return false;
    }

    registerError = failsForStage(QStringLiteral("airplay_register"))
        ? -1 : dnssd_register_airplay(dnssd, port);
    if (registerError != 0) {
        recordFailure(QStringLiteral("airplay_register"),
                      {{QStringLiteral("code"), QString::number(registerError)}});
        setLastError(QStringLiteral("Failed to register DNS-SD services: %1").arg(registerError));
        return false;
    }

    int raopTxtLength = 0;
    const char *raopTxt = dnssd_get_raop_txt(dnssd, &raopTxtLength);
    int airplayTxtLength = 0;
    const char *airplayTxt = dnssd_get_airplay_txt(dnssd, &airplayTxtLength);

    if (!m_mdnsPublisher) {
        m_mdnsPublisher = new MdnsPublisher(m_config.diagnosticSink, {}, this);
        m_ownsMdnsPublisher = true;
    }
    const QByteArray hwAddress = defaultHardwareAddress();
    const bool published = !failsForStage(QStringLiteral("mdns_publish"))
        && m_mdnsPublisher->publish(m_config.receiverName, hwAddress, port,
                                    raopTxt, raopTxtLength,
                                    airplayTxt, airplayTxtLength);
    if (!published) {
        recordFailure(QStringLiteral("mdns_publish"),
                      {{QStringLiteral("classification"), QStringLiteral("publisher_rejected")}});
        setLastError(QStringLiteral("Failed to publish mDNS services"));
        return false;
    }
    m_lastError.clear();
    recordEvent(QStringLiteral("services_registered"));
    return true;
#else
    Q_UNUSED(port);
    return false;
#endif
}

void UxPlayDiscovery::stopBroadcast()
{
#if AIRPLAY_WITH_UXPLAY
    if (m_mdnsPublisher) {
        m_mdnsPublisher->stop();
        if (m_ownsMdnsPublisher) {
            delete m_mdnsPublisher;
            m_mdnsPublisher = nullptr;
            m_ownsMdnsPublisher = false;
        }
    }

    if (!m_dnssd) {
        return;
    }

    auto *dnssd = static_cast<dnssd_t *>(m_dnssd);
    dnssd_unregister_raop(dnssd);
    dnssd_unregister_airplay(dnssd);
    recordEvent(QStringLiteral("services_unregistered"));
    dnssd_destroy(dnssd);
    m_dnssd = nullptr;
    recordEvent(QStringLiteral("dns_sd_destroyed"));
#endif
}

void UxPlayDiscovery::unregisterBroadcast()
{
#if AIRPLAY_WITH_UXPLAY
    if (m_mdnsPublisher) {
        m_mdnsPublisher->stop();
    }

    if (!m_dnssd) {
        return;
    }

    auto *dnssd = static_cast<dnssd_t *>(m_dnssd);
    dnssd_unregister_raop(dnssd);
    dnssd_unregister_airplay(dnssd);
    recordEvent(QStringLiteral("services_unregistered"));
#endif
}

void UxPlayDiscovery::destroyBroadcast()
{
#if AIRPLAY_WITH_UXPLAY
    if (m_mdnsPublisher) {
        m_mdnsPublisher->stop();
        if (m_ownsMdnsPublisher) {
            delete m_mdnsPublisher;
            m_mdnsPublisher = nullptr;
            m_ownsMdnsPublisher = false;
        }
    }

    if (!m_dnssd) {
        return;
    }

    auto *dnssd = static_cast<dnssd_t *>(m_dnssd);
    dnssd_destroy(dnssd);
    m_dnssd = nullptr;
    recordEvent(QStringLiteral("dns_sd_destroyed"));
#endif
}

void UxPlayDiscovery::stopHttpdIfStarted()
{
#if AIRPLAY_WITH_UXPLAY
    if (m_raop && m_raopHttpdStarted) {
        raop_stop_httpd(static_cast<raop_t *>(m_raop));
        m_raopHttpdStarted = false;
        recordEvent(QStringLiteral("http_server_stopped"));
    }
#endif
}
