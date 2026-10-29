# This suite runs real DNS code with an in-memory interface/socket boundary.
cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(MdnsNetworkRegression LANGUAGES CXX)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Core Network)
endif()
set(MDNS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
file(GLOB MDNS_SOURCES CONFIGURE_DEPENDS "${MDNS_ROOT}/third_party/qmdnsengine/src/src/*.cpp")
file(GLOB MDNS_HEADERS CONFIGURE_DEPENDS "${MDNS_ROOT}/third_party/qmdnsengine/src/include/qmdnsengine/*.h")
set(BUILD_SHARED_LIBS OFF)
configure_file("${MDNS_ROOT}/third_party/qmdnsengine/src/qmdnsengine_export.h.in" qmdnsengine_export.h)
add_library(qmdnsengine_sim STATIC ${MDNS_SOURCES} ${MDNS_HEADERS})
target_compile_definitions(qmdnsengine_sim PRIVATE SIMULATE_QMDNS_NETWORK)
if(MSVC)
    target_compile_options(qmdnsengine_sim PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/simulated_network.h")
else()
    target_compile_options(qmdnsengine_sim PRIVATE "-include${CMAKE_CURRENT_SOURCE_DIR}/simulated_network.h")
endif()
target_include_directories(qmdnsengine_sim PUBLIC
    "${MDNS_ROOT}/third_party/qmdnsengine/src/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(qmdnsengine_sim PUBLIC Qt6::Network)
target_compile_features(qmdnsengine_sim PUBLIC cxx_std_17)
set_target_properties(qmdnsengine_sim PROPERTIES AUTOMOC ON)
add_executable(MdnsNetworkTest DiscoveryNetworkTest.cpp simulated_network.cpp simulated_network.h
    "${MDNS_ROOT}/src/platform/MdnsPublisher.cpp" "${MDNS_ROOT}/src/platform/MdnsPublisher.h"
    "${MDNS_ROOT}/src/platform/MdnsServerAdapter.cpp" "${MDNS_ROOT}/src/platform/MdnsServerAdapter.h"
    "${MDNS_ROOT}/src/diagnostics/DiagnosticSanitizer.cpp" "${MDNS_ROOT}/src/diagnostics/DiagnosticLogSink.cpp")
set_target_properties(MdnsNetworkTest PROPERTIES AUTOMOC ON)
target_compile_definitions(MdnsNetworkTest PRIVATE AIRPLAY_WITH_UXPLAY=1)
target_include_directories(MdnsNetworkTest PRIVATE "${MDNS_ROOT}/src")
target_link_libraries(MdnsNetworkTest PRIVATE qmdnsengine_sim Qt6::Core Qt6::Network)
foreach(scenario lan_only dual_wifi_default dual_lan_default campus_client hot_switch legacy_unicast
        ipv6 overlap metadata_single metadata_dual ineligible address_change stale_reply join_retry send_failure
        loopback)
    add_test(NAME MdnsNetwork_${scenario} COMMAND MdnsNetworkTest ${scenario})
    set_tests_properties(MdnsNetwork_${scenario} PROPERTIES TIMEOUT 30)
endforeach()
