#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>

struct DiscoveryRestartOperations {
    std::function<bool()> canRestart;
    std::function<void()> stopHttpdIfStarted;
    std::function<void()> unregisterBroadcast;
    std::function<void()> destroyBroadcast;
    std::function<bool()> createBroadcast;
    std::function<bool()> startHttpdIfNeeded;
    std::function<bool()> registerBroadcast;
    std::function<void(QString)> restoreReceiverName;
    std::function<void(QString)> fail;
    std::function<bool()> canContinue;
};

class DiscoveryRestartController : public QObject {
    Q_OBJECT
public:
    explicit DiscoveryRestartController(int restartDelayMs = 2000, QObject *parent = nullptr);
    ~DiscoveryRestartController() override;

    bool schedule(QString recoveryName, bool shouldStartHttpd, DiscoveryRestartOperations ops);
    void cancel();
    bool pending() const;
    QString recoveryName() const;

    void trigger();

private:
    void handleTimeout();
    void cleanupResources(const DiscoveryRestartOperations &ops);
    bool tryRecoverRegistration(const DiscoveryRestartOperations &ops, bool httpdWasStarted);

    QTimer *m_timer = nullptr;
    bool m_pending = false;
    QString m_recoveryName;
    bool m_shouldStartHttpd = false;
    DiscoveryRestartOperations m_ops;
    int m_delayMs;
};
