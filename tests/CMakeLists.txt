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
