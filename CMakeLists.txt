cmake_minimum_required(VERSION 3.24)
project(AirPlayReceiver VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project requires Windows")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Test)

include(cmake/UxPlayDependency.cmake)

add_library(airplay_app_lib
    src/app/AppSettings.cpp
    src/app/AppSettings.h
    src/app/AppSettingsStore.cpp
    src/app/AppSettingsStore.h
    src/app/MainWindow.cpp
    src/app/MainWindow.h
    src/app/SettingsDialog.cpp
    src/app/SettingsDialog.h
    src/app/ShortcutAction.h
    src/app/ShortcutBinding.h
    src/app/ToolbarWidget.cpp
    src/app/ToolbarWidget.h
    src/app/VideoRenderGeometry.cpp
    src/app/VideoRenderGeometry.h
    src/app/VideoSurfaceWidget.cpp
    src/app/VideoSurfaceWidget.h
    src/backend/AirPlayReceiver.h
    src/backend/FakeAirPlayReceiver.h
    src/backend/ReceiverState.h
    src/backend/UxPlayReceiver.cpp
    src/backend/UxPlayReceiver.h
    src/backend/VideoFrameBridge.cpp
    src/backend/VideoFrameBridge.h
    src/platform/DependencyDiagnostics.cpp
    src/platform/DependencyDiagnostics.h
    src/platform/AspectRatioSizing.cpp
    src/platform/AspectRatioSizing.h
    src/platform/FakeHotkeyService.h
    src/platform/HotkeyService.h
    src/platform/MdnsPublisher.cpp
    src/platform/MdnsPublisher.h
    src/platform/WindowsHotkeyService.cpp
    src/platform/WindowsHotkeyService.h
)
target_include_directories(airplay_app_lib PUBLIC src)
target_compile_definitions(airplay_app_lib PUBLIC AIRPLAY_WITH_UXPLAY=$<BOOL:${AIRPLAY_WITH_UXPLAY}>)
target_link_libraries(airplay_app_lib PUBLIC Qt6::Widgets)
if(AIRPLAY_WITH_UXPLAY)
    target_link_libraries(airplay_app_lib PRIVATE uxplay_dependency)
    if(TARGET qmdnsengine)
        get_target_property(_qmdns_inc qmdnsengine INTERFACE_INCLUDE_DIRECTORIES)
        if(_qmdns_inc)
            target_include_directories(airplay_app_lib PRIVATE ${_qmdns_inc})
        endif()
    endif()
endif()
if(WIN32)
    target_link_libraries(airplay_app_lib PRIVATE user32 dwmapi)
endif()

add_executable(airplay_receiver src/app/main.cpp)
target_link_libraries(airplay_receiver PRIVATE airplay_app_lib)
set_target_properties(airplay_receiver PROPERTIES WIN32_EXECUTABLE TRUE)

enable_testing()
add_subdirectory(tests)
