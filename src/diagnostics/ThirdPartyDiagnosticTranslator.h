#pragma once

#include "diagnostics/DiagnosticEvent.h"

#include <QByteArrayView>

#include <optional>

class ThirdPartyDiagnosticTranslator {
public:
    static std::optional<DiagnosticEvent> translate(int upstreamLevel, QByteArrayView message);
};
