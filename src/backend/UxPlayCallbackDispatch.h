#pragma once

#include <QMutex>
#include <QRecursiveMutex>
#include <QtGlobal>

#include <atomic>
#include <utility>

class UxPlayCallbackDispatch {
public:
    UxPlayCallbackDispatch(std::atomic_bool &acceptingCallbacks,
                           std::atomic<quint64> &callbackGeneration,
                           std::atomic_bool &renderersStarted,
                           QRecursiveMutex &rendererMutex);

    quint64 currentGeneration() const;
    bool accepts(quint64 generation) const;

    template <typename Callback>
    bool runIfAccepting(Callback &&callback) const {
        if (!m_acceptingCallbacks.load()) {
            return false;
        }
        std::forward<Callback>(callback)();
        return true;
    }

    template <typename Callback>
    bool runWithRendererLock(Callback &&callback) const {
        QMutexLocker locker(&m_rendererMutex);
        return runIfAccepting(std::forward<Callback>(callback));
    }

    template <typename Callback>
    bool runIfCurrent(quint64 generation, Callback &&callback) const {
        if (!accepts(generation)) {
            return false;
        }
        std::forward<Callback>(callback)();
        return true;
    }

    template <typename Callback>
    bool runWithRendererStarted(Callback &&callback) const {
        QMutexLocker locker(&m_rendererMutex);
        if (!m_acceptingCallbacks.load() || !m_renderersStarted.load()) {
            return false;
        }
        std::forward<Callback>(callback)();
        return true;
    }

    template <typename Callback>
    bool runWithRendererStarted(quint64 generation, Callback &&callback) const {
        QMutexLocker locker(&m_rendererMutex);
        if (!accepts(generation) || !m_renderersStarted.load()) {
            return false;
        }
        std::forward<Callback>(callback)();
        return true;
    }

private:
    std::atomic_bool &m_acceptingCallbacks;
    std::atomic<quint64> &m_callbackGeneration;
    std::atomic_bool &m_renderersStarted;
    QRecursiveMutex &m_rendererMutex;
};
