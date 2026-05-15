#include "backend/UxPlayCallbackDispatch.h"

UxPlayCallbackDispatch::UxPlayCallbackDispatch(std::atomic_bool &acceptingCallbacks,
                                               std::atomic<quint64> &callbackGeneration,
                                               std::atomic_bool &renderersStarted,
                                               QRecursiveMutex &rendererMutex)
    : m_acceptingCallbacks(acceptingCallbacks),
      m_callbackGeneration(callbackGeneration),
      m_renderersStarted(renderersStarted),
      m_rendererMutex(rendererMutex) {
}

quint64 UxPlayCallbackDispatch::currentGeneration() const {
    return m_callbackGeneration.load();
}

bool UxPlayCallbackDispatch::accepts(quint64 generation) const {
    return m_acceptingCallbacks.load() && generation == m_callbackGeneration.load();
}
