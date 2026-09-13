add_executable(fo-benchmarks bench_main.cpp)
target_link_libraries(fo-benchmarks PRIVATE SummonSoftwareLabs::FederationObservatory)
fo_apply_warnings(fo-benchmarks)
fo_apply_sanitizers(fo-benchmarks)
