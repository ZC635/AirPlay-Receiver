#pragma once

#include <QMetaType>

enum class ReceiverState {
    Idle,
    Starting,
    Discoverable,
    Connecting,
    Connected,
    Error,
};

Q_DECLARE_METATYPE(ReceiverState)
