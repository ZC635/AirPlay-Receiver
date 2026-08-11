#pragma once

#include "diagnostics/DiagnosticEvent.h"

class DiagnosticLogSink {
public:
    virtual ~DiagnosticLogSink() = default;

    virtual void record(DiagnosticEvent event) = 0;
    virtual bool isActive() const { return false; }
};

DiagnosticLogSink &nullDiagnosticLogSink();
