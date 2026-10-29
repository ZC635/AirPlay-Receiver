# QMdnsEngine 0.2.1, with the AirPlay multi-interface fixes. See PATCHES.md.
file(GLOB QMDNS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/src/*.cpp")
file(GLOB QMDNS_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/include/qmdnsengine/*.h")
set(BUILD_SHARED_LIBS ON)
configure_file(src/qmdnsengine_export.h.in qmdnsengine_export.h)
add_library(qmdnsengine SHARED ${QMDNS_SOURCES} ${QMDNS_HEADERS})
set_target_properties(qmdnsengine PROPERTIES AUTOMOC ON)
target_compile_definitions(qmdnsengine PRIVATE QMDNSENGINE_LIBRARY)
target_compile_features(qmdnsengine PUBLIC cxx_std_17)
target_include_directories(qmdnsengine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(qmdnsengine PUBLIC Qt6::Network)
if(WIN32)
    # ws2_32: socket-level IPv4 TTL.
    target_link_libraries(qmdnsengine PRIVATE ws2_32)
endif()
