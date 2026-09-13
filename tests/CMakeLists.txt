set(FO_TEST_SOURCES
  test_main.cpp
  fixture.cpp
  test_foundation.cpp
  test_capability.cpp
  test_compatibility.cpp
  test_placement.cpp
  test_stranded.cpp
  test_fragmentation.cpp
  test_migration.cpp
  test_portability.cpp
  test_drift.cpp
  test_observatory.cpp
  test_protocol.cpp
  test_persistence.cpp
  test_property.cpp
  test_race.cpp
  test_synthetic.cpp
  test_hardware.cpp
  test_multiprocess.cpp)

add_executable(fo-tests ${FO_TEST_SOURCES})
target_include_directories(fo-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fo-tests PRIVATE SummonSoftwareLabs::FederationObservatory)
fo_apply_warnings(fo-tests)
fo_apply_sanitizers(fo-tests)

# Tests run plainly. No TIMEOUT property is set anywhere: a test that hangs is a defect
# to diagnose, never something to bound away.
foreach(source IN LISTS FO_TEST_SOURCES)
  if(NOT source STREQUAL "test_main.cpp" AND NOT source STREQUAL "fixture.cpp")
    string(REGEX REPLACE "\\.cpp$" "" target_name "${source}")
    add_test(NAME ${target_name} COMMAND fo-tests --filter=${target_name})
  endif()
endforeach()
add_test(NAME fo-tests-all COMMAND fo-tests)
