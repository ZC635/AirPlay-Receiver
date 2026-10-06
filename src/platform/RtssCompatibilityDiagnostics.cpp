#include "platform/RtssCompatibilityDiagnostics.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {
// Recovered RTSS vkCreateDevice/vkDestroyDevice diagnostics only:
// max fixed text (38) + function name (15) + NUL leaves 202 ASCII bytes.
constexpr int kKnownVulkanPathByteBudget = 202;

QString modulePath(HMODULE module, QStringList &errors) {
    for (DWORD capacity = 512; capacity <= 32768; capacity *= 2) {
        std::vector<wchar_t> buffer(capacity);
        const DWORD length = GetModuleFileNameW(module,buffer.data(),capacity);
        if (!length) {
            errors.append(QStringLiteral("module_path_error_%1").arg(GetLastError()));
            return {};
        }
        if (length < capacity) return QString::fromWCharArray(buffer.data(),int(length));
    }
    errors.append(QStringLiteral("module_path_truncated"));
    return {};
}
QString basename(const QString &path) {
    return path.mid(std::max(path.lastIndexOf('/'),path.lastIndexOf('\\')) + 1);
}
int knownAnsiBytes(const QString &path) {
    // The verified message/path evidence is ASCII. Other encodings remain unassessed.
    for (QChar character : path) if (character.unicode() > 127) return -1;
    const auto wide = path.toStdWString();
    const UINT codePage = GetACP();
    BOOL substituted = FALSE;
    const bool utf8 = codePage == CP_UTF8;
    const int bytes = WideCharToMultiByte(codePage,utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS,
        wide.data(),int(wide.size()),nullptr,0,nullptr,utf8 ? nullptr : &substituted);
    return bytes > 0 && !substituted ? bytes : -1;
}
RtssPresence rtssPresence(QStringList &errors) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        errors.append(QStringLiteral("process_snapshot_error_%1").arg(GetLastError()));
        return RtssPresence::Unknown;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot,&entry)) {
        const DWORD error = GetLastError();
        CloseHandle(snapshot);
        errors.append(QStringLiteral("process_enumeration_error_%1").arg(error));
        return RtssPresence::Unknown;
    }
    do {
        if (_wcsicmp(entry.szExeFile,L"RTSS.exe") == 0 || _wcsicmp(entry.szExeFile,L"RTSS64.exe") == 0) {
            CloseHandle(snapshot);
            return RtssPresence::Running;
        }
    } while (Process32NextW(snapshot,&entry));
    const DWORD error = GetLastError();
    CloseHandle(snapshot);
    if (error != ERROR_NO_MORE_FILES) {
        errors.append(QStringLiteral("process_enumeration_error_%1").arg(error));
        return RtssPresence::Unknown;
    }
    return RtssPresence::NotRunning;
}
std::wstring attributePath(const QString &path) {
    std::wstring value = path.toStdWString();
    if (value.rfind(L"\\\\?\\",0) == 0) return value;
    if (value.rfind(L"\\\\",0) == 0) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}
} // namespace

bool RtssCompatibilitySnapshot::shouldWarn() const {
    return rtssPresence == RtssPresence::Running && dllPathRisk == RtssDllPathRisk::KnownLengthRisk;
}

RtssDllPathRisk RtssCompatibilityDiagnostics::classifyVulkanDllPath(const QString &fullPath) {
    const bool driveAbsolute = fullPath.size() >= 3 && fullPath.at(1) == ':'
        && (fullPath.at(2) == '/' || fullPath.at(2) == '\\');
    const bool uncAbsolute = fullPath.startsWith("\\\\") || fullPath.startsWith("//");
    if ((!driveAbsolute && !uncAbsolute) || basename(fullPath).compare("vulkan-1.dll",Qt::CaseInsensitive) != 0)
        return RtssDllPathRisk::Unknown;
    const int bytes = knownAnsiBytes(fullPath);
    if (bytes < 0) return RtssDllPathRisk::Unknown;
    return bytes > kKnownVulkanPathByteBudget ? RtssDllPathRisk::KnownLengthRisk : RtssDllPathRisk::NotObserved;
}

RtssCompatibilitySnapshot RtssCompatibilityDiagnostics::inspectCurrentProcess() {
    // No application-state API, window, DLL load, process launch, or configuration write.
    RtssCompatibilitySnapshot result;
    result.rtssPresence = rtssPresence(result.detectionErrors);
    const QString executable = modulePath(nullptr,result.detectionErrors);
    result.executableFileName = basename(executable);
    bool observedPath = false;
    bool unknownPath = false;
    auto consider = [&](const QString &path,const QString &source) {
        const auto risk = classifyVulkanDllPath(path);
        observedPath = true;
        if (risk == RtssDllPathRisk::Unknown) {
            unknownPath = true;
            result.detectionErrors.append(QStringLiteral("dll_path_unassessed_%1").arg(source));
        }
        if (result.dllPathRisk != RtssDllPathRisk::KnownLengthRisk
            && (risk == RtssDllPathRisk::KnownLengthRisk || result.runtimeDllPath.isEmpty())) {
            result.runtimeDllPath = path;
            result.dllPathSource = source;
            result.ansiPathBytes = knownAnsiBytes(path);
            result.dllPathRisk = risk;
        }
    };
    // A handle query observes an already-loaded module; it never loads Vulkan.
    if (HMODULE loaded = GetModuleHandleW(L"vulkan-1.dll")) {
        const QString path = modulePath(loaded,result.detectionErrors);
        if (path.isEmpty()) unknownPath = true;
        else consider(path,QStringLiteral("loaded_module"));
    }
    if (!executable.isEmpty()) {
        const int separator = std::max(executable.lastIndexOf('/'),executable.lastIndexOf('\\'));
        const QString candidate = executable.left(separator+1) + QStringLiteral("vulkan-1.dll");
        const DWORD attributes = GetFileAttributesW(attributePath(candidate).c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) && candidate != result.runtimeDllPath)
                consider(candidate,QStringLiteral("application_local_candidate"));
        } else {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                unknownPath = true;
                result.detectionErrors.append(QStringLiteral("candidate_attributes_error_%1").arg(error));
            }
        }
    } else unknownPath = true;
    if (!observedPath) {
        unknownPath = true;
        result.detectionErrors.append(QStringLiteral("vulkan_dll_path_unresolved"));
    }
    if (unknownPath && result.dllPathRisk != RtssDllPathRisk::KnownLengthRisk)
        result.dllPathRisk = RtssDllPathRisk::Unknown;
    return result;
}