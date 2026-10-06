#include <QtTest/QtTest>
#include <QFileInfo>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "platform/RtssCompatibilityDiagnostics.h"

class RtssCompatibilityDiagnosticsTest final : public QObject {
    Q_OBJECT
private slots:
    // An off-by-one DLL byte budget must fail these literal boundary cases.
    void knownVulkanMessageBoundary_data() {
        QTest::addColumn<int>("padding");
        QTest::addColumn<int>("expectedLength");
        QTest::addColumn<int>("expectedRisk");
        QTest::newRow("202 fits both recovered messages") << 186 << 202 << int(RtssDllPathRisk::NotObserved);
        QTest::newRow("203 exceeds vkDestroyDevice message") << 187 << 203 << int(RtssDllPathRisk::KnownLengthRisk);
        QTest::newRow("204 exceeds known message") << 188 << 204 << int(RtssDllPathRisk::KnownLengthRisk);
        QTest::newRow("214 original crash length") << 198 << 214 << int(RtssDllPathRisk::KnownLengthRisk);
    }
    void knownVulkanMessageBoundary() {
        QFETCH(int,padding); QFETCH(int,expectedLength); QFETCH(int,expectedRisk);
        const QString path = "C:/" + QString(padding,'a') + "/vulkan-1.dll";
        QCOMPARE(path.size(),expectedLength);
        QCOMPARE(int(RtssCompatibilityDiagnostics::classifyVulkanDllPath(path)),expectedRisk);
    }
    void unresolvedOrUnrelatedDllIsUnknown() {
        for (const auto &path : {QString(),QString("vulkan-1.dll"),QString::fromUtf8(u8"C:/中文/vulkan-1.dll"),QString("C:/" + QString(220,'a') + "/other.dll")})
            QCOMPARE(RtssCompatibilityDiagnostics::classifyVulkanDllPath(path),RtssDllPathRisk::Unknown);
    }
    // A wrong RTSS presence/unknown branch must not invent absence or show a confirmed-risk notice.
    void presenceAndRiskDecision_data() {
        QTest::addColumn<int>("presence"); QTest::addColumn<int>("risk"); QTest::addColumn<bool>("warn");
        QTest::newRow("running and risk") << int(RtssPresence::Running) << int(RtssDllPathRisk::KnownLengthRisk) << true;
        QTest::newRow("running and short") << int(RtssPresence::Running) << int(RtssDllPathRisk::NotObserved) << false;
        QTest::newRow("running unresolved DLL") << int(RtssPresence::Running) << int(RtssDllPathRisk::Unknown) << false;
        QTest::newRow("unknown RTSS") << int(RtssPresence::Unknown) << int(RtssDllPathRisk::KnownLengthRisk) << false;
        QTest::newRow("RTSS not running") << int(RtssPresence::NotRunning) << int(RtssDllPathRisk::KnownLengthRisk) << false;
    }
    void presenceAndRiskDecision() {
        QFETCH(int,presence); QFETCH(int,risk); QFETCH(bool,warn);
        RtssCompatibilitySnapshot result;
        result.rtssPresence = RtssPresence(presence); result.dllPathRisk = RtssDllPathRisk(risk);
        QCOMPARE(result.shouldWarn(),warn);
        QCOMPARE(int(result.rtssPresence),presence);
        QCOMPARE(int(result.dllPathRisk),risk);
    }
    void nativeSnapshotUsesActualExeWithoutLoadingVulkan() {
        const HMODULE vulkanBefore = GetModuleHandleW(L"vulkan-1.dll");
        const HMODULE rtssBefore = GetModuleHandleW(L"RTSSHooks64.dll");
        const auto before = RtssCompatibilityDiagnostics::inspectCurrentProcess();
        QCOMPARE(GetModuleHandleW(L"vulkan-1.dll"),vulkanBefore);
        QCOMPARE(GetModuleHandleW(L"RTSSHooks64.dll"),rtssBefore);
        QCOMPARE(before.executableFileName,QFileInfo(QCoreApplication::applicationFilePath()).fileName());
        QVERIFY(before.rtssPresence != RtssPresence::Unknown);
        if (before.runtimeDllPath.isEmpty()) {
            QCOMPARE(before.dllPathRisk,RtssDllPathRisk::Unknown);
            QVERIFY(!before.detectionErrors.isEmpty());
        }
    }
};
QTEST_GUILESS_MAIN(RtssCompatibilityDiagnosticsTest)
#include "RtssCompatibilityDiagnosticsTest.moc"