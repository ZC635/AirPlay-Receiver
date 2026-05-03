#include "backend/UxPlayReceiver.h"

#include <utility>

#if AIRPLAY_WITH_UXPLAY
// UxPlay integration is added in a later task.
#endif

UxPlayReceiver::UxPlayReceiver(UxPlayReceiverConfig config, QObject *parent)
    : AirPlayReceiver(parent), m_config(std::move(config)) {}

void UxPlayReceiver::start() {
#if AIRPLAY_WITH_UXPLAY
    setState(ReceiverState::Discoverable);
#else
    setError("UxPlay support is not enabled in this build");
    setState(ReceiverState::Error);
#endif
}

void UxPlayReceiver::stop() {
    if (m_state != ReceiverState::Idle) {
        setState(ReceiverState::Idle);
    }
}

void UxPlayReceiver::setVolume(double volume) {
    m_volume = volume;
}

ReceiverState UxPlayReceiver::state() const {
    return m_state;
}

void UxPlayReceiver::setState(ReceiverState state) {
    if (m_state == state) {
        return;
    }

    m_state = state;
    emit stateChanged(m_state);
}

void UxPlayReceiver::setError(QString error) {
    if (m_error == error) {
        return;
    }

    m_error = std::move(error);
    emit errorChanged(m_error);
}
