set(AIRPLAY_WITH_UXPLAY OFF CACHE BOOL "Build with vendored UxPlay receiver")

if(AIRPLAY_WITH_UXPLAY)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/uxplay/CMakeLists.txt")
        message(FATAL_ERROR "AIRPLAY_WITH_UXPLAY requires third_party/uxplay")
    endif()
    find_package(Qt6 REQUIRED COMPONENTS Network)
    find_package(QMdnsEngine REQUIRED)
    add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/uxplay" "${CMAKE_CURRENT_BINARY_DIR}/third_party/uxplay" EXCLUDE_FROM_ALL)
    add_library(uxplay_dependency INTERFACE)
    target_include_directories(uxplay_dependency INTERFACE
        "${PROJECT_SOURCE_DIR}/third_party/uxplay")
    target_link_libraries(uxplay_dependency INTERFACE renderers airplay qmdnsengine)
endif()
