cmake_minimum_required(VERSION 3.20)

project(FederationObservatory
        VERSION 1.0.0
        DESCRIPTION "Observability runtime for cross-cluster and heterogeneous-accelerator federation behavior"
        HOMEPAGE_URL "https://github.com/pngen/Federation-Observatory"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(FO_BUILD_TESTS "Build the Federation Observatory test suites" ON)
option(FO_BUILD_EXAMPLES "Build the Federation Observatory examples" ON)
option(FO_BUILD_TOOLS "Build the fo-cli / fo-coordinator / fo-publisher tools" ON)
option(FO_BUILD_BENCHMARKS "Build the Federation Observatory benchmarks" ON)
option(FO_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer where supported" OFF)
option(FO_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)

set(FO_LIBRARY_TYPE STATIC CACHE STRING "Library type: STATIC or SHARED")
set_property(CACHE FO_LIBRARY_TYPE PROPERTY STRINGS STATIC SHARED)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------
# Build-quality flags are applied directly to each first-party target rather than
# through interface libraries, so that nothing internal leaks into the installed export
# set and a downstream consumer never inherits our warning policy.
function(fo_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc /bigobj)
    if(FO_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
      -Wunused -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2)
    if(FO_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

function(fo_apply_sanitizers target)
  if(FO_SANITIZE)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
      target_link_options(${target} PRIVATE /fsanitize=address)
    else()
      target_compile_options(${target} PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
      target_link_options(${target} PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(FO_PUBLIC_HEADERS
  include/federation_observatory/version.hpp
  include/federation_observatory/export.hpp
  include/federation_observatory/errors.hpp
  include/federation_observatory/ids.hpp
  include/federation_observatory/codec.hpp
  include/federation_observatory/bounds.hpp
  include/federation_observatory/evidence.hpp
  include/federation_observatory/explanation.hpp
  include/federation_observatory/capability.hpp
  include/federation_observatory/capacity.hpp
  include/federation_observatory/model.hpp
  include/federation_observatory/compatibility.hpp
  include/federation_observatory/portability.hpp
  include/federation_observatory/migration.hpp
  include/federation_observatory/placement.hpp
  include/federation_observatory/analysis.hpp
  include/federation_observatory/snapshot.hpp
  include/federation_observatory/records_codec.hpp
  include/federation_observatory/state_store.hpp
  include/federation_observatory/observatory.hpp
  include/federation_observatory/net.hpp
  include/federation_observatory/process.hpp
  include/federation_observatory/protocol.hpp
  include/federation_observatory/coordinator.hpp
  include/federation_observatory/client.hpp
  include/federation_observatory/publisher.hpp
  include/federation_observatory/synthetic.hpp
  include/federation_observatory/hardware.hpp)

set(FO_SOURCES
  src/foundation.cpp
  src/capability.cpp
  src/model.cpp
  src/compatibility.cpp
  src/capacity.cpp
  src/placement.cpp
  src/portability.cpp
  src/migration.cpp
  src/analysis.cpp
  src/snapshot.cpp
  src/records_codec.cpp
  src/state_store.cpp
  src/observatory.cpp
  src/net.cpp
  src/process.cpp
  src/protocol.cpp
  src/coordinator.cpp
  src/client.cpp
  src/publisher.cpp
  src/synthetic.cpp
  src/hardware.cpp)

add_library(FederationObservatory ${FO_LIBRARY_TYPE} ${FO_SOURCES} ${FO_PUBLIC_HEADERS})
add_library(SummonSoftwareLabs::FederationObservatory ALIAS FederationObservatory)

target_include_directories(FederationObservatory
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(FederationObservatory PUBLIC Threads::Threads)

fo_apply_warnings(FederationObservatory)
fo_apply_sanitizers(FederationObservatory)

target_compile_definitions(FederationObservatory PRIVATE FO_BUILDING_LIBRARY=1)
if(FO_LIBRARY_TYPE STREQUAL "SHARED")
  target_compile_definitions(FederationObservatory PUBLIC FO_USE_SHARED=1 PRIVATE FO_BUILD_SHARED=1)
endif()

# ws2_32 provides the sockets; iphlpapi provides the real adapter inventory the host
# probe reports. Both are Windows system libraries, so they do not become a portable
# dependency of the installed package.
if(WIN32)
  target_link_libraries(FederationObservatory PRIVATE ws2_32 iphlpapi)
endif()

set_target_properties(FederationObservatory PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  OUTPUT_NAME "FederationObservatory")

# ---------------------------------------------------------------------------
# Subprojects
# ---------------------------------------------------------------------------
if(FO_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
if(FO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(FO_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(FO_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
install(TARGETS FederationObservatory
  EXPORT FederationObservatoryTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT FederationObservatoryTargets
  FILE FederationObservatoryTargets.cmake
  NAMESPACE SummonSoftwareLabs::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FederationObservatory)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FederationObservatoryConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/FederationObservatoryConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FederationObservatory)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/FederationObservatoryConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/FederationObservatoryConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/FederationObservatoryConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FederationObservatory)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/FederationObservatory)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/FederationObservatory)

message(STATUS "Federation Observatory ${PROJECT_VERSION}: library=${FO_LIBRARY_TYPE} sanitize=${FO_SANITIZE} warnings_as_errors=${FO_WARNINGS_AS_ERRORS}")
