set(FO_TOOLS fo-coordinator fo-publisher fo-cli)

foreach(tool IN LISTS FO_TOOLS)
  add_executable(${tool} ${tool}.cpp)
  target_link_libraries(${tool} PRIVATE SummonSoftwareLabs::FederationObservatory)
  fo_apply_warnings(${tool})
  fo_apply_sanitizers(${tool})
  install(TARGETS ${tool} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endforeach()
