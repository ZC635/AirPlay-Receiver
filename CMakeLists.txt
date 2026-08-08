cmake_minimum_required(VERSION 3.24)
project(AirPlayReceiver VERSION 0.1.0 LANGUAGES CXX RC)

set(AIRPLAY_BUILD_ID "" CACHE STRING "Optional build identity for diagnostic logs")

function(airplay_escape_cpp_string output input)
    set(escaped "${input}")
    string(REPLACE "\\" "\\\\" escaped "${escaped}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REPLACE "\r" "\\r" escaped "${escaped}")
    string(REPLACE "\n" "\\n" escaped "${escaped}")
    string(REPLACE "\t" "\\t" escaped "${escaped}")
    set(${output} "${escaped}" PARENT_SCOPE)
endfunction()

airplay_escape_cpp_string(_airplay_version_cpp "${PROJECT_VERSION}")
airplay_escape_cpp_string(_airplay_build_id_cpp "${AIRPLAY_BUILD_ID}")
# Keep identity strings out of compiler command lines, which can split or warn on cache values.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/app")
file(CONFIGURE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/app/BuildIdentity.h"
    CONTENT "#pragma once\n\nnamespace AirPlayBuildIdentity {\ninline constexpr char16_t version[] = u\"@_airplay_version_cpp@\";\ninline constexpr char16_t buildId[] = u\"@_airplay_build_id_cpp@\";\n}\n"
    @ONLY
    NEWLINE_STYLE LF
)

if(NOT WIN32)
    message(FATAL_ERROR "This project requires Windows")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Test Network)

include(cmake/UxPlayDependency.cmake)

add_library(airplay_app_lib
    src/diagnostics/DiagnosticEvent.h
    src/diagnostics/DiagnosticLogSink.cpp
    src/diagnostics/DiagnosticLogSink.h
    src/diagnostics/DiagnosticSanitizer.h
    src/diagnostics/DiagnosticSanitizer.cpp
    src/diagnostics/DiagnosticSession.h
    src/diagnostics/DiagnosticSession.cpp
    src/diagnostics/QtDiagnosticMessageBridge.h
    src/diagnostics/QtDiagnosticMessageBridge.cpp
    src/diagnostics/ThirdPartyDiagnosticTranslator.h
    src/diagnostics/ThirdPartyDiagnosticTranslator.cpp
    src/app/AppSettings.cpp
    src/app/DiagnosticActivation.cpp
    src/app/DiagnosticActivation.h
    src/app/AppSettings.h
    src/app/AppSettingsStore.cpp
    src/app/AppSettingsStore.h
    src/app/SettingsApplyTypes.cpp
    src/app/SettingsApplyTypes.h
    src/app/SettingsApplyCoordinator.cpp
    src/app/SettingsApplyCoordinator.h
    src/app/MainWindow.cpp
    src/app/MainWindow.h
    src/app/RecordingStartupCleanup.cpp
    src/app/RecordingStartupCleanup.h
    src/app/SettingsChangeDeferrer.cpp
    src/app/SettingsChangeDeferrer.h
    src/app/SettingsDialog.cpp
    src/app/SettingsDialog.h
    src/app/ShortcutAction.h
    src/app/ShortcutActionKey.h
    src/app/ShortcutActionKey.cpp
    src/app/ShortcutBinding.h
    src/app/ToolbarWidget.cpp
    src/app/ToolbarWidget.h
    src/app/app_resources.qrc
    src/app/VideoRenderGeometry.cpp
    src/app/VideoRenderGeometry.h
    src/app/VideoSurfaceWidget.cpp
    src/app/VideoSurfaceWidget.h
    src/app/WindowStateStore.cpp
    src/app/WindowStateStore.h
    src/backend/AirPlayReceiver.h
    src/backend/RecordingTypes.h
    src/backend/DiscoveryRestartController.cpp
    src/backend/DiscoveryRestartController.h
    src/backend/FakeAirPlayReceiver.h
    src/backend/ReceiverConfigurationChange.cpp
    src/backend/ReceiverConfigurationChange.h
    src/backend/ReceiverState.h
    src/backend/ReceiverStatePolicy.h
    src/backend/ReceiverStatePolicy.cpp
    src/backend/RecordingFileTransaction.cpp
    src/backend/RecordingFileTransaction.h
    src/backend/RecordingTimeline.cpp
    src/backend/RecordingTimeline.h
    src/backend/UxPlayCallbackDispatch.cpp
    src/backend/UxPlayCallbackDispatch.h
    src/backend/UxPlayDiscovery.cpp
    src/backend/UxPlayDiscovery.h
    src/backend/UxPlayReceiver.cpp
    src/backend/UxPlayReceiver.h
    src/backend/GstAppSinkFrameSource.cpp
    src/backend/GstAppSinkFrameSource.h
    src/backend/VideoFrameBridge.cpp
    src/backend/VideoFrameBridge.h
    src/backend/VideoQualitySettings.h
    src/platform/DependencyDiagnostics.cpp
    src/platform/DependencyDiagnostics.h
    src/platform/EnvironmentDiagnostics.cpp
    src/platform/EnvironmentDiagnostics.h
    src/platform/WindowsEnvironmentDiagnostics.cpp
    src/platform/WindowsEnvironmentDiagnostics.h
    src/platform/AspectRatioSizing.cpp
    src/platform/AspectRatioSizing.h
    src/platform/FakeHotkeyService.h
    src/platform/HotkeyService.h
    src/platform/MdnsPublishing.h
    src/platform/MdnsPublisher.cpp
    src/platform/MdnsPublisher.h
    src/platform/RecordingPathActions.cpp
    src/platform/RecordingPathActions.h
    src/platform/WindowsWindowBehavior.cpp
    src/platform/WindowsWindowBehavior.h
    src/platform/WindowsHotkeyService.cpp
    src/platform/WindowsHotkeyService.h
)
target_include_directories(airplay_app_lib PUBLIC
    src
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_compile_definitions(airplay_app_lib PUBLIC
    AIRPLAY_WITH_UXPLAY=$<BOOL:${AIRPLAY_WITH_UXPLAY}>
)
target_link_libraries(airplay_app_lib PUBLIC Qt6::Widgets Qt6::Network)
if(AIRPLAY_WITH_UXPLAY)
    target_sources(airplay_app_lib PRIVATE
        src/backend/RecordingController.cpp
        src/backend/RecordingController.h
        src/backend/GstRecordingPipeline.cpp
        src/backend/GstRecordingPipeline.h
        src/backend/GstSampleQueue.cpp
        src/backend/GstSampleQueue.h
    )
    target_link_libraries(airplay_app_lib PUBLIC uxplay_dependency)
    if(TARGET qmdnsengine)
        get_target_property(_qmdns_inc qmdnsengine INTERFACE_INCLUDE_DIRECTORIES)
        if(_qmdns_inc)
            target_include_directories(airplay_app_lib PRIVATE ${_qmdns_inc})
        endif()
    endif()
endif()
if(WIN32)
    target_link_libraries(airplay_app_lib PRIVATE user32 dwmapi iphlpapi advapi32)
endif()

add_executable(airplay_receiver src/app/main.cpp src/app/app-icon.rc)
target_link_libraries(airplay_receiver PRIVATE airplay_app_lib)
set_target_properties(airplay_receiver PROPERTIES WIN32_EXECUTABLE TRUE)

configure_file(
    "${PROJECT_SOURCE_DIR}/config/portable-runtime-manifest.txt"
    "${PROJECT_BINARY_DIR}/config/portable-runtime-manifest.txt"
    COPYONLY
)

enable_testing()
add_subdirectory(tests)
