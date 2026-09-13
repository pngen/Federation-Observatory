set(FO_EXAMPLES
  ex_basic_snapshot
  ex_placement_explanation
  ex_capability_mismatch
  ex_stranded_capacity
  ex_federation_fragmentation
  ex_migration_analysis
  ex_portability_failure
  ex_publisher_fencing
  ex_installed_consumer)

foreach(example IN LISTS FO_EXAMPLES)
  add_executable(${example} ${example}.cpp)
  target_link_libraries(${example} PRIVATE SummonSoftwareLabs::FederationObservatory)
  fo_apply_warnings(${example})
  fo_apply_sanitizers(${example})
endforeach()
