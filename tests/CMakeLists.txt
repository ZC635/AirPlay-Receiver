add_executable(MainWindowSmokeTest app/MainWindowSmokeTest.cpp)
target_link_libraries(MainWindowSmokeTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME MainWindowSmokeTest COMMAND MainWindowSmokeTest)

add_executable(AppSettingsTest app/AppSettingsTest.cpp)
target_link_libraries(AppSettingsTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME AppSettingsTest COMMAND AppSettingsTest)

add_executable(AppSettingsStoreTest app/AppSettingsStoreTest.cpp)
target_link_libraries(AppSettingsStoreTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME AppSettingsStoreTest COMMAND AppSettingsStoreTest)

add_executable(ToolbarWidgetTest app/ToolbarWidgetTest.cpp)
target_link_libraries(ToolbarWidgetTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME ToolbarWidgetTest COMMAND ToolbarWidgetTest)

add_executable(SettingsDialogTest app/SettingsDialogTest.cpp)
target_link_libraries(SettingsDialogTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME SettingsDialogTest COMMAND SettingsDialogTest)

add_executable(HotkeyServiceTest platform/HotkeyServiceTest.cpp)
target_link_libraries(HotkeyServiceTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME HotkeyServiceTest COMMAND HotkeyServiceTest)

add_executable(DependencyDiagnosticsTest platform/DependencyDiagnosticsTest.cpp)
target_link_libraries(DependencyDiagnosticsTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME DependencyDiagnosticsTest COMMAND DependencyDiagnosticsTest)

add_executable(AirPlayReceiverTest backend/AirPlayReceiverTest.cpp)
target_link_libraries(AirPlayReceiverTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME AirPlayReceiverTest COMMAND AirPlayReceiverTest)

add_executable(UxPlayReceiverConfigTest backend/UxPlayReceiverConfigTest.cpp)
target_link_libraries(UxPlayReceiverConfigTest PRIVATE airplay_app_lib Qt6::Test)
if(AIRPLAY_WITH_UXPLAY)
    target_include_directories(UxPlayReceiverConfigTest PRIVATE "${CMAKE_SOURCE_DIR}/third_party/uxplay")
endif()
add_test(NAME UxPlayReceiverConfigTest COMMAND UxPlayReceiverConfigTest)

add_executable(VideoRendererLifecycleTest backend/VideoRendererLifecycleTest.cpp)
target_link_libraries(VideoRendererLifecycleTest PRIVATE airplay_app_lib Qt6::Test)
if(AIRPLAY_WITH_UXPLAY)
    target_include_directories(VideoRendererLifecycleTest PRIVATE "${CMAKE_SOURCE_DIR}/third_party/uxplay")
    target_link_libraries(VideoRendererLifecycleTest PRIVATE renderers)
endif()
add_test(NAME VideoRendererLifecycleTest COMMAND VideoRendererLifecycleTest)

add_executable(VideoSurfaceWidgetTest app/VideoSurfaceWidgetTest.cpp)
target_link_libraries(VideoSurfaceWidgetTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME VideoSurfaceWidgetTest COMMAND VideoSurfaceWidgetTest)

get_target_property(AIRPLAY_QT_CORE_LOCATION Qt6::Core IMPORTED_LOCATION_RELEASE)
if(NOT AIRPLAY_QT_CORE_LOCATION)
    get_target_property(AIRPLAY_QT_CORE_LOCATION Qt6::Core IMPORTED_LOCATION)
endif()
get_filename_component(AIRPLAY_QT_BIN_DIR "${AIRPLAY_QT_CORE_LOCATION}" DIRECTORY)
execute_process(
    COMMAND "${Qt6Core_QMAKE_EXECUTABLE}" -query QT_INSTALL_PLUGINS
    OUTPUT_VARIABLE AIRPLAY_QT_PLUGIN_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(AIRPLAY_QT_TESTS
    MainWindowSmokeTest
    AppSettingsTest
    AppSettingsStoreTest
    ToolbarWidgetTest
    SettingsDialogTest
    HotkeyServiceTest
    DependencyDiagnosticsTest
    AirPlayReceiverTest
    UxPlayReceiverConfigTest
    VideoRendererLifecycleTest
    VideoSurfaceWidgetTest
)
foreach(AIRPLAY_QT_TEST IN LISTS AIRPLAY_QT_TESTS)
    set_tests_properties(${AIRPLAY_QT_TEST} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${AIRPLAY_QT_PLUGIN_DIR}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AIRPLAY_QT_BIN_DIR}"
    )
endforeach()
