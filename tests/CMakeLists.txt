add_library(airplay_test_support INTERFACE)
target_include_directories(airplay_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(airplay_test_support INTERFACE airplay_app_lib)

add_executable(DiagnosticLogSinkTest diagnostics/DiagnosticLogSinkTest.cpp)
target_link_libraries(DiagnosticLogSinkTest PRIVATE airplay_test_support Qt6::Test)
add_test(NAME DiagnosticLogSinkTest COMMAND DiagnosticLogSinkTest)

add_executable(SettingsChangeDeferrerTest app/SettingsChangeDeferrerTest.cpp)
target_link_libraries(SettingsChangeDeferrerTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME SettingsChangeDeferrerTest COMMAND SettingsChangeDeferrerTest)

add_executable(SettingsApplyTypesTest app/SettingsApplyTypesTest.cpp)
target_link_libraries(SettingsApplyTypesTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME SettingsApplyTypesTest COMMAND SettingsApplyTypesTest)

add_executable(SettingsApplyCoordinatorTest app/SettingsApplyCoordinatorTest.cpp)
target_link_libraries(SettingsApplyCoordinatorTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME SettingsApplyCoordinatorTest COMMAND SettingsApplyCoordinatorTest)

add_executable(MainWindowSmokeTest app/MainWindowSmokeTest.cpp)
target_link_libraries(MainWindowSmokeTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME MainWindowSmokeTest COMMAND MainWindowSmokeTest)

add_executable(RecordingStartupCleanupTest app/RecordingStartupCleanupTest.cpp)
target_link_libraries(RecordingStartupCleanupTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME RecordingStartupCleanupTest COMMAND RecordingStartupCleanupTest)

set(AIRPLAY_ENABLE_WINDOWS_QPA_TESTS_DEFAULT ON)
if(DEFINED ENV{CI})
    set(AIRPLAY_ENABLE_WINDOWS_QPA_TESTS_DEFAULT OFF)
endif()
option(AIRPLAY_ENABLE_WINDOWS_QPA_TESTS "Enable tests that require the Windows QPA platform" ${AIRPLAY_ENABLE_WINDOWS_QPA_TESTS_DEFAULT})

if(WIN32 AND AIRPLAY_ENABLE_WINDOWS_QPA_TESTS)
    add_test(NAME MainWindowTopmostFlickerTest COMMAND MainWindowSmokeTest visibleAlwaysOnTopToggleDoesNotHideOrShowWindow)
    add_test(NAME MainWindowNativeAspectSizingTest COMMAND MainWindowSmokeTest nativeAspectSizingAdjustsPendingRightEdgeRect)
    add_test(NAME MainWindowNativeLeftAspectSizingTest COMMAND MainWindowSmokeTest nativeAspectSizingAdjustsPendingLeftEdgeRect)
    add_test(NAME MainWindowSmokeTestNativeOverlayStacking COMMAND MainWindowSmokeTest videoSurfaceDoesNotCoverVisibleOverlays)
endif()

add_executable(AppSettingsTest app/AppSettingsTest.cpp)
target_link_libraries(AppSettingsTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME AppSettingsTest COMMAND AppSettingsTest)

add_executable(AppSettingsStoreTest app/AppSettingsStoreTest.cpp)
target_link_libraries(AppSettingsStoreTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME AppSettingsStoreTest COMMAND AppSettingsStoreTest)

add_executable(WindowStateStoreTest app/WindowStateStoreTest.cpp)
target_link_libraries(WindowStateStoreTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME WindowStateStoreTest COMMAND WindowStateStoreTest)

add_executable(ShortcutActionKeyTest app/ShortcutActionKeyTest.cpp)
target_link_libraries(ShortcutActionKeyTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME ShortcutActionKeyTest COMMAND ShortcutActionKeyTest)

add_executable(ToolbarWidgetTest app/ToolbarWidgetTest.cpp)
target_link_libraries(ToolbarWidgetTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME ToolbarWidgetTest COMMAND ToolbarWidgetTest)

add_executable(SettingsDialogTest app/SettingsDialogTest.cpp)
target_link_libraries(SettingsDialogTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME SettingsDialogTest COMMAND SettingsDialogTest)

add_executable(HotkeyServiceTest platform/HotkeyServiceTest.cpp)
target_link_libraries(HotkeyServiceTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME HotkeyServiceTest COMMAND HotkeyServiceTest)

add_executable(RecordingPathActionsTest platform/RecordingPathActionsTest.cpp)
target_link_libraries(RecordingPathActionsTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME RecordingPathActionsTest COMMAND RecordingPathActionsTest)

add_executable(AspectRatioSizingTest platform/AspectRatioSizingTest.cpp)
target_link_libraries(AspectRatioSizingTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME AspectRatioSizingTest COMMAND AspectRatioSizingTest)

add_executable(WindowsWindowBehaviorTest platform/WindowsWindowBehaviorTest.cpp)
target_link_libraries(WindowsWindowBehaviorTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME WindowsWindowBehaviorTest COMMAND WindowsWindowBehaviorTest)

add_executable(DependencyDiagnosticsTest platform/DependencyDiagnosticsTest.cpp)
target_link_libraries(DependencyDiagnosticsTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME DependencyDiagnosticsTest COMMAND DependencyDiagnosticsTest)

add_executable(MdnsPublisherTest platform/MdnsPublisherTest.cpp)
target_link_libraries(MdnsPublisherTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME MdnsPublisherTest COMMAND MdnsPublisherTest)

add_executable(DiscoveryRestartControllerTest backend/DiscoveryRestartControllerTest.cpp)
target_link_libraries(DiscoveryRestartControllerTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME DiscoveryRestartControllerTest COMMAND DiscoveryRestartControllerTest)

add_executable(ReceiverStatePolicyTest backend/ReceiverStatePolicyTest.cpp)
target_link_libraries(ReceiverStatePolicyTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME ReceiverStatePolicyTest COMMAND ReceiverStatePolicyTest)

add_executable(ReceiverConfigurationChangeTest backend/ReceiverConfigurationChangeTest.cpp)
target_link_libraries(ReceiverConfigurationChangeTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME ReceiverConfigurationChangeTest COMMAND ReceiverConfigurationChangeTest)

add_executable(AirPlayReceiverTest backend/AirPlayReceiverTest.cpp)
target_link_libraries(AirPlayReceiverTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME AirPlayReceiverTest COMMAND AirPlayReceiverTest)

add_executable(RecordingFileTransactionTest backend/RecordingFileTransactionTest.cpp)
target_link_libraries(RecordingFileTransactionTest PRIVATE airplay_app_lib Qt6::Test)
if(WIN32)
    target_link_libraries(RecordingFileTransactionTest PRIVATE advapi32)
endif()
add_test(NAME RecordingFileTransactionTest COMMAND RecordingFileTransactionTest)

add_executable(RecordingTimelineTest backend/RecordingTimelineTest.cpp)
target_link_libraries(RecordingTimelineTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME RecordingTimelineTest COMMAND RecordingTimelineTest)

add_executable(UxPlayReceiverConfigTest backend/UxPlayReceiverConfigTest.cpp)
target_link_libraries(UxPlayReceiverConfigTest PRIVATE airplay_app_lib Qt6::Test)
if(AIRPLAY_WITH_UXPLAY)
    target_include_directories(UxPlayReceiverConfigTest PRIVATE "${PROJECT_SOURCE_DIR}/third_party/uxplay")
    target_link_libraries(UxPlayReceiverConfigTest PRIVATE renderers)
endif()
add_test(NAME UxPlayReceiverConfigTest COMMAND UxPlayReceiverConfigTest)

add_executable(UxPlayDiscoveryTest backend/UxPlayDiscoveryTest.cpp)
target_link_libraries(UxPlayDiscoveryTest PRIVATE airplay_app_lib Qt6::Test)
if(AIRPLAY_WITH_UXPLAY)
    target_include_directories(UxPlayDiscoveryTest PRIVATE "${PROJECT_SOURCE_DIR}/third_party/uxplay")
    target_link_libraries(UxPlayDiscoveryTest PRIVATE renderers)
endif()
add_test(NAME UxPlayDiscoveryTest COMMAND UxPlayDiscoveryTest)

add_executable(VideoRendererLifecycleTest backend/VideoRendererLifecycleTest.cpp)
target_link_libraries(VideoRendererLifecycleTest PRIVATE airplay_app_lib Qt6::Test)
if(AIRPLAY_WITH_UXPLAY)
    target_include_directories(VideoRendererLifecycleTest PRIVATE "${PROJECT_SOURCE_DIR}/third_party/uxplay")
    target_link_libraries(VideoRendererLifecycleTest PRIVATE renderers)
endif()
add_test(NAME VideoRendererLifecycleTest COMMAND VideoRendererLifecycleTest)

add_executable(VideoFrameBridgeTest backend/VideoFrameBridgeTest.cpp)
target_link_libraries(VideoFrameBridgeTest PRIVATE airplay_app_lib Qt6::Test)
if(AIRPLAY_WITH_UXPLAY)
    target_include_directories(VideoFrameBridgeTest PRIVATE "${PROJECT_SOURCE_DIR}/third_party/uxplay")
    target_link_libraries(VideoFrameBridgeTest PRIVATE renderers)
endif()
add_test(NAME VideoFrameBridgeTest COMMAND VideoFrameBridgeTest)

if(AIRPLAY_WITH_UXPLAY)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GST_PBUTILS REQUIRED IMPORTED_TARGET gstreamer-pbutils-1.0)
    add_executable(GstSampleQueueTest backend/GstSampleQueueTest.cpp)
    target_link_libraries(GstSampleQueueTest PRIVATE
        airplay_app_lib
        Qt6::Test
        renderers
    )
    add_test(NAME GstSampleQueueTest COMMAND GstSampleQueueTest)

    add_executable(RecordingMuxFeasibilityTest backend/RecordingMuxFeasibilityTest.cpp)
    target_link_libraries(RecordingMuxFeasibilityTest PRIVATE
        airplay_app_lib
        Qt6::Test
        renderers
        PkgConfig::GST_PBUTILS
    )
    add_test(NAME RecordingMuxFeasibilityTest COMMAND RecordingMuxFeasibilityTest)

    add_executable(GstRecordingPipelineTest backend/GstRecordingPipelineTest.cpp)
    target_link_libraries(GstRecordingPipelineTest PRIVATE
        airplay_app_lib
        Qt6::Test
        renderers
        PkgConfig::GST_PBUTILS
    )
    add_test(NAME GstRecordingPipelineTest COMMAND GstRecordingPipelineTest)

    add_executable(RecordingControllerTest backend/RecordingControllerTest.cpp)
    target_link_libraries(RecordingControllerTest PRIVATE
        airplay_app_lib
        Qt6::Test
        renderers
        PkgConfig::GST_PBUTILS
    )
    # Keep CMake as the launcher: this has been the stable Windows boundary
    # across loader and antivirus timing variations while preserving CTest env.
    add_test(NAME RecordingControllerTest
        COMMAND ${CMAKE_COMMAND} -E env $<TARGET_FILE:RecordingControllerTest>)

    add_executable(RecordingSyntheticMp4Test backend/RecordingSyntheticMp4Test.cpp)
    target_link_libraries(RecordingSyntheticMp4Test PRIVATE
        airplay_app_lib
        Qt6::Test
        renderers
        PkgConfig::GST_PBUTILS
    )
    # QtTest's console text output is not captured by CTest on Windows.  One
    # CTest invocation runs this named test once, so this target-local file has
    # no concurrent writer and preserves failure diagnostics.
    add_test(NAME RecordingSyntheticMp4Test
        COMMAND ${CMAKE_COMMAND} -E env $<TARGET_FILE:RecordingSyntheticMp4Test>
            -o ${CMAKE_CURRENT_BINARY_DIR}/RecordingSyntheticMp4Test-results.txt,txt)
    get_filename_component(AIRPLAY_GSTREAMER_BIN_DIR "${PKG_CONFIG_EXECUTABLE}" DIRECTORY)
endif()

add_executable(VideoSurfaceWidgetTest app/VideoSurfaceWidgetTest.cpp)
target_link_libraries(VideoSurfaceWidgetTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME VideoSurfaceWidgetTest COMMAND VideoSurfaceWidgetTest)

add_executable(VideoRenderGeometryTest app/VideoRenderGeometryTest.cpp)
target_link_libraries(VideoRenderGeometryTest PRIVATE airplay_app_lib Qt6::Test)
add_test(NAME VideoRenderGeometryTest COMMAND VideoRenderGeometryTest)

find_program(AIRPLAY_POWERSHELL NAMES powershell pwsh)
if(AIRPLAY_POWERSHELL)
    add_test(
        NAME PortablePackageValidationTest
        COMMAND ${AIRPLAY_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/PortablePackageValidationTest.ps1"
            -ProjectRoot "${PROJECT_SOURCE_DIR}"
    )
    add_test(
        NAME RunScriptTest
        COMMAND ${AIRPLAY_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/RunScriptTest.ps1"
            -ProjectRoot "${PROJECT_SOURCE_DIR}"
    )
endif()


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
    DiagnosticLogSinkTest
    SettingsChangeDeferrerTest
    SettingsApplyTypesTest
    SettingsApplyCoordinatorTest
    MainWindowSmokeTest
    RecordingStartupCleanupTest
    AppSettingsTest
    AppSettingsStoreTest
    WindowStateStoreTest
    ShortcutActionKeyTest
    ToolbarWidgetTest
    SettingsDialogTest
    HotkeyServiceTest
    RecordingPathActionsTest
    AspectRatioSizingTest
    WindowsWindowBehaviorTest
    DependencyDiagnosticsTest
    MdnsPublisherTest
    DiscoveryRestartControllerTest
    ReceiverStatePolicyTest
    ReceiverConfigurationChangeTest
    AirPlayReceiverTest
    RecordingFileTransactionTest
    RecordingTimelineTest
    UxPlayReceiverConfigTest
    UxPlayDiscoveryTest
    VideoRendererLifecycleTest
    VideoSurfaceWidgetTest
    VideoFrameBridgeTest
    VideoRenderGeometryTest
)
if(AIRPLAY_WITH_UXPLAY)
    list(APPEND AIRPLAY_QT_TESTS GstSampleQueueTest RecordingMuxFeasibilityTest GstRecordingPipelineTest RecordingControllerTest RecordingSyntheticMp4Test)
endif()
foreach(AIRPLAY_QT_TEST IN LISTS AIRPLAY_QT_TESTS)
    set_tests_properties(${AIRPLAY_QT_TEST} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${AIRPLAY_QT_PLUGIN_DIR}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AIRPLAY_QT_BIN_DIR}"
    )
endforeach()

if(AIRPLAY_WITH_UXPLAY)
    set_tests_properties(GstSampleQueueTest RecordingMuxFeasibilityTest GstRecordingPipelineTest RecordingControllerTest RecordingSyntheticMp4Test PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${AIRPLAY_QT_PLUGIN_DIR}"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${AIRPLAY_QT_BIN_DIR};PATH=path_list_prepend:${AIRPLAY_GSTREAMER_BIN_DIR}"
        TIMEOUT 30
    )
    set_tests_properties(RecordingControllerTest PROPERTIES TIMEOUT 90)
    set_tests_properties(RecordingSyntheticMp4Test PROPERTIES TIMEOUT 180)
endif()

if(WIN32 AND AIRPLAY_ENABLE_WINDOWS_QPA_TESTS)
    set_tests_properties(MainWindowTopmostFlickerTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=${AIRPLAY_QT_PLUGIN_DIR}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AIRPLAY_QT_BIN_DIR}"
    )
    set_tests_properties(MainWindowNativeAspectSizingTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=${AIRPLAY_QT_PLUGIN_DIR}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AIRPLAY_QT_BIN_DIR}"
    )
    set_tests_properties(MainWindowNativeLeftAspectSizingTest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=${AIRPLAY_QT_PLUGIN_DIR}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AIRPLAY_QT_BIN_DIR}"
    )
    set_tests_properties(MainWindowSmokeTestNativeOverlayStacking PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=${AIRPLAY_QT_PLUGIN_DIR}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AIRPLAY_QT_BIN_DIR}"
    )
endif()
