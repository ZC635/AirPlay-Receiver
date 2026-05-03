cmake_minimum_required(VERSION 3.24)
project(AirPlayReceiver VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Test)

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
    src/backend/AirPlayReceiver.h
    src/backend/FakeAirPlayReceiver.h
    src/backend/ReceiverState.h
    src/platform/FakeHotkeyService.h
    src/platform/HotkeyService.h
    src/platform/WindowsHotkeyService.cpp
    src/platform/WindowsHotkeyService.h
)
target_include_directories(airplay_app_lib PUBLIC src)
target_link_libraries(airplay_app_lib PUBLIC Qt6::Widgets)
if(WIN32)
    target_link_libraries(airplay_app_lib PUBLIC user32)
endif()

add_executable(airplay_receiver src/app/main.cpp)
target_link_libraries(airplay_receiver PRIVATE airplay_app_lib)

enable_testing()
add_subdirectory(tests)
