#pragma once

#include "platform/EnvironmentDiagnostics.h"

// Only CPUID vendor and feature bits are read. No brand, serial or APIC ID.
struct CpuCapabilityProbe {
    bool cpuidAvailable = false;
    QString vendorId;
    quint32 leaf1Ecx = 0;
    quint32 leaf1Edx = 0;
    quint32 leaf7Ebx = 0;
    bool xstateAvailable = false;
    quint64 enabledXstate = 0;
};

DiagnosticValue<CpuEnvironmentFact> cpuCapabilitiesFromProbe(const CpuCapabilityProbe &probe);
DiagnosticValue<CpuEnvironmentFact> windowsCpuCompatibilityFact(QDeadlineTimer deadline);
