#include "platform/CpuCompatibilityDiagnostics.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstring>

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

DiagnosticValue<CpuEnvironmentFact> cpuCapabilitiesFromProbe(const CpuCapabilityProbe &probe) {
    if (!probe.cpuidAvailable)
        return DiagnosticValue<CpuEnvironmentFact>::unavailable();
    CpuEnvironmentFact result;
    result.vendor = DiagnosticFact::available(probe.vendorId == QStringLiteral("GenuineIntel")
        ? QStringLiteral("intel") : probe.vendorId == QStringLiteral("AuthenticAMD")
        ? QStringLiteral("amd") : QStringLiteral("other"));
    const auto add = [&result](const QString &name, bool present) {
        if (present) result.hardwareFeatures.append(name);
    };
    add(QStringLiteral("sse2"), probe.leaf1Edx & (quint32(1) << 26));
    add(QStringLiteral("sse3"), probe.leaf1Ecx & (quint32(1) << 0));
    add(QStringLiteral("ssse3"), probe.leaf1Ecx & (quint32(1) << 9));
    add(QStringLiteral("sse4_1"), probe.leaf1Ecx & (quint32(1) << 19));
    add(QStringLiteral("sse4_2"), probe.leaf1Ecx & (quint32(1) << 20));
    add(QStringLiteral("popcnt"), probe.leaf1Ecx & (quint32(1) << 23));
    add(QStringLiteral("cx16"), probe.leaf1Ecx & (quint32(1) << 13));
    add(QStringLiteral("avx"), probe.leaf1Ecx & (quint32(1) << 28));
    add(QStringLiteral("avx2"), probe.leaf7Ebx & (quint32(1) << 5));
    add(QStringLiteral("fma"), probe.leaf1Ecx & (quint32(1) << 12));
    const bool osReady = probe.xstateAvailable &&
        (probe.leaf1Ecx & (quint32(1) << 26)) && (probe.leaf1Ecx & (quint32(1) << 27)) &&
        (probe.enabledXstate & quint64(6)) == quint64(6);
    result.avxOsState = probe.xstateAvailable
        ? DiagnosticFact::available(osReady ? QStringLiteral("enabled") : QStringLiteral("disabled"))
        : DiagnosticFact::unavailable();
    const bool avxUsable = osReady && result.hardwareFeatures.contains(QStringLiteral("avx"));
    for (const QString &feature : result.hardwareFeatures) {
        if ((feature == QStringLiteral("avx") || feature == QStringLiteral("avx2") ||
             feature == QStringLiteral("fma")) && !avxUsable)
            continue;
        result.usableFeatures.append(feature);
    }
    return DiagnosticValue<CpuEnvironmentFact>::available(std::move(result));
}

DiagnosticValue<CpuEnvironmentFact> windowsCpuCompatibilityFact(QDeadlineTimer deadline) {
    if (deadline.hasExpired()) return DiagnosticValue<CpuEnvironmentFact>::timedOut();
    CpuCapabilityProbe probe;
#if (defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))) || \
    (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
    const auto leaf = [](unsigned int number, unsigned int &a, unsigned int &b,
                         unsigned int &c, unsigned int &d) {
#if defined(__GNUC__)
        return __get_cpuid_count(number, 0, &a, &b, &c, &d) != 0;
#else
        int registers[4]{};
        __cpuidex(registers, static_cast<int>(number), 0);
        a = registers[0]; b = registers[1]; c = registers[2]; d = registers[3];
        return true;
#endif
    };
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (leaf(0, a, b, c, d)) {
        probe.cpuidAvailable = true;
        const auto maximum = a;
        char vendor[12];
        std::memcpy(vendor, &b, 4);
        std::memcpy(vendor + 4, &d, 4);
        std::memcpy(vendor + 8, &c, 4);
        probe.vendorId = QString::fromLatin1(vendor, 12);
        if (maximum >= 1 && leaf(1, a, b, c, d)) {
            probe.leaf1Ecx = c;
            probe.leaf1Edx = d;
        }
        if (maximum >= 7 && leaf(7, a, b, c, d)) probe.leaf7Ebx = b;
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        const auto enabledXstate = kernel ? reinterpret_cast<DWORD64 (WINAPI *)()>(
            GetProcAddress(kernel, "GetEnabledXStateFeatures")) : nullptr;
        if (enabledXstate) {
            probe.xstateAvailable = true;
            probe.enabledXstate = enabledXstate();
        }
    }
#endif
    if (deadline.hasExpired()) return DiagnosticValue<CpuEnvironmentFact>::timedOut();
    return cpuCapabilitiesFromProbe(probe);
}
