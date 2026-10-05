# Use one portable default for direct CMake and scripted release builds.
# The existing UxPlay opt-out remains available for explicit local builds.
option(NO_MARCH_NATIVE "Disable build-host-specific CPU instructions" ON)
set(AIRPLAY_CPU_BASELINE "toolchain-default")

if(NOT NO_MARCH_NATIVE)
    set(AIRPLAY_CPU_BASELINE "unrestricted")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
       AND CMAKE_SIZEOF_VOID_P EQUAL 8
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
    set(AIRPLAY_CPU_BASELINE "x86-64")
    # Language guards also protect Windows resource compilation. C is enabled
    # later by vendored UxPlay, so select its compiler at generation time.
    add_compile_options(
        "$<$<COMPILE_LANG_AND_ID:C,GNU>:-march=x86-64;-mtune=generic>"
        "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:-march=x86-64;-mtune=generic>"
    )
    # Preserve generated-command evidence for compatibility acceptance.
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

message(STATUS "AirPlay CPU policy: ${AIRPLAY_CPU_BASELINE}; NO_MARCH_NATIVE=${NO_MARCH_NATIVE}")
