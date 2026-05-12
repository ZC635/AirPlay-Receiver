#include "backend/DiscoveryRestartController.h"

DiscoveryRestartController::DiscoveryRestartController(int restartDelayMs, QObject *parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
    , m_delayMs(restartDelayMs)
{
    m_timer->setSingleShot(true);
    QObject::connect(m_timer, &QTimer::timeout, this, &DiscoveryRestartController::handleTimeout);
}

DiscoveryRestartController::~DiscoveryRestartController()
{
    cancel();
}

bool DiscoveryRestartController::schedule(QString recoveryName, bool shouldStartHttpd,
                                          DiscoveryRestartOperations ops)
{
    if (m_pending) {
        return true;
    }

    if (!ops.canRestart || !ops.canRestart()) {
        if (ops.stopHttpdIfStarted) {
            ops.stopHttpdIfStarted();
        }
        if (ops.fail) {
            ops.fail(QStringLiteral("Cannot restart discovery before RAOP HTTP server is ready"));
        }
        return false;
    }

    if (ops.stopHttpdIfStarted) {
        ops.stopHttpdIfStarted();
    }
    if (ops.unregisterBroadcast) {
        ops.unregisterBroadcast();
    }

    m_pending = true;
    m_recoveryName = std::move(recoveryName);
    m_shouldStartHttpd = shouldStartHttpd;
    m_ops = std::move(ops);
    m_timer->start(m_delayMs);
    return true;
}

void DiscoveryRestartController::cancel()
{
    m_timer->stop();
    m_pending = false;
    m_recoveryName.clear();
    m_ops = {};
}

bool DiscoveryRestartController::pending() const
{
    return m_pending;
}

QString DiscoveryRestartController::recoveryName() const
{
    return m_recoveryName;
}

void DiscoveryRestartController::trigger()
{
    if (!m_pending) {
        return;
    }
    m_timer->stop();
    handleTimeout();
}

void DiscoveryRestartController::cleanupResources(const DiscoveryRestartOperations &ops)
{
    if (ops.stopHttpdIfStarted) {
        ops.stopHttpdIfStarted();
    }
    if (ops.destroyBroadcast) {
        ops.destroyBroadcast();
    }
}

bool DiscoveryRestartController::tryRecoverRegistration(const DiscoveryRestartOperations &ops, bool httpdWasStarted)
{
    if (m_recoveryName.isEmpty()) {
        return false;
    }

    if (ops.restoreReceiverName) {
        ops.restoreReceiverName(m_recoveryName);
    }

    if (!ops.createBroadcast || !ops.createBroadcast()) {
        return false;
    }

    bool httpdStarted = false;
    if (m_shouldStartHttpd) {
        if (ops.startHttpdIfNeeded && ops.startHttpdIfNeeded()) {
            httpdStarted = true;
        } else {
            if (ops.destroyBroadcast) {
                ops.destroyBroadcast();
            }
            return false;
        }
    }

    if (!ops.registerBroadcast || !ops.registerBroadcast()) {
        if (httpdStarted) {
            if (ops.stopHttpdIfStarted) {
                ops.stopHttpdIfStarted();
            }
        }
        if (ops.destroyBroadcast) {
            ops.destroyBroadcast();
        }
        return false;
    }

    return true;
}

void DiscoveryRestartController::handleTimeout()
{
    if (!m_pending) {
        return;
    }

    if (m_ops.canContinue && !m_ops.canContinue()) {
        m_pending = false;
        return;
    }

    if (m_ops.destroyBroadcast) {
        m_ops.destroyBroadcast();
    }

    if (m_ops.createBroadcast && !m_ops.createBroadcast()) {
        if (!m_recoveryName.isEmpty()) {
            if (m_ops.restoreReceiverName) {
                m_ops.restoreReceiverName(m_recoveryName);
            }
            if (!m_ops.createBroadcast || !m_ops.createBroadcast()) {
                if (m_ops.fail) {
                    m_ops.fail(QStringLiteral("Failed to create discovery broadcast after restart"));
                }
                m_pending = false;
                return;
            }
        } else {
            if (m_ops.fail) {
                m_ops.fail(QStringLiteral("Failed to create discovery broadcast after restart"));
            }
            m_pending = false;
            return;
        }
    }

    bool httpdStarted = false;
    if (m_shouldStartHttpd) {
        if (m_ops.startHttpdIfNeeded && m_ops.startHttpdIfNeeded()) {
            httpdStarted = true;
        } else {
            cleanupResources(m_ops);
            if (m_ops.fail) {
                m_ops.fail(QStringLiteral("Failed to restart RAOP HTTP server"));
            }
            m_pending = false;
            return;
        }
    }

    if (m_ops.registerBroadcast && !m_ops.registerBroadcast()) {
        if (httpdStarted && m_ops.stopHttpdIfStarted) {
            m_ops.stopHttpdIfStarted();
        }
        if (m_ops.destroyBroadcast) {
            m_ops.destroyBroadcast();
        }

        if (tryRecoverRegistration(m_ops, httpdStarted)) {
            m_recoveryName.clear();
            m_pending = false;
            return;
        }

        if (m_ops.fail) {
            m_ops.fail(m_recoveryName.isEmpty()
                           ? QStringLiteral("Failed to register discovery broadcast after restart")
                           : QStringLiteral("Failed to recover discovery after receiver rename failure"));
        }
        m_pending = false;
        return;
    }

    m_recoveryName.clear();
    m_pending = false;
}
